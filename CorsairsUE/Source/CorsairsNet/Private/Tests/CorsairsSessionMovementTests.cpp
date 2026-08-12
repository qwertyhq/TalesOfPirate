#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsSession.h"

#include "CorsairsNet/include/CommandMessages.h"
#include "CorsairsNet/include/Packet.h"
#include "Misc/AutomationTest.h"

namespace
{
using Corsairs::Net::RPacket;
using Corsairs::Net::WPacket;
namespace Msg = Corsairs::Net::Msg;

constexpr int64 LocalWorldId = 77;
constexpr int64 RemoteWorldId = 88;
constexpr int64 MovementSpeedAttribute = 44;
constexpr double LocalSpeed = 450.0;

const FIntPoint Spawn(1000, 2000);

const uint8 SpawnToFirstEndpointBytes[] = {
	0xE8, 0x03, 0x00, 0x00,
	0xD0, 0x07, 0x00, 0x00,
	0x14, 0x05, 0x00, 0x00,
	0x60, 0x09, 0x00, 0x00,
};

const uint8 ConfirmedToTargetBytes[] = {
	0xDC, 0x05, 0x00, 0x00,
	0x28, 0x0A, 0x00, 0x00,
	0xD0, 0x07, 0x00, 0x00,
	0xB8, 0x0B, 0x00, 0x00,
};

const uint8 Endpoint1100Bytes[] = {
	0x4C, 0x04, 0x00, 0x00,
	0xE8, 0x03, 0x00, 0x00,
};

const uint8 Endpoint1250Bytes[] = {
	0xE2, 0x04, 0x00, 0x00,
	0xE8, 0x03, 0x00, 0x00,
};

const uint8 Endpoint1500Bytes[] = {
	0xDC, 0x05, 0x00, 0x00,
	0xE8, 0x03, 0x00, 0x00,
};

const uint8 RemotePathBytes[] = {
	0xD0, 0x07, 0x00, 0x00,
	0xB8, 0x0B, 0x00, 0x00,
	0x34, 0x08, 0x00, 0x00,
	0x1C, 0x0C, 0x00, 0x00,
};

const uint8 SpawnToMovedRemoteBytes[] = {
	0xE8, 0x03, 0x00, 0x00,
	0xD0, 0x07, 0x00, 0x00,
	0x34, 0x08, 0x00, 0x00,
	0x1C, 0x0C, 0x00, 0x00,
};

template <typename MessageType>
void Deliver(UCorsairsSession* Session, const MessageType& Message)
{
	WPacket Wire = Msg::serialize(Message);
	RPacket Packet(Wire.Data(), Wire.GetPacketSize());
	Session->HandlePacketForTests(Packet);
}

std::vector<uint8_t> CopyBytes(const uint8* Data, const int32 Count)
{
	return std::vector<uint8_t>(Data, Data + Count);
}

Msg::McEnterMapMessage MakeEnterMap(
	const FIntPoint Position,
	const TOptional<int64> Speed = TOptional<int64>(
		static_cast<int64>(LocalSpeed)))
{
	Msg::McEnterMapMessage Message;
	Message.errCode = 0;
	Message.data.emplace();
	auto& Data = Message.data.value();
	Data.mapName = "garner";
	Data.baseInfo.worldId = LocalWorldId;
	Data.baseInfo.name = "Tester";
	Data.baseInfo.posX = Position.X;
	Data.baseInfo.posY = Position.Y;
	Data.baseInfo.handle = 7007;
	Data.baseInfo.look.typeId = 1;
	Msg::SkillEntry Skill;
	Skill.id = 26;
	Skill.level = 1;
	Data.skillBag.skills.push_back(Skill);
	Data.attr.attrs.push_back({1, 1234});
	if (Speed.IsSet())
	{
		Data.attr.attrs.push_back({MovementSpeedAttribute, Speed.GetValue()});
	}
	return Message;
}

Msg::McChaBeginSeeMessage MakeActorSeen(
	const int64 WorldId,
	const FIntPoint Position,
	const TOptional<int64> Speed)
{
	Msg::McChaBeginSeeMessage Message;
	Message.seeType = 1;
	Message.base.worldId = WorldId;
	Message.base.name = "Remote";
	Message.base.posX = Position.X;
	Message.base.posY = Position.Y;
	Message.base.handle = 9009 + WorldId;
	Message.base.look.typeId = 2;
	if (Speed.IsSet())
	{
		Message.attr.attrs.push_back(
			{MovementSpeedAttribute, Speed.GetValue()});
	}
	return Message;
}

Msg::McCharacterActionMessage MakeMove(
	const int64 WorldId,
	const int64 PacketId,
	const int64 MoveState,
	const uint8* Waypoints,
	const int32 WaypointCount)
{
	Msg::McCharacterActionMessage Message;
	Message.worldId = WorldId;
	Message.packetId = PacketId;
	Message.actionType = Msg::ActionType::MOVE;
	Msg::ActionMoveData Move;
	Move.moveState = MoveState;
	Move.stopState = 0;
	Move.waypoints = CopyBytes(Waypoints, WaypointCount);
	Message.data = MoveTemp(Move);
	return Message;
}

void DeliverSpeed(
	UCorsairsSession* Session,
	const int64 WorldId,
	const int64 Speed)
{
	Msg::McSynAttributeMessage Message;
	Message.worldId = WorldId;
	Message.attr.attrs.push_back({MovementSpeedAttribute, Speed});
	Deliver(Session, Message);
}

void TestResult(
	FAutomationTestBase* Test,
	const TCHAR* What,
	const ECorsairsActionRequestResult Actual,
	const ECorsairsActionRequestResult Expected)
{
	Test->TestEqual(
		What,
		static_cast<uint8>(Actual),
		static_cast<uint8>(Expected));
}

bool SameBytes(
	const char* Actual,
	const uint16 ActualCount,
	const uint8* Expected,
	const int32 ExpectedCount)
{
	return ActualCount == ExpectedCount &&
		FMemory::Memcmp(Actual, Expected, ExpectedCount) == 0;
}

struct FAuthorityTransition
{
	bool bLocked = false;
	int64 Epoch = 0;
};

UCorsairsSession* CreateSkillSession()
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, Spawn);
	Msg::McSynSkillBagMessage SkillBag;
	SkillBag.worldId = LocalWorldId;
	SkillBag.skillBag.synType = 0;
	Msg::SkillEntry Skill;
	Skill.id = 26;
	Skill.level = 1;
	SkillBag.skillBag.skills.push_back(Skill);
	Deliver(Session, SkillBag);
	Session->SetMovementSpeedForTests(static_cast<int64>(LocalSpeed));
	Deliver(
		Session,
		MakeActorSeen(RemoteWorldId, FIntPoint(2000, 3000), 325));
	return Session;
}

