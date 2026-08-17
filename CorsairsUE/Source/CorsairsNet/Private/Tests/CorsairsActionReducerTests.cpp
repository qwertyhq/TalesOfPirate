#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CorsairsActionReducer.h"

namespace
{
constexpr int64 LocalWorldId = 77;
constexpr int64 SkillPacketId = 700;
constexpr int64 MovePacketId = 701;

constexpr int64 MoveAction = 1;
constexpr int64 MoveOn = 0;
constexpr int64 MoveArrive = 1;
constexpr int64 MoveBlock = 2;
constexpr int64 MoveCancel = 4;
constexpr int64 MoveNoTarget = 16;
constexpr int64 MoveCantMove = 32;
constexpr int64 FailedActionForbidden = 0;
constexpr int64 FailedActionExisting = 1;
constexpr int64 FailedActionMovePath = 2;

constexpr int64 ManualPacketA = 10;
constexpr int64 ManualPacketB = 11;
constexpr int64 MismatchedPacket = 12;
constexpr int64 RemoteWorldId = 88;
constexpr int64 RemotePacketId = 90;

const FIntPoint ManualStart(223325, 278475);
const FIntPoint ManualEndpointA(223825, 278475);
const FIntPoint ManualEndpointB(224325, 278475);

const uint8 FirstWaypointBytes[] = {
	0x6E, 0x00, 0x00, 0x00,
	0xDC, 0x00, 0x00, 0x00,
};

const uint8 TerminalWaypointBytes[] = {
	0x2C, 0x01, 0x00, 0x00,
	0x90, 0x01, 0x00, 0x00,
};

const uint8 ManualPathABytes[] = {
	0x57, 0x69, 0x03, 0x00,
	0xCB, 0x3F, 0x04, 0x00,
	0x51, 0x6A, 0x03, 0x00,
	0xCB, 0x3F, 0x04, 0x00,
};

const uint8 ManualEndpointABytes[] = {
	0x51, 0x6A, 0x03, 0x00,
	0xCB, 0x3F, 0x04, 0x00,
};

const uint8 ManualEndpointBBytes[] = {
	0x45, 0x6C, 0x03, 0x00,
	0xCB, 0x3F, 0x04, 0x00,
};

const uint8 RemotePathBytes[] = {
	0xE8, 0x03, 0x00, 0x00,
	0xD0, 0x07, 0x00, 0x00,
	0x4C, 0x04, 0x00, 0x00,
	0x34, 0x08, 0x00, 0x00,
};

const uint8 RemoteOtherEndpointBytes[] = {
	0xB0, 0x04, 0x00, 0x00,
	0x98, 0x08, 0x00, 0x00,
};

const uint8 SevenWaypointBytes[] = {
	0x51, 0x6A, 0x03, 0x00,
	0xCB, 0x3F, 0x04,
};

const uint8 NineWaypointBytes[] = {
	0x51, 0x6A, 0x03, 0x00,
	0xCB, 0x3F, 0x04, 0x00,
	0xFF,
};

TConstArrayView<uint8> FirstWaypoint()
{
	return MakeArrayView(FirstWaypointBytes);
}

TConstArrayView<uint8> TerminalWaypoint()
{
	return MakeArrayView(TerminalWaypointBytes);
}

TConstArrayView<uint8> ManualPathA()
{
	return MakeArrayView(ManualPathABytes);
}

TConstArrayView<uint8> ManualEndpointAPath()
{
	return MakeArrayView(ManualEndpointABytes);
}

TConstArrayView<uint8> ManualEndpointBPath()
{
	return MakeArrayView(ManualEndpointBBytes);
}

TConstArrayView<uint8> RemotePath()
{
	return MakeArrayView(RemotePathBytes);
}

TConstArrayView<uint8> RemoteOtherEndpointPath()
{
	return MakeArrayView(RemoteOtherEndpointBytes);
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
	FCorsairsReducerCancelLifecycleTest,
	"Corsairs.Net.ActionReducer.CancelLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerCancelLifecycleTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	const FCorsairsPendingMove PendingMove{
		MovePacketId,
		ManualStart,
		ManualEndpointA,
	};
	Reducer.Begin(
		MovePacketId,
		ECorsairsBeginActionType::Move,
		PendingMove,
		[]() { return true; });
	TestTrue(TEXT("MOVE виден как active action"),
		Reducer.HasActiveAction());
	TestFalse(TEXT("до запроса отмена не pending"),
		Reducer.IsCancelPending());

