#pragma once

#include "CoreMinimal.h"
#include "CorsairsCharacterCatalog.h"
#include "CorsairsCharacterGround.h"
#include "CorsairsSession.h"
#include "GameFramework/GameModeBase.h"

#include "CorsairsGameMode.generated.h"

class ACorsairsCharacter;
class ACorsairsPlayerCharacter;

/**
 * Режим игры вертикального среза: подключается к серверу и ставит персонажа
 * в мир, как только сервер подтвердит вход.
 *
 * Учётные данные пока в настройках, а не на экране входа: интерфейса ещё нет,
 * а проверять связку с миром нужно уже сейчас. Экран появится следующим шагом
 * и заменит эти поля.
 */
UCLASS()
class CORSAIRSGAME_API ACorsairsGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ACorsairsGameMode();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	UPROPERTY(EditDefaultsOnly, Category = "Corsairs|Сервер")
	FString Host = TEXT("127.0.0.1");

	UPROPERTY(EditDefaultsOnly, Category = "Corsairs|Сервер")
	int32 Port = 1973;

	UPROPERTY(EditDefaultsOnly, Category = "Corsairs|Сервер")
	FString Account = TEXT("admin");

	UPROPERTY(EditDefaultsOnly, Category = "Corsairs|Сервер")
	FString Password = TEXT("admin");

	/** Подключаться ли при старте. Выключается, когда уровень открывают для
	 *  осмотра геометрии, а сервера под рукой нет. */
	UPROPERTY(EditDefaultsOnly, Category = "Corsairs|Сервер")
	bool bAutoLogin = true;

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	UCorsairsSession* GetSession() const { return Session; }

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	bool IsStartupReady() const
	{
		return CharacterCatalog != nullptr && StartupError.IsEmpty();
	}

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	FString GetStartupError() const { return StartupError; }

	/** Начинает вход с текущими Account и Password.
	 *
	 *  Вызывается и при старте уровня, когда включён bAutoLogin, и с экрана
	 *  входа после ввода. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void StartLogin();

#if WITH_DEV_AUTOMATION_TESTS
	bool LoadCharacterGroundFromBytesForTests(
		int32 TileWidth,
		int32 TileHeight,
		TConstArrayView<uint8> Bytes,
		FString& OutError);
	void GroundCharacterForTests(
		ACorsairsCharacter* Character,
		FIntPoint SourcePosition);
	ACorsairsCharacter* SpawnRemoteCharacterForTests(
		FIntPoint SourcePosition,
		FRotator Rotation);
	void HandleActorSeenForTests(const FCorsairsWorldActor& Actor);
	FIntVector GetRemoteRegistryCountsForTests() const
	{
		return FIntVector(
			WorldActors.Num(),
			WorldActorNames.Num(),
			WorldActorArchetypes.Num());
	}
#endif

private:
	UFUNCTION()
	void HandleMovementChanged(const FCorsairsMovementEvent& Event);

	UFUNCTION()
	void HandleStageChanged(ECorsairsLoginStage Stage, const FString& Message);

	/** Персонаж попал в поле зрения — ставим его в мир. */
	UFUNCTION()
	void HandleActorSeen(const FCorsairsWorldActor& Actor);

	/** Персонаж вышел из поля зрения — убираем. */
	UFUNCTION()
	void HandleActorLeft(int64 WorldId);

	/** Внешность персонажа изменилась — обновляем уже существующий актёр. */
	UFUNCTION()
	void HandleActorLookChanged(
		int64 WorldId,
		const FCorsairsCharacterLook& Look);

	bool ResolveAndApplyAppearance(
		ACorsairsCharacter* Character,
		int32 ArchetypeId,
		const FCorsairsCharacterLook& Look,
		const FString& ActorName,
		int64 WorldId);
	bool LoadCharacterGround(const FString& MapName);
	void GroundCharacter(
		ACorsairsCharacter* Character,
		FIntPoint SourcePosition);
	ACorsairsCharacter* SpawnRemoteCharacter(
		FIntPoint SourcePosition,
		FRotator Rotation);
	bool ActivateLocalCharacter(
		ACorsairsPlayerCharacter* Character,
		const FString& MapName,
		FIntPoint SourcePosition);
	void CleanupRemoteActors();
	void ScheduleSessionLogoutAfterGroundFailure();

	TUniquePtr<FCorsairsCharacterCatalog> CharacterCatalog;
	TUniquePtr<FCorsairsCharacterGround> CharacterGround;
	FString StartupError;

	UPROPERTY()
	TObjectPtr<UCorsairsSession> Session;

	/** Кого сервер держит в поле зрения, по идентификатору в мире.
	 *
	 *  Указатель нужен, чтобы убрать актёра, когда сервер сообщит об уходе:
	 *  искать его перебором всех актёров уровня на карте с сотней тысяч
	 *  объектов недопустимо. */
	UPROPERTY()
	TMap<int64, TObjectPtr<ACorsairsCharacter>> WorldActors;

	TMap<int64, FString> WorldActorNames;
	TMap<int64, int32> WorldActorArchetypes;
};
