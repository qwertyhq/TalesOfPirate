#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsPlayerCharacter.h"

#include "CorsairsNet/include/Packet.h"
#include "CorsairsSession.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

namespace
{
using Corsairs::Net::WPacket;

constexpr int64 LocalWorldId = 77;
constexpr int64 MovementSpeed = 450;
constexpr uint16 BeginActionCommand = 6;
const FIntPoint Spawn(223325, 278475);

void BroadcastLocalEvent(
	UCorsairsSession* Session,
	const ECorsairsMovementEventType Type,
	const FIntPoint Endpoint,
	const bool bRequireNeutral = false)
{
	FCorsairsMovementEvent Event;
	Event.WorldId = LocalWorldId;
	Event.Type = Type;
	Event.Waypoints = {Endpoint};
	Event.Endpoint = Endpoint;
	Event.bLocal = true;
	Event.bServerDriven = false;
	Event.MovementSpeedCmPerSecond = MovementSpeed;
	Event.bRequireNeutral = bRequireNeutral;
	Session->OnMovementChanged.Broadcast(Event);
}

bool SkipMsgPackInteger(const uint8*& Cursor, const uint8* End)
{
	if (Cursor >= End)
	{
		return false;
	}
	const uint8 Prefix = *Cursor++;
	int32 PayloadSize = 0;
	if (Prefix <= 0x7f || Prefix >= 0xe0)
	{
		return true;
	}
	switch (Prefix)
	{
	case 0xcc:
	case 0xd0:
		PayloadSize = 1;
		break;
	case 0xcd:
	case 0xd1:
		PayloadSize = 2;
		break;
	case 0xce:
	case 0xd2:
		PayloadSize = 4;
		break;
	case 0xcf:
	case 0xd3:
		PayloadSize = 8;
		break;
	default:
		return false;
	}
	if (End - Cursor < PayloadSize)
	{
		return false;
	}
	Cursor += PayloadSize;
	return true;
}

TArray<FIntPoint> ReadMovePath(WPacket& Wire)
{
	const uint8* Data = Wire.Data();
	const int32 PacketSize =
		(static_cast<int32>(Data[0]) << 8) | Data[1];
	const uint16 Command =
		(static_cast<uint16>(Data[6]) << 8) | Data[7];
	if (PacketSize < 8 || Command != BeginActionCommand)
	{
		return {};
	}

	const uint8* Cursor = Data + 8;
	const uint8* End = Data + PacketSize;
	if (!SkipMsgPackInteger(Cursor, End) ||
		!SkipMsgPackInteger(Cursor, End) ||
		!SkipMsgPackInteger(Cursor, End) ||
		Cursor >= End)
	{
		return {};
	}

	uint32 Count = 0;
	const uint8 Prefix = *Cursor++;
	if (Prefix == 0xc4 && Cursor < End)
	{
		Count = *Cursor++;
	}
	else if (Prefix == 0xc5 && End - Cursor >= 2)
	{
		Count = (static_cast<uint32>(Cursor[0]) << 8) | Cursor[1];
		Cursor += 2;
	}
	else
	{
		return {};
	}
	if (Count % 8 != 0 ||
		static_cast<uint32>(End - Cursor) < Count)
	{
		return {};
	}

	TArray<FIntPoint> Path;
	for (int32 Offset = 0; Offset + 8 <= Count; Offset += 8)
	{
		int32 X = 0;
		int32 Y = 0;
		FMemory::Memcpy(&X, Cursor + Offset, sizeof(X));
		FMemory::Memcpy(&Y, Cursor + Offset + sizeof(X), sizeof(Y));
		Path.Emplace(X, Y);
	}
	return Path;
}

int32 CountMovementBindings(
	const UCorsairsSession* Session,
	const ACorsairsPlayerCharacter* Pawn)
{
	int32 Count = 0;
	for (const UObject* Object : Session->OnMovementChanged.GetAllObjects())
	{
		if (Object == Pawn)
		{
			++Count;
		}
	}
	return Count;
}

int32 CountAuthorityBindings(
	const UCorsairsSession* Session,
	const ACorsairsPlayerCharacter* Pawn)
{
	int32 Count = 0;
	for (const UObject* Object :
		Session->OnMovementAuthorityChanged.GetAllObjects())
	{
		if (Object == Pawn)
		{
			++Count;
		}
	}
	return Count;
}

bool CreatePossessedPawn(
	FAutomationTestBase* Test,
	FTestWorldWrapper& TestWorld,
	ACorsairsPlayerCharacter*& Pawn,
	UCorsairsSession*& Session)
{
	if (!Test->TestTrue(
		TEXT("game world created"),
		TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = TestWorld.GetTestWorld();
	if (!Test->TestNotNull(TEXT("game world exists"), World))
	{
		return false;
	}
	if (!Test->TestTrue(
		TEXT("game world begins play"),
		TestWorld.BeginPlayInTestWorld()))
	{
		TestWorld.ForwardErrorMessages(Test);
		return false;
	}

	APlayerController* Controller = World->SpawnActor<APlayerController>();
	Pawn = World->SpawnActor<ACorsairsPlayerCharacter>();
	if (!Test->TestNotNull(TEXT("player controller spawned"), Controller) ||
		!Test->TestNotNull(TEXT("player pawn spawned"), Pawn))
	{
		return false;
	}
	Controller->Possess(Pawn);
	Pawn->GetCharacterMovement()->GravityScale = 0.0f;
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Pawn->SetActorLocation(FVector(-Spawn.Y, Spawn.X, 321.0));

	Session = NewObject<UCorsairsSession>(World);
	Session->SetInWorldForTests(LocalWorldId, Spawn);
	Session->SetMovementSpeedForTests(MovementSpeed);
	Pawn->AttachSession(Session);
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerFirstSegmentFromSpawnTest,
	"Corsairs.Movement.Pawn.FirstSegmentFromSpawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerFirstSegmentFromSpawnTest::RunTest(const FString&)
{
	// Mutation: вернуть initial-report discard или начинать первый MOVE с
	// текущей predicted position вместо reducer-confirmed spawn.
	FTestWorldWrapper TestWorld;
	ACorsairsPlayerCharacter* Pawn = nullptr;
	UCorsairsSession* Session = nullptr;
	if (!CreatePossessedPawn(this, TestWorld, Pawn, Session))
	{
		return false;
	}

	TArray<TArray<FIntPoint>> SentPaths;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			SentPaths.Add(ReadMovePath(Wire));
			return true;
		});
	const FIntPoint Current(223525, 278575);
	Pawn->SetActorLocation(FVector(-Current.Y, Current.X, 321.0));
	Pawn->Tick(0.51f);

	TestEqual(TEXT("first timer report sends one path"), SentPaths.Num(), 1);
	if (SentPaths.Num() == 1)
	{
		TestEqual(TEXT("first path has two distinct points"), SentPaths[0].Num(), 2);
		if (SentPaths[0].Num() == 2)
		{
			TestEqual(TEXT("first path starts at server spawn"), SentPaths[0][0], Spawn);
			TestEqual(TEXT("first path ends at predicted position"), SentPaths[0][1], Current);
		}
	}
	TestEqual(
		TEXT("positive ATTR_MSPD configures flying speed"),
		Pawn->GetCharacterMovement()->MaxFlySpeed,
		static_cast<float>(MovementSpeed));
	TestWorld.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerHeldInputNoPacketFloodTest,
	"Corsairs.Movement.Pawn.HeldInputNoPacketFlood",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerHeldInputNoPacketFloodTest::RunTest(const FString&)
{
	// Mutation: MoveForward returns on zero before recording it, unlocking
	// clears the latch, held input bypasses the gate, либо zero ATTR_MSPD
	// оставляет ранее выставленную скорость валидной.
	FTestWorldWrapper TestWorld;
	ACorsairsPlayerCharacter* Pawn = nullptr;
	UCorsairsSession* Session = nullptr;
	if (!CreatePossessedPawn(this, TestWorld, Pawn, Session))
	{
		return false;
	}

	int32 SendCount = 0;
	Session->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++SendCount;
			return true;
		});
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
	TestTrue(
		TEXT("MoveForward probe reaches the real axis callback"),
		!Pawn->GetPendingMovementInputVector().IsNearlyZero());
	BroadcastLocalEvent(
		Session,
		ECorsairsMovementEventType::Rejected,
		Spawn,
		true);
	const int32 SendsAfterReject = SendCount;

	constexpr float DeltaSeconds = 1.0f / 60.0f;
	for (int32 Tick = 0; Tick < 120; ++Tick)
	{
		Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
		TestWorld.TickTestWorld(DeltaSeconds);
	}
	TestEqual(
		TEXT("two seconds of held input sends no MOVE"),
		SendCount,
		SendsAfterReject);
	TestTrue(
		TEXT("held input remains consumed while latched"),
		Pawn->GetPendingMovementInputVector().IsNearlyZero());

	Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 0.0f);
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveRight"), 0.0f);
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveDiagonal"), 1.0f);
	TestTrue(
		TEXT("unknown probe axis has no side effects"),
		Pawn->GetPendingMovementInputVector().IsNearlyZero());
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveRight"), 1.0f);
	TestTrue(
		TEXT("MoveRight probe reaches the real axis callback"),
		!Pawn->GetPendingMovementInputVector().IsNearlyZero());
	Pawn->ConsumeMovementInputVector();
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveRight"), 0.0f);
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
	Pawn->SetActorLocation(FVector(-Spawn.Y, Spawn.X + 200, 321.0));
	Pawn->Tick(0.51f);
	TestEqual(
		TEXT("neutral and re-press sends exactly one MOVE"),
		SendCount,
		SendsAfterReject + 1);

