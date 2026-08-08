#include "CorsairsGameMode.h"

#include "CorsairsCharacter.h"
#include "CorsairsLoginHud.h"
#include "CorsairsPlayerCharacter.h"

#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsGameMode, Log, All);

namespace
{
	/** Таблица «тип персонажа -> модель тела». Готовится скриптом
	 *  Scripts/build_character_map.py по таблице characters игровых данных:
	 *  внутри игры нет ни sqlite3, ни доступа к исходникам. */
	const TCHAR* CharacterMapRelativePath = TEXT("Data/character_map.json");
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
	Session->OnMovementChanged.AddDynamic(
		this,
		&ACorsairsGameMode::HandleMovementChanged);

	CharacterCatalog = MakeUnique<FCorsairsCharacterCatalog>();
	StartupError.Empty();
	const FString CatalogPath =
		FPaths::ProjectDir() / CharacterMapRelativePath;
	FString CatalogError;
	if (!CharacterCatalog->Load(CatalogPath, CatalogError))
	{
		StartupError = FString::Printf(
			TEXT("каталог персонажей '%s' не загрузился: %s"),
			*CatalogPath,
			*CatalogError);
		UE_LOG(
			LogCorsairsGameMode,
			Error,
			TEXT("ошибка запуска: %s"),
			*StartupError);
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
	if (!IsStartupReady())
	{
		UE_LOG(
			LogCorsairsGameMode,
			Error,
			TEXT("вход заблокирован ошибкой запуска: %s"),
			*StartupError);
		return;
	}

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
		Session->OnMovementChanged.RemoveDynamic(
			this,
			&ACorsairsGameMode::HandleMovementChanged);
	}

	APlayerController* Controller =
		UGameplayStatics::GetPlayerController(this, 0);
	if (ACorsairsPlayerCharacter* Local = Controller != nullptr
		? Cast<ACorsairsPlayerCharacter>(Controller->GetPawn())
		: nullptr)
	{
		Local->AttachSession(nullptr);
		Local->AttachCharacterGround(nullptr);
	}

	CleanupRemoteActors();
	CharacterGround.Reset();

	if (Session != nullptr)
	{
		Session->Logout();
		Session = nullptr;
	}
	Super::EndPlay(Reason);
}

void ACorsairsGameMode::HandleMovementChanged(
	const FCorsairsMovementEvent& Event)
{
	if (Event.bLocal)
	{
		return;
	}

	const TObjectPtr<ACorsairsCharacter>* Found =
		WorldActors.Find(Event.WorldId);
	if (Found == nullptr || *Found == nullptr)
	{
		return;
	}

	(*Found)->HandleServerMovementChanged(Event);
}

void ACorsairsGameMode::HandleStageChanged(ECorsairsLoginStage Stage, const FString& Message)
{
	UE_LOG(LogCorsairsGameMode, Log, TEXT("вход: %s"), *Message);

	switch (Stage)
	{
	case ECorsairsLoginStage::SelectingCha:
	{
		if (!IsStartupReady())
		{
			UE_LOG(
				LogCorsairsGameMode,
				Error,
				TEXT("выбор персонажа заблокирован ошибкой запуска: %s"),
				*StartupError);
			break;
		}

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
		if (!IsStartupReady())
		{
			UE_LOG(
				LogCorsairsGameMode,
				Error,
				TEXT("вход в мир заблокирован ошибкой запуска: %s"),
				*StartupError);
			break;
		}

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

		if (!ResolveAndApplyAppearance(
				Character,
				LocalActor.TypeId,
				LocalActor.Look,
				LocalActor.Name,
				LocalActor.WorldId))
		{
			UE_LOG(
				LogCorsairsGameMode,
				Error,
				TEXT("вход в мир остановлен: внешность локального персонажа недоступна"));
			break;
		}

		// Персонажа ставим ровно в source-cell, который подтвердил сервер.
		// Z и block flag берутся из half-meter character grid, отдельно от
		// интерполируемой поверхности сцены.
		const FIntPoint Spawn = LocalActor.Position;
		const bool bGroundLoaded =
			ActivateLocalCharacter(
				Character,
				Session->GetMapName(),
				Spawn);
		if (!bGroundLoaded)
		{
			break;
		}
		const FVector Location = Character->GetActorLocation();
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
			bGroundLoaded ? TEXT("найдена") : TEXT("НЕ НАЙДЕНА"));

		break;
	}

	default:
		break;
	}
}

bool ACorsairsGameMode::LoadCharacterGround(const FString& MapName)
{
	if (CharacterGround == nullptr)
	{
		CharacterGround = MakeUnique<FCorsairsCharacterGround>();
	}
	FString Error;
	if (CharacterGround->Load(MapName, Error))
	{
		return true;
	}

	StartupError = FString::Printf(
		TEXT("character ground карты %s не загрузился: %s"),
		*MapName,
		*Error);
	UE_LOG(
		LogCorsairsGameMode,
		Error,
		TEXT("%s"),
		*StartupError);
	return false;
}

