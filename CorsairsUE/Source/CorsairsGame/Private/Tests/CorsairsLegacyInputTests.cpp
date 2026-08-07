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
	FCorsairsLegacyInputPossessedPawnMovesTest,
	"Corsairs.Movement.LegacyInput.PossessedPawnMoves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsLegacyInputPossessedPawnMovesTest::RunTest(const FString&)
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
	TestTrue(
		TEXT("normal pawn input binds MoveForward"),
		PawnInputComponent != nullptr && PawnInputComponent->AxisBindings.ContainsByPredicate(
			[](const FInputAxisBinding& Binding)
			{
				return Binding.AxisName == TEXT("MoveForward");
			}));
	TestTrue(TEXT("possessed pawn is locally controlled"), Pawn->IsLocallyControlled());
	TestTrue(
		TEXT("character movement remains in flying mode"),
		MovementComponent != nullptr && MovementComponent->IsFlying());

	const FVector Start = Pawn->GetActorLocation();
	PlayerController->InputKey(FInputKeyEventArgs(
		nullptr,
		INPUTDEVICEID_NONE,
		EKeys::W,
		IE_Pressed,
		FPlatformTime::Cycles64()));

	constexpr float DeltaSeconds = 1.0f / 60.0f;
	PlayerController->PlayerTick(DeltaSeconds);
	TestEqual(TEXT("W remains pressed"), PlayerInput->GetKeyValue(EKeys::W), 1.0f);
	TestTrue(
		TEXT("MoveForward binding adds pending movement input"),
		Pawn->GetPendingMovementInputVector().SizeSquared() > 0.0);
	TestWorld.TickTestWorld(DeltaSeconds);
	TestTrue(
		TEXT("character movement consumes the MoveForward input"),
		Pawn->GetLastMovementInputVector().SizeSquared() > 0.0);
	TestTrue(
		TEXT("character movement changes the updated actor location"),
		!Pawn->GetActorLocation().Equals(Start));
	TestTrue(
		TEXT("character movement produces velocity"),
		MovementComponent != nullptr && MovementComponent->Velocity.SizeSquared() > 0.0);

	for (int32 Tick = 1; Tick < 120; ++Tick)
	{
		PlayerController->PlayerTick(DeltaSeconds);
		TestWorld.TickTestWorld(DeltaSeconds);
	}

	PlayerController->InputKey(FInputKeyEventArgs(
		nullptr,
		INPUTDEVICEID_NONE,
		EKeys::W,
		IE_Released,
		FPlatformTime::Cycles64()));

	const FVector End = Pawn->GetActorLocation();
	TestTrue(
		TEXT("W moves possessed pawn through legacy MoveForward mapping"),
		FVector::Dist2D(Start, End) > 100.0);

	TestWorld.ForwardErrorMessages(this);
	return true;
}

#endif
