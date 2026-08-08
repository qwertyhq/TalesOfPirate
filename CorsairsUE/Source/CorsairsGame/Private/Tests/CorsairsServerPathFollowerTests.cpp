#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsServerPathFollower.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsServerPathFollowerTest,
	"Corsairs.Movement.Follower.ServerWaypoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsServerPathFollowerTest::RunTest(const FString&)
{
	// Mutation: потерять порядок сегментов, не инвертировать map-Y для yaw,
	// округлить остаток пути либо продолжить путь после terminal/rejection.
	FCorsairsServerPathFollower Follower;
	const TArray<FIntPoint> Waypoints = {
		FIntPoint(0, 0),
		FIntPoint(300, 0),
		FIntPoint(300, 400),
	};
	Follower.Accept(Waypoints);
	TestTrue(TEXT("accepted path starts active"), Follower.IsActive());
	TestEqual(TEXT("accepted path starts at first waypoint"),
		Follower.GetPosition(), FIntPoint(0, 0));
	TestTrue(TEXT("+map-X faces UE yaw zero"),
		FMath::IsNearlyEqual(Follower.GetFacingYaw(), 0.0));

	Follower.Advance(200.0);
	TestEqual(TEXT("first advance stays on first segment"),
		Follower.GetPosition(), FIntPoint(200, 0));
	TestTrue(TEXT("first segment keeps +map-X facing"),
		FMath::IsNearlyEqual(Follower.GetFacingYaw(), 0.0));

	Follower.Advance(200.0);
	TestEqual(TEXT("second advance crosses into second segment"),
		Follower.GetPosition(), FIntPoint(300, 100));
	TestTrue(TEXT("+map-Y faces negative UE yaw after map-Y inversion"),
		FMath::IsNearlyEqual(Follower.GetFacingYaw(), -90.0));

	Follower.Advance(300.0);
	TestEqual(TEXT("third advance reaches exact endpoint"),
		Follower.GetPosition(), FIntPoint(300, 400));
	TestFalse(TEXT("endpoint stops playback"), Follower.IsActive());

	const FIntPoint Terminal(17, 29);
	Follower.Reconcile(Terminal);
	TestEqual(TEXT("terminal reconciliation snaps exactly"),
		Follower.GetPosition(), Terminal);
	TestFalse(TEXT("terminal reconciliation is inactive"), Follower.IsActive());

	Follower.Accept(Waypoints);
	Follower.Advance(100.0);
	Follower.Stop();
	const FIntPoint RejectedPosition = Follower.GetPosition();
	Follower.Advance(1000.0);
	TestFalse(TEXT("rejection stop disables playback"), Follower.IsActive());
	TestEqual(TEXT("rejection stop preserves the stopped position"),
		Follower.GetPosition(), RejectedPosition);
	return true;
}

#endif
