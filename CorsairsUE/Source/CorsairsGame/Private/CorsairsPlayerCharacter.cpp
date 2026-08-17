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
#include "SceneManifest.h"
#include "TerrainHeights.h"

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
		// Перевод живёт в CorsairsImport вместе с прямым: пара обязана
		// меняться разом. Своя копия здесь уже отставала от прямой формулы,
		// и серверу уходило зеркальное положение — он отвергал бы движение
		// молча, ничего не отвечая.
		return UCorsairsSceneManifestLibrary::WorldPointToMap(Location);
	}

	/** Длина кронштейна камеры. Обзор в оригинале — с заметного отдаления,
	 *  чтобы видеть окружение боя, а не затылок. Мерка взята с оригинального
	 *  клиента: персонаж занимает примерно седьмую часть высоты экрана. */
	constexpr float CameraDistance = 1400.0f;

	/** Наклон камеры. Мир показывается сверху под углом, как в оригинале.
	 *  В Unreal отрицательный тангаж означает взгляд вниз. */
	constexpr float CameraPitch = -45.0f;

	/** Пределы наклона камеры. Оригинал не позволяет ни смотреть себе под
	 *  ноги, ни задирать взгляд в небо. */
	constexpr float CameraPitchMin = -70.0f;
	constexpr float CameraPitchMax = -10.0f;

	/** На сколько поднята точка крепления камеры над центром капсулы. */
	constexpr float CameraBoomHeight = 120.0f;
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
	// Подтягивание отключено намеренно. В плотной застройке кронштейн упирался
	// в каждый второй дом и подтаскивал камеру вплотную к персонажу — вид
	// падал к самой траве. Оригинал камеру не подтягивает вовсе.
	CameraBoom->bDoCollisionTest = false;
	// Точка крепления поднята к плечам: от центра капсулы камера смотрит
	// слишком низко, и половину кадра занимает земля под ногами.
	CameraBoom->SetRelativeLocation(FVector(0.0f, 0.0f, CameraBoomHeight));

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// Меш смещён вниз на половину капсулы: начало координат модели — под
	// ногами, а капсулы — в центре.
	GetMesh()->SetRelativeLocation(FVector(0.0f, 0.0f, -CapsuleHalfHeight));
	// Поворот: -90 по рысканью ставит модель лицом вперёд, как принято в
	// Unreal, а 90 по крену поднимает её из положения лёжа. Ось «вверх» у
	// исходных моделей не совпадает с движковой, и в файле она не записана —
	// это знание живёт в самом движке оригинала.
	GetMesh()->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
}

bool ACorsairsPlayerCharacter::SetBodyMesh(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return false;
	}

	USkeletalMesh* LoadedMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
	if (LoadedMesh == nullptr)
	{
		UE_LOG(LogCorsairsCharacter, Warning, TEXT("модель не загрузилась: %s"), *AssetPath);
		return false;
	}

	// Прежние части снимаются: иначе при смене внешности они остаются висеть
	// поверх новой модели.
	for (USkeletalMeshComponent* Part : BodyParts)
	{
		if (Part != nullptr)
		{
			Part->DestroyComponent();
		}
	}
	BodyParts.Reset();

	GetMesh()->SetSkeletalMesh(LoadedMesh);

	// Масштаб и наведение камеры откладываются: рост считается по всей
	// сборке, а сюда приходит только первая часть — голова.
	return true;
}

bool ACorsairsPlayerCharacter::AddBodyPart(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return false;
	}

	USkeletalMesh* LoadedMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
	if (LoadedMesh == nullptr)
	{
		UE_LOG(LogCorsairsCharacter, Warning, TEXT("часть тела не загрузилась: %s"), *AssetPath);
		return false;
	}

	USkeletalMeshComponent* Part = NewObject<USkeletalMeshComponent>(this);
	if (Part == nullptr)
	{
		return false;
	}

	Part->SetupAttachment(GetMesh());
	Part->RegisterComponent();
	Part->SetSkeletalMesh(LoadedMesh);

	// Поза берётся у основной части: скелет у всех кусков один, и анимировать
	// каждый отдельно значило бы получить рассыпающегося персонажа.
	Part->SetLeaderPoseComponent(GetMesh());
	Part->SetRelativeTransform(FTransform::Identity);

	BodyParts.Add(Part);
	UE_LOG(LogCorsairsCharacter, Log, TEXT("часть тела: %s"), *AssetPath);
	return true;
}

