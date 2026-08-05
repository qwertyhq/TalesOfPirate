#include "CorsairsPlayerCharacter.h"

#include "Camera/CameraComponent.h"
#include "CorsairsSession.h"
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

	/** Как часто сообщать серверу о движении и с какого смещения.
	 *
	 *  Отправка каждый кадр забила бы канал сообщениями о сдвиге в сантиметр,
	 *  а редкая — рассинхронизировала бы положение. Полсекунды и метр
	 *  соответствуют шагу, с которым двигался оригинальный клиент. */
	constexpr float ReportInterval = 0.5f;
	constexpr float ReportDistance = 100.0f;

	/** Сколько единиц координат карты приходится на сантиметр UE.
	 *
	 *  Клетка занимает 100 единиц карты и 100 сантиметров UE, поэтому
	 *  масштаб единичный. Ось Y инвертируется, как в размещении объектов:
	 *  без этого мир вышел бы зеркальным. */
	FIntPoint ToMapCoordinates(const FVector& Location)
	{
		return FIntPoint(FMath::RoundToInt(Location.X),
						 FMath::RoundToInt(-Location.Y));
	}

	/** Длина кронштейна камеры. Обзор в оригинале — с заметного отдаления,
	 *  чтобы видеть окружение боя, а не затылок. */
	constexpr float CameraDistance = 600.0f;
}

ACorsairsPlayerCharacter::ACorsairsPlayerCharacter()
{
	// Тик нужен для периодической отправки положения серверу.
	PrimaryActorTick.bCanEverTick = true;

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

void ACorsairsPlayerCharacter::AttachSession(UCorsairsSession* InSession)
{
	Session = InSession;
	bHasReported = false;
	TimeSinceReport = 0.0f;
}

void ACorsairsPlayerCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	TimeSinceReport += DeltaSeconds;
	if (TimeSinceReport >= ReportInterval)
	{
		TimeSinceReport = 0.0f;
		ReportMovement();
	}
}

void ACorsairsPlayerCharacter::ReportMovement()
{
	if (Session == nullptr || Session->GetStage() != ECorsairsLoginStage::InWorld)
	{
		return;
	}

	const FIntPoint Current = ToMapCoordinates(GetActorLocation());

	if (!bHasReported)
	{
		// Первое сообщение отсчитывается от места, куда персонажа поставил
		// сервер, а не от нуля: иначе первый же путь пройдёт через полкарты.
		ReportedPosition = Current;
		bHasReported = true;
		return;
	}

	const FVector2D Delta(Current.X - ReportedPosition.X, Current.Y - ReportedPosition.Y);
	if (Delta.SizeSquared() < ReportDistance * ReportDistance)
	{
		return;
	}

	TArray<FIntPoint> Path;
	Path.Add(ReportedPosition);
	Path.Add(Current);
	if (Session->SendMovePath(Path))
	{
		ReportedPosition = Current;
	}
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
