#include "CorsairsPlayerCharacter.h"

#include "Camera/CameraComponent.h"
#include "CorsairsCameraProfile.h"
#include "CorsairsLoginHud.h"
#include "CorsairsSession.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "TerrainHeights.h"

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

	const auto Profile = Corsairs::Game::Camera::LegacyDefaultProfile();
	const auto Rig = Corsairs::Game::Camera::DeriveRig(Profile, 16.0 / 9.0);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = Rig.ArmLengthCm;
	CameraBoom->bUsePawnControlRotation = true;
	// Подтягивание отключено намеренно. В плотной застройке кронштейн упирался
	// в каждый второй дом и подтаскивал камеру вплотную к персонажу — вид
	// падал к самой траве. Оригинал камеру не подтягивает вовсе.
	CameraBoom->bDoCollisionTest = false;
	CameraBoom->SetRelativeLocation(FVector(
		0.0, 0.0, Profile.TargetHeightCm - 88.0));

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	FollowCamera->FieldOfView = Rig.HorizontalFovDegrees;
	FollowCamera->AspectRatio = 16.0f / 9.0f;
	FollowCamera->bOverrideAspectRatioAxisConstraint = true;
	FollowCamera->AspectRatioAxisConstraint =
		EAspectRatioAxisConstraint::AspectRatio_MaintainYFOV;
}

void ACorsairsPlayerCharacter::AttachSession(UCorsairsSession* InSession)
{
	if (Session != nullptr)
	{
		Session->OnMovementChanged.RemoveDynamic(
			this,
			&ACorsairsPlayerCharacter::HandleMovementChanged);
	}

	Session = InSession;
	TimeSinceReport = 0.0f;
	bHasValidMovementSpeed = false;
	bMovementSpeedProtocolErrorReported = false;
	if (Session != nullptr)
	{
		Session->OnMovementChanged.AddDynamic(
			this,
			&ACorsairsPlayerCharacter::HandleMovementChanged);
	}
	UpdateMovementPredictionState();
}

void ACorsairsPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	const auto Profile = Corsairs::Game::Camera::LegacyDefaultProfile();
	const auto Rig = Corsairs::Game::Camera::DeriveRig(Profile, 16.0 / 9.0);

	if (AController* OwningController = GetController())
	{
		OwningController->SetControlRotation(FRotator(
			Rig.PitchDegrees,
			Profile.InitialYawDegrees,
			0.0));

		if (APlayerController* PlayerController = Cast<APlayerController>(OwningController))
		{
			if (PlayerController->PlayerCameraManager != nullptr)
			{
				PlayerController->PlayerCameraManager->ViewPitchMin = Rig.PitchDegrees;
				PlayerController->PlayerCameraManager->ViewPitchMax = Rig.PitchDegrees;
			}
		}
	}
}