void ACorsairsPlayerCharacter::FinishBody()
{
	USkeletalMesh* Body = GetMesh()->GetSkeletalMeshAsset();
	if (Body == nullptr)
	{
		return;
	}

	// Объём считается по всем частям сразу. Голова висит на высоте роста, ноги
	// у нуля — по отдельности ни одна часть роста не показывает.
	FBox Combined(ForceInit);
	const FBoxSphereBounds MainBounds = Body->GetBounds();
	Combined += FBox::BuildAABB(MainBounds.Origin, MainBounds.BoxExtent);
	for (const USkeletalMeshComponent* Part : BodyParts)
	{
		if (Part == nullptr || Part->GetSkeletalMeshAsset() == nullptr)
		{
			continue;
		}
		const FBoxSphereBounds PartBounds = Part->GetSkeletalMeshAsset()->GetBounds();
		Combined += FBox::BuildAABB(PartBounds.Origin, PartBounds.BoxExtent);
	}

	const FVector Size = Combined.GetSize();
	if (Size.Z <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const double Factor = (CapsuleHalfHeight * 2.0) / Size.Z;
	GetMesh()->SetRelativeScale3D(FVector(Factor));
	for (USkeletalMeshComponent* Part : BodyParts)
	{
		if (Part != nullptr)
		{
			Part->SetRelativeScale3D(FVector::OneVector);
		}
	}

	// Подошвы ставятся на низ капсулы по объединённому объёму. Границы одного
	// компонента тут не годятся: основной меш — это голова, её низ на высоте
	// плеч, и персонаж уходил в землю по грудь.
	GetMesh()->SetRelativeLocation(
		FVector(0.0, 0.0, -CapsuleHalfHeight - Combined.Min.Z * Factor));

	// Посадка уточняется по факту. Расчёт по границам ассетов промахивается
	// на десяток сантиметров: те описывают модель в позе покоя, а компонент
	// живёт уже с наложенной позой, и низ фигуры оказывается ниже.
	GetMesh()->UpdateBounds();
	FBox Actual(ForceInit);
	Actual += FBox::BuildAABB(GetMesh()->Bounds.Origin, GetMesh()->Bounds.BoxExtent);
	for (USkeletalMeshComponent* Part : BodyParts)
	{
		if (Part != nullptr)
		{
			Part->UpdateBounds();
			Actual += FBox::BuildAABB(Part->Bounds.Origin, Part->Bounds.BoxExtent);
		}
	}
	const double WantedBottom = GetActorLocation().Z - CapsuleHalfHeight;
	GetMesh()->AddRelativeLocation(FVector(0.0, 0.0, WantedBottom - Actual.Min.Z));

	// Камера наводится на середину фигуры. Рыскание поворачивает модель
	// вокруг вертикали, поэтому смещение середины надо повернуть тем же
	// поворотом, иначе поправка уйдёт не в ту сторону.
	const FRotator MeshRotation = GetMesh()->GetRelativeRotation();
	const FVector Aimed = MeshRotation.RotateVector(Combined.GetCenter() * Factor);
	CameraBoom->TargetOffset = FVector(Aimed.X, Aimed.Y, 0.0);

	UE_LOG(LogCorsairsCharacter, Log,
		   TEXT("сборка: высота %.0f см, частей %d, масштаб %.2f, поправка камеры (%.0f, %.0f)"),
		   Size.Z, BodyParts.Num() + 1, Factor, Aimed.X, Aimed.Y);
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

		// Наклон ограничивается: без предела первый же рывок мыши уводит
		// камеру отвесно вниз или в зенит, и мир пропадает из кадра. Границы
		// подобраны под обзор оригинала — он показывает мир сверху, но не
		// с высоты птичьего полёта.
		if (APlayerController* PlayerController = Cast<APlayerController>(OwningController))
		{
			if (PlayerController->PlayerCameraManager != nullptr)
			{
				PlayerController->PlayerCameraManager->ViewPitchMin = CameraPitchMin;
				PlayerController->PlayerCameraManager->ViewPitchMax = CameraPitchMax;
			}
		}
	}
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

		UE_LOG(LogTemp, Warning,
			   TEXT("  рыскание: контроллер %.1f, камера %.1f, актёр %.1f; поправка цели (%.0f, %.0f)"),
			   Control.Yaw, CameraRotation.Yaw, GetActorRotation().Yaw,
			   CameraBoom->TargetOffset.X, CameraBoom->TargetOffset.Y);

		// Меряется вся фигура, а не основной меш: тот содержит одну голову, и
		// по нему «центр модели» получался у плеч, что читалось как ошибка
		// высоты там, где её не было.
		FBox Whole(ForceInit);
		Whole += FBox::BuildAABB(MeshBounds.Origin, MeshBounds.BoxExtent);
		for (const USkeletalMeshComponent* Part : BodyParts)
		{
			if (Part != nullptr)
			{
				const FBoxSphereBounds PartBounds = Part->Bounds;
				Whole += FBox::BuildAABB(PartBounds.Origin, PartBounds.BoxExtent);
			}
		}
		const FVector WholeCentre = Whole.GetCenter();
		UE_LOG(LogTemp, Warning,
			   TEXT("  вся фигура: центр (%.0f, %.0f, %.0f), высота %.0f, низ %.0f"),
			   WholeCentre.X, WholeCentre.Y, WholeCentre.Z,
			   Whole.GetSize().Z, Whole.Min.Z);
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
