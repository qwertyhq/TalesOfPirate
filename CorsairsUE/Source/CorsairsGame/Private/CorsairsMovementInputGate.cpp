#include "CorsairsMovementInputGate.h"

void FCorsairsMovementInputGate::SetAuthorityLocked(const bool bLocked)
{
	if (bLocked &&
		(!bAuthorityLocked ||
			!FMath::IsNearlyZero(Forward) ||
			!FMath::IsNearlyZero(Right)))
	{
		bWaitingForNeutral = true;
	}
	bAuthorityLocked = bLocked;
}

void FCorsairsMovementInputGate::RequireNeutral()
{
	bWaitingForNeutral = true;
}

void FCorsairsMovementInputGate::SetForward(const float Value)
{
	Forward = Value;
	if (bWaitingForNeutral &&
		FMath::IsNearlyZero(Forward) && FMath::IsNearlyZero(Right))
	{
		bWaitingForNeutral = false;
	}
}

void FCorsairsMovementInputGate::SetRight(const float Value)
{
	Right = Value;
	if (bWaitingForNeutral &&
		FMath::IsNearlyZero(Forward) && FMath::IsNearlyZero(Right))
	{
		bWaitingForNeutral = false;
	}
}

bool FCorsairsMovementInputGate::AllowsPrediction() const
{
	return !bAuthorityLocked && !bWaitingForNeutral;
}

bool FCorsairsMovementInputGate::IsWaitingForNeutral() const
{
	return bWaitingForNeutral;
}
