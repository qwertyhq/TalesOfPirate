#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCameraProfile.h"
#include "CorsairsPlayerCharacter.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCameraLifecycleAfterPossessionTest,
	"Corsairs.Camera.LocalPawnAppliesProfileAfterBeginPlayThenPossess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCameraLifecycleAfterPossessionTest::RunTest(const FString&)
{
	FTestWorldWrapper TestWorld;
	TestTrue(
		TEXT("camera lifecycle world created"),
		TestWorld.CreateTestWorld(EWorldType::Game));
	UWorld* World = TestWorld.GetTestWorld();
	TestNotNull(TEXT("camera lifecycle world exists"), World);
	if (World == nullptr || !TestWorld.BeginPlayInTestWorld())
	{
		TestWorld.ForwardErrorMessages(this);
		return false;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	TestNotNull(TEXT("camera lifecycle game instance exists"), GameInstance);
	if (GameInstance == nullptr)
	{
		return false;
	}

	ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine);
	TestNotNull(TEXT("camera lifecycle local player exists"), LocalPlayer);
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	TestNotNull(TEXT("camera lifecycle controller exists"), Controller);
	if (LocalPlayer == nullptr || Controller == nullptr)
	{
		return false;
	}
	Controller->SetPlayer(LocalPlayer);
	Controller->InitInputSystem();

	ACorsairsPlayerCharacter* Pawn =
		World->SpawnActor<ACorsairsPlayerCharacter>();
	TestNotNull(TEXT("pawn spawned after world BeginPlay"), Pawn);
	if (Pawn == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("pawn BeginPlay ran before possession"), Pawn->HasActorBegunPlay());
	TestTrue(TEXT("pawn is unpossessed before lifecycle check"), Pawn->GetController() == nullptr);

	Controller->Possess(Pawn);
	TestTrue(TEXT("pawn is locally possessed"), Pawn->IsLocallyControlled());

	const Corsairs::Game::Camera::FCameraProfile Profile =
		Corsairs::Game::Camera::LegacyDefaultProfile();
	const Corsairs::Game::Camera::FCameraRig Rig =
		Corsairs::Game::Camera::DeriveRig(Profile, 16.0 / 9.0);
	const FRotator ControlRotation = Controller->GetControlRotation();
	TestTrue(
		TEXT("local possession applies camera pitch"),
		FMath::IsNearlyEqual(
			ControlRotation.Pitch,
			static_cast<float>(Rig.PitchDegrees),
			0.01f));
	TestTrue(
		TEXT("local possession applies camera yaw"),
		FMath::IsNearlyEqual(
			ControlRotation.Yaw,
			static_cast<float>(Profile.InitialYawDegrees),
			0.01f));
	TestNotNull(
		TEXT("local possession has camera manager"),
		Controller->PlayerCameraManager.Get());
	if (Controller->PlayerCameraManager != nullptr)
	{
		TestTrue(
			TEXT("camera manager lower pitch clamp follows profile"),
			FMath::IsNearlyEqual(
				Controller->PlayerCameraManager->ViewPitchMin,
				static_cast<float>(Rig.PitchDegrees),
				0.01f));
		TestTrue(
			TEXT("camera manager upper pitch clamp follows profile"),
			FMath::IsNearlyEqual(
				Controller->PlayerCameraManager->ViewPitchMax,
				static_cast<float>(Rig.PitchDegrees),
				0.01f));
	}

	const FRotator UserRotation(-20.0, 37.0, 0.0);
	Controller->SetControlRotation(UserRotation);
	Controller->ClientRestart(Pawn);
	const FRotator RotationAfterRestart = Controller->GetControlRotation();
	TestTrue(
		TEXT("repeated client restart preserves user camera pitch"),
		FMath::IsNearlyEqual(
			RotationAfterRestart.Pitch,
			UserRotation.Pitch,
			0.01f));
	TestTrue(
		TEXT("repeated client restart preserves user camera yaw"),
		FMath::IsNearlyEqual(
			RotationAfterRestart.Yaw,
			UserRotation.Yaw,
			0.01f));

	Pawn->ApplyCameraZoom(0.0);
	USpringArmComponent* Boom =
		Pawn->FindComponentByClass<USpringArmComponent>();
	UCameraComponent* Camera = Pawn->FindComponentByClass<UCameraComponent>();
	TestNotNull(TEXT("camera boom exists"), Boom);
	TestNotNull(TEXT("follow camera exists"), Camera);
	if (Boom != nullptr && Camera != nullptr)
	{
		const Corsairs::Game::Camera::FCameraRig ZoomZeroRig =
			Corsairs::Game::Camera::DeriveRig(Profile, 16.0 / 9.0, 0.0);
		TestTrue(TEXT("zoom updates spring arm"), FMath::IsNearlyEqual(
			Boom->TargetArmLength,
			static_cast<float>(ZoomZeroRig.ArmLengthCm),
			0.01f));
		TestTrue(TEXT("zoom updates camera FOV"), FMath::IsNearlyEqual(
			Camera->FieldOfView,
			static_cast<float>(ZoomZeroRig.HorizontalFovDegrees),
			0.01f));
	}
	TestTrue(TEXT("zoom preserves user yaw"), FMath::IsNearlyEqual(
		Controller->GetControlRotation().Yaw,
		UserRotation.Yaw,
		0.01f));
	TestTrue(TEXT("zoom applies derived fixed pitch"), FMath::IsNearlyEqual(
		Controller->GetControlRotation().Pitch,
		static_cast<float>(Corsairs::Game::Camera::DeriveRig(
			Profile, 16.0 / 9.0, 0.0).PitchDegrees),
		0.01f));

	TestWorld.ForwardErrorMessages(this);
	return true;
}

#endif