void ACorsairsGameMode::GroundCharacter(
	ACorsairsCharacter* Character,
	const FIntPoint SourcePosition)
{
	if (Character == nullptr)
	{
		return;
	}
	if (CharacterGround == nullptr || !CharacterGround->IsLoaded())
	{
		return;
	}

	Character->AttachCharacterGround(CharacterGround.Get());
	const FVector Center = CharacterGround->ActorCenter(
		SourcePosition,
		Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	Character->SetActorLocation(
		Center,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
}

ACorsairsCharacter* ACorsairsGameMode::SpawnRemoteCharacter(
	const FIntPoint SourcePosition,
	const FRotator Rotation)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}
	if (CharacterGround == nullptr || !CharacterGround->IsLoaded())
	{
		return nullptr;
	}

	const ACorsairsCharacter* DefaultCharacter =
		GetDefault<ACorsairsCharacter>();
	const double HalfHeight = DefaultCharacter->GetCapsuleComponent()
		->GetScaledCapsuleHalfHeight();
	const FVector Center =
		CharacterGround->ActorCenter(SourcePosition, HalfHeight);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACorsairsCharacter* Spawned = World->SpawnActor<ACorsairsCharacter>(
		ACorsairsCharacter::StaticClass(),
		Center,
		Rotation,
		SpawnParameters);
	GroundCharacter(Spawned, SourcePosition);
	return Spawned;
}

bool ACorsairsGameMode::ActivateLocalCharacter(
	ACorsairsPlayerCharacter* Character,
	const FString& MapName,
	const FIntPoint SourcePosition)
{
	if (Character == nullptr || Session == nullptr)
	{
		return false;
	}
	if (!LoadCharacterGround(MapName))
	{
		Character->AttachSession(nullptr);
		Character->AttachCharacterGround(nullptr);
		CleanupRemoteActors();
		ScheduleSessionLogoutAfterGroundFailure();
		return false;
	}

	GroundCharacter(Character, SourcePosition);
	Character->AttachSession(Session);
	return true;
}

void ACorsairsGameMode::CleanupRemoteActors()
{
	for (TPair<int64, TObjectPtr<ACorsairsCharacter>>& Pair : WorldActors)
	{
		if (Pair.Value != nullptr)
		{
			Pair.Value->AttachCharacterGround(nullptr);
			Pair.Value->Destroy();
		}
	}
	WorldActors.Empty();
	WorldActorNames.Empty();
	WorldActorArchetypes.Empty();
}

void ACorsairsGameMode::ScheduleSessionLogoutAfterGroundFailure()
{
	UWorld* World = GetWorld();
	if (World == nullptr || Session == nullptr)
	{
		return;
	}

	const TWeakObjectPtr<UCorsairsSession> FailedSession(Session);
	World->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateWeakLambda(
			this,
			[FailedSession]()
			{
				if (FailedSession.IsValid())
				{
					FailedSession->Logout();
				}
			}));
}

#if WITH_DEV_AUTOMATION_TESTS
bool ACorsairsGameMode::LoadCharacterGroundFromBytesForTests(
	const int32 TileWidth,
	const int32 TileHeight,
	const TConstArrayView<uint8> Bytes,
	FString& OutError)
{
	if (CharacterGround == nullptr)
	{
		CharacterGround = MakeUnique<FCorsairsCharacterGround>();
	}
	return CharacterGround->LoadFromBytes(
		TileWidth,
		TileHeight,
		Bytes,
		OutError);
}

void ACorsairsGameMode::GroundCharacterForTests(
	ACorsairsCharacter* Character,
	const FIntPoint SourcePosition)
{
	GroundCharacter(Character, SourcePosition);
}

ACorsairsCharacter* ACorsairsGameMode::SpawnRemoteCharacterForTests(
	const FIntPoint SourcePosition,
	const FRotator Rotation)
{
	return SpawnRemoteCharacter(SourcePosition, Rotation);
}

void ACorsairsGameMode::HandleActorSeenForTests(
	const FCorsairsWorldActor& Actor)
{
	HandleActorSeen(Actor);
}
#endif

void ACorsairsGameMode::HandleActorSeen(const FCorsairsWorldActor& Actor)
{
	if (!IsStartupReady())
	{
		UE_LOG(
			LogCorsairsGameMode,
			Error,
			TEXT("появление персонажа %lld заблокировано ошибкой запуска: %s"),
			Actor.WorldId,
			*StartupError);
		return;
	}

	if (WorldActors.Contains(Actor.WorldId))
	{
		return;
	}

	// Координаты и поворот переводятся так же, как для объектов сцены: ось Y
	// инвертируется, угол приходит в десятых долях градуса.
	const FRotator Rotation(0.0, static_cast<double>(Actor.Angle) / 10.0, 0.0);
	ACorsairsCharacter* Spawned =
		SpawnRemoteCharacter(Actor.Position, Rotation);
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
			(*Found)->AttachCharacterGround(nullptr);
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
