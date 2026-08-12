#include "CorsairsGameMode.h"

#include "CorsairsCharacter.h"
#include "CorsairsLoginHud.h"
#include "CorsairsPlayerCharacter.h"
#include "CorsairsPlayerController.h"

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
	const TCHAR* SkillCatalogRelativePath = TEXT("Data/skills.json");
}

ACorsairsGameMode::ACorsairsGameMode()
{
	DefaultPawnClass = ACorsairsPlayerCharacter::StaticClass();
	PlayerControllerClass = ACorsairsPlayerController::StaticClass();
	// Экран состояния сессии: без него отказ сервера виден только в журнале.
	HUDClass = ACorsairsLoginHud::StaticClass();
}

void ACorsairsGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Сессия создаётся всегда: экран входа опирается на её стадию, и без
	// объекта ему нечего показывать даже до подключения.
	_session = NewObject<UCorsairsSession>(this);
	_session->OnStageChanged.AddDynamic(this, &ACorsairsGameMode::HandleStageChanged);
	_session->OnActorSeen.AddDynamic(this, &ACorsairsGameMode::HandleActorSeen);
	_session->OnActorLeft.AddDynamic(this, &ACorsairsGameMode::HandleActorLeft);
	_session->OnActorLookChanged.AddDynamic(
		this,
		&ACorsairsGameMode::HandleActorLookChanged);
	_session->OnMovementChanged.AddDynamic(
		this,
		&ACorsairsGameMode::HandleMovementChanged);

	_characterCatalog = MakeUnique<FCorsairsCharacterCatalog>();
	_startupError.Empty();
	const FString CatalogPath =
		FPaths::ProjectDir() / CharacterMapRelativePath;
	FString CatalogError;
	if (!_characterCatalog->Load(CatalogPath, CatalogError))
	{
		_startupError = FString::Printf(
			TEXT("каталог персонажей '%s' не загрузился: %s"),
			*CatalogPath,
			*CatalogError);
		UE_LOG(
			LogCorsairsGameMode,
			Error,
			TEXT("ошибка запуска: %s"),
			*_startupError);
		_characterCatalog.Reset();
		return;
	}

	_skillCatalog = MakeUnique<FCorsairsSkillCatalog>();
	const FString SkillCatalogPath =
		FPaths::ProjectDir() / SkillCatalogRelativePath;
	FString SkillCatalogError;
	if (!_skillCatalog->Load(SkillCatalogPath, SkillCatalogError))
	{
		_startupError = FString::Printf(
			TEXT("каталог навыков '%s' не загрузился: %s"),
			*SkillCatalogPath,
			*SkillCatalogError);
		UE_LOG(
			LogCorsairsGameMode,
			Error,
			TEXT("ошибка запуска: %s"),
			*_startupError);
		_skillCatalog.Reset();
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
			*_startupError);
		return;
	}

	if (_session == nullptr)
	{
		return;
	}
	_session->Login(Host, Port, Account, Password);
}

void ACorsairsGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (_session != nullptr)
	{
		_session->OnMovementChanged.RemoveDynamic(
			this,
			&ACorsairsGameMode::HandleMovementChanged);
	}

	APlayerController* Controller =
		UGameplayStatics::GetPlayerController(this, 0);
	if (ACorsairsPlayerController* CorsairsController =
		Cast<ACorsairsPlayerController>(Controller))
	{
		CorsairsController->AttachGameplay(nullptr, nullptr, nullptr);
	}
	if (ACorsairsPlayerCharacter* Local = Controller != nullptr
		? Cast<ACorsairsPlayerCharacter>(Controller->GetPawn())
		: nullptr)
	{
		Local->AttachSession(nullptr);
		Local->AttachCharacterGround(nullptr);
	}

	CleanupRemoteActors();
	_characterGround.Reset();
	_skillCatalog.Reset();

	if (_session != nullptr)
	{
		_session->Logout();
		_session = nullptr;
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
		_worldActors.Find(Event.WorldId);
	if (Found == nullptr || *Found == nullptr)
	{
		return;
	}

	(*Found)->HandleServerMovementChanged(Event);
}

