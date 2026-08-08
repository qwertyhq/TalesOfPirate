#pragma once

#include "CoreMinimal.h"

class CORSAIRSGAME_API FCorsairsServerPathFollower
{
public:
	void Accept(TConstArrayView<FIntPoint> Waypoints);
	FIntPoint Advance(double DistanceCm);
	void Reconcile(FIntPoint Endpoint);
	void Stop();
	FIntPoint GetPosition() const;
	double GetFacingYaw() const;
	bool IsActive() const;

private:
	void StartCurrentSegment();
	void UpdateFacing(const FVector2d& Target);

	TArray<FIntPoint> Path;
	int32 SegmentIndex = 0;
	FVector2d Position = FVector2d::ZeroVector;
	double FacingYaw = 0.0;
	bool bActive = false;
};
