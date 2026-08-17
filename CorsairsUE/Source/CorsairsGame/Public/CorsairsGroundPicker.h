#pragma once

#include "CoreMinimal.h"

#include "CorsairsCharacterGround.h"

/** Детерминированный hit результата camera ray по runtime heightfield. */
class CORSAIRSGAME_API FCorsairsGroundPicker
{
public:
	static bool TryPick(
		const FVector& RayOrigin,
		const FVector& RayDirection,
		const FCorsairsCharacterGround& Ground,
		FVector2d& OutSourcePoint,
		double& OutHeightCm);
	static bool TryPick(
		const FCorsairsCharacterGround& Ground,
		const FVector& RayOrigin,
		const FVector& RayDirection,
		FVector2d& OutSourcePoint,
		double& OutHeightCm)
	{
		return TryPick(
			RayOrigin,
			RayDirection,
			Ground,
			OutSourcePoint,
			OutHeightCm);
	}

	static bool TryPick(
		const FVector& RayOrigin,
		const FVector& RayDirection,
		const FCorsairsCharacterGround& Ground,
		FIntPoint& OutSourcePoint);
	static bool TryPick(
		const FCorsairsCharacterGround& Ground,
		const FVector& RayOrigin,
		const FVector& RayDirection,
		FIntPoint& OutSourcePoint)
	{
		return TryPick(RayOrigin, RayDirection, Ground, OutSourcePoint);
	}
};
