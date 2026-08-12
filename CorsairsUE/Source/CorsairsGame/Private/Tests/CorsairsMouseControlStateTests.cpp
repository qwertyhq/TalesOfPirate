#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsMouseControlState.h"

#include "Misc/AutomationTest.h"

namespace
{
using FClock = std::chrono::steady_clock;

FClock::time_point At(const int64 Milliseconds)
{
	return FClock::time_point(std::chrono::milliseconds(Milliseconds));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMouseDuplicateTargetTest,
	"Corsairs.Input.MouseState.DuplicateTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMouseDuplicateTargetTest::RunTest(const FString&)
{
	FCorsairsMouseControlState State;
	const FCorsairsMouseTarget First =
		FCorsairsMouseTarget::Ground(FIntPoint(100, 200));
	const FCorsairsMouseTarget Second =
		FCorsairsMouseTarget::Actor(77, 88);

	TestTrue(TEXT("first exact target is accepted"),
		State.AcceptTarget(First, At(0)));
	TestFalse(TEXT("same target at 100 ms is suppressed"),
		State.AcceptTarget(First, At(100)));
	TestTrue(TEXT("different target is accepted immediately"),
		State.AcceptTarget(Second, At(100)));
	TestTrue(TEXT("different skill on same actor is not suppressed"),
		State.AcceptTarget(FCorsairsMouseTarget::Actor(77, 88, 26), At(100)));
	TestTrue(TEXT("another skill on same actor is accepted immediately"),
		State.AcceptTarget(FCorsairsMouseTarget::Actor(77, 88, 27), At(100)));
	const FCorsairsMouseTarget Skilled =
		FCorsairsMouseTarget::Actor(77, 88, 27);
	TestFalse(TEXT("new same action at 100 ms is suppressed"),
		State.AcceptTarget(Skilled, At(200)));
	TestTrue(TEXT("same action after 100 ms is accepted"),
		State.AcceptTarget(Skilled, At(201)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMouseHeldWalkTimingTest,
	"Corsairs.Input.MouseState.HeldWalkTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMouseHeldWalkTimingTest::RunTest(const FString&)
{
	FCorsairsMouseControlState State;
	State.BeginLeft(At(0));

	TestFalse(TEXT("400 ms is still an ordinary click"),
		State.ConsumeHeldWalkSample(At(400)));
	TestTrue(TEXT("first sample is produced after 400 ms"),
		State.ConsumeHeldWalkSample(At(401)));
	TestFalse(TEXT("hold sampling is throttled below 500 ms"),
		State.ConsumeHeldWalkSample(At(900)));
	TestTrue(TEXT("hold sampling is allowed at 500 ms"),
		State.ConsumeHeldWalkSample(At(901)));

	State.EndLeft();
	TestFalse(TEXT("release stops hold immediately"),
		State.ConsumeHeldWalkSample(At(2000)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMouseLatestTargetWinsTest,
	"Corsairs.Input.MouseState.LatestTargetWins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMouseLatestTargetWinsTest::RunTest(const FString&)
{
	FCorsairsMouseControlState State;
	State.QueueLatestTarget(
		FCorsairsMouseTarget::Ground(FIntPoint(100, 200)));
	State.QueueLatestTarget(
		FCorsairsMouseTarget::Actor(77, 88));

	const TOptional<FCorsairsMouseTarget> Latest = State.TakeLatestTarget();
	TestTrue(TEXT("latest target exists"), Latest.IsSet());
	if (Latest.IsSet())
	{
		TestEqual(TEXT("latest actor world id"), Latest->WorldId, int64(77));
		TestEqual(TEXT("latest actor handle"), Latest->Handle, int64(88));
	}
	TestFalse(TEXT("take consumes pending target"),
		State.TakeLatestTarget().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMouseRightGestureTest,
	"Corsairs.Input.MouseState.RightGesture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMouseRightGestureTest::RunTest(const FString&)
{
	FCorsairsMouseControlState State;
	const FVector2D Cursor(123.5, 321.25);

	State.BeginRight(Cursor, At(0));
	State.AddRightDrag(FVector2D(2.0, 3.0));
	const FCorsairsRightRelease ShortRelease = State.EndRight(At(200));
	TestTrue(TEXT("right press is reported"), ShortRelease.bHadPress);
	TestTrue(TEXT("short gesture below threshold cancels intent"),
		ShortRelease.bCancelIntent);
	TestTrue(TEXT("cursor position is restored exactly"),
		ShortRelease.RestoreCursor.Equals(Cursor, UE_DOUBLE_SMALL_NUMBER));

	State.BeginRight(Cursor, At(300));
	State.AddRightDrag(FVector2D(2.4, 3.2));
	TestFalse(TEXT("drag at threshold does not cancel"),
		State.EndRight(At(400)).bCancelIntent);

	State.BeginRight(Cursor, At(500));
	TestFalse(TEXT("gesture longer than 200 ms does not cancel"),
		State.EndRight(At(701)).bCancelIntent);
	TestFalse(TEXT("release without press has no side effect"),
		State.EndRight(At(702)).bHadPress);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMouseStateResetTest,
	"Corsairs.Input.MouseState.ResetBetweenSessions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMouseStateResetTest::RunTest(const FString&)
{
	FCorsairsMouseControlState State;
	const FCorsairsMouseTarget Target =
		FCorsairsMouseTarget::Ground(FIntPoint(100, 200));
	State.AcceptTarget(Target, At(0));
	State.QueueLatestTarget(Target);
	State.BeginLeft(At(0));
	State.BeginRight(FVector2D(40.0, 50.0), At(0));

	State.Reset();

	TestTrue(TEXT("reset clears duplicate history"),
		State.AcceptTarget(Target, At(1)));
	TestFalse(TEXT("reset clears latest target"),
		State.TakeLatestTarget().IsSet());
	TestFalse(TEXT("reset stops held walk"),
		State.ConsumeHeldWalkSample(At(1000)));
	TestFalse(TEXT("reset ends RMB gesture"), State.IsRightHeld());
	TestFalse(TEXT("reset release has no stale cursor"),
		State.EndRight(At(1000)).bHadPress);
	return true;
}

#endif
