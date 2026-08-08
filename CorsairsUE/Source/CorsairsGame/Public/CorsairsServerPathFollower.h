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

	TArray<FIntPoint> _path;
	int32 _segmentIndex = 0;
	FVector2d _position = FVector2d::ZeroVector;
	double _facingYaw = 0.0;
	bool _isActive = false;
};
