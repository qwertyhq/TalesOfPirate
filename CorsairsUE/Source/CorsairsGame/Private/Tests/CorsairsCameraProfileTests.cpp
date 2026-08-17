#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCameraProfile.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCameraProfileTest,
	"Corsairs.Camera.Profile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCameraProfileTest::RunTest(const FString&)
{
	using namespace Corsairs::Game::Camera;
	const FCameraProfile Profile = LegacyDefaultProfile();
	const FCameraRig Rig = DeriveRig(Profile, 16.0 / 9.0);
	TestEqual(TEXT("zoom zero horizontal"),
		Profile.ZoomZeroHorizontalOffsetCm, 5000.0);
	TestEqual(TEXT("zoom one horizontal"),
		Profile.ZoomOneHorizontalOffsetCm, 3500.0);
	TestEqual(TEXT("zoom zero vertical"),
		Profile.ZoomZeroVerticalOffsetCm, 1000.0);
	TestEqual(TEXT("zoom one vertical"),
		Profile.ZoomOneVerticalOffsetCm, 5000.0);
	TestEqual(TEXT("zoom zero vertical FOV"),
		Profile.ZoomZeroVerticalFovDegrees, 12.0);
	TestEqual(TEXT("zoom one vertical FOV"),
		Profile.ZoomOneVerticalFovDegrees, 32.0);
	TestEqual(TEXT("default zoom"), Profile.DefaultZoom, 1.0);
	TestEqual(TEXT("wheel impulse"), Profile.WheelImpulse, 0.03);
	TestEqual(TEXT("target height"), Profile.TargetHeightCm, 100.0);
	TestEqual(TEXT("initial yaw follows rigid map basis"),
		Profile.InitialYawDegrees, 0.0);
	TestTrue(TEXT("arm"),
		FMath::IsNearlyEqual(Rig.ArmLengthCm, 6103.2778, 0.001));
	TestTrue(TEXT("pitch"),
		FMath::IsNearlyEqual(Rig.PitchDegrees, -55.00798, 0.001));
	TestTrue(TEXT("horizontal FOV"),
		FMath::IsNearlyEqual(
			Rig.HorizontalFovDegrees, 54.0222067, 0.001));

	const FCameraRig ZoomZeroRig = DeriveRig(Profile, 16.0 / 9.0, 0.0);
	TestTrue(TEXT("zoom zero arm"),
		FMath::IsNearlyEqual(
			ZoomZeroRig.ArmLengthCm, 5099.0195, 0.001));
	TestTrue(TEXT("zoom zero pitch"),
		FMath::IsNearlyEqual(
			ZoomZeroRig.PitchDegrees, -11.309932, 0.001));
	TestTrue(TEXT("zoom zero horizontal FOV"),
		FMath::IsNearlyEqual(
			ZoomZeroRig.HorizontalFovDegrees, 21.1675658, 0.001));

	TestTrue(TEXT("wheel up follows legacy negative impulse"),
		FMath::IsNearlyEqual(
			ApplyWheel(Profile, 0.5, 1.0), 0.47));
	TestTrue(TEXT("wheel down follows legacy positive impulse"),
		FMath::IsNearlyEqual(
			ApplyWheel(Profile, 0.5, -1.0), 0.53));
	TestEqual(TEXT("zoom clamps at zero"),
		ApplyWheel(Profile, 0.01, 1.0), 0.0);
	TestEqual(TEXT("zoom clamps at one"),
		ApplyWheel(Profile, 0.99, -1.0), 1.0);
	return true;
}

#endif
