#pragma once

#include "CoreMinimal.h"
#include "CorsairsSession.h"
#include "GameFramework/GameModeBase.h"

#include "CorsairsGameMode.generated.h"

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

	/** Начинает вход с текущими Account и Password.
	 *
	 *  Вызывается и при старте уровня, когда включён bAutoLogin, и с экрана
	 *  входа после ввода. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void StartLogin();

private:
	/** Карта высот текущей карты — общая для своего персонажа и для всех, кого
	 *  показывает сервер. Трассировка тут не годится: рельеф пришёл из glTF
	 *  без физической формы, и луч проходит сквозь него, оставляя каждого на
	 *  случайной высоте. */
	UPROPERTY()
	TObjectPtr<class UCorsairsTerrainHeights> TerrainHeights;

	UFUNCTION()
	void HandleStageChanged(ECorsairsLoginStage Stage, const FString& Message);

	/** Персонаж попал в поле зрения — ставим его в мир. */
	UFUNCTION()
	void HandleActorSeen(const FCorsairsWorldActor& Actor);

	/** Персонаж вышел из поля зрения — убираем. */
	UFUNCTION()
	void HandleActorLeft(int64 WorldId);

	/** Путь к модели тела по типу персонажа. Пустая строка — соответствия
	 *  нет, и тело останется невидимым. */
	FString ResolveBodyMesh(int32 TypeId) const;

	/** Пути ко всем частям тела: персонаж собирается из пяти кусков, а не из
	 *  одной модели. Первая часть основная, остальные крепятся к её позе. */
	TArray<FString> ResolveBodyParts(int32 TypeId) const;

	/** Значение поля из таблицы персонажей по типу. Пустая строка — записи
	 *  или поля нет. */
	FString ResolveField(int32 TypeId, const TCHAR* Field) const;

	UPROPERTY()
	TObjectPtr<UCorsairsSession> Session;

	/** Кого сервер держит в поле зрения, по идентификатору в мире.
	 *
	 *  Указатель нужен, чтобы убрать актёра, когда сервер сообщит об уходе:
	 *  искать его перебором всех актёров уровня на карте с сотней тысяч
	 *  объектов недопустимо. */
	UPROPERTY()
	TMap<int64, TObjectPtr<AActor>> WorldActors;
};
