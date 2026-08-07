#include "CorsairsGameMode.h"

#include "TerrainHeights.h"

#include "CorsairsCharacter.h"
#include "CorsairsLoginHud.h"
#include "CorsairsPlayerCharacter.h"

#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsGameMode, Log, All);

namespace
{
	/** Таблица «тип персонажа -> модель тела». Готовится скриптом
	 *  Scripts/build_character_map.py по таблице characters игровых данных:
	 *  внутри игры нет ни sqlite3, ни доступа к исходникам. */
	const TCHAR* CharacterMapRelativePath = TEXT("Data/character_map.json");

	/** На сколько поднять персонажа над серверной позицией при появлении.
	 *  Точная высота земли в этой точке клиенту неизвестна, а падение с
	 *  запасом безопаснее застревания в грунте. */
	constexpr double SpawnHeightMargin = 500.0;

	/** С какой высоты искать землю и как далеко вниз. Рельеф карты лежит в
	 *  пределах десяти метров от нуля, но объекты сцены поднимаются выше, и
	 *  запас берётся с большим избытком. */
	constexpr double GroundTraceStart = 100000.0;
	constexpr double GroundTraceDepth = 200000.0;

	/** Наклон камеры при появлении. Оригинал показывает мир сверху под углом
	 *  около сорока пяти градусов — отсюда и значение. */
	constexpr double CameraPitch = -45.0;

	/** Тег плиток рельефа. Метка актёра живёт только в редакторе, а отличить
	 *  землю от построек нужно в игре: иначе персонаж встаёт на крышу. */
	const FName TerrainTag(TEXT("CorsairsTerrain"));

	/** Ставит точку на землю под ней.
	 *
	 *  Без этого высота бралась от места, где стоит PlayerStart, а он на
	 *  карте один и к серверной позиции отношения не имеет: персонаж
	 *  оказывался в пустоте на высоте девятнадцати метров и висел в небе,
	 *  потому что земли под ним в этом месте нет вовсе. */
	bool DropToGround(UWorld* World, FVector& Location)
	{
		if (World == nullptr)
		{
			return false;
		}
		const FVector From(Location.X, Location.Y, GroundTraceStart);
		const FVector To(Location.X, Location.Y, GroundTraceStart - GroundTraceDepth);

		// Собираем все пересечения и ищем среди них рельеф. Брать первое
		// сверху нельзя — это крыша дома или ветка; брать самое нижнее тоже,
		// потому что ниже земли попадаются подвалы и вода. Нужен именно
		// грунт, и узнаётся он по тегу.
		TArray<FHitResult> Hits;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(CorsairsSpawnGround), false);
		if (!World->LineTraceMultiByChannel(Hits, From, To, ECC_WorldStatic, Params))
		{
			return false;
		}

		for (const FHitResult& Hit : Hits)
		{
			const AActor* HitActor = Hit.GetActor();
			if (HitActor != nullptr && HitActor->ActorHasTag(TerrainTag))
			{
				Location.Z = Hit.Location.Z + SpawnHeightMargin;
				return true;
			}
		}

		// Рельефа под точкой не оказалось — падаем на то, что нашлось ниже
		// всего, лишь бы не остаться висеть в воздухе.
		double Lowest = TNumericLimits<double>::Max();
		for (const FHitResult& Hit : Hits)
		{
			Lowest = FMath::Min(Lowest, Hit.Location.Z);
		}
		if (Lowest == TNumericLimits<double>::Max())
		{
			return false;
		}

		Location.Z = Lowest + SpawnHeightMargin;
		return true;
	}
}

ACorsairsGameMode::ACorsairsGameMode()
{
	DefaultPawnClass = ACorsairsPlayerCharacter::StaticClass();
	// Экран состояния сессии: без него отказ сервера виден только в журнале.
	HUDClass = ACorsairsLoginHud::StaticClass();
}

void ACorsairsGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Сессия создаётся всегда: экран входа опирается на её стадию, и без
	// объекта ему нечего показывать даже до подключения.
	Session = NewObject<UCorsairsSession>(this);
	Session->OnStageChanged.AddDynamic(this, &ACorsairsGameMode::HandleStageChanged);
	Session->OnActorSeen.AddDynamic(this, &ACorsairsGameMode::HandleActorSeen);
	Session->OnActorLeft.AddDynamic(this, &ACorsairsGameMode::HandleActorLeft);
	Session->OnActorLookChanged.AddDynamic(
		this,
		&ACorsairsGameMode::HandleActorLookChanged);

	CharacterCatalog = MakeUnique<FCorsairsCharacterCatalog>();
	const FString CatalogPath =
		FPaths::ProjectDir() / CharacterMapRelativePath;
	FString CatalogError;
	if (!CharacterCatalog->Load(CatalogPath, CatalogError))
	{
		UE_LOG(
			LogCorsairsGameMode,
			Error,
			TEXT("каталог персонажей не загрузился: %s: %s"),
			*CatalogPath,
			*CatalogError);
		CharacterCatalog.Reset();
		return;
	}

	if (!bAutoLogin)
	{
		UE_LOG(LogCorsairsGameMode, Log, TEXT("автоматический вход выключен"));
		return;
	}

	StartLogin();
}

