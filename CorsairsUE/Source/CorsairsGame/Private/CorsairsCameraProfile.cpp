#include "CorsairsCameraProfile.h"

namespace Corsairs::Game::Camera
{
FCameraProfile LegacyDefaultProfile()
{
	return {
		5000.0,
		3500.0,
		1000.0,
		5000.0,
		12.0,
		32.0,
		100.0,
		0.0,
		1.0,
		0.03,
	};
}

FCameraRig DeriveRig(
	const FCameraProfile& Profile,
	double ReferenceAspectRatio)
{
	return DeriveRig(Profile, ReferenceAspectRatio, Profile.DefaultZoom);
}

FCameraRig DeriveRig(
	const FCameraProfile& Profile,
	double ReferenceAspectRatio,
	double Zoom)
{
	const double ClampedZoom = FMath::Clamp(Zoom, 0.0, 1.0);
	const double HorizontalOffsetCm = FMath::Lerp(
		Profile.ZoomZeroHorizontalOffsetCm,
		Profile.ZoomOneHorizontalOffsetCm,
		ClampedZoom);
	const double VerticalOffsetCm = FMath::Lerp(
		Profile.ZoomZeroVerticalOffsetCm,
		Profile.ZoomOneVerticalOffsetCm,
		ClampedZoom);
	const double VerticalFovDegrees = FMath::Lerp(
		Profile.ZoomZeroVerticalFovDegrees,
		Profile.ZoomOneVerticalFovDegrees,
		ClampedZoom);
	const double VerticalRadians =
		FMath::DegreesToRadians(VerticalFovDegrees);
	return {
		FMath::Sqrt(FMath::Square(HorizontalOffsetCm) +
			FMath::Square(VerticalOffsetCm)),
		-FMath::RadiansToDegrees(FMath::Atan2(
			VerticalOffsetCm,
			HorizontalOffsetCm)),
		FMath::RadiansToDegrees(
			2.0 * FMath::Atan(
				FMath::Tan(VerticalRadians * 0.5) *
				ReferenceAspectRatio))
	};
}

double ApplyWheel(
	const FCameraProfile& Profile,
	const double CurrentZoom,
	const double WheelDelta)
{
	if (!FMath::IsFinite(CurrentZoom) || !FMath::IsFinite(WheelDelta))
	{
		return FMath::Clamp(Profile.DefaultZoom, 0.0, 1.0);
	}
	return FMath::Clamp(
		CurrentZoom - WheelDelta * Profile.WheelImpulse,
		0.0,
		1.0);
}
} // namespace Corsairs::Game::Camera
