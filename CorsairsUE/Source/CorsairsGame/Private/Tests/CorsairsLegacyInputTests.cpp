#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsPlayerCharacter.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedPlayerInput.h"
#include "GameFramework/PlayerController.h"
#include "Components/InputComponent.h"
#include "HAL/PlatformTime.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"

namespace
{
	UPlayerInput* GetPlayerInput(APlayerController* PlayerController)
	{
		const FObjectProperty* PlayerInputProperty =
			FindFProperty<FObjectProperty>(
				APlayerController::StaticClass(),
				TEXT("PlayerInput"));
		return PlayerInputProperty != nullptr
			? Cast<UPlayerInput>(
				PlayerInputProperty->GetObjectPropertyValue_InContainer(
					PlayerController))
			: nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsLegacyInputPossessedPawnDoesNotMoveTest,
	"Corsairs.Movement.LegacyInput.PossessedPawnDoesNotMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsLegacyInputPossessedPawnDoesNotMoveTest::RunTest(const FString&)
{
	FTestWorldWrapper TestWorld;
	TestTrue(
		TEXT("game world created"),
		TestWorld.CreateTestWorld(EWorldType::Game));
	UWorld* World = TestWorld.GetTestWorld();
	TestNotNull(TEXT("game world created"), World);
	if (World == nullptr)
	{
		TestWorld.ForwardErrorMessages(this);
		return false;
	}

	TestTrue(TEXT("game world begins play"), TestWorld.BeginPlayInTestWorld());
	TestWorld.ForwardErrorMessages(this);
	if (TestWorld.HasFailed())
	{
		return false;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	TestNotNull(TEXT("game instance created"), GameInstance);

	ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine);
	TestNotNull(TEXT("local player created"), LocalPlayer);

	APlayerController* PlayerController =
		World->SpawnActor<APlayerController>();
	TestNotNull(TEXT("player controller spawned"), PlayerController);
	if (GameInstance == nullptr || LocalPlayer == nullptr || PlayerController == nullptr)
	{
		return false;
	}

	PlayerController->SetPlayer(LocalPlayer);
	PlayerController->InitInputSystem();

	UPlayerInput* PlayerInput = GetPlayerInput(PlayerController);
	TestNotNull(TEXT("player input created"), PlayerInput);
	TestTrue(
		TEXT("DefaultInput.ini creates UEnhancedPlayerInput"),
		PlayerInput != nullptr && PlayerInput->IsA<UEnhancedPlayerInput>());
	if (PlayerInput == nullptr)
	{
		return false;
	}

	ACorsairsPlayerCharacter* Pawn =
		World->SpawnActor<ACorsairsPlayerCharacter>();
	TestNotNull(TEXT("player character spawned"), Pawn);
	if (Pawn == nullptr)
	{
		return false;
	}
	PlayerController->Possess(Pawn);
	// У тестового мира нет поверхности; в игре этот режим включает карта высот.
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	TestTrue(
		TEXT("possessed pawn"),
		PlayerController->GetPawn() == Pawn);
	TestTrue(TEXT("controller is local"), PlayerController->IsLocalController());
	TestTrue(TEXT("pawn input is enabled"), Pawn->InputEnabled());
	UCharacterMovementComponent* MovementComponent = Pawn->GetCharacterMovement();
	TestTrue(
		TEXT("character movement component is registered and active"),
		MovementComponent != nullptr && MovementComponent->IsRegistered() &&
			MovementComponent->IsActive());
	TestTrue(
		TEXT("character movement tick is registered"),
		MovementComponent != nullptr &&
			MovementComponent->PrimaryComponentTick.IsTickFunctionRegistered());
	UInputComponent* PawnInputComponent = Pawn->FindComponentByClass<UInputComponent>();
	TestNotNull(
		TEXT("normal pawn input component created"),
		PawnInputComponent);
	const auto HasAxisBinding = [PawnInputComponent](const FName Name)
	{
		return PawnInputComponent != nullptr &&
			PawnInputComponent->AxisBindings.ContainsByPredicate(
				[Name](const FInputAxisBinding& Binding)
				{
					return Binding.AxisName == Name;
				});
	};
	TestFalse(
		TEXT("production pawn does not bind MoveForward"),
		HasAxisBinding(TEXT("MoveForward")));
	TestFalse(
		TEXT("production pawn does not bind MoveRight"),
		HasAxisBinding(TEXT("MoveRight")));
	TestFalse(
		TEXT("production pawn does not bind Turn"),
		HasAxisBinding(TEXT("Turn")));
	TestFalse(
		TEXT("production pawn does not bind LookUp"),
		HasAxisBinding(TEXT("LookUp")));
	TestTrue(TEXT("possessed pawn is locally controlled"), Pawn->IsLocallyControlled());
	TestTrue(
		TEXT("character movement remains in flying mode"),
		MovementComponent != nullptr && MovementComponent->IsFlying());

	const FVector Start = Pawn->GetActorLocation();
	const FRotator StartControlRotation = PlayerController->GetControlRotation();
	constexpr float DeltaSeconds = 1.0f / 60.0f;
	PlayerController->InputKey(FInputKeyEventArgs(
		nullptr,
		INPUTDEVICEID_NONE,
		EKeys::W,
		IE_Pressed,
		FPlatformTime::Cycles64()));
	PlayerController->InputKey(FInputKeyEventArgs(
		nullptr,
		INPUTDEVICEID_NONE,
		EKeys::MouseX,
		1.0f,
		DeltaSeconds,
		1,
		FPlatformTime::Cycles64()));

	for (int32 Tick = 0; Tick < 120; ++Tick)
	{
		PlayerController->PlayerTick(DeltaSeconds);
		TestWorld.TickTestWorld(DeltaSeconds);
	}

	TestTrue(
		TEXT("W creates no pending movement input"),
		Pawn->GetPendingMovementInputVector().IsNearlyZero());
	TestEqual(
		TEXT("WASD does not move the possessed pawn"),
		Pawn->GetActorLocation(),
		Start);
	TestTrue(
		TEXT("free MouseX does not rotate camera"),
		PlayerController->GetControlRotation().Equals(
			StartControlRotation,
			0.01));
	TestTrue(
		TEXT("legacy keys produce no velocity"),
		MovementComponent != nullptr && MovementComponent->Velocity.IsNearlyZero());

	PlayerController->InputKey(FInputKeyEventArgs(
		nullptr,
		INPUTDEVICEID_NONE,
		EKeys::W,
		IE_Released,
		FPlatformTime::Cycles64()));

	TestWorld.ForwardErrorMessages(this);
	return true;
}

#endif
