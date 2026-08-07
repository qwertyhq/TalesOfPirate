#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CorsairsActionReducer.h"

namespace
{
constexpr int64 LocalWorldId = 77;
constexpr int64 SkillPacketId = 700;
constexpr int64 MovePacketId = 701;

const uint8 FirstWaypointBytes[] = {
	0x6E, 0x00, 0x00, 0x00,
	0xDC, 0x00, 0x00, 0x00,
};

const uint8 TerminalWaypointBytes[] = {
	0x2C, 0x01, 0x00, 0x00,
	0x90, 0x01, 0x00, 0x00,
};

TConstArrayView<uint8> FirstWaypoint()
{
	return MakeArrayView(FirstWaypointBytes);
}

TConstArrayView<uint8> TerminalWaypoint()
{
	return MakeArrayView(TerminalWaypointBytes);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerReservesBeforeSendTest,
	"Corsairs.Movement.Reducer.ReservesBeforeSend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerReservesBeforeSendTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, FIntPoint(10, 20));

	int32 OuterSendCount = 0;
	int32 InnerSendCount = 0;
	ECorsairsActionRequestResult NestedResult =
		ECorsairsActionRequestResult::Invalid;
	const FCorsairsPendingMove PendingMove{
		MovePacketId,
		FIntPoint(10, 20),
		FIntPoint(30, 40),
	};

	const ECorsairsActionRequestResult OuterResult = Reducer.Begin(
		SkillPacketId,
		ECorsairsBeginActionType::Skill,
		TOptional<FCorsairsPendingMove>(),
		[&]()
		{
			++OuterSendCount;
			NestedResult = Reducer.Begin(
				MovePacketId,
				ECorsairsBeginActionType::Move,
				PendingMove,
				[&]()
				{
					++InnerSendCount;
					return true;
				});
			return true;
		});

	TestEqual(TEXT("outer request is sent"),
		static_cast<uint8>(OuterResult),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));
	TestEqual(TEXT("nested request is busy"),
		static_cast<uint8>(NestedResult),
		static_cast<uint8>(ECorsairsActionRequestResult::Busy));
	TestEqual(TEXT("outer send count"), OuterSendCount, 1);
	TestEqual(TEXT("inner send count"), InnerSendCount, 0);
	TestTrue(TEXT("outer action remains reserved"),
		Reducer.GetActiveAction().IsSet());
	if (Reducer.GetActiveAction().IsSet())
	{
		TestEqual(TEXT("active packet is outer packet"),
			Reducer.GetActiveAction()->PacketId,
			SkillPacketId);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerRollsBackFailedSendTest,
	"Corsairs.Movement.Reducer.RollsBackFailedSend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerRollsBackFailedSendTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	const FIntPoint SpawnPosition(223325, 278475);
	Reducer.EnterWorld(LocalWorldId, SpawnPosition);
	const FCorsairsPendingMove PendingMove{
		MovePacketId,
		SpawnPosition,
		FIntPoint(223825, 278475),
	};

	const ECorsairsActionRequestResult Result = Reducer.Begin(
		MovePacketId,
		ECorsairsBeginActionType::Move,
		PendingMove,
		[]()
		{
			return false;
		});

	TestEqual(TEXT("transport failure is reported"),
		static_cast<uint8>(Result),
		static_cast<uint8>(ECorsairsActionRequestResult::TransportFailed));
	TestFalse(TEXT("active action is rolled back"),
		Reducer.GetActiveAction().IsSet());
	TestFalse(TEXT("pending move is rolled back"),
		Reducer.GetPendingMove().IsSet());
	TestEqual(TEXT("confirmed position is unchanged"),
		Reducer.GetConfirmedPosition(),
		SpawnPosition);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerItemLifecycleTest,
	"Corsairs.Movement.Reducer.ItemLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerItemLifecycleTest::RunTest(const FString&)
{
	constexpr int64 ItemPacketId = 800;
	constexpr int64 SecondPacketId = 801;
	constexpr int64 WrongWorldId = 78;
	constexpr int64 WrongPacketId = 802;
	constexpr int64 Kitbag = 6;
	constexpr int64 Look = 5;
	constexpr int64 ItemFailed = 15;
	constexpr int64 WrongNotification = 9;

	const auto BeginItem = [](FCorsairsActionReducer& Reducer,
		ECorsairsBeginActionType ActionType)
	{
		return Reducer.Begin(
			ItemPacketId,
			ActionType,
			TOptional<FCorsairsPendingMove>(),
			[]()
			{
				return true;
			});
	};

	const int64 ItemUseTerminals[] = {Kitbag, Look, ItemFailed};
	for (const int64 Notification : ItemUseTerminals)
	{
		FCorsairsActionReducer Reducer;
		Reducer.EnterWorld(LocalWorldId, FIntPoint::ZeroValue);
		TestEqual(TEXT("item use begins"),
			static_cast<uint8>(BeginItem(
				Reducer,
				ECorsairsBeginActionType::ItemUse)),
			static_cast<uint8>(ECorsairsActionRequestResult::Sent));
		const FCorsairsReducerEffects Effects = Reducer.OnItemNotification(
			LocalWorldId,
			ItemPacketId,
			Notification);
		TestFalse(TEXT("valid item use terminal is accepted"),
			Effects.bProtocolError);
		TestFalse(TEXT("valid item use terminal closes reservation"),
			Reducer.GetActiveAction().IsSet());
	}

	const int64 ItemPickTerminals[] = {Kitbag, ItemFailed};
	for (const int64 Notification : ItemPickTerminals)
	{
		FCorsairsActionReducer Reducer;
		Reducer.EnterWorld(LocalWorldId, FIntPoint::ZeroValue);
		BeginItem(Reducer, ECorsairsBeginActionType::ItemPick);
		const FCorsairsReducerEffects Effects = Reducer.OnItemNotification(
			LocalWorldId,
			ItemPacketId,
			Notification);
		TestFalse(TEXT("valid item pick terminal is accepted"),
			Effects.bProtocolError);
		TestFalse(TEXT("valid item pick terminal closes reservation"),
			Reducer.GetActiveAction().IsSet());
	}

	struct FInvalidItemNotification
	{
		ECorsairsBeginActionType ActionType;
		int64 WorldId;
		int64 PacketId;
		int64 Notification;
	};
	const FInvalidItemNotification InvalidCases[] = {
		{ECorsairsBeginActionType::ItemUse,
			WrongWorldId, ItemPacketId, Kitbag},
		{ECorsairsBeginActionType::ItemUse,
			LocalWorldId, WrongPacketId, Kitbag},
		{ECorsairsBeginActionType::ItemUse,
			LocalWorldId, ItemPacketId, WrongNotification},
		{ECorsairsBeginActionType::ItemPick,
			LocalWorldId, ItemPacketId, Look},
	};
	for (const FInvalidItemNotification& Invalid : InvalidCases)
	{
		FCorsairsActionReducer Reducer;
		Reducer.EnterWorld(LocalWorldId, FIntPoint::ZeroValue);
		BeginItem(Reducer, Invalid.ActionType);
		const FCorsairsReducerEffects Effects = Reducer.OnItemNotification(
			Invalid.WorldId,
			Invalid.PacketId,
			Invalid.Notification);
		TestTrue(TEXT("invalid notification reports protocol error"),
			Effects.bProtocolError);
		TestTrue(TEXT("invalid notification keeps reservation busy"),
			Reducer.GetActiveAction().IsSet());
	}

	FCorsairsActionReducer SerializedReducer;
	SerializedReducer.EnterWorld(LocalWorldId, FIntPoint::ZeroValue);
	BeginItem(SerializedReducer, ECorsairsBeginActionType::ItemUse);
	int32 SecondSendCount = 0;
	const ECorsairsActionRequestResult BusyResult = SerializedReducer.Begin(
		SecondPacketId,
		ECorsairsBeginActionType::ItemPick,
		TOptional<FCorsairsPendingMove>(),
		[&]()
		{
			++SecondSendCount;
			return true;
		});
	TestEqual(TEXT("second item action is busy"),
		static_cast<uint8>(BusyResult),
		static_cast<uint8>(ECorsairsActionRequestResult::Busy));
	TestEqual(TEXT("busy action never reaches send"), SecondSendCount, 0);
	SerializedReducer.OnItemNotification(LocalWorldId, ItemPacketId, Kitbag);
	const ECorsairsActionRequestResult SentAfterTerminal =
		SerializedReducer.Begin(
			SecondPacketId,
			ECorsairsBeginActionType::ItemPick,
			TOptional<FCorsairsPendingMove>(),
			[&]()
			{
				++SecondSendCount;
				return true;
			});
	TestEqual(TEXT("second item action sends after terminal"),
		static_cast<uint8>(SentAfterTerminal),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));
	TestEqual(TEXT("send occurs exactly once after terminal"),
		SecondSendCount,
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerSkillLifecycleTest,
	"Corsairs.Movement.Reducer.SkillLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerSkillLifecycleTest::RunTest(const FString&)
{
	constexpr int64 MoveOn = 0;
	constexpr int64 MoveInRange = 8;
	constexpr int64 FightOn = 0;
	constexpr int64 FightTerminal = 1;
	constexpr int64 MoveAction = 1;
	constexpr int64 SkillAction = 2;
	constexpr int64 ItemUseAction = 11;

	FCorsairsActionReducer FightReducer;
	FightReducer.EnterWorld(LocalWorldId, FIntPoint(10, 20));
	FightReducer.Begin(
		SkillPacketId,
		ECorsairsBeginActionType::Skill,
		TOptional<FCorsairsPendingMove>(),
		[]()
		{
			return true;
		});
	TestTrue(TEXT("skill locks movement authority"),
		FightReducer.IsMovementAuthorityLocked());
	FightReducer.OnMove(
		LocalWorldId,
		SkillPacketId,
		MoveOn,
		FirstWaypoint());
	TestTrue(TEXT("server move keeps skill active"),
		FightReducer.GetActiveAction().IsSet());
	if (FightReducer.GetActiveAction().IsSet())
	{
		TestEqual(TEXT("skill enters server move phase"),
			static_cast<uint8>(FightReducer.GetActiveAction()->Phase),
			static_cast<uint8>(ECorsairsActionPhase::ServerMove));
	}
	const FCorsairsReducerEffects InRangeEffects = FightReducer.OnMove(
		LocalWorldId,
		SkillPacketId,
		MoveInRange,
		TerminalWaypoint());
	TestTrue(TEXT("in-range emits reconciliation"),
		InRangeEffects.Movement.IsSet());
	TestTrue(TEXT("in-range keeps skill reservation"),
		FightReducer.GetActiveAction().IsSet());
	if (FightReducer.GetActiveAction().IsSet())
	{
		TestEqual(TEXT("in-range enters fight phase"),
			static_cast<uint8>(FightReducer.GetActiveAction()->Phase),
			static_cast<uint8>(ECorsairsActionPhase::Fight));
	}
	FightReducer.OnSkillSource(
		LocalWorldId,
		SkillPacketId,
		FightOn);
	TestTrue(TEXT("fight-on remains active"),
		FightReducer.GetActiveAction().IsSet());
	FightReducer.OnSkillSource(
		LocalWorldId,
		SkillPacketId,
		FightTerminal);
	TestFalse(TEXT("terminal skill source clears skill"),
		FightReducer.GetActiveAction().IsSet());
	TestFalse(TEXT("terminal skill source unlocks movement"),
		FightReducer.IsMovementAuthorityLocked());

	const int64 InterruptStates[] = {1, 2, 4, 16, 32};
	for (const int64 InterruptState : InterruptStates)
	{
		FCorsairsActionReducer Reducer;
		Reducer.EnterWorld(LocalWorldId, FIntPoint(10, 20));
		Reducer.Begin(
			SkillPacketId,
			ECorsairsBeginActionType::Skill,
			TOptional<FCorsairsPendingMove>(),
			[]()
			{
				return true;
			});
		Reducer.OnMove(
			LocalWorldId,
			SkillPacketId,
			MoveOn,
			FirstWaypoint());
		const FCorsairsReducerEffects Effects = Reducer.OnMove(
			LocalWorldId,
			SkillPacketId,
			InterruptState,
			TerminalWaypoint());
		TestFalse(TEXT("interrupt clears skill and server move"),
			Reducer.GetActiveAction().IsSet());
		TestEqual(TEXT("interrupt confirms exact endpoint"),
			Reducer.GetConfirmedPosition(),
			FIntPoint(300, 400));
		TestTrue(TEXT("interrupt emits movement reconciliation"),
			Effects.Movement.IsSet());
		if (Effects.Movement.IsSet())
		{
			TestEqual(TEXT("reconciliation world"),
				Effects.Movement->WorldId,
				LocalWorldId);
			TestEqual(TEXT("reconciliation packet"),
				Effects.Movement->PacketId,
				SkillPacketId);
			TestEqual(TEXT("reconciliation type"),
				static_cast<uint8>(Effects.Movement->Type),
				static_cast<uint8>(ECorsairsMovementEventType::Rejected));
			TestEqual(TEXT("reconciliation state"),
				Effects.Movement->MoveState,
				static_cast<uint8>(InterruptState));
			TestEqual(TEXT("reconciliation waypoint count"),
				Effects.Movement->Waypoints.Num(),
				1);
			TestEqual(TEXT("reconciliation endpoint"),
				Effects.Movement->Endpoint,
				FIntPoint(300, 400));
			TestTrue(TEXT("reconciliation is local"),
				Effects.Movement->bLocal);
			TestTrue(TEXT("reconciliation is server driven"),
				Effects.Movement->bServerDriven);
			TestTrue(TEXT("interrupt requires neutral"),
				Effects.Movement->bRequireNeutral);
		}
	}

	const int64 ClearingFailureTypes[] = {SkillAction, MoveAction};
	for (const int64 FailureType : ClearingFailureTypes)
	{
		FCorsairsActionReducer Reducer;
		Reducer.EnterWorld(LocalWorldId, FIntPoint(10, 20));
		Reducer.Begin(
			SkillPacketId,
			ECorsairsBeginActionType::Skill,
			TOptional<FCorsairsPendingMove>(),
			[]()
			{
				return true;
			});
		Reducer.OnMove(
			LocalWorldId,
			SkillPacketId,
			MoveOn,
			FirstWaypoint());
		const FCorsairsReducerEffects Effects = Reducer.OnFailedAction(
			LocalWorldId,
			FailureType,
			99);
		TestFalse(TEXT("related failure is accepted"),
			Effects.bProtocolError);
		TestFalse(TEXT("related failure clears skill"),
			Reducer.GetActiveAction().IsSet());
	}

	FCorsairsActionReducer UnrelatedReducer;
	UnrelatedReducer.EnterWorld(LocalWorldId, FIntPoint(10, 20));
	UnrelatedReducer.Begin(
		SkillPacketId,
		ECorsairsBeginActionType::Skill,
		TOptional<FCorsairsPendingMove>(),
		[]()
		{
			return true;
		});
	const FCorsairsReducerEffects UnrelatedEffects =
		UnrelatedReducer.OnFailedAction(
			LocalWorldId,
			ItemUseAction,
			99);
	TestTrue(TEXT("unrelated failure reports protocol error"),
		UnrelatedEffects.bProtocolError);
	TestTrue(TEXT("unrelated failure leaves skill active"),
		UnrelatedReducer.GetActiveAction().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerResetOnDisconnectTest,
	"Corsairs.Movement.Reducer.ResetOnDisconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerResetOnDisconnectTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, FIntPoint(10, 20));
	const FCorsairsPendingMove PendingMove{
		MovePacketId,
		FIntPoint(10, 20),
		FIntPoint(30, 40),
	};
	Reducer.Begin(
		MovePacketId,
		ECorsairsBeginActionType::Move,
		PendingMove,
		[]()
		{
			return true;
		});
	TestTrue(TEXT("pending move can queue an endpoint"),
		Reducer.QueueEndpoint(FIntPoint(50, 60)));

	Reducer.Reset();

	TestFalse(TEXT("reset clears active action"),
		Reducer.GetActiveAction().IsSet());
	TestFalse(TEXT("reset clears pending move"),
		Reducer.GetPendingMove().IsSet());
	TestFalse(TEXT("reset clears queued endpoint"),
		Reducer.GetQueuedEndpoint().IsSet());
	TestEqual(TEXT("reset clears confirmed position"),
		Reducer.GetConfirmedPosition(),
		FIntPoint::ZeroValue);
	TestFalse(TEXT("reset clears movement authority lock"),
		Reducer.IsMovementAuthorityLocked());
	return true;
}

#endif
