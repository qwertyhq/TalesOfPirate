#pragma once

#include "CoreMinimal.h"

class CORSAIRSGAME_API FCorsairsMovementInputGate
{
public:
	void SetAuthorityLocked(bool bLocked);
	void RequireNeutral();
	void SetForward(float Value);
	void SetRight(float Value);
	bool AllowsPrediction() const;
	bool IsWaitingForNeutral() const;

private:
	float Forward = 0.0f;
	float Right = 0.0f;
	bool bAuthorityLocked = false;
	bool bWaitingForNeutral = false;
};
