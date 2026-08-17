#pragma once

#include "CoreMinimal.h"

#include <chrono>

enum class ECorsairsMouseTargetKind : uint8
{
	Ground,
	Actor,
};

/** Точная цель мыши без доступа к World или списку акторов. */
struct FCorsairsMouseTarget
{
	ECorsairsMouseTargetKind Kind = ECorsairsMouseTargetKind::Ground;
	FIntPoint GroundPoint = FIntPoint::ZeroValue;
	int64 WorldId = 0;
	int64 Handle = 0;
	/** Отличает разные навыки/действия над одной и той же exact целью. */
	int64 ActionId = 0;

	static FCorsairsMouseTarget Ground(FIntPoint Point, int64 ActionId = 0);
	static FCorsairsMouseTarget Actor(
		int64 WorldId,
		int64 Handle,
		int64 ActionId = 0);

	bool operator==(const FCorsairsMouseTarget&) const = default;
};

struct FCorsairsRightRelease
{
	bool bHadPress = false;
	bool bCancelIntent = false;
	FVector2D RestoreCursor = FVector2D::ZeroVector;
};

/** Чистое состояние жестов мыши; время приходит только от steady_clock. */
class CORSAIRSGAME_API FCorsairsMouseControlState
{
public:
	bool AcceptTarget(
		const FCorsairsMouseTarget& Target,
		std::chrono::steady_clock::time_point Now);

	void BeginLeft(std::chrono::steady_clock::time_point Now);
	void EndLeft();
	bool ConsumeHeldWalkSample(std::chrono::steady_clock::time_point Now);

	void QueueLatestTarget(const FCorsairsMouseTarget& Target);
	TOptional<FCorsairsMouseTarget> TakeLatestTarget();

	void BeginRight(
		FVector2D Cursor,
		std::chrono::steady_clock::time_point Now);
	void AddRightDrag(FVector2D PixelDelta);
	FCorsairsRightRelease EndRight(
		std::chrono::steady_clock::time_point Now);
	bool IsRightHeld() const;
	void Reset();

private:
	TOptional<FCorsairsMouseTarget> _lastAcceptedTarget;
	std::chrono::steady_clock::time_point _lastAcceptedAt{};
	TOptional<FCorsairsMouseTarget> _latestTarget;

	bool _leftHeld = false;
	std::chrono::steady_clock::time_point _leftPressedAt{};
	TOptional<std::chrono::steady_clock::time_point> _lastHeldSampleAt;

	bool _rightHeld = false;
	FVector2D _rightRestoreCursor = FVector2D::ZeroVector;
	std::chrono::steady_clock::time_point _rightPressedAt{};
	FVector2D _rightDragPixels = FVector2D::ZeroVector;
};
