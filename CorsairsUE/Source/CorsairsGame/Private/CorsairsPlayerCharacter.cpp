#include "CorsairsPlayerCharacter.h"

#include "Camera/CameraComponent.h"
#include "CorsairsLoginHud.h"
#include "CorsairsSession.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
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
	 *  чтобы видеть окружение боя, а не затылок. Мерка взята с оригинального
	 *  клиента: персонаж занимает примерно седьмую часть высоты экрана. */
	constexpr float CameraDistance = 1400.0f;

	/** Наклон камеры. Мир показывается сверху под углом, как в оригинале.
	 *  В Unreal отрицательный тангаж означает взгляд вниз. */
	constexpr float CameraPitch = -45.0f;
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

bool ACorsairsPlayerCharacter::SetBodyAnimation(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return false;
	}

	UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *AssetPath);
	if (Sequence == nullptr)
	{
		UE_LOG(LogCorsairsCharacter, Warning, TEXT("анимация не загрузилась: %s"), *AssetPath);
		return false;
	}

	// Скелеты обязаны быть либо одним ассетом, либо объявленными совместимыми.
	//
	// Interchange заводит отдельный Skeleton на каждый импортируемый файл,
	// поэтому у тела и дорожки они разные, даже когда деревья костей совпадают
	// полностью. Совместимость проставляет link_character_skeletons.py; без
	// неё UE проигрывает дорожку по именам и молча выдаёт искажённую позу
	// вместо отказа.
	USkeletalMesh* Body = GetMesh()->GetSkeletalMeshAsset();
	USkeleton* BodySkeleton = Body != nullptr ? Body->GetSkeleton() : nullptr;
	USkeleton* AnimSkeleton = Sequence->GetSkeleton();

	if (BodySkeleton == nullptr || AnimSkeleton == nullptr)
	{
		UE_LOG(LogCorsairsCharacter, Warning, TEXT("нет скелета для %s"), *AssetPath);
		return false;
	}
	if (BodySkeleton != AnimSkeleton &&
		!AnimSkeleton->IsCompatibleForEditor(BodySkeleton))
	{
		UE_LOG(LogCorsairsCharacter, Warning,
			   TEXT("скелет анимации несовместим со скелетом тела: %s"), *AssetPath);
		return false;
	}

	GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	GetMesh()->PlayAnimation(Sequence, true);
	return true;
}

void ACorsairsPlayerCharacter::AttachSession(UCorsairsSession* InSession)
{
	Session = InSession;
	bHasReported = false;
	TimeSinceReport = 0.0f;
}

void ACorsairsPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Наклон камеры задаётся здесь, а не в режиме игры: там контроллера у
	// персонажа может ещё не быть, и вызов уходит в пустоту. Без наклона
	// камера наследует ориентацию PlayerStart — единственной точки на карте,
	// к игре отношения не имеющей, — и смотрит мимо мира.
	if (AController* OwningController = GetController())
	{
		const FRotator Current = OwningController->GetControlRotation();
		OwningController->SetControlRotation(FRotator(CameraPitch, Current.Yaw, 0.0f));
	}
}

void ACorsairsPlayerCharacter::Tick(float DeltaSeconds)
{
	// Разовый снимок состояния через несколько секунд после старта: по нему
	// видно, где персонаж и куда смотрит камера. Без этих чисел причина
	// «видно только небо» неотличима от десятка других.
	DiagnosticTimer += DeltaSeconds;
	if (!bDiagnosticLogged && DiagnosticTimer > 4.0f)
	{
		bDiagnosticLogged = true;
		const FVector Location = GetActorLocation();
		const FRotator Control = GetController() != nullptr
			? GetController()->GetControlRotation() : FRotator::ZeroRotator;
		const FVector CameraLocation = FollowCamera->GetComponentLocation();
		const FRotator CameraRotation = FollowCamera->GetComponentRotation();
		UE_LOG(LogTemp, Warning,
			   TEXT("ДИАГНОСТИКА: персонаж (%.0f, %.0f, %.0f), контроллер тангаж %.1f, "
					"камера (%.0f, %.0f, %.0f) тангаж %.1f, рука %.0f"),
			   Location.X, Location.Y, Location.Z, Control.Pitch,
			   CameraLocation.X, CameraLocation.Y, CameraLocation.Z,
			   CameraRotation.Pitch, CameraBoom->TargetArmLength);
	}

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

	// Клавиши для экрана входа. Перебираются буквы, цифры и служебные:
	// заводить действие под каждую бессмысленно, а UMG с полями ввода
	// потребовал бы ассета, который из кода не создать.
	TArray<FKey> TypedKeys;
	for (TCHAR Letter = 'A'; Letter <= 'Z'; ++Letter)
	{
		TypedKeys.Add(FKey(*FString::Chr(Letter)));
	}
	for (TCHAR Digit = '0'; Digit <= '9'; ++Digit)
	{
		TypedKeys.Add(FKey(*FString::Chr(Digit)));
	}
	TypedKeys.Append({EKeys::BackSpace, EKeys::Tab, EKeys::Enter});

	for (const FKey& Key : TypedKeys)
	{
		FInputKeyBinding Binding(FInputChord(Key, false, false, false, false), IE_Pressed);
		Binding.bConsumeInput = false;
		Binding.KeyDelegate.GetDelegateForManualSet().BindLambda(
			[this, Key]() { HandleTypedKey(Key); });
		PlayerInputComponent->KeyBindings.Emplace(MoveTemp(Binding));
	}
}

void ACorsairsPlayerCharacter::HandleTypedKey(FKey Key)
{
	ACorsairsLoginHud* Hud = Controller != nullptr
		? Cast<ACorsairsLoginHud>(Cast<APlayerController>(Controller)->GetHUD())
		: nullptr;
	if (Hud == nullptr || !Hud->IsAcceptingInput())
	{
		return;
	}

	if (Key == EKeys::BackSpace)
	{
		Hud->EraseCharacter();
		return;
	}
	if (Key == EKeys::Tab)
	{
		Hud->NextField();
		return;
	}
	if (Key == EKeys::Enter)
	{
		Hud->SubmitLogin();
		return;
	}

	// Имя клавиши совпадает с символом для букв и цифр. Регистр приводится к
	// нижнему: учётные записи в базе записаны строчными.
	const FString Name = Key.GetFName().ToString();
	if (Name.Len() == 1)
	{
		Hud->AppendCharacter(Name.ToLower());
	}
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
