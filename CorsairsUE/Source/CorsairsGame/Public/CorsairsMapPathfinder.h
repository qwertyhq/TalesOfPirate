#pragma once

#include "CoreMinimal.h"

#include "CorsairsCharacterGround.h"

enum class ECorsairsPathMode : uint8
{
	Normal,
	StraightOnly,
};

enum class ECorsairsPathStatus : uint8
{
	Found,
	Partial,
	Invalid,
};

struct FCorsairsPathResult
{
	ECorsairsPathStatus Status = ECorsairsPathStatus::Invalid;
	TArray<FIntPoint> Waypoints;
	FIntPoint RequestedTarget = FIntPoint::ZeroValue;
	FIntPoint ResolvedTarget = FIntPoint::ZeroValue;
	bool bTruncated = false;
};

/** Чистый legacy-compatible поиск по половинной клетке source raster. */
class CORSAIRSGAME_API FCorsairsMapPathfinder
{
public:
	static constexpr int32 CellSize = 50;
	static constexpr int32 CellCenterOffset = 25;
	static constexpr int32 MaxVisitedCells = 16384;
	static constexpr int32 MaxWireWaypoints = 32;

	static FCorsairsPathResult FindPath(
		const FCorsairsCharacterGround& Ground,
		FIntPoint Start,
		FIntPoint Target,
		ECorsairsPathMode Mode = ECorsairsPathMode::Normal,
		ECorsairsTraversalKind Traversal = ECorsairsTraversalKind::Land);
	static FCorsairsPathResult FindPath(
		FIntPoint Start,
		FIntPoint Target,
		const FCorsairsCharacterGround& Ground,
		ECorsairsPathMode Mode = ECorsairsPathMode::Normal,
		ECorsairsTraversalKind Traversal = ECorsairsTraversalKind::Land)
	{
		return FindPath(Ground, Start, Target, Mode, Traversal);
	}
};
