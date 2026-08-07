#pragma once

#include "CoreMinimal.h"

namespace Corsairs::Game::Camera
{
struct FCameraProfile
{
	double HorizontalOffsetCm;
	double VerticalOffsetCm;
	double VerticalFovDegrees;
	double TargetHeightCm;
	double InitialYawDegrees;
};

struct FCameraRig
{
	double ArmLengthCm;
	double PitchDegrees;
	double HorizontalFovDegrees;
};

CORSAIRSGAME_API FCameraProfile LegacyDefaultProfile();
CORSAIRSGAME_API FCameraRig DeriveRig(
	const FCameraProfile& Profile,
	double ReferenceAspectRatio);
} // namespace Corsairs::Game::Camera