	Pawn->ConsumeMovementInputVector();
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 0.0f);
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveRight"), 0.0f);
	AddExpectedError(
		TEXT("ATTR_MSPD"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Session->SetMovementSpeedForTests(0);
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
	TestTrue(
		TEXT("zero speed invalidates prior positive speed and locks prediction"),
		Pawn->GetPendingMovementInputVector().IsNearlyZero());
	TestWorld.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerAuthorityTransitionRaceTest,
	"Corsairs.Movement.Pawn.AuthorityTransitionRace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerAuthorityTransitionRaceTest::RunTest(const FString&)
{
	// Mutation: poll Session authority only from axis callbacks, omit report
	// gating, or forget stop/consume on a rising combined lock. A skill that
	// reserves and finishes between samples then leaks held prediction/MOVE.
	FTestWorldWrapper SkillWorld;
	ACorsairsPlayerCharacter* SkillPawn = nullptr;
	UCorsairsSession* SkillSession = nullptr;
	if (!CreatePossessedPawn(
		this, SkillWorld, SkillPawn, SkillSession))
	{
		return false;
	}

	int32 SkillSendCount = 0;
	SkillSession->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++SkillSendCount;
			return true;
		});
	SkillPawn->SetActorLocation(
		FVector(-Spawn.Y, Spawn.X + 200, 321.0));
	SkillPawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
	SkillPawn->GetCharacterMovement()->Velocity =
		FVector(300.0, 125.0, 0.0);
	const FVector BeforeSkill = SkillPawn->GetActorLocation();
	SkillSession->OnMovementAuthorityChanged.Broadcast(true, 1);
	SkillSession->OnMovementAuthorityChanged.Broadcast(false, 2);
	TestTrue(TEXT("transient skill lock immediately zeros velocity"),
		SkillPawn->GetCharacterMovement()->Velocity.IsNearlyZero());
	TestTrue(TEXT("transient skill lock immediately consumes input"),
		SkillPawn->GetPendingMovementInputVector().IsNearlyZero());

	constexpr float DeltaSeconds = 1.0f / 60.0f;
	for (int32 Tick = 0; Tick < 120; ++Tick)
	{
		SkillPawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
		SkillWorld.TickTestWorld(DeltaSeconds);
	}
	TestEqual(TEXT("locked/unlocked race keeps transform stable"),
		SkillPawn->GetActorLocation(), BeforeSkill);
	TestEqual(TEXT("held input after transient skill sends no MOVE"),
		SkillSendCount, 0);

	SkillPawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 0.0f);
	SkillPawn->ApplyMovementAxisForProbe(TEXT("MoveRight"), 0.0f);
	SkillPawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
	SkillPawn->Tick(0.51f);
	TestEqual(TEXT("neutral and re-press permit exactly one MOVE"),
		SkillSendCount, 1);
	SkillWorld.ForwardErrorMessages(this);

	FTestWorldWrapper SpeedWorld;
	ACorsairsPlayerCharacter* SpeedPawn = nullptr;
	UCorsairsSession* SpeedSession = nullptr;
	if (!CreatePossessedPawn(
		this, SpeedWorld, SpeedPawn, SpeedSession))
	{
		return false;
	}
	int32 SpeedSendCount = 0;
	SpeedSession->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++SpeedSendCount;
			return true;
		});
	SpeedPawn->SetActorLocation(
		FVector(-Spawn.Y, Spawn.X + 200, 321.0));
	SpeedPawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
	SpeedPawn->GetCharacterMovement()->Velocity =
		FVector(300.0, 125.0, 0.0);
	const FVector BeforeZeroSpeed = SpeedPawn->GetActorLocation();
	AddExpectedError(
		TEXT("ATTR_MSPD"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	SpeedSession->SetMovementSpeedForTests(0);
	for (int32 Tick = 0; Tick < 120; ++Tick)
	{
		SpeedWorld.TickTestWorld(DeltaSeconds);
	}
	TestEqual(TEXT("zero ATTR_MSPD keeps transform stable"),
		SpeedPawn->GetActorLocation(), BeforeZeroSpeed);
	TestTrue(TEXT("zero ATTR_MSPD immediately zeros velocity"),
		SpeedPawn->GetCharacterMovement()->Velocity.IsNearlyZero());
	TestTrue(TEXT("zero ATTR_MSPD consumes pending input"),
		SpeedPawn->GetPendingMovementInputVector().IsNearlyZero());
	TestEqual(TEXT("zero ATTR_MSPD suppresses timer reports"),
		SpeedSendCount, 0);
	SpeedWorld.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerSessionAuthorityRollbackTest,
	"Corsairs.Movement.Pawn.SessionAuthorityRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerSessionAuthorityRollbackTest::RunTest(const FString&)
{
	// Mutation: remove Session's production dynamic Broadcast or the pawn's
	// AddDynamic binding. Direct delegate tests still pass, but a real
	// UseSkillOn transport rollback then fails to stop/latch held prediction.
	FTestWorldWrapper TestWorld;
	ACorsairsPlayerCharacter* Pawn = nullptr;
	UCorsairsSession* Session = nullptr;
	if (!CreatePossessedPawn(this, TestWorld, Pawn, Session))
	{
		return false;
	}

	FCorsairsWorldActor Target;
	Target.WorldId = 88;
	Target.Position = FIntPoint(224000, 279000);
	Target.Handle = 9088;
	Session->AddVisibleActorForTests(Target);
	TestEqual(TEXT("pawn has one production authority listener"),
		CountAuthorityBindings(Session, Pawn), 1);

	int32 SkillTransportAttempts = 0;
	Session->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++SkillTransportAttempts;
			return false;
		});
	Pawn->SetActorLocation(FVector(-Spawn.Y, Spawn.X + 200, 321.0));
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
	Pawn->GetCharacterMovement()->Velocity = FVector(300.0, 125.0, 0.0);
	const FVector BeforeSkill = Pawn->GetActorLocation();
	const ECorsairsActionRequestResult SkillResult =
		Session->UseSkillOn(26, Target.WorldId);
	TestEqual(TEXT("real Skill rollback is transport failure"),
		static_cast<uint8>(SkillResult),
		static_cast<uint8>(ECorsairsActionRequestResult::TransportFailed));
	TestEqual(TEXT("failed Skill attempts transport exactly once"),
		SkillTransportAttempts, 1);
	TestEqual(TEXT("real Session publishes rising and falling epochs"),
		Session->GetMovementAuthorityEpoch(), 2LL);
	TestTrue(TEXT("real rising Broadcast zeros velocity"),
		Pawn->GetCharacterMovement()->Velocity.IsNearlyZero());
	TestTrue(TEXT("real rising Broadcast consumes pending input"),
		Pawn->GetPendingMovementInputVector().IsNearlyZero());

	int32 MoveSendCount = 0;
	Session->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++MoveSendCount;
			return true;
		});
	constexpr float DeltaSeconds = 1.0f / 60.0f;
	for (int32 Tick = 0; Tick < 120; ++Tick)
	{
		Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
		TestWorld.TickTestWorld(DeltaSeconds);
	}
	TestEqual(TEXT("real rollback keeps transform stable"),
		Pawn->GetActorLocation(), BeforeSkill);
	TestEqual(TEXT("held input after real rollback sends no MOVE"),
		MoveSendCount, 0);

	Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 0.0f);
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveRight"), 0.0f);
	Pawn->ApplyMovementAxisForProbe(TEXT("MoveForward"), 1.0f);
	Pawn->Tick(0.51f);
	TestEqual(TEXT("real rollback neutral/re-press sends one MOVE"),
		MoveSendCount, 1);
	TestWorld.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsPlayerReconcilesTerminalExactlyTest,
	"Corsairs.Movement.Pawn.ReconcilesTerminalExactly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsPlayerReconcilesTerminalExactlyTest::RunTest(const FString&)
{
	// Mutation: не снять старый AddDynamic при reattach, перепутать знак Y,
	// не погасить velocity/input либо применить manual AcceptedPath как follower.
	FTestWorldWrapper TestWorld;
	ACorsairsPlayerCharacter* Pawn = nullptr;
	UCorsairsSession* Session = nullptr;
	if (!CreatePossessedPawn(this, TestWorld, Pawn, Session))
	{
		return false;
	}

	Pawn->AttachSession(Session);
	TestEqual(
		TEXT("reattach leaves exactly one delegate receiver"),
		CountMovementBindings(Session, Pawn),
		1);
	TestEqual(
		TEXT("reattach leaves exactly one authority receiver"),
		CountAuthorityBindings(Session, Pawn),
		1);
	Pawn->SetActorLocation(FVector(-279000.0, 224000.0, 321.0));
	Pawn->GetCharacterMovement()->Velocity = FVector(120.0, 80.0, 0.0);
	Pawn->AddMovementInput(FVector::ForwardVector, 1.0f);
	const FIntPoint Terminal(223725, 278875);
	BroadcastLocalEvent(
		Session,
		ECorsairsMovementEventType::Terminal,
		Terminal);

	TestEqual(
		TEXT("terminal teleports exact authoritative XY and keeps current Z"),
		Pawn->GetActorLocation(),
		FVector(-Terminal.Y, Terminal.X, 321.0));
	TestTrue(
		TEXT("terminal zeros character velocity"),
		Pawn->GetCharacterMovement()->Velocity.IsNearlyZero());
	TestTrue(
		TEXT("terminal consumes pending movement input"),
		Pawn->GetPendingMovementInputVector().IsNearlyZero());

	const FVector Reconciled = Pawn->GetActorLocation();
	FCorsairsMovementEvent ManualAccepted;
	ManualAccepted.WorldId = LocalWorldId;
	ManualAccepted.Type = ECorsairsMovementEventType::AcceptedPath;
	ManualAccepted.Waypoints = {Terminal, FIntPoint(224500, 279500)};
	ManualAccepted.Endpoint = ManualAccepted.Waypoints.Last();
	ManualAccepted.bLocal = true;
	ManualAccepted.bServerDriven = false;
	ManualAccepted.MovementSpeedCmPerSecond = MovementSpeed;
	Session->OnMovementChanged.Broadcast(ManualAccepted);
	TestEqual(
		TEXT("manual AcceptedPath never moves the pawn"),
		Pawn->GetActorLocation(),
		Reconciled);

	UWorld* World = TestWorld.GetTestWorld();
	UCorsairsSession* Replacement = NewObject<UCorsairsSession>(World);
	Replacement->SetInWorldForTests(LocalWorldId, Terminal);
	Replacement->SetMovementSpeedForTests(MovementSpeed);
	Pawn->AttachSession(Replacement);
	Pawn->AttachSession(Replacement);
	TestFalse(
		TEXT("reattach detaches the previous session"),
		CountMovementBindings(Session, Pawn) > 0);
	TestEqual(
		TEXT("replacement has one receiver after repeated attach"),
		CountMovementBindings(Replacement, Pawn),
		1);
	TestEqual(
		TEXT("replacement has one authority receiver after repeated attach"),
		CountAuthorityBindings(Replacement, Pawn),
		1);

	FCorsairsMovementEvent ReplacementTerminal;
	ReplacementTerminal.WorldId = LocalWorldId;
	ReplacementTerminal.Type = ECorsairsMovementEventType::Terminal;
	ReplacementTerminal.Endpoint = FIntPoint(223900, 278900);
	ReplacementTerminal.bLocal = true;
	ReplacementTerminal.MovementSpeedCmPerSecond = MovementSpeed;
	Replacement->OnMovementChanged.Broadcast(ReplacementTerminal);
	TestEqual(
		TEXT("one replacement broadcast reaches one handler"),
		Pawn->GetActorLocation(),
		FVector(-278900.0, 223900.0, 321.0));

	World->DestroyActor(Pawn);
	TestFalse(
		TEXT("EndPlay removes movement delegate"),
		CountMovementBindings(Replacement, Pawn) > 0);
	TestFalse(
		TEXT("EndPlay removes authority delegate"),
		CountAuthorityBindings(Replacement, Pawn) > 0);
	TestWorld.ForwardErrorMessages(this);
	return true;
}

#endif