void ACorsairsGameMode::StartLogin()
{
	if (Session == nullptr)
	{
		return;
	}
	Session->Login(Host, Port, Account, Password);
}

void ACorsairsGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Session != nullptr)
	{
		Session->Logout();
		Session = nullptr;
	}
	Super::EndPlay(Reason);
}

void ACorsairsGameMode::HandleStageChanged(ECorsairsLoginStage Stage, const FString& Message)
{
	UE_LOG(LogCorsairsGameMode, Log, TEXT("вход: %s"), *Message);

	switch (Stage)
	{
	case ECorsairsLoginStage::SelectingCha:
	{
		// Пока экрана выбора нет — берём первого пригодного персонажа.
		const TArray<FCorsairsCharacterSlot>& Characters = Session->GetCharacters();
		for (int32 Index = 0; Index < Characters.Num(); ++Index)
		{
			if (Characters[Index].Valid)
			{
				Session->EnterWorld(Index);
				return;
			}
		}
		UE_LOG(LogCorsairsGameMode, Warning,
			   TEXT("в учётной записи нет персонажей — входить некем"));
		break;
	}

	case ECorsairsLoginStage::InWorld:
	{
		const FCorsairsWorldActor LocalActor = Session->GetLocalActor();
		APlayerController* Controller =
			UGameplayStatics::GetPlayerController(this, 0);
		ACorsairsPlayerCharacter* Character =
			Controller != nullptr
				? Cast<ACorsairsPlayerCharacter>(Controller->GetPawn())
				: nullptr;
		if (Character == nullptr)
		{
			UE_LOG(
				LogCorsairsGameMode,
				Warning,
				TEXT("персонаж игрока ещё не создан"));
			break;
		}

		ResolveAndApplyAppearance(
			Character,
			LocalActor.TypeId,
			LocalActor.Look,
			LocalActor.Name,
			LocalActor.WorldId);

		// Персонажа ставим туда, где его держит сервер. Ось Y
		// инвертируется, как при размещении объектов; высота берётся
		// с запасом над рельефом, дальше персонаж падает на землю сам.
		const FIntPoint Spawn = LocalActor.Position;
		FVector Location(
			static_cast<double>(Spawn.X),
			-static_cast<double>(Spawn.Y),
			Character->GetActorLocation().Z + SpawnHeightMargin);

		// Высота берётся из карты высот — той же, из которой построена
		// видимая земля. Трассировка тут не годится: рельеф пришёл из
		// glTF без физической формы, и луч проходит сквозь него.
		// Карта высот загружается один раз на всех: и свой персонаж, и
		// показанные сервером ставятся по ней.
		if (TerrainHeights == nullptr)
		{
			TerrainHeights = NewObject<UCorsairsTerrainHeights>(this);
		}
		TerrainHeights->Load(Session->GetMapName());

		const bool bGrounded =
			Character->UseTerrainHeights(Session->GetMapName());
		if (!bGrounded)
		{
			DropToGround(GetWorld(), Location);
		}

		Character->SetActorLocation(
			Location,
			false,
			nullptr,
			ETeleportType::TeleportPhysics);
		UE_LOG(
			LogCorsairsGameMode,
			Log,
			TEXT("позиция от сервера: (%d, %d) на карте %s -> (%.0f, %.0f, %.0f), земля %s"),
			Spawn.X,
			Spawn.Y,
			*Session->GetMapName(),
			Location.X,
			Location.Y,
			Location.Z,
			bGrounded ? TEXT("найдена") : TEXT("НЕ НАЙДЕНА"));

		// Направление взгляда задаётся явно. Камера следует за поворотом
		// контроллера, а тот наследует поворот PlayerStart — единственной
		// точки на карте, ориентация которой к игре отношения не имеет:
		// при её нулевом наклоне камера смотрит в горизонт, а при любом
		// другом — в небо или в землю. Вид сверху под наклоном повторяет
		// обзор оригинала.
		if (AController* ViewController = Character->GetController())
		{
			ViewController->SetControlRotation(
				FRotator(CameraPitch, 0.0, 0.0));
		}

		// С этого момента персонаж сам сообщает серверу о перемещении.
		Character->AttachSession(Session);
		break;
	}

	default:
		break;
	}
}

