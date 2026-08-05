#include "CorsairsPlayerCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsCharacter, Log, All);

namespace
{
	/** Размеры капсулы столкновений. Взяты от роста персонажа в оригинале:
	 *  около 1.7 метра при ширине плеч в полметра. */
	constexpr float CapsuleHalfHeight = 88.0f;
	constexpr float CapsuleRadius = 34.0f;

	/** Длина кронштейна камеры. Обзор в оригинале — с заметного отдаления,
	 *  чтобы видеть окружение боя, а не затылок. */
	constexpr float CameraDistance = 600.0f;
}

ACorsairsPlayerCharacter::ACorsairsPlayerCharacter()
{
	PrimaryActorTick.bCanEverTick = false;

	GetCapsuleComponent()->InitCapsuleSize(CapsuleRadius, CapsuleHalfHeight);

	// Поворот тела следует за направлением движения, а не за камерой: так
	// ведёт себя персонаж в оригинале, где камера вращается независимо.
	bUseControllerRotationYaw = false;
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 540.0f, 0.0f);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = CameraDistance;
	CameraBoom->bUsePawnControlRotation = true;
	// Кронштейн подтягивает камеру, когда между ней и персонажем оказывается
	// стена: в плотной городской застройке иначе постоянно виден интерьер.
	CameraBoom->bDoCollisionTest = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// Меш смещён вниз на половину капсулы: начало координат модели — под
	// ногами, а капсулы — в центре.
	GetMesh()->SetRelativeLocation(FVector(0.0f, 0.0f, -CapsuleHalfHeight));
	GetMesh()->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
}

bool ACorsairsPlayerCharacter::SetBodyMesh(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return false;
	}

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
	if (Mesh == nullptr)
	{
		UE_LOG(LogCorsairsCharacter, Warning, TEXT("модель не загрузилась: %s"), *AssetPath);
		return false;
	}

	GetMesh()->SetSkeletalMesh(Mesh);
	return true;
}

void ACorsairsPlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Привязки по осям, а не через EnhancedInput: последний требует ассетов
	// действий, которых в коде не создать, а на этом этапе важно, чтобы
	// персонаж поехал без ручной настройки в редакторе.
	PlayerInputComponent->BindAxis(TEXT("MoveForward"), this,
								   &ACorsairsPlayerCharacter::MoveForward);
	PlayerInputComponent->BindAxis(TEXT("MoveRight"), this,
								   &ACorsairsPlayerCharacter::MoveRight);
	PlayerInputComponent->BindAxis(TEXT("Turn"), this,
								   &ACorsairsPlayerCharacter::TurnCamera);
	PlayerInputComponent->BindAxis(TEXT("LookUp"), this,
								   &ACorsairsPlayerCharacter::PitchCamera);
}

void ACorsairsPlayerCharacter::MoveForward(float Value)
{
	if (Controller == nullptr || FMath::IsNearlyZero(Value))
	{
		return;
	}

	// Направление берётся от поворота управляющего, а не тела: иначе персонаж
	// не сможет бежать вбок относительно камеры.
	const FRotator YawOnly(0.0f, Controller->GetControlRotation().Yaw, 0.0f);
	AddMovementInput(FRotationMatrix(YawOnly).GetUnitAxis(EAxis::X), Value);
}

void ACorsairsPlayerCharacter::MoveRight(float Value)
{
	if (Controller == nullptr || FMath::IsNearlyZero(Value))
	{
		return;
	}

	const FRotator YawOnly(0.0f, Controller->GetControlRotation().Yaw, 0.0f);
	AddMovementInput(FRotationMatrix(YawOnly).GetUnitAxis(EAxis::Y), Value);
}

void ACorsairsPlayerCharacter::TurnCamera(float Value)
{
	AddControllerYawInput(Value);
}

void ACorsairsPlayerCharacter::PitchCamera(float Value)
{
	AddControllerPitchInput(Value);
}
