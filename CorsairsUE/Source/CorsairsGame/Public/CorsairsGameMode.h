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
	UFUNCTION()
	void HandleStageChanged(ECorsairsLoginStage Stage, const FString& Message);

	/** Путь к модели тела по типу персонажа. Пустая строка — соответствия
	 *  нет, и тело останется невидимым. */
	FString ResolveBodyMesh(int32 TypeId) const;

	UPROPERTY()
	TObjectPtr<UCorsairsSession> Session;
};
