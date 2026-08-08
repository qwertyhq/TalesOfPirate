#include "CorsairsServerPathFollower.h"

void FCorsairsServerPathFollower::Accept(
	const TConstArrayView<FIntPoint> Waypoints)
{
	_path.Reset();
	_path.Append(Waypoints.GetData(), Waypoints.Num());
	_segmentIndex = 0;
	_isActive = false;
	if (_path.IsEmpty())
	{
		return;
	}

	_position = FVector2d(_path[0].X, _path[0].Y);
	StartCurrentSegment();
}

FIntPoint FCorsairsServerPathFollower::Advance(const double DistanceCm)
{
	if (!_isActive || !FMath::IsFinite(DistanceCm) || DistanceCm <= 0.0)
	{
		return GetPosition();
	}

	double Remaining = DistanceCm;
	while (_isActive)
	{
		const FIntPoint& Endpoint = _path[_segmentIndex + 1];
		const FVector2d Target(Endpoint.X, Endpoint.Y);
		const FVector2d Delta = Target - _position;
		const double SegmentLength = Delta.Length();
		if (SegmentLength <= SMALL_NUMBER)
		{
			_position = Target;
			++_segmentIndex;
			StartCurrentSegment();
			continue;
		}

		UpdateFacing(Target);
		if (Remaining < SegmentLength)
		{
			_position += Delta * (Remaining / SegmentLength);
			break;
		}

		_position = Target;
		Remaining -= SegmentLength;
		++_segmentIndex;
		StartCurrentSegment();
	}

	return GetPosition();
}

void FCorsairsServerPathFollower::Reconcile(const FIntPoint Endpoint)
{
	Stop();
	_position = FVector2d(Endpoint.X, Endpoint.Y);
}

void FCorsairsServerPathFollower::Stop()
{
	_path.Reset();
	_segmentIndex = 0;
	_isActive = false;
}

FIntPoint FCorsairsServerPathFollower::GetPosition() const
{
	return FIntPoint(
		FMath::RoundToInt(_position.X),
		FMath::RoundToInt(_position.Y));
}

double FCorsairsServerPathFollower::GetFacingYaw() const
{
	return _facingYaw;
}

bool FCorsairsServerPathFollower::IsActive() const
{
	return _isActive;
}

void FCorsairsServerPathFollower::StartCurrentSegment()
{
	while (_segmentIndex + 1 < _path.Num())
	{
		const FIntPoint& Endpoint = _path[_segmentIndex + 1];
		const FVector2d Target(Endpoint.X, Endpoint.Y);
		if ((Target - _position).SizeSquared() > SMALL_NUMBER)
		{
			UpdateFacing(Target);
			_isActive = true;
			return;
		}
		_position = Target;
		++_segmentIndex;
	}

	_isActive = false;
}

void FCorsairsServerPathFollower::UpdateFacing(const FVector2d& Target)
{
	const FVector2d Delta = Target - _position;
	if (Delta.SizeSquared() <= SMALL_NUMBER)
	{
		return;
	}

	_facingYaw = FRotator::NormalizeAxis(FMath::RadiansToDegrees(
		FMath::Atan2(-Delta.Y, Delta.X)));
}