void ACorsairsGameMode::HandleActorSeen(const FCorsairsWorldActor& Actor)
{
	if (WorldActors.Contains(Actor.WorldId))
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Координаты и поворот переводятся так же, как для объектов сцены: ось Y
	// инвертируется, угол приходит в десятых долях градуса.
	FVector Location(static_cast<double>(Actor.Position.X),
					 -static_cast<double>(Actor.Position.Y),
					 SpawnHeightMargin);

	// Высота — из карты высот, той же, из которой построена видимая земля.
	// Трассировка оставляла каждого на своей высоте: кого на крыше, кого в
	// воздухе, и толпа стояла ступеньками.
	if (TerrainHeights != nullptr && TerrainHeights->IsLoaded())
	{
		Location.Z = TerrainHeights->HeightAt(Location.X, Location.Y);
	}
	else
	{
		DropToGround(World, Location);
	}
	const FRotator Rotation(0.0, static_cast<double>(Actor.Angle) / 10.0, 0.0);

	ACorsairsCharacter* Spawned = World->SpawnActor<ACorsairsCharacter>(
		ACorsairsCharacter::StaticClass(),
		Location,
		Rotation);
	if (Spawned == nullptr)
	{
		return;
	}

#if WITH_EDITOR
	Spawned->SetActorLabel(Actor.Name.IsEmpty()
							   ? FString::Printf(TEXT("Actor_%lld"), Actor.WorldId)
							   : Actor.Name);
#endif

	ResolveAndApplyAppearance(
		Spawned,
		Actor.TypeId,
		Actor.Look,
		Actor.Name,
		Actor.WorldId);

	WorldActors.Add(Actor.WorldId, Spawned);
	WorldActorNames.Add(Actor.WorldId, Actor.Name);
	WorldActorArchetypes.Add(Actor.WorldId, Actor.TypeId);
}

void ACorsairsGameMode::HandleActorLeft(int64 WorldId)
{
	if (TObjectPtr<ACorsairsCharacter>* Found = WorldActors.Find(WorldId))
	{
		if (*Found != nullptr)
		{
			(*Found)->Destroy();
		}
		WorldActors.Remove(WorldId);
		WorldActorNames.Remove(WorldId);
		WorldActorArchetypes.Remove(WorldId);
	}
}

void ACorsairsGameMode::HandleActorLookChanged(
	const int64 WorldId,
	const FCorsairsCharacterLook& Look)
{
	if (Session != nullptr)
	{
		const FCorsairsWorldActor LocalActor = Session->GetLocalActor();
		if (LocalActor.WorldId == WorldId)
		{
			APlayerController* Controller =
				UGameplayStatics::GetPlayerController(this, 0);
			ACorsairsCharacter* Character =
				Controller != nullptr
					? Cast<ACorsairsCharacter>(Controller->GetPawn())
					: nullptr;
			ResolveAndApplyAppearance(
				Character,
				LocalActor.TypeId,
				LocalActor.Look,
				LocalActor.Name,
				LocalActor.WorldId);
			return;
		}
	}

	const TObjectPtr<ACorsairsCharacter>* Found =
		WorldActors.Find(WorldId);
	if (Found == nullptr || *Found == nullptr)
	{
		return;
	}

	const int32 ArchetypeId = Look.TypeId != 0
		? Look.TypeId
		: WorldActorArchetypes.FindRef(WorldId);
	if (Look.TypeId != 0)
	{
		WorldActorArchetypes.Add(WorldId, Look.TypeId);
	}
	ResolveAndApplyAppearance(
		*Found,
		ArchetypeId,
		Look,
		WorldActorNames.FindRef(WorldId),
		WorldId);
}

bool ACorsairsGameMode::ResolveAndApplyAppearance(
	ACorsairsCharacter* Character,
	const int32 ArchetypeId,
	const FCorsairsCharacterLook& Look,
	const FString& ActorName,
	const int64 WorldId)
{
	if (Character == nullptr || CharacterCatalog == nullptr)
	{
		return false;
	}

	const FString DisplayName = ActorName.IsEmpty()
		? FString::Printf(TEXT("Actor_%lld"), WorldId)
		: ActorName;
	FCorsairsResolvedAppearance Appearance;
	FString Error;
	if (!CharacterCatalog->Resolve(
			ArchetypeId,
			Look,
			Appearance,
			Error))
	{
		UE_LOG(
			LogCorsairsGameMode,
			Warning,
			TEXT("внешность %s (%lld) не разрешилась: %s"),
			*DisplayName,
			WorldId,
			*Error);
		return false;
	}

	for (const FString& Warning : Appearance.Warnings)
	{
		UE_LOG(
			LogCorsairsGameMode,
			Warning,
			TEXT("внешность %s (%lld): %s"),
			*DisplayName,
			WorldId,
			*Warning);
	}

	if (!Character->ApplyAppearance(Appearance))
	{
		UE_LOG(
			LogCorsairsGameMode,
			Warning,
			TEXT("внешность %s (%lld) не применилась"),
			*DisplayName,
			WorldId);
		return false;
	}

	return true;
}
