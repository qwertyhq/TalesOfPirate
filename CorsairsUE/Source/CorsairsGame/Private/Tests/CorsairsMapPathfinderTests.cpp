#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsMapPathfinder.h"

#include "CorsairsCharacterGround.h"
#include "Misc/AutomationTest.h"

namespace
{
TArray<uint8> OpenBlock(const int32 Width, const int32 Height)
{
	TArray<uint8> Bytes;
	Bytes.Init(0, Width * Height * 4);
	return Bytes;
}

TArray<uint8> FlatHeight(const int32 Width, const int32 Height)
{
	TArray<uint8> Bytes;
	Bytes.Init(0, Width * Height * 2);
	return Bytes;
}

TArray<uint8> LandRegion(const int32 Width, const int32 Height)
{
	TArray<uint8> Bytes;
	Bytes.Init(1, Width * Height * 2);
	for (int32 Index = 1; Index < Bytes.Num(); Index += 2)
	{
		Bytes[Index] = 0;
	}
	return Bytes;
}

void BlockCellWithTileWidth(
	TArray<uint8>& Bytes,
	const int32 TileWidth,
	const int32 CellX,
	const int32 CellY)
{
	const int32 TileX = CellX / 2;
	const int32 TileY = CellY / 2;
	const int32 Quadrant = (CellY % 2) * 2 + (CellX % 2);
	Bytes[(TileY * TileWidth + TileX) * 4 + Quadrant] = 0x80;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderSameCellIsNoOpTest,
	"Corsairs.Movement.Pathfinder.SameCellIsNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderSameCellIsNoOpTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TestTrue(TEXT("same-cell fixture loads"), Ground.LoadRuntimeFromBytes(
		2, 2, FlatHeight(2, 2), OpenBlock(2, 2), LandRegion(2, 2), Error));
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground, FIntPoint(1, 1), FIntPoint(49, 49));
	TestEqual(TEXT("same half-cell is not a wire path"), Result.Status,
		ECorsairsPathStatus::Invalid);
	TestEqual(TEXT("same half-cell emits no waypoint"),
		Result.Waypoints.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderCentersAndCompressionTest,
	"Corsairs.Movement.Pathfinder.CentersAndCompression",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderCentersAndCompressionTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TestTrue(TEXT("navigation loads"), Ground.LoadRuntimeFromBytes(
		8, 2, FlatHeight(8, 2), OpenBlock(8, 2), LandRegion(8, 2), Error));
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground, FIntPoint(10, 25), FIntPoint(310, 25));
	TestEqual(TEXT("direct route is found"), Result.Status,
		ECorsairsPathStatus::Found);
	TestEqual(TEXT("path has compressed endpoints"), Result.Waypoints.Num(), 2);
	TestEqual(TEXT("waypoint starts at cell center"), Result.Waypoints[0], FIntPoint(25, 25));
	TestEqual(TEXT("waypoint ends at cell center"), Result.Waypoints[1], FIntPoint(325, 25));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderBlockedTargetAndBudgetTest,
	"Corsairs.Movement.Pathfinder.RetreatAndBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderBlockedTargetAndBudgetTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TArray<uint8> Block = OpenBlock(8, 2);
	Block[2 * 4] = 0x80;
	TestTrue(TEXT("navigation loads"), Ground.LoadRuntimeFromBytes(
		8, 2, FlatHeight(8, 2), Block, LandRegion(8, 2), Error));
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground, FIntPoint(25, 25), FIntPoint(225, 25));
	TestEqual(TEXT("blocked target retreats to free cell"), Result.ResolvedTarget,
		FIntPoint(175, 25));
	TestTrue(TEXT("requested target is preserved"),
		Result.RequestedTarget == FIntPoint(225, 25));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderDeterministicTieTest,
	"Corsairs.Movement.Pathfinder.DeterministicTie",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderDeterministicTieTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TArray<uint8> Block = OpenBlock(3, 2);
	BlockCellWithTileWidth(Block, 3, 2, 1);
	TestTrue(TEXT("tie fixture loads"), Ground.LoadRuntimeFromBytes(
		3, 2, FlatHeight(3, 2), Block, LandRegion(3, 2), Error));
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground, FIntPoint(25, 75), FIntPoint(225, 75));
	TestEqual(TEXT("tie route is found"), Result.Status,
		ECorsairsPathStatus::Found);
	const TArray<FIntPoint> Expected = {
		FIntPoint(25, 75), FIntPoint(75, 75),
		FIntPoint(125, 25), FIntPoint(175, 25),
		FIntPoint(225, 75)};
	TestTrue(TEXT("FIFO order chooses north first"), Result.Waypoints == Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderLegacyDiagonalCornerCutTest,
	"Corsairs.Movement.Pathfinder.LegacyDiagonalCornerCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderLegacyDiagonalCornerCutTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TArray<uint8> Block = OpenBlock(2, 2);
	BlockCellWithTileWidth(Block, 2, 1, 0);
	BlockCellWithTileWidth(Block, 2, 0, 1);
	TestTrue(TEXT("corner-cut fixture loads"), Ground.LoadRuntimeFromBytes(
		2, 2, FlatHeight(2, 2), Block, LandRegion(2, 2), Error));
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground, FIntPoint(25, 25), FIntPoint(75, 75));
	TestEqual(TEXT("legacy BFS keeps diagonal corner cut"), Result.Status,
		ECorsairsPathStatus::Found);
	TestEqual(TEXT("corner cut is one diagonal run"), Result.Waypoints.Num(), 2);
	TestEqual(TEXT("corner cut reaches target center"), Result.Waypoints.Last(),
		FIntPoint(75, 75));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderStraightOnlyNoDetourTest,
	"Corsairs.Movement.Pathfinder.StraightOnlyNoDetour",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderStraightOnlyNoDetourTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TArray<uint8> Block = OpenBlock(3, 2);
	BlockCellWithTileWidth(Block, 3, 2, 1);
	TestTrue(TEXT("straight-only fixture loads"), Ground.LoadRuntimeFromBytes(
		3, 2, FlatHeight(3, 2), Block, LandRegion(3, 2), Error));
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground,
		FIntPoint(25, 75),
		FIntPoint(225, 75),
		ECorsairsPathMode::StraightOnly);
	TestEqual(TEXT("straight-only reports partial"), Result.Status,
		ECorsairsPathStatus::Partial);
	TestTrue(
		TEXT("straight-only keeps only safe prefix"),
		Result.Waypoints ==
		TArray<FIntPoint>({FIntPoint(25, 75), FIntPoint(75, 75)}));
	TestFalse(TEXT("straight-only does not detour to target"),
		Result.Waypoints.Contains(FIntPoint(225, 75)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderNoRouteSafePrefixTest,
	"Corsairs.Movement.Pathfinder.NoRouteSafePrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderNoRouteSafePrefixTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TArray<uint8> Block = OpenBlock(3, 2);
	for (int32 CellY = 0; CellY < 4; ++CellY)
	{
		BlockCellWithTileWidth(Block, 3, 2, CellY);
	}
	TestTrue(TEXT("no-route fixture loads"), Ground.LoadRuntimeFromBytes(
		3, 2, FlatHeight(3, 2), Block, LandRegion(3, 2), Error));
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground, FIntPoint(25, 75), FIntPoint(225, 75));
	TestEqual(TEXT("no route reports partial"), Result.Status,
		ECorsairsPathStatus::Partial);
	TestTrue(
		TEXT("no route returns safe prefix"),
		Result.Waypoints ==
		TArray<FIntPoint>({FIntPoint(25, 75), FIntPoint(75, 75)}));
	TestFalse(TEXT("no route never fakes target endpoint"),
		Result.Waypoints.Contains(FIntPoint(225, 75)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderTruncatedPrefixTest,
	"Corsairs.Movement.Pathfinder.TruncatedPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderTruncatedPrefixTest::RunTest(const FString&)
{
	constexpr int32 TileWidth = 22;
	constexpr int32 TileHeight = 2;
	FCorsairsCharacterGround Ground;
	FString Error;
	TArray<uint8> Block = OpenBlock(TileWidth, TileHeight);
	for (int32 CellX = 2; CellX <= 40; CellX += 2)
	{
		const int32 GapY = (CellX / 2) % 2 == 0 ? 3 : 0;
		for (int32 CellY = 0; CellY < 4; ++CellY)
		{
			if (CellY != GapY)
			{
				BlockCellWithTileWidth(Block, TileWidth, CellX, CellY);
			}
		}
	}
	TestTrue(TEXT("long zigzag fixture loads"), Ground.LoadRuntimeFromBytes(
		TileWidth,
		TileHeight,
		FlatHeight(TileWidth, TileHeight),
		Block,
		LandRegion(TileWidth, TileHeight),
		Error));
	const FIntPoint RequestedTarget(42 * 50 + 25, 75);
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground, FIntPoint(25, 75), RequestedTarget);
	TestEqual(TEXT("long route is found as partial wire prefix"), Result.Status,
		ECorsairsPathStatus::Partial);
	TestTrue(TEXT("wire prefix is marked truncated"), Result.bTruncated);
	TestTrue(TEXT("wire prefix has at most 32 waypoints"),
		Result.Waypoints.Num() <= 32);
	TestEqual(TEXT("requested target survives truncation"),
		Result.RequestedTarget, RequestedTarget);
	TestFalse(TEXT("truncation does not fake endpoint"),
		Result.Waypoints.Contains(RequestedTarget));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMapPathfinderVisitBudgetTest,
	"Corsairs.Movement.Pathfinder.VisitBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMapPathfinderVisitBudgetTest::RunTest(const FString&)
{
	constexpr int32 TileWidth = 130;
	constexpr int32 TileHeight = 130;
	FCorsairsCharacterGround Ground;
	FString Error;
	TArray<uint8> Block = OpenBlock(TileWidth, TileHeight);
	constexpr int32 IsolatedCellX = 200;
	constexpr int32 IsolatedCellY = 200;
	for (int32 CellY = IsolatedCellY - 1; CellY <= IsolatedCellY + 1; ++CellY)
	{
		for (int32 CellX = IsolatedCellX - 1;
			CellX <= IsolatedCellX + 1;
			++CellX)
		{
			if (CellX != IsolatedCellX || CellY != IsolatedCellY)
			{
				BlockCellWithTileWidth(Block, TileWidth, CellX, CellY);
			}
		}
	}
	TestTrue(TEXT("budget fixture loads"), Ground.LoadRuntimeFromBytes(
		TileWidth,
		TileHeight,
		FlatHeight(TileWidth, TileHeight),
		Block,
		LandRegion(TileWidth, TileHeight),
		Error));
	const FCorsairsPathResult Result = FCorsairsMapPathfinder::FindPath(
		Ground,
		FIntPoint(25, 25),
		FIntPoint(IsolatedCellX * 50 + 25, IsolatedCellY * 50 + 25));
	TestEqual(TEXT("visit budget reports partial"), Result.Status,
		ECorsairsPathStatus::Partial);
	TestFalse(TEXT("budget exhaustion does not fake isolated target"),
		Result.Waypoints.Contains(
			FIntPoint(IsolatedCellX * 50 + 25, IsolatedCellY * 50 + 25)));
	return true;
}

#endif
