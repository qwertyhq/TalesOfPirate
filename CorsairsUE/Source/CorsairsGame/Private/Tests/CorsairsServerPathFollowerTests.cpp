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

	// Mutation: округлять _position после каждого Advance. Накопление
	// субсантиметровое расстояние тогда потеряет carry между вызовами.
	FCorsairsServerPathFollower FractionalFollower;
	FractionalFollower.Accept({FIntPoint(0, 0), FIntPoint(3, 0)});
	for (int32 Index = 0; Index < 5; ++Index)
	{
		FractionalFollower.Advance(0.2);
	}
	TestEqual(TEXT("five sub-centimeter advances retain accumulated carry"),
		FractionalFollower.GetPosition(), FIntPoint(1, 0));
	TestTrue(TEXT("fractional carry keeps path active"),
		FractionalFollower.IsActive());

	// Mutation: хранить internal position как FIntPoint. После 0.6 + 0.6
	// cm надо израсходовать 0.4 cm до corner и оставить 0.2 cm на +map-Y.
	FCorsairsServerPathFollower FractionalCornerFollower;
	FractionalCornerFollower.Accept({
		FIntPoint(0, 0),
		FIntPoint(1, 0),
		FIntPoint(1, 2),
	});
	FractionalCornerFollower.Advance(0.6);
	FractionalCornerFollower.Advance(0.6);
	TestEqual(TEXT("fractional corner retains the post-corner remainder"),
		FractionalCornerFollower.GetPosition(), FIntPoint(1, 0));
	TestTrue(TEXT("fractional corner turns toward map-Y"),
		FMath::IsNearlyEqual(
			FractionalCornerFollower.GetFacingYaw(),
			-90.0));
	FractionalCornerFollower.Advance(0.8);
	TestEqual(TEXT("fractional corner carry continues on second segment"),
		FractionalCornerFollower.GetPosition(), FIntPoint(1, 1));

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