void ObserveAuthority(
	UCorsairsSession* Session,
	TArray<FAuthorityTransition>& Transitions)
{
	Session->SetMovementAuthorityObserverForTests(
		[&Transitions](const bool bLocked, const int64 Epoch)
		{
			Transitions.Add({bLocked, Epoch});
		});
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionSerializesAtomicBeginActionTest,
	"Corsairs.Movement.Session.SerializesAtomicBeginAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionSerializesAtomicBeginActionTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, Spawn);

	int32 SendCount = 0;
	ECorsairsActionRequestResult NestedResult =
		ECorsairsActionRequestResult::Invalid;
	bool bWireMatches = false;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			++SendCount;
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			const bool bHeaderMatches =
				Packet.GetCmd() == CMD_CM_BEGINACTION &&
				Packet.ReadInt64() == LocalWorldId &&
				Packet.ReadInt64() == 1 &&
				Packet.ReadInt64() == Msg::ActionType::MOVE;
			uint16 WaypointCount = 0;
			const char* Waypoints = Packet.ReadSequence(WaypointCount);
			bWireMatches = bHeaderMatches &&
				SameBytes(
					Waypoints,
					WaypointCount,
					SpawnToFirstEndpointBytes,
					UE_ARRAY_COUNT(SpawnToFirstEndpointBytes));

			NestedResult = Session->PickUpItem(900, 901);
			return true;
		});

	const ECorsairsActionRequestResult Result = Session->SendMovePath(
		{Spawn, FIntPoint(1300, 2400)});
	TestResult(this, TEXT("outer MOVE is sent"), Result,
		ECorsairsActionRequestResult::Sent);
	TestResult(this, TEXT("recursive BeginAction is busy"), NestedResult,
		ECorsairsActionRequestResult::Busy);
	TestEqual(TEXT("only one packet reaches transport"), SendCount, 1);
	TestTrue(TEXT("serialized MOVE bytes match the protocol"), bWireMatches);
	TestEqual(
		TEXT("diagnostic count advances after Sent"),
		Session->GetMovementBeginSendCountForDiagnostics(),
		1LL);
	TestTrue(TEXT("Sent MOVE is pending"),
		Session->HasPendingMoveForDiagnostics());

	UCorsairsSession* Failed = NewObject<UCorsairsSession>();
	Failed->SetInWorldForTests(LocalWorldId, Spawn);
	Failed->SetSendOverrideForTests([](WPacket&) { return false; });
	TestResult(
		this,
		TEXT("failed transport is explicit"),
		Failed->SendMovePath({Spawn, FIntPoint(1300, 2400)}),
		ECorsairsActionRequestResult::TransportFailed);
	TestEqual(
		TEXT("failed transport does not advance diagnostic count"),
		Failed->GetMovementBeginSendCountForDiagnostics(),
		0LL);
	TestFalse(TEXT("failed transport rolls pending state back"),
		Failed->HasPendingMoveForDiagnostics());

	const ECorsairsConnectionState ResetStates[] = {
		ECorsairsConnectionState::Failed,
		ECorsairsConnectionState::Disconnected,
	};
	for (const ECorsairsConnectionState ResetState : ResetStates)
	{
		UCorsairsSession* ResetDuringSend =
			NewObject<UCorsairsSession>();
		ResetDuringSend->SetInWorldForTests(LocalWorldId, Spawn);
		ResetDuringSend->SetSendOverrideForTests(
			[ResetDuringSend, ResetState](WPacket&)
			{
				ResetDuringSend->HandleConnectionStateForTests(
					ResetState,
					TEXT("synchronous transport failure"));
				return false;
			});
		TestResult(
			this,
			TEXT("synchronous reset still reports transport failure"),
			ResetDuringSend->SendMovePath(
				{Spawn, FIntPoint(1300, 2400)}),
			ECorsairsActionRequestResult::TransportFailed);
		TestEqual(TEXT("synchronous reset survives reducer rollback"),
			ResetDuringSend->GetConfirmedPosition(),
			FIntPoint::ZeroValue);
		TestFalse(TEXT("synchronous reset leaves no pending MOVE"),
			ResetDuringSend->HasPendingMoveForDiagnostics());
		TestFalse(TEXT("synchronous reset leaves no authority lock"),
			ResetDuringSend->IsMovementAuthorityLocked());
		TestEqual(TEXT("synchronous reset keeps diagnostics cleared"),
			ResetDuringSend->GetMovementBeginSendCountForDiagnostics(),
			0LL);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionStrictMoveWireTest,
	"Corsairs.Net.Session.StrictMoveWire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionStrictMoveWireTest::RunTest(const FString&)
{
	UCorsairsSession* Invalid = NewObject<UCorsairsSession>();
	Invalid->SetInWorldForTests(LocalWorldId, Spawn);
	int32 InvalidSendCount = 0;
	Invalid->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++InvalidSendCount;
			return true;
		});
	TestResult(this, TEXT("одна точка отклонена"),
		Invalid->SendMovePath({Spawn}),
		ECorsairsActionRequestResult::Invalid);
	TArray<FIntPoint> TooLong;
	TooLong.SetNum(33);
	TestResult(this, TEXT("33 точки отклонены"),
		Invalid->SendMovePath(TooLong),
		ECorsairsActionRequestResult::Invalid);
	TestEqual(TEXT("невалидные пути не отправляются"), InvalidSendCount, 0);

	UCorsairsSession* Minimum = NewObject<UCorsairsSession>();
	Minimum->SetInWorldForTests(LocalWorldId, Spawn);
	bool bMinimumWireMatches = false;
	Minimum->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			const bool bHeaderMatches =
				Packet.GetCmd() == CMD_CM_BEGINACTION &&
				Packet.ReadInt64() == LocalWorldId &&
				Packet.ReadInt64() == 1 &&
				Packet.ReadInt64() == Msg::ActionType::MOVE;
			uint16 Count = 0;
			const char* Bytes = Packet.ReadSequence(Count);
			bMinimumWireMatches = bHeaderMatches &&
				SameBytes(
					Bytes,
					Count,
					SpawnToFirstEndpointBytes,
					UE_ARRAY_COUNT(SpawnToFirstEndpointBytes));
			return true;
		});
	TestResult(this, TEXT("две точки отправлены"),
		Minimum->SendMovePath({Spawn, FIntPoint(1300, 2400)}),
		ECorsairsActionRequestResult::Sent);
	TestTrue(TEXT("две точки имеют точные wire bytes"),
		bMinimumWireMatches);

	UCorsairsSession* Maximum = NewObject<UCorsairsSession>();
	Maximum->SetInWorldForTests(LocalWorldId, Spawn);
	TArray<FIntPoint> MaximumPath;
	for (int32 Index = 0; Index < 32; ++Index)
	{
		MaximumPath.Emplace(1000 + Index * 50, 2000 - Index * 25);
	}
	bool bMaximumWireMatches = false;
	Maximum->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			const bool bHeaderMatches =
				Packet.GetCmd() == CMD_CM_BEGINACTION &&
				Packet.ReadInt64() == LocalWorldId &&
				Packet.ReadInt64() == 1 &&
				Packet.ReadInt64() == Msg::ActionType::MOVE;
			uint16 Count = 0;
			const char* Bytes = Packet.ReadSequence(Count);
			bMaximumWireMatches = bHeaderMatches &&
				Count == 256 &&
				FMemory::Memcmp(
					Bytes,
					MaximumPath.GetData(),
					Count) == 0;
			return true;
		});
	TestResult(this, TEXT("32 точки отправлены"),
		Maximum->SendMovePath(MaximumPath),
		ECorsairsActionRequestResult::Sent);
	TestTrue(TEXT("32 точки занимают точные 256 wire bytes"),
		bMaximumWireMatches);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionEndActiveActionTest,
	"Corsairs.Net.Session.EndActiveAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionEndActiveActionTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, Spawn);
	int32 MoveSendCount = 0;
	int32 CancelSendCount = 0;
	bool bCancelIsCommandOnly = false;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			if (Packet.GetCmd() == CMD_CM_ENDACTION)
			{
				++CancelSendCount;
				bCancelIsCommandOnly = Packet.PayloadLength() == 0;
			}
			else if (Packet.GetCmd() == CMD_CM_BEGINACTION)
			{
				++MoveSendCount;
			}
			return true;
		});
	TestResult(this, TEXT("MOVE резервирует действие"),
		Session->SendMovePath({Spawn, FIntPoint(1300, 2400)}),
		ECorsairsActionRequestResult::Sent);
	TestTrue(TEXT("Session публикует наличие active action"),
		Session->HasActiveAction());
	TestFalse(TEXT("до EndActiveAction cancel не pending"),
		Session->IsCancelPending());
	TestTrue(TEXT("первая отмена отправлена"), Session->EndActiveAction());
	TestTrue(TEXT("после EndActiveAction cancel pending"),
		Session->IsCancelPending());
	TestFalse(TEXT("повторная отмена до terminal занята"),
		Session->EndActiveAction());
	TestResult(this, TEXT("новый BEGIN до terminal занят"),
		Session->SendMovePath({Spawn, FIntPoint(1400, 2400)}),
		ECorsairsActionRequestResult::Busy);
	TestEqual(TEXT("ENDACTION отправлен один раз"), CancelSendCount, 1);
	TestEqual(TEXT("до terminal второй MOVE не отправлен"), MoveSendCount, 1);
	TestTrue(TEXT("ENDACTION не содержит payload"), bCancelIsCommandOnly);

	Deliver(
		Session,
		MakeMove(
			LocalWorldId,
			1,
			1,
			Endpoint1500Bytes,
			UE_ARRAY_COUNT(Endpoint1500Bytes)));
	TestResult(this, TEXT("после terminal новый BEGIN разрешён"),
		Session->SendMovePath(
			{FIntPoint(1500, 1000), FIntPoint(1700, 1000)}),
		ECorsairsActionRequestResult::Sent);
	TestEqual(TEXT("после terminal отправлен второй MOVE"), MoveSendCount, 2);
	TestTrue(TEXT("после нового BEGIN active seam снова поднят"),
		Session->HasActiveAction());
	TestFalse(TEXT("terminal очистил прежнюю cancel reservation"),
		Session->IsCancelPending());

	UCorsairsSession* Retry = NewObject<UCorsairsSession>();
	Retry->SetInWorldForTests(LocalWorldId, Spawn);
	bool bFailCancel = true;
	int32 RetryCancelCount = 0;
	Retry->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			if (Wire.GetCmd() == CMD_CM_ENDACTION)
			{
				++RetryCancelCount;
				return !bFailCancel;
			}
			return true;
		});
	Retry->SendMovePath({Spawn, FIntPoint(1300, 2400)});
	TestFalse(TEXT("transport failure отмены возвращает false"),
		Retry->EndActiveAction());
	bFailCancel = false;
	TestTrue(TEXT("rollback разрешает retry отмены"),
		Retry->EndActiveAction());
	TestEqual(TEXT("retry делает ровно вторую попытку"), RetryCancelCount, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionReentrantCancelBeforeSkillTransportTest,
	"Corsairs.Net.Session.ReentrantCancelBeforeSkillTransport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionReentrantCancelBeforeSkillTransportTest::RunTest(
	const FString&)
{
	// Мутация: считать ту же пару пакета и фазы достаточной после уведомления
	// о блокировке. Тогда слушатель успевает отправить ENDACTION, а следом
	// уходит BEGIN навыка.
	UCorsairsSession* Session = CreateSkillSession();
	int32 SkillBeginCount = 0;
	int32 CancelCount = 0;
	int32 ItemBeginCount = 0;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			if (Wire.GetCmd() == CMD_CM_ENDACTION)
			{
				++CancelCount;
				return true;
			}

			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			if (Packet.GetCmd() == CMD_CM_BEGINACTION)
			{
				Packet.ReadInt64();
				Packet.ReadInt64();
				const int64 ActionType = Packet.ReadInt64();
				if (ActionType == Msg::ActionType::SKILL)
				{
					++SkillBeginCount;
				}
				else if (ActionType == Msg::ActionType::ITEM_PICK)
				{
					++ItemBeginCount;
				}
			}
			return true;
		});

	bool bCancelAccepted = false;
	Session->SetMovementAuthorityObserverForTests(
		[&](const bool bLocked, const int64)
		{
			if (bLocked)
			{
				bCancelAccepted = Session->EndActiveAction();
			}
		});

	TestResult(this, TEXT("локально отменённый outer Skill не считается Sent"),
		Session->UseSkillOn(26, RemoteWorldId),
		ECorsairsActionRequestResult::TransportFailed);
	TestTrue(TEXT("reentrant cancel принят"), bCancelAccepted);
	TestEqual(TEXT("unsent Skill не требует ENDACTION"), CancelCount, 0);
	TestEqual(TEXT("после локальной отмены Skill BEGIN не отправлен"),
		SkillBeginCount, 0);
	TestFalse(TEXT("локальная отмена освобождает active action"),
		Session->HasActiveAction());
	TestFalse(TEXT("локальная отмена не ждёт server terminal"),
		Session->IsCancelPending());
	TestResult(this, TEXT("после локальной отмены новый BEGIN разрешён"),
		Session->PickUpItem(901, 902),
		ECorsairsActionRequestResult::Sent);
	TestEqual(TEXT("следующий BEGIN отправлен ровно раз"), ItemBeginCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionReentrantCommittedMovementStateTest,
	"Corsairs.Net.Session.ReentrantCommittedMovementState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionReentrantCommittedMovementStateTest::RunTest(
	const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, Spawn);
	ECorsairsActionRequestResult NestedResult =
		ECorsairsActionRequestResult::Invalid;
	int32 ItemSendCount = 0;
	Session->SetEventObserversForTests(
		[&](const FCorsairsMovementEvent& Event)
		{
			if (Event.bLocal &&
				Event.Type == ECorsairsMovementEventType::Terminal)
			{
				NestedResult = Session->PickUpItem(901, 902);
			}
		},
		TFunction<void(const FString&)>(),
		TFunction<void(ECorsairsLoginStage)>());
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			if (Packet.GetCmd() != CMD_CM_BEGINACTION)
			{
				return true;
			}
			Packet.ReadInt64();
			const int64 PacketId = Packet.ReadInt64();
			const int64 ActionType = Packet.ReadInt64();
			if (ActionType == Msg::ActionType::MOVE)
			{
				Deliver(
					Session,
					MakeMove(
						LocalWorldId,
						PacketId,
						1,
						Endpoint1500Bytes,
						UE_ARRAY_COUNT(Endpoint1500Bytes)));
				return false;
			}
			if (ActionType == Msg::ActionType::ITEM_PICK)
			{
				++ItemSendCount;
			}
			return true;
		});

	TestResult(this, TEXT("outer transport failure остаётся явным"),
		Session->SendMovePath({Spawn, FIntPoint(1300, 2400)}),
		ECorsairsActionRequestResult::TransportFailed);
	TestResult(this, TEXT("observer видит уже завершённый MOVE"),
		NestedResult, ECorsairsActionRequestResult::Sent);
	TestEqual(TEXT("вложенный action отправлен ровно раз"), ItemSendCount, 1);
	TestResult(this, TEXT("вложенный committed action не затёрт rollback"),
		Session->PickUpItem(903, 904),
		ECorsairsActionRequestResult::Busy);
	TestEqual(TEXT("terminal confirmation пережила outer rollback"),
		Session->GetConfirmedPosition(), FIntPoint(1500, 1000));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionEnterMapAndSkillOriginTest,
	"Corsairs.Movement.Session.EnterMapAndSkillOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionEnterMapAndSkillOriginTest::RunTest(const FString&)
{
	UCorsairsSession* Entering = NewObject<UCorsairsSession>();
	FIntPoint ConfirmedAtInWorld = FIntPoint::ZeroValue;
	double SpeedAtInWorld = 0.0;
	Entering->SetEventObserversForTests(
		TFunction<void(const FCorsairsMovementEvent&)>(),
		TFunction<void(const FString&)>(),
		[&](const ECorsairsLoginStage Stage)
		{
			if (Stage == ECorsairsLoginStage::InWorld)
			{
				ConfirmedAtInWorld = Entering->GetConfirmedPosition();
				SpeedAtInWorld =
					Entering->GetMovementSpeedCmPerSecond();
			}
		});
	Deliver(Entering, MakeEnterMap(Spawn));
	TestEqual(TEXT("ENTERMAP enters the world"),
		static_cast<uint8>(Entering->GetStage()),
		static_cast<uint8>(ECorsairsLoginStage::InWorld));
	TestEqual(TEXT("confirmed position exists before InWorld broadcast"),
		ConfirmedAtInWorld, Spawn);
	TestEqual(TEXT("movement speed exists before InWorld broadcast"),
		SpeedAtInWorld, LocalSpeed);
	TestEqual(TEXT("ENTERMAP resets MOVE diagnostics"),
		Entering->GetMovementBeginSendCountForDiagnostics(), 0LL);

	Entering->SetSendOverrideForTests([](WPacket&) { return true; });
	TestResult(
		this,
		TEXT("manual MOVE before skill is sent"),
		Entering->SendMovePath({Spawn, FIntPoint(1300, 2400)}),
		ECorsairsActionRequestResult::Sent);
	const uint8 ConfirmedEndpointBytes[] = {
		0xDC, 0x05, 0x00, 0x00,
		0x28, 0x0A, 0x00, 0x00,
	};
	Deliver(
		Entering,
		MakeMove(
			LocalWorldId,
			1,
			1,
			ConfirmedEndpointBytes,
			UE_ARRAY_COUNT(ConfirmedEndpointBytes)));
	TestEqual(TEXT("server terminal changes reducer confirmation"),
		Entering->GetConfirmedPosition(), FIntPoint(1500, 2600));
	TestEqual(TEXT("legacy spawn stays unchanged"),
		Entering->GetSpawnPosition(), Spawn);

	Deliver(
		Entering,
		MakeActorSeen(RemoteWorldId, FIntPoint(2000, 3000), 325));
	bool bSkillWireMatches = false;
	Entering->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			const bool bHeaderMatches =
				Packet.GetCmd() == CMD_CM_BEGINACTION &&
				Packet.ReadInt64() == LocalWorldId &&
				Packet.ReadInt64() == 2 &&
				Packet.ReadInt64() == Msg::ActionType::SKILL &&
				Packet.ReadInt64() == 2 &&
				Packet.ReadInt64() == 2;
			uint16 WaypointCount = 0;
			const char* Waypoints = Packet.ReadSequence(WaypointCount);
			bSkillWireMatches = bHeaderMatches &&
				SameBytes(
					Waypoints,
					WaypointCount,
					ConfirmedToTargetBytes,
					UE_ARRAY_COUNT(ConfirmedToTargetBytes)) &&
				Packet.ReadInt64() == 26 &&
				Packet.ReadInt64() == RemoteWorldId &&
				Packet.ReadInt64() == 9009 + RemoteWorldId;
			return true;
		});
	TestResult(
		this,
		TEXT("skill is sent"),
		Entering->UseSkillOn(26, RemoteWorldId),
		ECorsairsActionRequestResult::Sent);
	TestTrue(
		TEXT("skill path starts at reducer confirmation, not spawn"),
		bSkillWireMatches);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionSkillUsesMovedTargetTest,
	"Corsairs.Movement.Session.SkillUsesMovedTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionSkillUsesMovedTargetTest::RunTest(const FString&)
{
	// Мутация: уведомить мир о движении до обновления адресуемого состояния.
	// Вложенная атака тогда всё ещё пойдёт к исходной точке CHABEGINSEE.
	UCorsairsSession* Session = CreateSkillSession();
	bool bSkillUsesMovedPosition = false;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			const bool bHeaderMatches =
				Packet.GetCmd() == CMD_CM_BEGINACTION &&
				Packet.ReadInt64() == LocalWorldId &&
				Packet.ReadInt64() == 1 &&
				Packet.ReadInt64() == Msg::ActionType::SKILL &&
				Packet.ReadInt64() == 2 &&
				Packet.ReadInt64() == 1;
			uint16 WaypointCount = 0;
			const char* Waypoints = Packet.ReadSequence(WaypointCount);
			bSkillUsesMovedPosition = bHeaderMatches &&
				SameBytes(
					Waypoints,
					WaypointCount,
					SpawnToMovedRemoteBytes,
					UE_ARRAY_COUNT(SpawnToMovedRemoteBytes)) &&
				Packet.ReadInt64() == 26 &&
				Packet.ReadInt64() == RemoteWorldId &&
				Packet.ReadInt64() == 9009 + RemoteWorldId;
			return true;
		});
	bool bObserverCalled = false;
	bool bPositionUpdatedBeforeObserver = false;
	ECorsairsActionRequestResult NestedSkillResult =
		ECorsairsActionRequestResult::Invalid;
	Session->SetEventObserversForTests(
		[&](const FCorsairsMovementEvent& Event)
		{
			if (Event.WorldId != RemoteWorldId)
			{
				return;
			}
			bObserverCalled = true;
			const FCorsairsWorldActor* MovedTarget =
				Session->GetVisibleActors().FindByPredicate(
					[](const FCorsairsWorldActor& Actor)
					{
						return Actor.WorldId == RemoteWorldId;
					});
			bPositionUpdatedBeforeObserver = MovedTarget != nullptr &&
				MovedTarget->Position == FIntPoint(2100, 3100);
			NestedSkillResult = Session->UseSkillOn(26, RemoteWorldId);
		},
		TFunction<void(const FString&)>(),
		TFunction<void(ECorsairsLoginStage)>());

	Deliver(
		Session,
		MakeMove(
			RemoteWorldId,
			90,
			0,
			RemotePathBytes,
			UE_ARRAY_COUNT(RemotePathBytes)));

	TestTrue(TEXT("remote movement observer is called"), bObserverCalled);
	TestTrue(
		TEXT("target position is current inside movement observer"),
		bPositionUpdatedBeforeObserver);
	TestResult(
		this,
		TEXT("nested skill against moved target is sent"),
		NestedSkillResult,
		ECorsairsActionRequestResult::Sent);
	TestTrue(
		TEXT("skill approaches the latest remote endpoint"),
		bSkillUsesMovedPosition);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionAuthorityTransitionsTest,
	"Corsairs.Movement.Session.AuthorityTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionAuthorityTransitionsTest::RunTest(const FString&)
{
	// Mutation: publish only from pawn axis polling, after transport, or only
	// on MOVE notifications. Any of those misses a synchronous skill lock or
	// its rollback when no axis sample occurs between the two transitions.
	UCorsairsSession* Failure = CreateSkillSession();
	TArray<FAuthorityTransition> FailureTransitions;
	ObserveAuthority(Failure, FailureTransitions);
	bool bRisingObservedInsideTransport = false;
	Failure->SetSendOverrideForTests(
		[&](WPacket&)
		{
			bRisingObservedInsideTransport =
				FailureTransitions.Num() == 1 &&
				FailureTransitions[0].bLocked &&
				FailureTransitions[0].Epoch == 1;
			return true;
		});
	TestResult(
		this,
		TEXT("successful skill reserves authority"),
		Failure->UseSkillOn(26, RemoteWorldId),
		ECorsairsActionRequestResult::Sent);
	TestTrue(TEXT("rising transition precedes the transport callback"),
		bRisingObservedInsideTransport);
	Deliver(
		Failure,
		Msg::McFailedActionMessage{
			LocalWorldId,
			Msg::ActionType::SKILL,
			0,
		});
	TestEqual(TEXT("FAILEDACTION emits rising and falling"),
		FailureTransitions.Num(), 2);
	if (FailureTransitions.Num() == 2)
	{
		TestTrue(TEXT("skill reservation rises before transport returns"),
			FailureTransitions[0].bLocked);
		TestFalse(TEXT("FAILEDACTION releases skill authority"),
			FailureTransitions[1].bLocked);
		TestEqual(TEXT("authority epochs are monotonic"),
			FailureTransitions[0].Epoch, 1LL);
		TestEqual(TEXT("falling transition advances the epoch"),
			FailureTransitions[1].Epoch, 2LL);
	}

	UCorsairsSession* Terminal = CreateSkillSession();
	TArray<FAuthorityTransition> TerminalTransitions;
	ObserveAuthority(Terminal, TerminalTransitions);
	Terminal->SetSendOverrideForTests([](WPacket&) { return true; });
	Terminal->UseSkillOn(26, RemoteWorldId);
	Deliver(
		Terminal,
		MakeMove(
			LocalWorldId,
			1,
			2,
			Endpoint1100Bytes,
			UE_ARRAY_COUNT(Endpoint1100Bytes)));
	TestEqual(TEXT("skill MOVE terminal emits rising and falling"),
		TerminalTransitions.Num(), 2);
	if (TerminalTransitions.Num() == 2)
	{
		TestTrue(TEXT("terminal case starts locked"),
			TerminalTransitions[0].bLocked);
		TestFalse(TEXT("terminal case ends unlocked"),
			TerminalTransitions[1].bLocked);
	}

	UCorsairsSession* Rollback = CreateSkillSession();
	TArray<FAuthorityTransition> RollbackTransitions;
	ObserveAuthority(Rollback, RollbackTransitions);
	bool bRollbackRisingInsideTransport = false;
	Rollback->SetSendOverrideForTests(
		[&](WPacket&)
		{
			bRollbackRisingInsideTransport =
				RollbackTransitions.Num() == 1 &&
				RollbackTransitions[0].bLocked &&
				RollbackTransitions[0].Epoch == 1;
			return false;
		});
	TestResult(
		this,
		TEXT("skill transport failure is explicit"),
		Rollback->UseSkillOn(26, RemoteWorldId),
		ECorsairsActionRequestResult::TransportFailed);
	TestTrue(TEXT("rollback rising precedes the failed transport callback"),
		bRollbackRisingInsideTransport);
	TestEqual(TEXT("transport rollback emits ordered pair"),
		RollbackTransitions.Num(), 2);
	if (RollbackTransitions.Num() == 2)
	{
		TestTrue(TEXT("rollback publishes reservation before send"),
			RollbackTransitions[0].bLocked);
		TestFalse(TEXT("rollback publishes release after failed send"),
			RollbackTransitions[1].bLocked);
	}

	UCorsairsSession* Disconnect = CreateSkillSession();
	TArray<FAuthorityTransition> DisconnectTransitions;
	ObserveAuthority(Disconnect, DisconnectTransitions);
	Disconnect->SetSendOverrideForTests([](WPacket&) { return true; });
	Disconnect->UseSkillOn(26, RemoteWorldId);
	Disconnect->HandleConnectionStateForTests(
		ECorsairsConnectionState::Disconnected,
		TEXT("authority transition test"));
	TestEqual(TEXT("disconnect emits rising and falling"),
		DisconnectTransitions.Num(), 2);
	if (DisconnectTransitions.Num() == 2)
	{
		TestFalse(TEXT("disconnect releases authority"),
			DisconnectTransitions[1].bLocked);
	}

	UCorsairsSession* Logout = CreateSkillSession();
	TArray<FAuthorityTransition> LogoutTransitions;
	ObserveAuthority(Logout, LogoutTransitions);
	Logout->SetSendOverrideForTests([](WPacket&) { return true; });
	Logout->UseSkillOn(26, RemoteWorldId);
	Logout->Logout();
	TestEqual(TEXT("logout reset emits rising and falling"),
		LogoutTransitions.Num(), 2);
	if (LogoutTransitions.Num() == 2)
	{
		TestFalse(TEXT("logout reset releases authority"),
			LogoutTransitions[1].bLocked);
	}

	UCorsairsSession* Manual = NewObject<UCorsairsSession>();
	Manual->SetInWorldForTests(LocalWorldId, Spawn);
	TArray<FAuthorityTransition> ManualTransitions;
	ObserveAuthority(Manual, ManualTransitions);
	Manual->SetSendOverrideForTests([](WPacket&) { return true; });
	TestResult(
		this,
		TEXT("manual MOVE is sent"),
		Manual->SendMovePath({Spawn, FIntPoint(1300, 2400)}),
		ECorsairsActionRequestResult::Sent);
	TestEqual(TEXT("manual PendingMove emits no authority transition"),
		ManualTransitions.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionAuthorityReentrantSupersessionTest,
	"Corsairs.Movement.Session.AuthorityReentrantSupersession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionAuthorityReentrantSupersessionTest::RunTest(
	const FString&)
{
	// Mutation: guard only reducer generation after the rising broadcast.
	// A synchronous terminal can replace the reserved Skill without changing
	// generation, so the outer callback sends a stale packet or a failed-send
	// rollback restores its old reducer snapshot over the new reservation.
	UCorsairsSession* Session = CreateSkillSession();
	TArray<FAuthorityTransition> Transitions;
	int32 SkillTransportCount = 0;
	int32 ItemTransportCount = 0;
	ECorsairsActionRequestResult NestedItemResult =
		ECorsairsActionRequestResult::Invalid;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			Packet.ReadInt64();
			Packet.ReadInt64();
			const int64 ActionType = Packet.ReadInt64();
			if (ActionType == Msg::ActionType::SKILL)
			{
				++SkillTransportCount;
			}
			else if (ActionType == Msg::ActionType::ITEM_PICK)
			{
				++ItemTransportCount;
			}
			return true;
		});
	Session->SetMovementAuthorityObserverForTests(
		[&](const bool bLocked, const int64 Epoch)
		{
			Transitions.Add({bLocked, Epoch});
			if (!bLocked)
			{
				return;
			}

			Deliver(
				Session,
				MakeMove(
					LocalWorldId,
					1,
					2,
					Endpoint1250Bytes,
					UE_ARRAY_COUNT(Endpoint1250Bytes)));
			NestedItemResult = Session->PickUpItem(901, 902);
		});

	const ECorsairsActionRequestResult OuterResult =
		Session->UseSkillOn(26, RemoteWorldId);
	TestResult(this, TEXT("superseded outer Skill is non-Sent"),
		OuterResult, ECorsairsActionRequestResult::TransportFailed);
	TestEqual(TEXT("superseded Skill never reaches transport"),
		SkillTransportCount, 0);
	TestResult(this, TEXT("reentrant item reservation is sent"),
		NestedItemResult, ECorsairsActionRequestResult::Sent);
	TestEqual(TEXT("only the reentrant item reaches transport"),
		ItemTransportCount, 1);
	TestEqual(TEXT("terminal confirmation survives outer Begin"),
		Session->GetConfirmedPosition(), FIntPoint(1250, 1000));
	TestFalse(TEXT("terminal releases movement authority"),
		Session->IsMovementAuthorityLocked());
	TestEqual(TEXT("reentrant transitions remain ordered"),
		Transitions.Num(), 2);
	if (Transitions.Num() == 2)
	{
		TestTrue(TEXT("first reentrant transition locks"),
			Transitions[0].bLocked);
		TestEqual(TEXT("first reentrant epoch is one"),
			Transitions[0].Epoch, 1LL);
		TestFalse(TEXT("second reentrant transition unlocks"),
			Transitions[1].bLocked);
		TestEqual(TEXT("second reentrant epoch is two"),
			Transitions[1].Epoch, 2LL);
	}
	TestResult(this, TEXT("reentrant item remains the active reservation"),
		Session->PickUpItem(903, 904),
		ECorsairsActionRequestResult::Busy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionPredictedQueueTest,
	"Corsairs.Movement.Session.PredictedQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionPredictedQueueTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, FIntPoint(1000, 1000));
	TArray<TArray<FIntPoint>> SentPaths;
	Session->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			Packet.ReadInt64();
			Packet.ReadInt64();
			const int64 ActionType = Packet.ReadInt64();
			if (ActionType == Msg::ActionType::MOVE)
			{
				uint16 Count = 0;
				const char* Bytes = Packet.ReadSequence(Count);
				TArray<FIntPoint> Path;
				for (int32 Offset = 0; Offset < Count; Offset += 8)
				{
					int32 X = 0;
					int32 Y = 0;
					FMemory::Memcpy(&X, Bytes + Offset, sizeof(X));
					FMemory::Memcpy(&Y, Bytes + Offset + 4, sizeof(Y));
					Path.Emplace(X, Y);
				}
				SentPaths.Add(MoveTemp(Path));
			}
			return true;
		});

	TestResult(this, TEXT("stationary report is invalid"),
		Session->SubmitPredictedPosition(FIntPoint(1000, 1000)),
		ECorsairsActionRequestResult::Invalid);
	TestResult(this, TEXT("99 cm is below the literal gate"),
		Session->SubmitPredictedPosition(FIntPoint(1099, 1000)),
		ECorsairsActionRequestResult::Invalid);
	TestResult(this, TEXT("100 cm reaches the literal gate"),
		Session->SubmitPredictedPosition(FIntPoint(1100, 1000)),
		ECorsairsActionRequestResult::Sent);
	TestResult(this, TEXT("near pending endpoint remains busy"),
		Session->SubmitPredictedPosition(FIntPoint(1150, 1000)),
		ECorsairsActionRequestResult::Busy);
	TestResult(this, TEXT("far pending endpoint is queued"),
		Session->SubmitPredictedPosition(FIntPoint(1300, 1000)),
		ECorsairsActionRequestResult::Busy);
	TestResult(this, TEXT("near repeated endpoint does not replace queue"),
		Session->SubmitPredictedPosition(FIntPoint(1350, 1000)),
		ECorsairsActionRequestResult::Busy);
	TestResult(this, TEXT("latest far endpoint replaces the one-slot queue"),
		Session->SubmitPredictedPosition(FIntPoint(1500, 1000)),
		ECorsairsActionRequestResult::Busy);
	TestEqual(TEXT("pending reports do not send"), SentPaths.Num(), 1);
	ECorsairsActionRequestResult ReentrantResult =
		ECorsairsActionRequestResult::Invalid;
	Session->SetEventObserversForTests(
		[&](const FCorsairsMovementEvent&)
		{
			ReentrantResult = Session->PickUpItem(901, 902);
		},
		TFunction<void(const FString&)>(),
		TFunction<void(ECorsairsLoginStage)>());

	Deliver(
		Session,
		MakeMove(
			LocalWorldId,
			1,
			1,
			Endpoint1100Bytes,
			UE_ARRAY_COUNT(Endpoint1100Bytes)));
	TestResult(this, TEXT("queued MOVE is reserved before ARRIVE observer"),
		ReentrantResult, ECorsairsActionRequestResult::Busy);
	TestEqual(TEXT("ARRIVE sends one queued path"), SentPaths.Num(), 2);
	if (SentPaths.Num() == 2)
	{
		TestEqual(TEXT("queued path has two waypoints"),
			SentPaths[1].Num(), 2);
		if (SentPaths[1].Num() == 2)
		{
			TestEqual(TEXT("queued path starts at new confirmation"),
				SentPaths[1][0], FIntPoint(1100, 1000));
			TestEqual(TEXT("queued path ends at latest far report"),
				SentPaths[1][1], FIntPoint(1500, 1000));
		}
	}
	TestEqual(TEXT("queued MOVE increments diagnostic once"),
		Session->GetMovementBeginSendCountForDiagnostics(), 2LL);

	UCorsairsSession* NearQueue = NewObject<UCorsairsSession>();
	NearQueue->SetInWorldForTests(LocalWorldId, FIntPoint(1000, 1000));
	int32 NearQueueSendCount = 0;
	FIntPoint NearQueueSentEndpoint = FIntPoint::ZeroValue;
	NearQueue->SetSendOverrideForTests(
		[&](WPacket& Wire)
		{
			++NearQueueSendCount;
			RPacket Packet(Wire.Data(), Wire.GetPacketSize());
			Packet.ReadInt64();
			Packet.ReadInt64();
			if (Packet.ReadInt64() == Msg::ActionType::MOVE)
			{
				uint16 Count = 0;
				const char* Bytes = Packet.ReadSequence(Count);
				if (Count >= 8)
				{
					FMemory::Memcpy(
						&NearQueueSentEndpoint.X,
						Bytes + Count - 8,
						sizeof(NearQueueSentEndpoint.X));
					FMemory::Memcpy(
						&NearQueueSentEndpoint.Y,
						Bytes + Count - 4,
						sizeof(NearQueueSentEndpoint.Y));
				}
			}
			return true;
		});
	NearQueue->SubmitPredictedPosition(FIntPoint(1100, 1000));
	NearQueue->SubmitPredictedPosition(FIntPoint(1300, 1000));
	NearQueue->SubmitPredictedPosition(FIntPoint(1350, 1000));
	Deliver(
		NearQueue,
		MakeMove(
			LocalWorldId,
			1,
			1,
			Endpoint1100Bytes,
			UE_ARRAY_COUNT(Endpoint1100Bytes)));
	TestEqual(TEXT("near repeated endpoint still resends one queued MOVE"),
		NearQueueSendCount, 2);
	TestEqual(TEXT("near repeated endpoint does not mutate queue"),
		NearQueueSentEndpoint, FIntPoint(1300, 1000));

	UCorsairsSession* StationaryQueue = NewObject<UCorsairsSession>();
	StationaryQueue->SetInWorldForTests(
		LocalWorldId, FIntPoint(1000, 1000));
	int32 StationarySendCount = 0;
	StationaryQueue->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++StationarySendCount;
			return true;
		});
	StationaryQueue->SubmitPredictedPosition(FIntPoint(1100, 1000));
	StationaryQueue->SubmitPredictedPosition(FIntPoint(1300, 1000));
	Deliver(
		StationaryQueue,
		MakeMove(
			LocalWorldId,
			1,
			1,
			Endpoint1250Bytes,
			UE_ARRAY_COUNT(Endpoint1250Bytes)));
	TestEqual(TEXT("queued endpoint within 100 cm does not resend"),
		StationarySendCount, 1);
	TestFalse(TEXT("stationary queue still clears pending state"),
		StationaryQueue->HasPendingMoveForDiagnostics());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionNegativeTerminalsTest,
	"Corsairs.Movement.Session.NegativeTerminals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionNegativeTerminalsTest::RunTest(const FString&)
{
	const int64 MoveStates[] = {2, 4, 8, 16, 32};
	for (const int64 MoveState : MoveStates)
	{
		UCorsairsSession* Session = NewObject<UCorsairsSession>();
		Session->SetInWorldForTests(LocalWorldId, FIntPoint(1000, 1000));
		int32 SendCount = 0;
		Session->SetSendOverrideForTests(
			[&](WPacket&)
			{
				++SendCount;
				return true;
			});
		Session->SubmitPredictedPosition(FIntPoint(1100, 1000));
		Session->SubmitPredictedPosition(FIntPoint(1500, 1000));
		Deliver(
			Session,
			MakeMove(
				LocalWorldId,
				1,
				MoveState,
				Endpoint1100Bytes,
				UE_ARRAY_COUNT(Endpoint1100Bytes)));
		TestEqual(
			FString::Printf(TEXT("terminal %lld never sends queued MOVE"),
				MoveState),
			SendCount,
			1);
		TestFalse(
			FString::Printf(TEXT("terminal %lld clears pending MOVE"),
				MoveState),
			Session->HasPendingMoveForDiagnostics());
	}

	const int64 FailureReasons[] = {0, 1, 2};
	for (const int64 Reason : FailureReasons)
	{
		UCorsairsSession* Session = NewObject<UCorsairsSession>();
		Session->SetInWorldForTests(LocalWorldId, FIntPoint(1000, 1000));
		int32 SendCount = 0;
		Session->SetSendOverrideForTests(
			[&](WPacket&)
			{
				++SendCount;
				return true;
			});
		Session->SubmitPredictedPosition(FIntPoint(1100, 1000));
		Session->SubmitPredictedPosition(FIntPoint(1500, 1000));
		Deliver(
			Session,
			Msg::McFailedActionMessage{
				LocalWorldId,
				Msg::ActionType::MOVE,
				Reason,
			});
		TestEqual(
			FString::Printf(TEXT("failure %lld command 520 does not resend"),
				Reason),
			SendCount,
			1);
		TestFalse(
			FString::Printf(TEXT("failure %lld clears pending MOVE"),
				Reason),
			Session->HasPendingMoveForDiagnostics());
		TestEqual(TEXT("failure keeps confirmed position"),
			Session->GetConfirmedPosition(), FIntPoint(1000, 1000));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionItemNotificationsTest,
	"Corsairs.Movement.Session.ItemNotifications",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionItemNotificationsTest::RunTest(const FString&)
{
	UCorsairsSession* LookSession = NewObject<UCorsairsSession>();
	LookSession->SetInWorldForTests(LocalWorldId, Spawn);
	int32 LookSends = 0;
	LookSession->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++LookSends;
			return true;
		});
	TestResult(this, TEXT("equip starts item reservation"),
		LookSession->EquipItem(3, 9),
		ECorsairsActionRequestResult::Sent);
	Msg::McCharacterActionMessage LookMessage;
	LookMessage.worldId = LocalWorldId;
	LookMessage.packetId = 1;
	LookMessage.actionType = Msg::ActionType::LOOK;
	Msg::ChaLookInfo Look;
	Look.typeId = 42;
	Look.hairId = 2124;
	LookMessage.data = Look;
	Deliver(LookSession, LookMessage);
	TestEqual(TEXT("LOOK still mutates the local actor"),
		LookSession->GetLocalActor().TypeId, 42);
	TestResult(this, TEXT("LOOK closes matching item reservation"),
		LookSession->PickUpItem(901, 902),
		ECorsairsActionRequestResult::Sent);
	TestEqual(TEXT("second action sends after LOOK terminal"),
		LookSends, 2);

	UCorsairsSession* KitbagSession = NewObject<UCorsairsSession>();
	KitbagSession->SetInWorldForTests(LocalWorldId, Spawn);
	int32 KitbagSends = 0;
	KitbagSession->SetSendOverrideForTests(
		[&](WPacket&)
		{
			++KitbagSends;
			return true;
		});
	KitbagSession->PickUpItem(901, 902);
	TestResult(this, TEXT("manual input is busy behind non-MOVE"),
		KitbagSession->SubmitPredictedPosition(FIntPoint(1400, 2000)),
		ECorsairsActionRequestResult::Busy);
	Msg::McCharacterActionMessage KitbagMessage;
	KitbagMessage.worldId = LocalWorldId;
	KitbagMessage.packetId = 1;
	KitbagMessage.actionType = Msg::ActionType::KITBAG;
	Msg::ChaKitbagInfo Kitbag;
	Kitbag.synType = 1;
	Msg::KitbagItem Item;
	Item.gridId = 5;
	Item.itemId = 888;
	Kitbag.items.push_back(Item);
	KitbagMessage.data = Kitbag;
	Deliver(KitbagSession, KitbagMessage);
	TestEqual(TEXT("KITBAG still mutates inventory"),
		KitbagSession->GetKitbag().FindRef(5), 888LL);
	TestEqual(TEXT("busy manual input was not queued or sent"),
		KitbagSends, 1);
	TestResult(this, TEXT("KITBAG closes matching item reservation"),
		KitbagSession->SubmitPredictedPosition(FIntPoint(1400, 2000)),
		ECorsairsActionRequestResult::Sent);
	TestEqual(TEXT("manual input sends only after item terminal"),
		KitbagSends, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionResetTest,
	"Corsairs.Movement.Session.ResetOnDisconnectAndLogout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionResetTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, Spawn);
	Session->SetSendOverrideForTests([](WPacket&) { return true; });
	Session->SubmitPredictedPosition(FIntPoint(1300, 2000));
	TestTrue(TEXT("precondition has pending MOVE"),
		Session->HasPendingMoveForDiagnostics());
	Session->HandleConnectionStateForTests(
		ECorsairsConnectionState::Disconnected,
		TEXT("test disconnect"));
	TestFalse(TEXT("disconnect clears reducer pending MOVE"),
		Session->HasPendingMoveForDiagnostics());
	TestEqual(TEXT("disconnect clears confirmed position"),
		Session->GetConfirmedPosition(), FIntPoint::ZeroValue);
	TestFalse(TEXT("disconnect clears movement authority lock"),
		Session->IsMovementAuthorityLocked());
	TestEqual(TEXT("disconnect resets MOVE diagnostics"),
		Session->GetMovementBeginSendCountForDiagnostics(), 0LL);

	Session->SetInWorldForTests(LocalWorldId, Spawn);
	Session->SetSendOverrideForTests([](WPacket&) { return true; });
	FCorsairsWorldActor VisibleActor;
	VisibleActor.WorldId = RemoteWorldId;
	VisibleActor.Position = FIntPoint(2000, 3000);
	Session->AddVisibleActorForTests(VisibleActor);
	Session->SubmitPredictedPosition(FIntPoint(1300, 2000));
	TestEqual(TEXT("logout precondition is in world"),
		Session->GetStage(), ECorsairsLoginStage::InWorld);
	TestEqual(TEXT("logout precondition has visible actor"),
		Session->GetVisibleActors().Num(), 1);
	Session->Logout();
	TestEqual(TEXT("logout returns stage to idle"),
		Session->GetStage(), ECorsairsLoginStage::Idle);
	TestEqual(TEXT("logout clears visible actors"),
		Session->GetVisibleActors().Num(), 0);
	TestFalse(TEXT("logout clears reducer pending MOVE"),
		Session->HasPendingMoveForDiagnostics());
	TestEqual(TEXT("logout clears confirmed position"),
		Session->GetConfirmedPosition(), FIntPoint::ZeroValue);
	TestFalse(TEXT("logout clears movement authority lock"),
		Session->IsMovementAuthorityLocked());
	TestEqual(TEXT("logout resets MOVE diagnostics"),
		Session->GetMovementBeginSendCountForDiagnostics(), 0LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionMovementSpeedTest,
	"Corsairs.Movement.Session.MovementSpeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionMovementSpeedTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Deliver(Session, MakeEnterMap(Spawn));
	Session->SetSendOverrideForTests([](WPacket&) { return true; });
	TArray<FCorsairsMovementEvent> Events;
	TArray<FString> Errors;
	Session->SetEventObserversForTests(
		[&](const FCorsairsMovementEvent& Event) { Events.Add(Event); },
		[&](const FString& Error) { Errors.Add(Error); },
		TFunction<void(ECorsairsLoginStage)>());

	TestEqual(TEXT("ENTERMAP reads local ATTR_MSPD"),
		Session->GetMovementSpeedCmPerSecond(), LocalSpeed);
	Session->SendMovePath({Spawn, FIntPoint(1300, 2400)});
	Deliver(
		Session,
		MakeMove(
			LocalWorldId,
			1,
			0,
			SpawnToFirstEndpointBytes,
			UE_ARRAY_COUNT(SpawnToFirstEndpointBytes)));
	Deliver(
		Session,
		MakeMove(
			LocalWorldId,
			1,
			1,
			Endpoint1500Bytes,
			UE_ARRAY_COUNT(Endpoint1500Bytes)));

	Deliver(
		Session,
		MakeActorSeen(RemoteWorldId, FIntPoint(2000, 3000), 300));
	const FCorsairsWorldActor* Remote =
		Session->GetVisibleActors().FindByPredicate(
			[](const FCorsairsWorldActor& Actor)
			{
				return Actor.WorldId == RemoteWorldId;
			});
	TestNotNull(TEXT("remote actor exists"), Remote);
	if (Remote != nullptr)
	{
		TestEqual(TEXT("CHABEGINSEE reads remote ATTR_MSPD"),
			Remote->MovementSpeedCmPerSecond, 300.0);
	}
	Deliver(
		Session,
		MakeMove(
			RemoteWorldId,
			90,
			0,
			RemotePathBytes,
			UE_ARRAY_COUNT(RemotePathBytes)));
	DeliverSpeed(Session, RemoteWorldId, 350);
	Deliver(
		Session,
		MakeMove(
			RemoteWorldId,
			91,
			0,
			RemotePathBytes,
			UE_ARRAY_COUNT(RemotePathBytes)));

	DeliverSpeed(Session, LocalWorldId, 600);
	TestEqual(TEXT("SYNATTR updates local ATTR_MSPD"),
		Session->GetMovementSpeedCmPerSecond(), 600.0);
	TestResult(this, TEXT("skill begins for local server-driven MOVE"),
		Session->UseSkillOn(26, RemoteWorldId),
		ECorsairsActionRequestResult::Sent);
	Deliver(
		Session,
		MakeMove(
			LocalWorldId,
			2,
			0,
			Endpoint1500Bytes,
			UE_ARRAY_COUNT(Endpoint1500Bytes)));

	TestEqual(TEXT("all valid movement events are broadcast"),
		Events.Num(), 5);
	if (Events.Num() == 5)
	{
		const double ExpectedSpeeds[] = {
			LocalSpeed, LocalSpeed, 300.0, 350.0, 600.0};
		for (int32 Index = 0; Index < Events.Num(); ++Index)
		{
			TestEqual(
				FString::Printf(TEXT("event %d receives matching speed"),
					Index),
				Events[Index].MovementSpeedCmPerSecond,
				ExpectedSpeeds[Index]);
			TestTrue(
				FString::Printf(TEXT("event %d receives positive speed"),
					Index),
				Events[Index].MovementSpeedCmPerSecond > 0.0);
		}
	}
	TestTrue(TEXT("remote MOVE is server-driven"),
		Events.Num() > 2 && Events[2].bServerDriven);
	TestTrue(TEXT("skill MOVE is server-driven"),
		Events.Num() > 4 && Events[4].bServerDriven);
	TestEqual(TEXT("valid speeds do not report protocol errors"),
		Errors.Num(), 0);

	UCorsairsSession* MissingRemote = NewObject<UCorsairsSession>();
	MissingRemote->SetInWorldForTests(LocalWorldId, Spawn);
	TArray<FCorsairsMovementEvent> MissingEvents;
	TArray<FString> MissingErrors;
	MissingRemote->SetEventObserversForTests(
		[&](const FCorsairsMovementEvent& Event)
		{
			MissingEvents.Add(Event);
		},
		[&](const FString& Error) { MissingErrors.Add(Error); },
		TFunction<void(ECorsairsLoginStage)>());
	Deliver(
		MissingRemote,
		MakeActorSeen(RemoteWorldId, FIntPoint(2000, 3000),
			TOptional<int64>()));
	Deliver(
		MissingRemote,
		MakeMove(
			RemoteWorldId,
			90,
			0,
			RemotePathBytes,
			UE_ARRAY_COUNT(RemotePathBytes)));
	TestEqual(TEXT("missing remote speed reports protocol error"),
		MissingErrors.Num(), 1);
	TestEqual(TEXT("missing remote speed blocks follower event"),
		MissingEvents.Num(), 0);

	UCorsairsSession* ZeroLocal = NewObject<UCorsairsSession>();
	Deliver(ZeroLocal, MakeEnterMap(Spawn, 0));
	ZeroLocal->SetSendOverrideForTests([](WPacket&) { return true; });
	TArray<FCorsairsMovementEvent> ZeroEvents;
	TArray<FString> ZeroErrors;
	ZeroLocal->SetEventObserversForTests(
		[&](const FCorsairsMovementEvent& Event) { ZeroEvents.Add(Event); },
		[&](const FString& Error) { ZeroErrors.Add(Error); },
		TFunction<void(ECorsairsLoginStage)>());
	Deliver(ZeroLocal,
		MakeActorSeen(RemoteWorldId, FIntPoint(2000, 3000), 300));
	ZeroLocal->UseSkillOn(26, RemoteWorldId);
	Deliver(
		ZeroLocal,
		MakeMove(
			LocalWorldId,
			1,
			0,
			Endpoint1500Bytes,
			UE_ARRAY_COUNT(Endpoint1500Bytes)));
	TestEqual(TEXT("zero local speed reports protocol error"),
		ZeroErrors.Num(), 1);
	TestEqual(TEXT("zero local speed blocks follower event"),
		ZeroEvents.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionChaEndSeeLayoutTest,
	"Corsairs.Movement.Session.ChaEndSeeLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionChaEndSeeLayoutTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	Session->SetInWorldForTests(LocalWorldId, Spawn);
	Deliver(Session, MakeActorSeen(1, FIntPoint(100, 100), 200));
	Deliver(Session, MakeActorSeen(77, FIntPoint(200, 200), 250));
	Deliver(Session, Msg::McChaEndSeeMessage{1, 77});

	TestEqual(TEXT("one actor remains"),
		Session->GetVisibleActors().Num(), 1);
	if (Session->GetVisibleActors().Num() == 1)
	{
		TestEqual(TEXT("seeType is not mistaken for worldId"),
			Session->GetVisibleActors()[0].WorldId, 1LL);
	}
	return true;
}

#endif
