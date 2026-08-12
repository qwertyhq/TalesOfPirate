#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCharacter.h"
#include "CorsairsGameMode.h"
#include "CorsairsPlayerCharacter.h"

#include "Components/CapsuleComponent.h"
#include "CorsairsNet/include/Packet.h"
#include "CorsairsSession.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

namespace
{
	using Corsairs::Net::WPacket;

	constexpr int64 LocalWorldId = 77;
	constexpr double ServerSpeedCmPerSecond = 200.0;

	FActorSpawnParameters AlwaysSpawnParameters()
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Parameters;
	}

	int32 CountMovementBindings(
		const UCorsairsSession* Session,
		const UObject* Object)
	{
		int32 Count = 0;
		for (const UObject* BoundObject :
			Session->OnMovementChanged.GetAllObjects())
		{
			if (BoundObject == Object)
			{
				++Count;
			}
		}
		return Count;
	}

	FCorsairsMovementEvent MakeAcceptedPath(
		const int64 WorldId,
		const bool bLocal,
		const bool bServerDriven,
		const double Speed,
		const TArray<FIntPoint>& Waypoints)
	{
		FCorsairsMovementEvent Event;
		Event.WorldId = WorldId;
		Event.Type = ECorsairsMovementEventType::AcceptedPath;
		Event.Waypoints = Waypoints;
		Event.Endpoint = Waypoints.IsEmpty()
			? FIntPoint::ZeroValue
			: Waypoints.Last();
		Event.bLocal = bLocal;
		Event.bServerDriven = bServerDriven;
		Event.MovementSpeedCmPerSecond = Speed;
		return Event;
	}

	FCorsairsMovementEvent MakeTerminal(
		const int64 WorldId,
		const ECorsairsMovementEventType Type,
		const FIntPoint Endpoint)
	{
		FCorsairsMovementEvent Event;
		Event.WorldId = WorldId;
		Event.Type = Type;
		Event.Waypoints = {Endpoint};
		Event.Endpoint = Endpoint;
		Event.bServerDriven = true;
		Event.MovementSpeedCmPerSecond = ServerSpeedCmPerSecond;
		return Event;
	}

	ACorsairsCharacter* FindRemoteAt(
		UWorld* World,
		const ACorsairsPlayerCharacter* Local,
		const FIntPoint SourcePosition)
	{
		const FVector Expected(
			-static_cast<double>(SourcePosition.Y),
			static_cast<double>(SourcePosition.X),
			0.0);
		for (TActorIterator<ACorsairsCharacter> It(World); It; ++It)
		{
			if (*It != Local &&
				FMath::IsNearlyEqual((*It)->GetActorLocation().X, Expected.X) &&
				FMath::IsNearlyEqual((*It)->GetActorLocation().Y, Expected.Y))
			{
				return *It;
			}
		}
		return nullptr;
	}

	struct FMovementRoutingFixture
	{
		bool SetUp(FAutomationTestBase* Test)
		{
			if (!Test->TestTrue(
					TEXT("routing game world created"),
					TestWorld.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}

			UWorld* World = TestWorld.GetTestWorld();
			GameMode = World->SpawnActor<ACorsairsGameMode>(
				ACorsairsGameMode::StaticClass(),
				FVector::ZeroVector,
				FRotator::ZeroRotator,
				AlwaysSpawnParameters());
			Controller = World->SpawnActor<APlayerController>();
			Local = World->SpawnActor<ACorsairsPlayerCharacter>(
				ACorsairsPlayerCharacter::StaticClass(),
				FVector::ZeroVector,
				FRotator::ZeroRotator,
				AlwaysSpawnParameters());
			if (!Test->TestNotNull(TEXT("routing game mode spawned"), GameMode) ||
				!Test->TestNotNull(TEXT("routing controller spawned"), Controller) ||
				!Test->TestNotNull(TEXT("routing local pawn spawned"), Local))
			{
				return false;
			}

			GameMode->bAutoLogin = false;
			Controller->Possess(Local);
			if (!Test->TestTrue(
					TEXT("routing world begins play"),
					TestWorld.BeginPlayInTestWorld()))
			{
				TestWorld.ForwardErrorMessages(Test);
				return false;
			}
			if (!Test->TestTrue(
					TEXT("routing fixture startup is ready"),
					GameMode->IsStartupReady()))
			{
				TestWorld.ForwardErrorMessages(Test);
				return false;
			}

			Session = GameMode->GetSession();
			if (!Test->TestNotNull(TEXT("routing session exists"), Session))
			{
				return false;
			}
			Session->SetInWorldForTests(LocalWorldId, FIntPoint(0, 0));
			Session->SetMovementSpeedForTests(
				static_cast<int64>(ServerSpeedCmPerSecond));
			Local->AttachSession(Session);

			FString GroundError;
			// Один source-tile: (0..49,0..49)=10, (50..99,0..49)=20,
			// (0..49,50..99)=30, (50..99,50..99)=40 cm.
			const TArray<uint8> GroundBytes = {0x02, 0x04, 0x06, 0x08};
			if (!Test->TestTrue(
					TEXT("routing fixture loads ground"),
					GameMode->LoadCharacterGroundFromBytesForTests(
						1,
						1,
						GroundBytes,
						GroundError)))
			{
				TestWorld.ForwardErrorMessages(Test);
				return false;
			}
			GameMode->GroundCharacterForTests(Local, FIntPoint(0, 0));
			return true;
		}

		void ForwardErrors(FAutomationTestBase* Test)
		{
			TestWorld.ForwardErrorMessages(Test);
		}

		FTestWorldWrapper TestWorld;
		ACorsairsGameMode* GameMode = nullptr;
		APlayerController* Controller = nullptr;
		ACorsairsPlayerCharacter* Local = nullptr;
		UCorsairsSession* Session = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGameModeDynamicLifecycleTest,
	"Corsairs.Movement.Routing.GameModeDynamicLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGameModeDynamicLifecycleTest::RunTest(const FString&)
{
	// Mutation: не добавить GameMode dynamic receiver, подписать дважды либо
	// оставить receiver на сохранённой Session после EndPlay.
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	const FCorsairsWorldActor RemoteState = {
		88, TEXT("LifecycleRemote"), FIntPoint(0, 0), 0, 1};
	Fixture.GameMode->HandleActorSeenForTests(RemoteState);
	ACorsairsCharacter* Remote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, RemoteState.Position);
	if (!TestNotNull(TEXT("lifecycle remote is registered"), Remote))
	{
		return false;
	}

	TestEqual(TEXT("GameMode has exactly one movement binding"),
		CountMovementBindings(Fixture.Session, Fixture.GameMode), 1);
	const FVector BeforeMove = Remote->GetActorLocation();
	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		RemoteState.WorldId,
		false,
		true,
		ServerSpeedCmPerSecond,
		{FIntPoint(0, 0), FIntPoint(50, 0)}));
	Fixture.TestWorld.TickTestWorld(0.1f);
	TestEqual(TEXT("one real multicast advances mapped remote once"),
		Remote->GetActorLocation(),
		FVector(0.0, 20.0, BeforeMove.Z));

	UCorsairsSession* SavedSession = Fixture.Session;
	TestTrue(TEXT("GameMode accepts explicit destroy"), Fixture.GameMode->Destroy());
	TestEqual(TEXT("EndPlay removes GameMode movement binding"),
		CountMovementBindings(SavedSession, Fixture.GameMode), 0);
	TestTrue(TEXT("EndPlay destroys mapped remote"),
		Remote->IsActorBeingDestroyed());
	SavedSession->OnMovementChanged.Broadcast(MakeAcceptedPath(
		RemoteState.WorldId,
		false,
		true,
		ServerSpeedCmPerSecond,
		{FIntPoint(0, 0), FIntPoint(90, 0)}));
	TestTrue(TEXT("saved-session broadcast leaves destroyed remote destroyed"),
		Remote->IsActorBeingDestroyed());
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsLocalDeliveredOnceTest,
	"Corsairs.Movement.Routing.LocalDeliveredOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsLocalDeliveredOnceTest::RunTest(const FString&)
{
	// Mutation: GameMode forwards local events, the pawn starts manual paths,
	// or a skill path ignores the event speed/authority lock.
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	TestEqual(TEXT("pawn has one direct movement receiver"),
		CountMovementBindings(Fixture.Session, Fixture.Local), 1);
	TestEqual(TEXT("GameMode has one separate remote-only receiver"),
		CountMovementBindings(Fixture.Session, Fixture.GameMode), 1);
	const FVector Start = Fixture.Local->GetActorLocation();
	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		LocalWorldId,
		true,
		false,
		ServerSpeedCmPerSecond,
		{FIntPoint(0, 0), FIntPoint(80, 0)}));
	Fixture.TestWorld.TickTestWorld(0.1f);
	TestEqual(TEXT("manual local AcceptedPath leaves pawn under prediction"),
		Fixture.Local->GetActorLocation(), Start);
	TestEqual(TEXT("manual local path never enters inherited follower"),
		Fixture.Local->GetServerPathAcceptCountForTests(), 0);

	FCorsairsWorldActor Target;
	Target.WorldId = 99;
	Target.Position = FIntPoint(80, 0);
	Target.Handle = 9099;
	Fixture.Session->AddVisibleActorForTests(Target);
	Fixture.Session->SetSendOverrideForTests([](WPacket&) { return true; });
	TestEqual(TEXT("skill reserves server authority"),
		static_cast<uint8>(Fixture.Session->UseSkillOn(26, Target.WorldId)),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));
	TestTrue(TEXT("skill lock remains active before server path"),
		Fixture.Session->IsMovementAuthorityLocked());

	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		LocalWorldId,
		true,
		true,
		ServerSpeedCmPerSecond,
		{FIntPoint(0, 0), FIntPoint(50, 0)}));
	Fixture.TestWorld.TickTestWorld(0.1f);
	TestEqual(TEXT("one local server path transition advances by event speed"),
		Fixture.Local->GetActorLocation(),
		FVector(0.0, 20.0, Start.Z));
	TestEqual(TEXT("local server path reaches inherited follower exactly once"),
		Fixture.Local->GetServerPathAcceptCountForTests(), 1);
	TestTrue(TEXT("skill lock stays active while follower moves"),
		Fixture.Session->IsMovementAuthorityLocked());
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsRemoteByWorldIdTest,
	"Corsairs.Movement.Routing.RemoteByWorldId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsRemoteByWorldIdTest::RunTest(const FString&)
{
	// Mutation: route by any actor rather than Event.WorldId, replay an invalid
	// speed, fail to ground waypoint transforms, or fail to reconcile rejection.
	FMovementRoutingFixture Fixture;
	if (!Fixture.SetUp(this))
	{
		return false;
	}

	const FCorsairsWorldActor First = {
		101, TEXT("FirstRemote"), FIntPoint(10, 10), 0, 1};
	const FCorsairsWorldActor Second = {
		202, TEXT("SecondRemote"), FIntPoint(20, 20), 123, 1};
	Fixture.GameMode->HandleActorSeenForTests(First);
	Fixture.GameMode->HandleActorSeenForTests(Second);
	ACorsairsCharacter* FirstRemote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, First.Position);
	ACorsairsCharacter* SecondRemote = FindRemoteAt(
		Fixture.TestWorld.GetTestWorld(), Fixture.Local, Second.Position);
	if (!TestNotNull(TEXT("first mapped remote exists"), FirstRemote) ||
		!TestNotNull(TEXT("second mapped remote exists"), SecondRemote))
	{
		return false;
	}

	const FVector FirstBefore = FirstRemote->GetActorLocation();
	TestEqual(TEXT("second remote starts at its distinct first ground height"),
		SecondRemote->GetActorLocation(), FVector(-20.0, 20.0, 98.0));
	TestTrue(TEXT("remote actor preserves source angle semantics"),
		FMath::IsNearlyEqual(SecondRemote->GetActorRotation().Yaw, 123.0));
	TestEqual(TEXT("second remote starts with capsule bottom at 10 cm"),
		SecondRemote->GetActorLocation().Z -
			SecondRemote->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
		10.0);
	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		Second.WorldId,
		false,
		true,
		ServerSpeedCmPerSecond,
		{Second.Position, FIntPoint(70, 20)}));
	Fixture.TestWorld.TickTestWorld(0.25f);
	TestEqual(TEXT("unaddressed remote stays at its accepted position"),
		FirstRemote->GetActorLocation(), FirstBefore);
	TestEqual(TEXT("addressed remote follows waypoint at 20 cm ground height"),
		SecondRemote->GetActorLocation(),
		FVector(-20.0, 70.0, 108.0));
	TestEqual(TEXT("accepted waypoint preserves 20 cm capsule bottom"),
		SecondRemote->GetActorLocation().Z -
			SecondRemote->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
		20.0);
	TestTrue(TEXT("remote path applies rigid-basis facing"),
		FMath::IsNearlyEqual(SecondRemote->GetActorRotation().Yaw, 90.0));

	const FIntPoint RejectedEndpoint(30, 70);
	Fixture.Session->OnMovementChanged.Broadcast(MakeTerminal(
		Second.WorldId,
		ECorsairsMovementEventType::Rejected,
		RejectedEndpoint));
	TestEqual(TEXT("remote rejection reconciles into 30 cm ground cell"),
		SecondRemote->GetActorLocation(),
		FVector(-70.0, 30.0, 118.0));
	TestEqual(TEXT("rejected endpoint preserves 30 cm capsule bottom"),
		SecondRemote->GetActorLocation().Z -
			SecondRemote->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
		30.0);
	Fixture.TestWorld.TickTestWorld(0.5f);
	TestEqual(TEXT("rejected remote no longer replays old path"),
		SecondRemote->GetActorLocation(),
		FVector(-70.0, 30.0, 118.0));

	const FVector BeforeZeroSpeed = SecondRemote->GetActorLocation();
	Fixture.Session->OnMovementChanged.Broadcast(MakeAcceptedPath(
		Second.WorldId,
		false,
		true,
		0.0,
		{RejectedEndpoint, FIntPoint(90, 30)}));
	Fixture.TestWorld.TickTestWorld(0.5f);
	TestEqual(TEXT("zero speed never starts remote playback"),
		SecondRemote->GetActorLocation(), BeforeZeroSpeed);
	Fixture.ForwardErrors(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsUngroundedCharacterRigidBasisTest,
	"Corsairs.Movement.Routing.UngroundedCharacterRigidBasis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsUngroundedCharacterRigidBasisTest::RunTest(const FString&)
{
	// Mutation: в fallback-ветке без CharacterGround вернуть
	// старое отражение F=(x,-y) вместо rigid Q=(-y,x).
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("ungrounded character world created"), World))
	{
		return false;
	}

	ACorsairsCharacter* Character = World->SpawnActor<ACorsairsCharacter>(
		ACorsairsCharacter::StaticClass(),
		FVector(999.0, 999.0, 321.0),
		FRotator::ZeroRotator,
		AlwaysSpawnParameters());
	if (!TestNotNull(TEXT("ungrounded character spawned"), Character))
	{
		World->DestroyWorld(false);
		return false;
	}

	Character->HandleServerMovementChanged(MakeAcceptedPath(
		303,
		false,
		true,
		ServerSpeedCmPerSecond,
		{FIntPoint(10, 20), FIntPoint(60, 20)}));
	TestEqual(TEXT("ungrounded path start uses rigid basis and keeps Z"),
		Character->GetActorLocation(), FVector(-20.0, 10.0, 321.0));

	Character->Tick(0.1f);
	TestEqual(TEXT("ungrounded path advance uses rigid basis and keeps Z"),
		Character->GetActorLocation(), FVector(-20.0, 30.0, 321.0));
	TestTrue(TEXT("ungrounded path applies rigid-basis facing"),
		FMath::IsNearlyEqual(Character->GetActorRotation().Yaw, 90.0));

	World->DestroyWorld(false);
	return true;
}

#endif
