#include "CorsairsCameraProfile.h"

namespace Corsairs::Game::Camera
{
FCameraProfile LegacyDefaultProfile()
{
	return {3500.0, 5000.0, 32.0, 100.0, 90.0};
}

FCameraRig DeriveRig(
	const FCameraProfile& Profile,
	double ReferenceAspectRatio)
{
	const double VerticalRadians =
		FMath::DegreesToRadians(Profile.VerticalFovDegrees);
	return {
		FMath::Sqrt(FMath::Square(Profile.HorizontalOffsetCm) +
			FMath::Square(Profile.VerticalOffsetCm)),
		-FMath::RadiansToDegrees(FMath::Atan2(
			Profile.VerticalOffsetCm,
			Profile.HorizontalOffsetCm)),
		FMath::RadiansToDegrees(
			2.0 * FMath::Atan(
				FMath::Tan(VerticalRadians * 0.5) *
				ReferenceAspectRatio))
	};
}
} // namespace Corsairs::Game::Camera
