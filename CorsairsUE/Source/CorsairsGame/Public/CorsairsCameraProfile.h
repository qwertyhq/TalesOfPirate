#pragma once

#include "CoreMinimal.h"

namespace Corsairs::Game::Camera
{
struct FCameraProfile
{
	double ZoomZeroHorizontalOffsetCm;
	double ZoomOneHorizontalOffsetCm;
	double ZoomZeroVerticalOffsetCm;
	double ZoomOneVerticalOffsetCm;
	double ZoomZeroVerticalFovDegrees;
	double ZoomOneVerticalFovDegrees;
	double TargetHeightCm;
	double InitialYawDegrees;
	double DefaultZoom;
	double WheelImpulse;
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
CORSAIRSGAME_API FCameraRig DeriveRig(
	const FCameraProfile& Profile,
	double ReferenceAspectRatio,
	double Zoom);
CORSAIRSGAME_API double ApplyWheel(
	const FCameraProfile& Profile,
	double CurrentZoom,
	double WheelDelta);
} // namespace Corsairs::Game::Camera