	int32 CancelSendCount = 0;
	ECorsairsActionRequestResult NestedCancel =
		ECorsairsActionRequestResult::Invalid;
	const ECorsairsActionRequestResult FirstCancel = Reducer.RequestCancel(
		[&]()
		{
			++CancelSendCount;
			NestedCancel = Reducer.RequestCancel(
				[&]()
				{
					++CancelSendCount;
					return true;
				});
			return true;
		});
	TestEqual(TEXT("первая отмена отправлена"),
		static_cast<uint8>(FirstCancel),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));
	TestEqual(TEXT("reentrant повтор видит reservation"),
		static_cast<uint8>(NestedCancel),
		static_cast<uint8>(ECorsairsActionRequestResult::Busy));
	TestEqual(TEXT("первая отмена вызывает transport один раз"),
		CancelSendCount, 1);
	TestTrue(TEXT("успешная отмена остаётся pending до terminal"),
		Reducer.IsCancelPending());

	const ECorsairsActionRequestResult RepeatedCancel = Reducer.RequestCancel(
		[&]()
		{
			++CancelSendCount;
			return true;
		});
	TestEqual(TEXT("повторная отмена до terminal занята"),
		static_cast<uint8>(RepeatedCancel),
		static_cast<uint8>(ECorsairsActionRequestResult::Busy));
	TestEqual(TEXT("повторная отмена не достигает transport"),
		CancelSendCount, 1);

	Reducer.OnMove(
		LocalWorldId,
		MovePacketId,
		MoveArrive,
		ManualEndpointAPath());
	TestFalse(TEXT("terminal завершает active action"),
		Reducer.GetActiveAction().IsSet());
	TestFalse(TEXT("terminal очищает read-only active seam"),
		Reducer.HasActiveAction());
	TestFalse(TEXT("terminal очищает cancel pending seam"),
		Reducer.IsCancelPending());
	TestEqual(TEXT("без active action отмена невалидна"),
		static_cast<uint8>(Reducer.RequestCancel([]() { return true; })),
		static_cast<uint8>(ECorsairsActionRequestResult::Invalid));

	constexpr int64 RetryPacketId = MovePacketId + 1;
	const FCorsairsPendingMove RetryMove{
		RetryPacketId,
		ManualEndpointA,
		ManualEndpointB,
	};
	Reducer.Begin(
		RetryPacketId,
		ECorsairsBeginActionType::Move,
		RetryMove,
		[]() { return true; });
	TestEqual(TEXT("ошибка transport отмены явная"),
		static_cast<uint8>(Reducer.RequestCancel([]() { return false; })),
		static_cast<uint8>(ECorsairsActionRequestResult::TransportFailed));
	int32 RetryCount = 0;
	TestEqual(TEXT("после rollback отмену можно повторить"),
		static_cast<uint8>(Reducer.RequestCancel(
			[&]()
			{
				++RetryCount;
				return true;
			})),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));
	TestEqual(TEXT("retry достигает transport ровно раз"), RetryCount, 1);

	FCorsairsActionReducer QueueDuringFailedCancel;
	QueueDuringFailedCancel.EnterWorld(LocalWorldId, ManualStart);
	QueueDuringFailedCancel.Begin(
		MovePacketId,
		ECorsairsBeginActionType::Move,
		PendingMove,
		[]() { return true; });
	bool bQueueMutationApplied = false;
	TestEqual(TEXT("cancel transport failure остаётся явным после queue mutation"),
		static_cast<uint8>(QueueDuringFailedCancel.RequestCancel(
			[&]()
			{
				bQueueMutationApplied =
					QueueDuringFailedCancel.QueueEndpoint(ManualEndpointB);
				return false;
			})),
		static_cast<uint8>(ECorsairsActionRequestResult::TransportFailed));
	TestTrue(TEXT("callback действительно изменил независимую queue"),
		bQueueMutationApplied);
	TestEqual(TEXT("независимая queue mutation сохранена"),
		QueueDuringFailedCancel.GetQueuedEndpoint().GetValue(),
		ManualEndpointB);
	TestFalse(TEXT("failed cancel очищает только свою reservation"),
		QueueDuringFailedCancel.IsCancelPending());
	TestEqual(TEXT("после failed cancel retry не Busy"),
		static_cast<uint8>(QueueDuringFailedCancel.RequestCancel(
			[]() { return true; })),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));

	Reducer.Reset();
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	TestEqual(TEXT("reset освобождает cancel reservation"),
		static_cast<uint8>(Reducer.Begin(
			SkillPacketId,
			ECorsairsBeginActionType::Skill,
			TOptional<FCorsairsPendingMove>(),
			[]() { return true; })),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));
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

	FCorsairsActionReducer QueueMutation;
	QueueMutation.EnterWorld(LocalWorldId, SpawnPosition);
	bool bBeginQueueMutationApplied = false;
	const ECorsairsActionRequestResult QueueMutationResult =
		QueueMutation.Begin(
			MovePacketId,
			ECorsairsBeginActionType::Move,
			PendingMove,
			[&]()
			{
				bBeginQueueMutationApplied =
					QueueMutation.QueueEndpoint(ManualEndpointB);
				return false;
			});
	TestTrue(TEXT("Begin callback действительно изменил queue"),
		bBeginQueueMutationApplied);
	TestEqual(TEXT("failed Begin после queue mutation сообщает transport"),
		static_cast<uint8>(QueueMutationResult),
		static_cast<uint8>(ECorsairsActionRequestResult::TransportFailed));
	TestFalse(TEXT("failed Begin не оставляет unsent active reservation"),
		QueueMutation.HasActiveAction());
	TestFalse(TEXT("failed Begin не оставляет unsent pending MOVE"),
		QueueMutation.GetPendingMove().IsSet());
	TestFalse(TEXT("failed Begin откатывает queue своей reservation"),
		QueueMutation.GetQueuedEndpoint().IsSet());
	TestEqual(TEXT("после failed Begin следующий action не Busy"),
		static_cast<uint8>(QueueMutation.Begin(
			SkillPacketId,
			ECorsairsBeginActionType::Skill,
			TOptional<FCorsairsPendingMove>(),
			[]() { return true; })),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));

	bool bObservedMovement = false;
	FIntPoint ObservedEndpoint = FIntPoint::ZeroValue;
	ECorsairsActionPhase ObservedPhase = ECorsairsActionPhase::None;
	const ECorsairsActionRequestResult ReentrantResult = Reducer.Begin(
		SkillPacketId,
		ECorsairsBeginActionType::Skill,
		TOptional<FCorsairsPendingMove>(),
		[&]()
		{
			const FCorsairsReducerEffects Effects = Reducer.OnMove(
				LocalWorldId,
				SkillPacketId,
				0,
				TerminalWaypoint());
			bObservedMovement = Effects.Movement.IsSet();
			if (Effects.Movement.IsSet())
			{
				ObservedEndpoint = Effects.Movement->Endpoint;
			}
			if (Reducer.GetActiveAction().IsSet())
			{
				ObservedPhase = Reducer.GetActiveAction()->Phase;
			}
			return false;
		});

	TestTrue(TEXT("callback emitted a real movement event"),
		bObservedMovement);
	TestEqual(TEXT("callback changed the authoritative endpoint"),
		ObservedEndpoint,
		FIntPoint(300, 400));
	TestEqual(TEXT("callback entered server move phase"),
		static_cast<uint8>(ObservedPhase),
		static_cast<uint8>(ECorsairsActionPhase::ServerMove));
	TestEqual(TEXT("reentrant transport failure is reported"),
		static_cast<uint8>(ReentrantResult),
		static_cast<uint8>(ECorsairsActionRequestResult::TransportFailed));
	TestTrue(TEXT("reentrant committed action survives outer rollback"),
		Reducer.GetActiveAction().IsSet());
	TestFalse(TEXT("reentrant rollback clears pending move"),
		Reducer.GetPendingMove().IsSet());
	TestFalse(TEXT("reentrant rollback clears queued endpoint"),
		Reducer.GetQueuedEndpoint().IsSet());
	TestEqual(TEXT("reentrant committed endpoint survives outer rollback"),
		Reducer.GetConfirmedPosition(),
		FIntPoint(300, 400));
	TestTrue(TEXT("reentrant committed skill keeps authority lock"),
		Reducer.IsMovementAuthorityLocked());
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
	constexpr int64 MoveInRange = 8;
	constexpr int64 FightOn = 0;
	constexpr int64 FightTerminal = 1;
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

	FCorsairsActionReducer LateMoveFailureReducer;
	LateMoveFailureReducer.EnterWorld(LocalWorldId, FIntPoint(10, 20));
	LateMoveFailureReducer.Begin(
		SkillPacketId,
		ECorsairsBeginActionType::Skill,
		TOptional<FCorsairsPendingMove>(),
		[]()
		{
			return true;
		});
	LateMoveFailureReducer.OnMove(
		LocalWorldId,
		SkillPacketId,
		MoveInRange,
		TerminalWaypoint());
	const FCorsairsReducerEffects LateMoveFailureEffects =
		LateMoveFailureReducer.OnFailedAction(
			LocalWorldId,
			MoveAction,
			99);
	TestTrue(TEXT("late move failure is a protocol error"),
		LateMoveFailureEffects.bProtocolError);
	TestTrue(TEXT("late move failure keeps skill active"),
		LateMoveFailureReducer.GetActiveAction().IsSet());
	if (LateMoveFailureReducer.GetActiveAction().IsSet())
	{
		TestEqual(TEXT("late move failure preserves fight phase"),
			static_cast<uint8>(
				LateMoveFailureReducer.GetActiveAction()->Phase),
			static_cast<uint8>(ECorsairsActionPhase::Fight));
	}
	TestTrue(TEXT("late move failure keeps movement locked"),
		LateMoveFailureReducer.IsMovementAuthorityLocked());
	int32 LateBeginSendCount = 0;
	const ECorsairsActionRequestResult LateBeginResult =
		LateMoveFailureReducer.Begin(
			MovePacketId,
			ECorsairsBeginActionType::Move,
			FCorsairsPendingMove{
				MovePacketId,
				FIntPoint(300, 400),
				FIntPoint(500, 600),
			},
			[&]()
			{
				++LateBeginSendCount;
				return true;
			});
	TestEqual(TEXT("late move failure keeps begin gate busy"),
		static_cast<uint8>(LateBeginResult),
		static_cast<uint8>(ECorsairsActionRequestResult::Busy));
	TestEqual(TEXT("late move failure never sends a new begin"),
		LateBeginSendCount,
		0);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerOnArriveTest,
	"Corsairs.Movement.Reducer.OnArrive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerOnArriveTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	Reducer.Begin(
		ManualPacketA,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketA,
			ManualStart,
			ManualEndpointA,
		},
		[]()
		{
			return true;
		});

	const FCorsairsReducerEffects OnEffects = Reducer.OnMove(
		LocalWorldId,
		ManualPacketA,
		MoveOn,
		ManualPathA());
	TestFalse(TEXT("manual ON is accepted"), OnEffects.bProtocolError);
	TestTrue(TEXT("manual ON emits an accepted path"),
		OnEffects.Movement.IsSet());
	if (OnEffects.Movement.IsSet())
	{
		TestEqual(TEXT("manual ON type"),
			static_cast<uint8>(OnEffects.Movement->Type),
			static_cast<uint8>(ECorsairsMovementEventType::AcceptedPath));
		TestFalse(TEXT("manual ON is not server driven"),
			OnEffects.Movement->bServerDriven);
		TestEqual(TEXT("manual ON preserves both waypoints"),
			OnEffects.Movement->Waypoints.Num(),
			2);
	}
	TestTrue(TEXT("manual pending remains open after ON"),
		Reducer.GetPendingMove().IsSet());

	const FCorsairsReducerEffects ArriveEffects = Reducer.OnMove(
		LocalWorldId,
		ManualPacketA,
		MoveArrive,
		ManualEndpointAPath());
	TestFalse(TEXT("ARRIVE is accepted"), ArriveEffects.bProtocolError);
	TestTrue(TEXT("ARRIVE emits a terminal event"),
		ArriveEffects.Movement.IsSet());
	if (ArriveEffects.Movement.IsSet())
	{
		TestEqual(TEXT("ARRIVE type"),
			static_cast<uint8>(ArriveEffects.Movement->Type),
			static_cast<uint8>(ECorsairsMovementEventType::Terminal));
		TestEqual(TEXT("ARRIVE state"),
			ArriveEffects.Movement->MoveState,
			static_cast<uint8>(1));
		TestEqual(TEXT("ARRIVE endpoint"),
			ArriveEffects.Movement->Endpoint,
			FIntPoint(223825, 278475));
		TestTrue(TEXT("ARRIVE is local"),
			ArriveEffects.Movement->bLocal);
		TestFalse(TEXT("manual ARRIVE is not server driven"),
			ArriveEffects.Movement->bServerDriven);
		TestFalse(TEXT("ARRIVE does not require neutral"),
			ArriveEffects.Movement->bRequireNeutral);
	}
	TestEqual(TEXT("ARRIVE confirms the final waypoint"),
		Reducer.GetConfirmedPosition(),
		FIntPoint(223825, 278475));
	TestFalse(TEXT("ARRIVE clears active action"),
		Reducer.GetActiveAction().IsSet());
	TestFalse(TEXT("ARRIVE clears pending move"),
		Reducer.GetPendingMove().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerDirectTerminalsTest,
	"Corsairs.Movement.Reducer.DirectTerminals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerDirectTerminalsTest::RunTest(const FString&)
{
	struct FTerminalCase
	{
		int64 State;
		ECorsairsMovementEventType Type;
		bool bRequireNeutral;
		bool bExposeQueue;
	};
	const FTerminalCase Cases[] = {
		{MoveArrive, ECorsairsMovementEventType::Terminal, false, true},
		{MoveBlock, ECorsairsMovementEventType::Rejected, true, false},
		{MoveCancel, ECorsairsMovementEventType::Rejected, true, false},
		{MoveNoTarget, ECorsairsMovementEventType::Rejected, true, false},
		{MoveCantMove, ECorsairsMovementEventType::Rejected, true, false},
	};
	for (const FTerminalCase& TerminalCase : Cases)
	{
		FCorsairsActionReducer Reducer;
		Reducer.EnterWorld(LocalWorldId, ManualStart);
		Reducer.Begin(
			ManualPacketA,
			ECorsairsBeginActionType::Move,
			FCorsairsPendingMove{
				ManualPacketA,
				ManualStart,
				ManualEndpointA,
			},
			[]()
			{
				return true;
			});
		Reducer.QueueEndpoint(ManualEndpointB);

		const FCorsairsReducerEffects Effects = Reducer.OnMove(
			LocalWorldId,
			ManualPacketA,
			TerminalCase.State,
			ManualEndpointAPath());
		TestFalse(TEXT("direct terminal is accepted"),
			Effects.bProtocolError);
		TestTrue(TEXT("direct terminal emits movement"),
			Effects.Movement.IsSet());
		if (Effects.Movement.IsSet())
		{
			TestEqual(TEXT("direct terminal preserves literal state"),
				Effects.Movement->MoveState,
				static_cast<uint8>(TerminalCase.State));
			TestEqual(TEXT("direct terminal event type"),
				static_cast<uint8>(Effects.Movement->Type),
				static_cast<uint8>(TerminalCase.Type));
			TestEqual(TEXT("direct terminal neutral requirement"),
				Effects.Movement->bRequireNeutral,
				TerminalCase.bRequireNeutral);
		}
		TestEqual(TEXT("only direct ARRIVE exposes queue"),
			Effects.QueuedEndpoint.IsSet(),
			TerminalCase.bExposeQueue);
		if (Effects.QueuedEndpoint.IsSet())
		{
			TestEqual(TEXT("direct ARRIVE exposes literal B"),
				*Effects.QueuedEndpoint,
				FIntPoint(224325, 278475));
		}
		TestEqual(TEXT("direct terminal confirms endpoint"),
			Reducer.GetConfirmedPosition(),
			FIntPoint(223825, 278475));
		TestFalse(TEXT("direct terminal closes active action"),
			Reducer.GetActiveAction().IsSet());
		TestFalse(TEXT("direct terminal closes pending move"),
			Reducer.GetPendingMove().IsSet());
		TestFalse(TEXT("direct terminal consumes internal queue"),
			Reducer.GetQueuedEndpoint().IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerQueuesOnceTest,
	"Corsairs.Movement.Reducer.QueuesOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerQueuesOnceTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	Reducer.Begin(
		ManualPacketA,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketA,
			ManualStart,
			ManualEndpointA,
		},
		[]()
		{
			return true;
		});
	TestTrue(TEXT("B queues while A is pending"),
		Reducer.QueueEndpoint(ManualEndpointB));

	const FCorsairsReducerEffects Effects = Reducer.OnMove(
		LocalWorldId,
		ManualPacketA,
		MoveArrive,
		ManualEndpointAPath());
	TestTrue(TEXT("ARRIVE exposes one queued endpoint"),
		Effects.QueuedEndpoint.IsSet());
	if (Effects.QueuedEndpoint.IsSet())
	{
		TestEqual(TEXT("ARRIVE exposes exactly B"),
			*Effects.QueuedEndpoint,
			FIntPoint(224325, 278475));
	}
	TestFalse(TEXT("returned queue is consumed internally"),
		Reducer.GetQueuedEndpoint().IsSet());

	int32 SendCount = 0;
	const ECorsairsActionRequestResult Result = Reducer.Begin(
		ManualPacketB,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketB,
			FIntPoint(223825, 278475),
			FIntPoint(224325, 278475),
		},
		[&]()
		{
			++SendCount;
			return true;
		});
	TestEqual(TEXT("queued move begins after ARRIVE"),
		static_cast<uint8>(Result),
		static_cast<uint8>(ECorsairsActionRequestResult::Sent));
	TestEqual(TEXT("queued move sends exactly once"), SendCount, 1);
	TestTrue(TEXT("queued move is the new pending move"),
		Reducer.GetPendingMove().IsSet());
	if (Reducer.GetPendingMove().IsSet())
	{
		TestEqual(TEXT("queued move starts at confirmed A"),
			Reducer.GetPendingMove()->Start,
			FIntPoint(223825, 278475));
		TestEqual(TEXT("queued move ends at B"),
			Reducer.GetPendingMove()->RequestedEndpoint,
			FIntPoint(224325, 278475));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerDuplicateTerminalTest,
	"Corsairs.Movement.Reducer.DuplicateTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerDuplicateTerminalTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	Reducer.Begin(
		ManualPacketA,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketA,
			ManualStart,
			ManualEndpointA,
		},
		[]()
		{
			return true;
		});
	Reducer.QueueEndpoint(ManualEndpointB);
	const FCorsairsReducerEffects FirstTerminal = Reducer.OnMove(
		LocalWorldId,
		ManualPacketA,
		MoveArrive,
		ManualEndpointAPath());
	TestTrue(TEXT("first ARRIVE exposes queued B"),
		FirstTerminal.QueuedEndpoint.IsSet());
	TestFalse(TEXT("first ARRIVE consumes queue before replay"),
		Reducer.GetQueuedEndpoint().IsSet());

	const FCorsairsReducerEffects Duplicate = Reducer.OnMove(
		LocalWorldId,
		ManualPacketA,
		MoveArrive,
		ManualEndpointAPath());
	TestTrue(TEXT("replayed terminal is marked duplicate"),
		Duplicate.bDuplicate);
	TestFalse(TEXT("duplicate is not a protocol error"),
		Duplicate.bProtocolError);
	TestFalse(TEXT("duplicate emits no movement"),
		Duplicate.Movement.IsSet());
	TestFalse(TEXT("duplicate emits no queued endpoint"),
		Duplicate.QueuedEndpoint.IsSet());
	TestEqual(TEXT("duplicate preserves confirmed endpoint"),
		Reducer.GetConfirmedPosition(),
		FIntPoint(223825, 278475));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerStaleTerminalTest,
	"Corsairs.Movement.Reducer.StaleTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerStaleTerminalTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	Reducer.Begin(
		ManualPacketA,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketA,
			ManualStart,
			ManualEndpointA,
		},
		[]()
		{
			return true;
		});
	Reducer.OnMove(
		LocalWorldId,
		ManualPacketA,
		MoveArrive,
		ManualEndpointAPath());
	Reducer.Begin(
		ManualPacketB,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketB,
			ManualEndpointA,
			ManualEndpointB,
		},
		[]()
		{
			return true;
		});

	const FCorsairsReducerEffects Stale = Reducer.OnMove(
		LocalWorldId,
		ManualPacketA,
		MoveArrive,
		ManualEndpointAPath());
	TestTrue(TEXT("old terminal is rejected as stale"),
		Stale.bProtocolError);
	TestFalse(TEXT("reserved packet makes exact replay stale, not duplicate"),
		Stale.bDuplicate);
	TestFalse(TEXT("stale terminal emits no reconciliation"),
		Stale.Movement.IsSet());
	TestFalse(TEXT("stale terminal emits no queued endpoint"),
		Stale.QueuedEndpoint.IsSet());
	TestEqual(TEXT("stale terminal preserves confirmed A"),
		Reducer.GetConfirmedPosition(),
		FIntPoint(223825, 278475));
	TestTrue(TEXT("stale terminal preserves pending B"),
		Reducer.GetPendingMove().IsSet());
	if (Reducer.GetPendingMove().IsSet())
	{
		TestEqual(TEXT("pending B packet is preserved"),
			Reducer.GetPendingMove()->PacketId,
			static_cast<int64>(11));
		TestEqual(TEXT("pending B endpoint is preserved"),
			Reducer.GetPendingMove()->RequestedEndpoint,
			FIntPoint(224325, 278475));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerRejectsMismatchedLocalPacketTest,
	"Corsairs.Movement.Reducer.RejectsMismatchedLocalPacket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerRejectsMismatchedLocalPacketTest::RunTest(
	const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	Reducer.Begin(
		ManualPacketB,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketB,
			ManualStart,
			ManualEndpointB,
		},
		[]()
		{
			return true;
		});

	const FCorsairsReducerEffects Effects = Reducer.OnMove(
		LocalWorldId,
		MismatchedPacket,
		MoveOn,
		ManualEndpointAPath());
	TestTrue(TEXT("mismatched local packet is rejected"),
		Effects.bProtocolError);
	TestFalse(TEXT("mismatched packet emits no reconciliation"),
		Effects.Movement.IsSet());
	TestFalse(TEXT("mismatched packet emits no queued endpoint"),
		Effects.QueuedEndpoint.IsSet());
	TestEqual(TEXT("mismatched packet preserves confirmed start"),
		Reducer.GetConfirmedPosition(),
		FIntPoint(223325, 278475));
	TestTrue(TEXT("mismatched packet preserves pending 11"),
		Reducer.GetPendingMove().IsSet());
	if (Reducer.GetPendingMove().IsSet())
	{
		TestEqual(TEXT("pending packet remains 11"),
			Reducer.GetPendingMove()->PacketId,
			static_cast<int64>(11));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerSkillOpensServerDrivenWithoutPendingTest,
	"Corsairs.Movement.Reducer.SkillOpensServerDrivenWithoutPending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerSkillOpensServerDrivenWithoutPendingTest::RunTest(
	const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	Reducer.Begin(
		SkillPacketId,
		ECorsairsBeginActionType::Skill,
		TOptional<FCorsairsPendingMove>(),
		[]()
		{
			return true;
		});

	const FCorsairsReducerEffects ServerMove = Reducer.OnMove(
		LocalWorldId,
		SkillPacketId,
		MoveOn,
		ManualPathA());
	TestFalse(TEXT("matching skill MOVE is accepted"),
		ServerMove.bProtocolError);
	TestTrue(TEXT("matching skill MOVE emits movement"),
		ServerMove.Movement.IsSet());
	if (ServerMove.Movement.IsSet())
	{
		TestTrue(TEXT("skill MOVE is server driven"),
			ServerMove.Movement->bServerDriven);
	}
	TestTrue(TEXT("skill remains active"),
		Reducer.GetActiveAction().IsSet());
	if (Reducer.GetActiveAction().IsSet())
	{
		TestEqual(TEXT("skill opens server move phase"),
			static_cast<uint8>(Reducer.GetActiveAction()->Phase),
			static_cast<uint8>(ECorsairsActionPhase::ServerMove));
	}

	const FCorsairsReducerEffects OtherPacket = Reducer.OnMove(
		LocalWorldId,
		SkillPacketId + 1,
		MoveBlock,
		ManualEndpointBPath());
	TestTrue(TEXT("other local packet is stale"),
		OtherPacket.bProtocolError);
	TestFalse(TEXT("stale skill packet emits no movement"),
		OtherPacket.Movement.IsSet());
	TestEqual(TEXT("stale skill packet preserves confirmed endpoint"),
		Reducer.GetConfirmedPosition(),
		FIntPoint(223825, 278475));
	TestTrue(TEXT("stale skill packet preserves active skill"),
		Reducer.GetActiveAction().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerSkillFailedMoveTest,
	"Corsairs.Movement.Reducer.SkillFailedMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerSkillFailedMoveTest::RunTest(const FString&)
{
	struct FSkillFailureCase
	{
		bool bOpenServerMove;
		FIntPoint ExpectedEndpoint;
	};
	const FSkillFailureCase Cases[] = {
		{false, FIntPoint(223325, 278475)},
		{true, FIntPoint(223825, 278475)},
	};
	for (const FSkillFailureCase& SkillFailureCase : Cases)
	{
		FCorsairsActionReducer Reducer;
		Reducer.EnterWorld(LocalWorldId, ManualStart);
		Reducer.Begin(
			SkillPacketId,
			ECorsairsBeginActionType::Skill,
			TOptional<FCorsairsPendingMove>(),
			[]()
			{
				return true;
			});
		if (SkillFailureCase.bOpenServerMove)
		{
			Reducer.OnMove(
				LocalWorldId,
				SkillPacketId,
				MoveOn,
				ManualPathA());
		}

		const FCorsairsReducerEffects Effects = Reducer.OnFailedAction(
			LocalWorldId,
			MoveAction,
			FailedActionExisting);
		TestFalse(TEXT("skill MOVE failure is accepted"),
			Effects.bProtocolError);
		TestTrue(TEXT("skill MOVE failure emits reconciliation"),
			Effects.Movement.IsSet());
		if (Effects.Movement.IsSet())
		{
			TestEqual(TEXT("skill failure world"),
				Effects.Movement->WorldId,
				static_cast<int64>(77));
			TestEqual(TEXT("skill failure packet"),
				Effects.Movement->PacketId,
				static_cast<int64>(700));
			TestEqual(TEXT("skill failure type"),
				static_cast<uint8>(Effects.Movement->Type),
				static_cast<uint8>(ECorsairsMovementEventType::Rejected));
			TestEqual(TEXT("skill failure reason"),
				Effects.Movement->MoveState,
				static_cast<uint8>(1));
			TestEqual(TEXT("skill failure waypoint count"),
				Effects.Movement->Waypoints.Num(),
				1);
			if (Effects.Movement->Waypoints.Num() == 1)
			{
				TestEqual(TEXT("skill failure facing endpoint"),
					Effects.Movement->Waypoints[0],
					SkillFailureCase.ExpectedEndpoint);
			}
			TestEqual(TEXT("skill failure confirmed endpoint"),
				Effects.Movement->Endpoint,
				SkillFailureCase.ExpectedEndpoint);
			TestTrue(TEXT("skill failure is local"),
				Effects.Movement->bLocal);
			TestTrue(TEXT("skill failure is server driven"),
				Effects.Movement->bServerDriven);
			TestTrue(TEXT("skill failure requires neutral"),
				Effects.Movement->bRequireNeutral);
		}
		TestFalse(TEXT("skill failure exposes no queued endpoint"),
			Effects.QueuedEndpoint.IsSet());
		TestFalse(TEXT("skill failure clears active action"),
			Reducer.GetActiveAction().IsSet());
		TestFalse(TEXT("skill failure clears pending move"),
			Reducer.GetPendingMove().IsSet());
		TestFalse(TEXT("skill failure clears internal queue"),
			Reducer.GetQueuedEndpoint().IsSet());
		TestEqual(TEXT("skill failure preserves confirmation"),
			Reducer.GetConfirmedPosition(),
			SkillFailureCase.ExpectedEndpoint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerMalformedWaypointsTest,
	"Corsairs.Movement.Reducer.MalformedWaypoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerMalformedWaypointsTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	Reducer.Begin(
		ManualPacketA,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketA,
			ManualStart,
			ManualEndpointA,
		},
		[]()
		{
			return true;
		});
	Reducer.QueueEndpoint(ManualEndpointB);

	const TConstArrayView<uint8> InvalidBlobs[] = {
		TConstArrayView<uint8>(),
		MakeArrayView(SevenWaypointBytes),
		MakeArrayView(NineWaypointBytes),
	};
	for (const TConstArrayView<uint8> InvalidBlob : InvalidBlobs)
	{
		const FCorsairsReducerEffects Effects = Reducer.OnMove(
			LocalWorldId,
			ManualPacketA,
			MoveOn,
			InvalidBlob);
		TestTrue(TEXT("malformed waypoint blob is a protocol error"),
			Effects.bProtocolError);
		TestFalse(TEXT("malformed blob emits no movement"),
			Effects.Movement.IsSet());
		TestFalse(TEXT("malformed blob emits no queued endpoint"),
			Effects.QueuedEndpoint.IsSet());
		TestFalse(TEXT("malformed blob is not a duplicate"),
			Effects.bDuplicate);
		TestEqual(TEXT("malformed blob preserves confirmed position"),
			Reducer.GetConfirmedPosition(),
			FIntPoint(223325, 278475));
		TestTrue(TEXT("malformed blob preserves active action"),
			Reducer.GetActiveAction().IsSet());
		if (Reducer.GetActiveAction().IsSet())
		{
			TestEqual(TEXT("malformed blob preserves active packet"),
				Reducer.GetActiveAction()->PacketId,
				static_cast<int64>(10));
			TestEqual(TEXT("malformed blob preserves requested phase"),
				static_cast<uint8>(Reducer.GetActiveAction()->Phase),
				static_cast<uint8>(ECorsairsActionPhase::Requested));
		}
		TestTrue(TEXT("malformed blob preserves pending move"),
			Reducer.GetPendingMove().IsSet());
		if (Reducer.GetPendingMove().IsSet())
		{
			TestEqual(TEXT("malformed blob preserves pending start"),
				Reducer.GetPendingMove()->Start,
				FIntPoint(223325, 278475));
			TestEqual(TEXT("malformed blob preserves pending endpoint"),
				Reducer.GetPendingMove()->RequestedEndpoint,
				FIntPoint(223825, 278475));
		}
		TestTrue(TEXT("malformed blob preserves queued endpoint"),
			Reducer.GetQueuedEndpoint().IsSet());
		if (Reducer.GetQueuedEndpoint().IsSet())
		{
			TestEqual(TEXT("malformed blob preserves literal queue"),
				*Reducer.GetQueuedEndpoint(),
				FIntPoint(224325, 278475));
		}

		FCorsairsActionReducer CompletedReducer;
		CompletedReducer.EnterWorld(LocalWorldId, ManualStart);
		CompletedReducer.Begin(
			ManualPacketA,
			ECorsairsBeginActionType::Move,
			FCorsairsPendingMove{
				ManualPacketA,
				ManualStart,
				ManualEndpointA,
			},
			[]()
			{
				return true;
			});
		CompletedReducer.OnMove(
			LocalWorldId,
			ManualPacketA,
			MoveArrive,
			ManualEndpointAPath());
		const FCorsairsReducerEffects CompletedMalformed =
			CompletedReducer.OnMove(
				LocalWorldId,
				ManualPacketA,
				MoveArrive,
				InvalidBlob);
		TestTrue(TEXT("completed malformed packet is protocol error"),
			CompletedMalformed.bProtocolError);
		const FCorsairsReducerEffects Replay = CompletedReducer.OnMove(
			LocalWorldId,
			ManualPacketA,
			MoveArrive,
			ManualEndpointAPath());
		TestTrue(TEXT("malformed packet preserves terminal dedup state"),
			Replay.bDuplicate);
		TestFalse(TEXT("preserved duplicate emits no movement"),
			Replay.Movement.IsSet());
	}

	const FCorsairsReducerEffects Valid = Reducer.OnMove(
		LocalWorldId,
		ManualPacketA,
		MoveOn,
		ManualPathA());
	TestFalse(TEXT("nonzero 16-byte waypoint blob is valid"),
		Valid.bProtocolError);
	TestTrue(TEXT("valid waypoint blob emits movement"),
		Valid.Movement.IsSet());
	if (Valid.Movement.IsSet())
	{
		TestEqual(TEXT("valid blob decodes two waypoints"),
			Valid.Movement->Waypoints.Num(),
			2);
		TestEqual(TEXT("valid blob first waypoint"),
			Valid.Movement->Waypoints[0],
			FIntPoint(223575, 278475));
		TestEqual(TEXT("valid blob final waypoint"),
			Valid.Movement->Waypoints[1],
			FIntPoint(223825, 278475));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerFailedMoveTest,
	"Corsairs.Movement.Reducer.FailedMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerFailedMoveTest::RunTest(const FString&)
{
	struct FFailureCase
	{
		int64 Reason;
		uint8 ExpectedMoveState;
	};
	const FFailureCase Cases[] = {
		{FailedActionForbidden, 0},
		{FailedActionExisting, 1},
		{FailedActionMovePath, 2},
	};
	for (const FFailureCase& FailureCase : Cases)
	{
		FCorsairsActionReducer Reducer;
		Reducer.EnterWorld(LocalWorldId, ManualStart);
		int32 SendCount = 0;
		Reducer.Begin(
			ManualPacketA,
			ECorsairsBeginActionType::Move,
			FCorsairsPendingMove{
				ManualPacketA,
				ManualStart,
				ManualEndpointA,
			},
			[&]()
			{
				++SendCount;
				return true;
			});
		Reducer.QueueEndpoint(ManualEndpointB);

		const FCorsairsReducerEffects Effects = Reducer.OnFailedAction(
			LocalWorldId,
			MoveAction,
			FailureCase.Reason);
		TestFalse(TEXT("matching failed MOVE is accepted"),
			Effects.bProtocolError);
		TestTrue(TEXT("failed MOVE emits rejection"),
			Effects.Movement.IsSet());
		if (Effects.Movement.IsSet())
		{
			TestEqual(TEXT("failed MOVE packet"),
				Effects.Movement->PacketId,
				static_cast<int64>(10));
			TestEqual(TEXT("failed MOVE type"),
				static_cast<uint8>(Effects.Movement->Type),
				static_cast<uint8>(ECorsairsMovementEventType::Rejected));
			TestEqual(TEXT("failed MOVE literal reason"),
				Effects.Movement->MoveState,
				FailureCase.ExpectedMoveState);
			TestEqual(TEXT("failed MOVE rolls back to prior confirmation"),
				Effects.Movement->Endpoint,
				FIntPoint(223325, 278475));
			TestTrue(TEXT("failed MOVE is local"),
				Effects.Movement->bLocal);
			TestFalse(TEXT("failed manual MOVE is not server driven"),
				Effects.Movement->bServerDriven);
			TestTrue(TEXT("failed MOVE requires neutral"),
				Effects.Movement->bRequireNeutral);
		}
		TestFalse(TEXT("failed MOVE never exposes queued resend"),
			Effects.QueuedEndpoint.IsSet());
		TestFalse(TEXT("failed MOVE clears active action"),
			Reducer.GetActiveAction().IsSet());
		TestFalse(TEXT("failed MOVE clears pending move"),
			Reducer.GetPendingMove().IsSet());
		TestFalse(TEXT("failed MOVE clears queue"),
			Reducer.GetQueuedEndpoint().IsSet());
		TestEqual(TEXT("failed MOVE preserves prior confirmation"),
			Reducer.GetConfirmedPosition(),
			FIntPoint(223325, 278475));
		TestEqual(TEXT("failed MOVE never resends"), SendCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReducerRemoteMoveTest,
	"Corsairs.Movement.Reducer.RemoteMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReducerRemoteMoveTest::RunTest(const FString&)
{
	FCorsairsActionReducer Reducer;
	Reducer.EnterWorld(LocalWorldId, ManualStart);
	Reducer.Begin(
		ManualPacketA,
		ECorsairsBeginActionType::Move,
		FCorsairsPendingMove{
			ManualPacketA,
			ManualStart,
			ManualEndpointA,
		},
		[]()
		{
			return true;
		});
	Reducer.QueueEndpoint(ManualEndpointB);

	const FCorsairsReducerEffects RemoteOn = Reducer.OnMove(
		RemoteWorldId,
		RemotePacketId,
		MoveOn,
		RemotePath());
	TestFalse(TEXT("remote ON ignores local reservation correlation"),
		RemoteOn.bProtocolError);
	TestTrue(TEXT("remote ON emits movement"),
		RemoteOn.Movement.IsSet());
	if (RemoteOn.Movement.IsSet())
	{
		TestFalse(TEXT("remote ON is not local"),
			RemoteOn.Movement->bLocal);
		TestTrue(TEXT("remote ON is server driven"),
			RemoteOn.Movement->bServerDriven);
		TestEqual(TEXT("remote ON preserves waypoint count"),
			RemoteOn.Movement->Waypoints.Num(),
			2);
		TestEqual(TEXT("remote ON preserves first facing point"),
			RemoteOn.Movement->Waypoints[0],
			FIntPoint(1000, 2000));
		TestEqual(TEXT("remote ON preserves terminal point"),
			RemoteOn.Movement->Waypoints[1],
			FIntPoint(1100, 2100));
	}
	TestEqual(TEXT("remote ON preserves local confirmation"),
		Reducer.GetConfirmedPosition(),
		FIntPoint(223325, 278475));
	TestTrue(TEXT("remote ON preserves local pending"),
		Reducer.GetPendingMove().IsSet());
	TestTrue(TEXT("remote ON preserves local queue"),
		Reducer.GetQueuedEndpoint().IsSet());

	const FCorsairsReducerEffects RemoteTerminal = Reducer.OnMove(
		RemoteWorldId,
		RemotePacketId,
		MoveArrive,
		RemotePath());
	TestFalse(TEXT("remote terminal is accepted"),
		RemoteTerminal.bProtocolError);
	TestTrue(TEXT("remote terminal emits movement"),
		RemoteTerminal.Movement.IsSet());
	if (RemoteTerminal.Movement.IsSet())
	{
		TestEqual(TEXT("remote terminal type"),
			static_cast<uint8>(RemoteTerminal.Movement->Type),
			static_cast<uint8>(ECorsairsMovementEventType::Terminal));
		TestEqual(TEXT("remote terminal endpoint"),
			RemoteTerminal.Movement->Endpoint,
			FIntPoint(1100, 2100));
		TestTrue(TEXT("remote terminal remains server driven"),
			RemoteTerminal.Movement->bServerDriven);
	}

	const FCorsairsReducerEffects Duplicate = Reducer.OnMove(
		RemoteWorldId,
		RemotePacketId,
		MoveArrive,
		RemotePath());
	TestTrue(TEXT("same remote terminal tuple is duplicate"),
		Duplicate.bDuplicate);
	TestFalse(TEXT("remote duplicate emits no movement"),
		Duplicate.Movement.IsSet());

	struct FDedupKeyCase
	{
		int64 WorldId;
		int64 PacketId;
		int64 State;
		TConstArrayView<uint8> Waypoints;
	};
	const FDedupKeyCase KeyCases[] = {
		{RemoteWorldId + 1, RemotePacketId, MoveArrive, RemotePath()},
		{RemoteWorldId, RemotePacketId + 1, MoveArrive, RemotePath()},
		{RemoteWorldId, RemotePacketId, MoveBlock, RemotePath()},
		{RemoteWorldId, RemotePacketId, MoveArrive,
			RemoteOtherEndpointPath()},
	};
	for (const FDedupKeyCase& KeyCase : KeyCases)
	{
		FCorsairsActionReducer KeyReducer;
		KeyReducer.EnterWorld(LocalWorldId, ManualStart);
		KeyReducer.OnMove(
			RemoteWorldId,
			RemotePacketId,
			MoveArrive,
			RemotePath());
		const FCorsairsReducerEffects ChangedKey = KeyReducer.OnMove(
			KeyCase.WorldId,
			KeyCase.PacketId,
			KeyCase.State,
			KeyCase.Waypoints);
		TestFalse(TEXT("changing a dedup tuple field is not duplicate"),
			ChangedKey.bDuplicate);
		TestFalse(TEXT("changed remote tuple remains valid"),
			ChangedKey.bProtocolError);
		TestTrue(TEXT("changed remote tuple emits movement"),
			ChangedKey.Movement.IsSet());
	}
	return true;
}

#endif