void ACorsairsGameMode::HandleStageChanged(ECorsairsLoginStage Stage, const FString& Message)
{
	UE_LOG(LogCorsairsGameMode, Log, TEXT("вход: %s"), *Message);
	if (Stage == ECorsairsLoginStage::InWorld)
	{
		// Каждый ENTERMAP начинает новый authoritative world snapshot, даже
		// если предыдущая стадия также была InWorld.
		CleanupRemoteActors();
	}
	else
	{
		DeactivateLocalGameplay();
	}

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
				*_startupError);
			break;
		}

		// Пока экрана выбора нет — берём первого пригодного персонажа.
		const TArray<FCorsairsCharacterSlot>& Characters = _session->GetCharacters();
		for (int32 Index = 0; Index < Characters.Num(); ++Index)
		{
			if (Characters[Index].Valid)
			{
				_session->EnterWorld(Index);
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
				*_startupError);
			break;
		}

		const FCorsairsWorldActor LocalActor = _session->GetLocalActor();
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
			FailClosedLocalActivation();
			break;
		}

		// Персонажа ставим ровно в source-cell, который подтвердил сервер.
		// Z и block flag берутся из half-meter character grid, отдельно от
		// интерполируемой поверхности сцены.
		const FIntPoint Spawn = LocalActor.Position;
		const bool bGroundLoaded =
			ActivateLocalCharacter(
				Character,
				_session->GetMapName(),
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
			*_session->GetMapName(),
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
	if (_characterGround == nullptr)
	{
		_characterGround = MakeUnique<FCorsairsCharacterGround>();
	}
	FString Error;
	if (_characterGround->Load(MapName, Error))
	{
		return true;
	}

	_startupError = FString::Printf(
		TEXT("character ground карты %s не загрузился: %s"),
		*MapName,
		*Error);
	UE_LOG(
		LogCorsairsGameMode,
		Error,
		TEXT("%s"),
		*_startupError);
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
	if (_characterGround == nullptr || !_characterGround->IsLoaded())
	{
		return;
	}

	Character->AttachCharacterGround(_characterGround.Get());
	const FVector Center = _characterGround->ActorCenter(
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
	if (_characterGround == nullptr || !_characterGround->IsLoaded())
	{
		return nullptr;
	}

	const ACorsairsCharacter* DefaultCharacter =
		GetDefault<ACorsairsCharacter>();
	const double HalfHeight = DefaultCharacter->GetCapsuleComponent()
		->GetScaledCapsuleHalfHeight();
	const FVector Center =
		_characterGround->ActorCenter(SourcePosition, HalfHeight);
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
	if (Character == nullptr || _session == nullptr)
	{
		return false;
	}
	if (!LoadCharacterGround(MapName))
	{
		FailClosedLocalActivation();
		return false;
	}

	GroundCharacter(Character, SourcePosition);
	Character->AttachSession(_session);
	if (ACorsairsPlayerController* Controller =
		Cast<ACorsairsPlayerController>(Character->GetController()))
	{
		Controller->AttachGameplay(
			_session,
			_characterGround.Get(),
			_skillCatalog.Get());
	}
	_localGameplayActive = true;
	return true;
}

void ACorsairsGameMode::FailClosedLocalActivation()
{
	DeactivateLocalGameplay();
	ScheduleSessionLogoutAfterActivationFailure();
}

void ACorsairsGameMode::DeactivateLocalGameplay()
{
	_localGameplayActive = false;
	APlayerController* Controller =
		UGameplayStatics::GetPlayerController(this, 0);
	if (ACorsairsPlayerController* CorsairsController =
		Cast<ACorsairsPlayerController>(Controller))
	{
		CorsairsController->AttachGameplay(nullptr, nullptr, nullptr);
	}
	if (ACorsairsPlayerCharacter* Character = Controller != nullptr
		? Cast<ACorsairsPlayerCharacter>(Controller->GetPawn())
		: nullptr)
	{
		Character->AttachSession(nullptr);
		Character->AttachCharacterGround(nullptr);
	}

	CleanupRemoteActors();
}

void ACorsairsGameMode::CleanupRemoteActors()
{
	for (TPair<int64, TObjectPtr<ACorsairsCharacter>>& Pair : _worldActors)
	{
		if (Pair.Value != nullptr)
		{
			Pair.Value->AttachCharacterGround(nullptr);
			Pair.Value->Destroy();
		}
	}
	_worldActors.Empty();
	_worldActorNames.Empty();
	_worldActorArchetypes.Empty();
}

void ACorsairsGameMode::ScheduleSessionLogoutAfterActivationFailure()
{
	UWorld* World = GetWorld();
	if (World == nullptr || _session == nullptr)
	{
		return;
	}

	const TWeakObjectPtr<UCorsairsSession> FailedSession(_session);
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
	if (_characterGround == nullptr)
	{
		_characterGround = MakeUnique<FCorsairsCharacterGround>();
	}
	return _characterGround->LoadFromBytes(
		TileWidth,
		TileHeight,
		Bytes,
		OutError);
}

bool ACorsairsGameMode::LoadCharacterNavigationFromBytesForTests(
	const int32 TileWidth,
	const int32 TileHeight,
	const TConstArrayView<uint8> HeightBytes,
	const TConstArrayView<uint8> BlockBytes,
	const TConstArrayView<uint8> RegionBytes,
	FString& OutError)
{
	if (_characterGround == nullptr)
	{
		_characterGround = MakeUnique<FCorsairsCharacterGround>();
	}
	return _characterGround->LoadRuntimeFromBytes(
		TileWidth,
		TileHeight,
		HeightBytes,
		BlockBytes,
		RegionBytes,
		OutError);
}

void ACorsairsGameMode::GroundCharacterForTests(
	ACorsairsCharacter* Character,
	const FIntPoint SourcePosition)
{
	GroundCharacter(Character, SourcePosition);
	_localGameplayActive = true;
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
			*_startupError);
		return;
	}
	if (!_localGameplayActive)
	{
		return;
	}

	if (TObjectPtr<ACorsairsCharacter>* Existing = _worldActors.Find(Actor.WorldId))
	{
		FCorsairsServerIdentity ExistingIdentity;
		if (*Existing != nullptr &&
			(*Existing)->TryGetServerIdentity(ExistingIdentity) &&
			ExistingIdentity.Handle == Actor.Handle)
		{
			return;
		}
		HandleActorLeft(Actor.WorldId);
	}

	// ActorCenter переводит source-position через rigid Q=(-y,x).
	//
	// Угол приходит целыми градусами и кладётся как есть: поправку -90°
	// уже несёт меш-компонент ACorsairsCharacter, а поворот базиса на +90°
	// её ровно компенсирует.
	//
	// Делителя здесь быть не должно. Оригинал переводит угол в радианы
	// умножением на пи и делением на сто восемьдесят и ничего не делит
	// предварительно: `pCha->setYaw(sAngle)` в NetProtocol.cpp кладёт
	// пришедшее значение прямо в `_nYaw`, а `CCharacter::_UpdateYaw` берёт
	// его через `Angle2Radian`. Делитель ужимал бы весь круг в 36 градусов,
	// и каждый встречный смотрел бы почти в одну сторону.
	const FRotator Rotation(0.0, static_cast<double>(Actor.Angle), 0.0);
	ACorsairsCharacter* Spawned =
		SpawnRemoteCharacter(Actor.Position, Rotation);
	if (Spawned == nullptr)
	{
		return;
	}
	FCorsairsServerIdentity identity;
	identity.WorldId = Actor.WorldId;
	identity.Handle = Actor.Handle;
	identity.CtrlType = Actor.CtrlType;
	identity.ChaId = Actor.ChaId;
	if (!Spawned->InitializeServerIdentity(identity))
	{
		Spawned->AttachCharacterGround(nullptr);
		Spawned->Destroy();
		return;
	}