void ACorsairsPlayerCharacter::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (Session != nullptr)
	{
		Session->OnMovementChanged.RemoveDynamic(
			this,
			&ACorsairsPlayerCharacter::HandleMovementChanged);
		Session = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

bool ACorsairsPlayerCharacter::UseTerrainHeights(const FString& MapName)
{
	if (TerrainHeights == nullptr)
	{
		TerrainHeights = NewObject<UCorsairsTerrainHeights>(this);
	}
	if (!TerrainHeights->Load(MapName))
	{
		TerrainHeights = nullptr;
		return false;
	}

	// Гравитация выключается: высоту задаёт карта, а не падение. С включённой
	// персонаж проваливался сквозь землю — у рельефа нет физической формы.
	GetCharacterMovement()->GravityScale = 0.0f;
	GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	return true;
}

void ACorsairsPlayerCharacter::Tick(float DeltaSeconds)
{
	// Удержание на земле — каждый кадр: персонаж ходит, и высота под ним
	// меняется. Половина капсулы добавляется, потому что её начало отсчёта в
	// центре, а стоять надо подошвами.
	//
	// Перемещение только при заметном расхождении: телепорт каждый кадр даёт
	// дрожание и смазывание картинки в движении, даже когда высота уже верна.
	if (TerrainHeights != nullptr && TerrainHeights->IsLoaded())
	{
		// Без ввода персонаж обязан стоять. В режиме полёта, куда его
		// переводит удержание на земле, остаточная скорость не гасится ничем,
		// и он медленно уплывает — в застройку, за границу карты, куда
		// угодно.
		if (GetCharacterMovement()->GetCurrentAcceleration().IsNearlyZero())
		{
			GetCharacterMovement()->Velocity = FVector::ZeroVector;
		}

		FVector Location = GetActorLocation();
		const double Wanted = TerrainHeights->HeightAt(Location.X, Location.Y)
			+ GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (FMath::Abs(Location.Z - Wanted) > 1.0)
		{
			Location.Z = Wanted;
			SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		}
	}

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
		const FVector MeshLocation = GetMesh()->GetComponentLocation();
		const FVector MeshScale = GetMesh()->GetComponentScale();
		const bool bMeshVisible = GetMesh()->IsVisible()
			&& GetMesh()->GetSkeletalMeshAsset() != nullptr;
		const FBoxSphereBounds MeshBounds = GetMesh()->Bounds;

		UE_LOG(LogTemp, Warning,
			   TEXT("ДИАГНОСТИКА: персонаж (%.0f, %.0f, %.0f), контроллер тангаж %.1f, "
					"камера (%.0f, %.0f, %.0f) тангаж %.1f, рука %.0f; "
					"меш виден %s, в (%.0f, %.0f, %.0f), масштаб %.2f, "
					"полуразмер (%.0f, %.0f, %.0f)"),
			   Location.X, Location.Y, Location.Z, Control.Pitch,
			   CameraLocation.X, CameraLocation.Y, CameraLocation.Z,
			   CameraRotation.Pitch, CameraBoom->TargetArmLength,
			   bMeshVisible ? TEXT("да") : TEXT("НЕТ"),
			   MeshLocation.X, MeshLocation.Y, MeshLocation.Z, MeshScale.X,
			   MeshBounds.BoxExtent.X, MeshBounds.BoxExtent.Y, MeshBounds.BoxExtent.Z);
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
	Session->SubmitPredictedPosition(Current);
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
	MovementInputGate.SetForward(Value);
	UpdateMovementPredictionState();
	if (Session != nullptr && !bHasValidMovementSpeed &&
		!FMath::IsNearlyZero(Value))
	{
		ReportMovementSpeedProtocolError();
	}
	if (Controller == nullptr || FMath::IsNearlyZero(Value) ||
		!MovementInputGate.AllowsPrediction())
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
	MovementInputGate.SetRight(Value);
	UpdateMovementPredictionState();
	if (Session != nullptr && !bHasValidMovementSpeed &&
		!FMath::IsNearlyZero(Value))
	{
		ReportMovementSpeedProtocolError();
	}
	if (Controller == nullptr || FMath::IsNearlyZero(Value) ||
		!MovementInputGate.AllowsPrediction())
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

void ACorsairsPlayerCharacter::UpdateMovementPredictionState()
{
	if (Session == nullptr)
	{
		MovementInputGate.SetAuthorityLocked(false);
		return;
	}

	const double MovementSpeed = Session->GetMovementSpeedCmPerSecond();
	if (MovementSpeed > 0.0)
	{
		GetCharacterMovement()->MaxFlySpeed =
			static_cast<float>(MovementSpeed);
		bHasValidMovementSpeed = true;
		bMovementSpeedProtocolErrorReported = false;
	}
	else
	{
		bHasValidMovementSpeed = false;
	}
	MovementInputGate.SetAuthorityLocked(
		Session->IsMovementAuthorityLocked() || !bHasValidMovementSpeed);
}

void ACorsairsPlayerCharacter::ReportMovementSpeedProtocolError()
{
	if (Session == nullptr || bMovementSpeedProtocolErrorReported)
	{
		return;
	}

	bMovementSpeedProtocolErrorReported = true;
	const FString Message =
		TEXT("для локального движения отсутствует положительный ATTR_MSPD");
	UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
	Session->OnProtocolError.Broadcast(Message);
}

void ACorsairsPlayerCharacter::HandleMovementChanged(
	const FCorsairsMovementEvent& Event)
{
	if (!Event.bLocal)
	{
		return;
	}

	UpdateMovementPredictionState();
	if (!bHasValidMovementSpeed)
	{
		ReportMovementSpeedProtocolError();
	}

	if (Event.Type == ECorsairsMovementEventType::AcceptedPath)
	{
		return;
	}

	GetCharacterMovement()->StopMovementImmediately();
	ConsumeMovementInputVector();
	if (Event.bRequireNeutral)
	{
		MovementInputGate.RequireNeutral();
	}

	FVector AuthoritativeLocation = GetActorLocation();
	AuthoritativeLocation.X = Event.Endpoint.X;
	AuthoritativeLocation.Y = -Event.Endpoint.Y;
	SetActorLocation(
		AuthoritativeLocation,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
}

#if !UE_BUILD_SHIPPING
void ACorsairsPlayerCharacter::ApplyMovementAxisForProbe(
	const FName AxisName,
	const float Value)
{
	if (AxisName == TEXT("MoveForward"))
	{
		MoveForward(Value);
	}
	else if (AxisName == TEXT("MoveRight"))
	{
		MoveRight(Value);
	}
}
#endif
