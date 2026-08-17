#include "CorsairsMouseControlState.h"

namespace
{
constexpr std::chrono::milliseconds DuplicateTargetWindow{100};
constexpr std::chrono::milliseconds HeldWalkDelay{400};
constexpr std::chrono::milliseconds HeldWalkInterval{500};
constexpr std::chrono::milliseconds ShortRightWindow{200};
constexpr double RightDragThresholdPixels = 4.0;
} // namespace

FCorsairsMouseTarget FCorsairsMouseTarget::Ground(
	const FIntPoint Point,
	const int64 ActionId)
{
	FCorsairsMouseTarget Target;
	Target.Kind = ECorsairsMouseTargetKind::Ground;
	Target.GroundPoint = Point;
	Target.ActionId = ActionId;
	return Target;
}

FCorsairsMouseTarget FCorsairsMouseTarget::Actor(
	const int64 WorldId,
	const int64 Handle,
	const int64 ActionId)
{
	FCorsairsMouseTarget Target;
	Target.Kind = ECorsairsMouseTargetKind::Actor;
	Target.WorldId = WorldId;
	Target.Handle = Handle;
	Target.ActionId = ActionId;
	return Target;
}

bool FCorsairsMouseControlState::AcceptTarget(
	const FCorsairsMouseTarget& Target,
	const std::chrono::steady_clock::time_point Now)
{
	if (_lastAcceptedTarget.IsSet() &&
		_lastAcceptedTarget.GetValue() == Target &&
		Now <= _lastAcceptedAt + DuplicateTargetWindow)
	{
		return false;
	}

	_lastAcceptedTarget = Target;
	_lastAcceptedAt = Now;
	return true;
}

void FCorsairsMouseControlState::BeginLeft(
	const std::chrono::steady_clock::time_point Now)
{
	_leftHeld = true;
	_leftPressedAt = Now;
	_lastHeldSampleAt.Reset();
}

void FCorsairsMouseControlState::EndLeft()
{
	_leftHeld = false;
	_lastHeldSampleAt.Reset();
}

bool FCorsairsMouseControlState::ConsumeHeldWalkSample(
	const std::chrono::steady_clock::time_point Now)
{
	if (!_leftHeld || Now <= _leftPressedAt + HeldWalkDelay)
	{
		return false;
	}
	if (_lastHeldSampleAt.IsSet() &&
		Now < _lastHeldSampleAt.GetValue() + HeldWalkInterval)
	{
		return false;
	}

	_lastHeldSampleAt = Now;
	return true;
}

void FCorsairsMouseControlState::QueueLatestTarget(
	const FCorsairsMouseTarget& Target)
{
	_latestTarget = Target;
}

TOptional<FCorsairsMouseTarget>
FCorsairsMouseControlState::TakeLatestTarget()
{
	TOptional<FCorsairsMouseTarget> Target = MoveTemp(_latestTarget);
	_latestTarget.Reset();
	return Target;
}

void FCorsairsMouseControlState::BeginRight(
	const FVector2D Cursor,
	const std::chrono::steady_clock::time_point Now)
{
	_rightHeld = true;
	_rightRestoreCursor = Cursor;
	_rightPressedAt = Now;
	_rightDragPixels = FVector2D::ZeroVector;
}

void FCorsairsMouseControlState::AddRightDrag(const FVector2D PixelDelta)
{
	if (_rightHeld && FMath::IsFinite(PixelDelta.X) &&
		FMath::IsFinite(PixelDelta.Y))
	{
		_rightDragPixels += PixelDelta;
	}
}

FCorsairsRightRelease FCorsairsMouseControlState::EndRight(
	const std::chrono::steady_clock::time_point Now)
{
	FCorsairsRightRelease Release;
	if (!_rightHeld)
	{
		return Release;
	}

	Release.bHadPress = true;
	Release.RestoreCursor = _rightRestoreCursor;
	const auto Duration = Now - _rightPressedAt;
	Release.bCancelIntent =
		Duration >= std::chrono::steady_clock::duration::zero() &&
		Duration <= ShortRightWindow &&
		_rightDragPixels.Size() < RightDragThresholdPixels;

	_rightHeld = false;
	_rightDragPixels = FVector2D::ZeroVector;
	return Release;
}

bool FCorsairsMouseControlState::IsRightHeld() const
{
	return _rightHeld;
}

void FCorsairsMouseControlState::Reset()
{
	_lastAcceptedTarget.Reset();
	_lastAcceptedAt = {};
	_latestTarget.Reset();
	_leftHeld = false;
	_leftPressedAt = {};
	_lastHeldSampleAt.Reset();
	_rightHeld = false;
	_rightRestoreCursor = FVector2D::ZeroVector;
	_rightPressedAt = {};
	_rightDragPixels = FVector2D::ZeroVector;
}