#if WITH_EDITOR
	Spawned->SetActorLabel(Actor.Name.IsEmpty()
							   ? FString::Printf(TEXT("Actor_%lld"), Actor.WorldId)
							   : Actor.Name);
#endif

	if (!ResolveAndApplyAppearance(
			Spawned,
			Actor.TypeId,
			Actor.Look,
			Actor.Name,
			Actor.WorldId))
	{
		Spawned->AttachCharacterGround(nullptr);
		Spawned->Destroy();
		return;
	}

	_worldActors.Add(Actor.WorldId, Spawned);
	_worldActorNames.Add(Actor.WorldId, Actor.Name);
	_worldActorArchetypes.Add(Actor.WorldId, Actor.TypeId);
}

void ACorsairsGameMode::HandleActorLeft(int64 WorldId)
{
	if (TObjectPtr<ACorsairsCharacter>* Found = _worldActors.Find(WorldId))
	{
		if (*Found != nullptr)
		{
			(*Found)->AttachCharacterGround(nullptr);
			(*Found)->Destroy();
		}
		_worldActors.Remove(WorldId);
		_worldActorNames.Remove(WorldId);
		_worldActorArchetypes.Remove(WorldId);
	}
}

void ACorsairsGameMode::HandleActorLookChanged(
	const int64 WorldId,
	const FCorsairsCharacterLook& Look)
{
	if (_session != nullptr)
	{
		const FCorsairsWorldActor LocalActor = _session->GetLocalActor();
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
		_worldActors.Find(WorldId);
	if (Found == nullptr || *Found == nullptr)
	{
		return;
	}

	const int32 ArchetypeId = Look.TypeId != 0
		? Look.TypeId
		: _worldActorArchetypes.FindRef(WorldId);
	if (Look.TypeId != 0)
	{
		_worldActorArchetypes.Add(WorldId, Look.TypeId);
	}
	ResolveAndApplyAppearance(
		*Found,
		ArchetypeId,
		Look,
		_worldActorNames.FindRef(WorldId),
		WorldId);
}

bool ACorsairsGameMode::ResolveAndApplyAppearance(
	ACorsairsCharacter* Character,
	const int32 ArchetypeId,
	const FCorsairsCharacterLook& Look,
	const FString& ActorName,
	const int64 WorldId)
{
	if (Character == nullptr || _characterCatalog == nullptr)
	{
		return false;
	}

	const FString DisplayName = ActorName.IsEmpty()
		? FString::Printf(TEXT("Actor_%lld"), WorldId)
		: ActorName;
	FCorsairsResolvedAppearance Appearance;
	FString Error;
	if (!_characterCatalog->Resolve(
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
