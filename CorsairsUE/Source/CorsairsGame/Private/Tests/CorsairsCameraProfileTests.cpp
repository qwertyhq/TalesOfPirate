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
	TestEqual(TEXT("horizontal"), Profile.HorizontalOffsetCm, 3500.0);
	TestEqual(TEXT("vertical"), Profile.VerticalOffsetCm, 5000.0);
	TestEqual(TEXT("vertical FOV"), Profile.VerticalFovDegrees, 32.0);
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
	return true;
}

#endif
