#include "CorsairsServerPathFollower.h"

void FCorsairsServerPathFollower::Accept(
	const TConstArrayView<FIntPoint> Waypoints)
{
	Path.Reset();
	Path.Append(Waypoints.GetData(), Waypoints.Num());
	SegmentIndex = 0;
	bActive = false;
	if (Path.IsEmpty())
	{
		return;
	}

	Position = FVector2d(Path[0].X, Path[0].Y);
	StartCurrentSegment();
}

FIntPoint FCorsairsServerPathFollower::Advance(const double DistanceCm)
{
	if (!bActive || !FMath::IsFinite(DistanceCm) || DistanceCm <= 0.0)
	{
		return GetPosition();
	}

	double Remaining = DistanceCm;
	while (bActive)
	{
		const FIntPoint& Endpoint = Path[SegmentIndex + 1];
		const FVector2d Target(Endpoint.X, Endpoint.Y);
		const FVector2d Delta = Target - Position;
		const double SegmentLength = Delta.Length();
		if (SegmentLength <= SMALL_NUMBER)
		{
			Position = Target;
			++SegmentIndex;
			StartCurrentSegment();
			continue;
		}

		UpdateFacing(Target);
		if (Remaining < SegmentLength)
		{
			Position += Delta * (Remaining / SegmentLength);
			break;
		}

		Position = Target;
		Remaining -= SegmentLength;
		++SegmentIndex;
		StartCurrentSegment();
	}

	return GetPosition();
}

void FCorsairsServerPathFollower::Reconcile(const FIntPoint Endpoint)
{
	Stop();
	Position = FVector2d(Endpoint.X, Endpoint.Y);
}

void FCorsairsServerPathFollower::Stop()
{
	Path.Reset();
	SegmentIndex = 0;
	bActive = false;
}

FIntPoint FCorsairsServerPathFollower::GetPosition() const
{
	return FIntPoint(
		FMath::RoundToInt(Position.X),
		FMath::RoundToInt(Position.Y));
}

double FCorsairsServerPathFollower::GetFacingYaw() const
{
	return FacingYaw;
}

bool FCorsairsServerPathFollower::IsActive() const
{
	return bActive;
}

void FCorsairsServerPathFollower::StartCurrentSegment()
{
	while (SegmentIndex + 1 < Path.Num())
	{
		const FIntPoint& Endpoint = Path[SegmentIndex + 1];
		const FVector2d Target(Endpoint.X, Endpoint.Y);
		if ((Target - Position).SizeSquared() > SMALL_NUMBER)
		{
			UpdateFacing(Target);
			bActive = true;
			return;
		}
		Position = Target;
		++SegmentIndex;
	}

	bActive = false;
}

void FCorsairsServerPathFollower::UpdateFacing(const FVector2d& Target)
{
	const FVector2d Delta = Target - Position;
	if (Delta.SizeSquared() <= SMALL_NUMBER)
	{
		return;
	}

	FacingYaw = FRotator::NormalizeAxis(FMath::RadiansToDegrees(
		FMath::Atan2(-Delta.Y, Delta.X)));
}
