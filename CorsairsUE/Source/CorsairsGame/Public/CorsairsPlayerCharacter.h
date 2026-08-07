#pragma once

#include "CoreMinimal.h"
#include "CorsairsActionReducer.h"
#include "CorsairsCharacter.h"
#include "CorsairsMovementInputGate.h"

#include "CorsairsPlayerCharacter.generated.h"

class UCameraComponent;
class UCorsairsSession;
class USpringArmComponent;

/**
 * Персонаж игрока: тело, камера и движение.
 *
 * Наследуется от ACharacter ради CharacterMovementComponent — он уже умеет
 * ходьбу по поверхности, ступеньки и гравитацию. Переносить эту механику из
 * MindPower3D не нужно: сервер всё равно остаётся источником истины о
 * положении, а клиентское движение служит предсказанием.
 *
 * Модель подбирается по типу персонажа из ответа на вход. Соответствие «тип ->
 * скелет и наборы кожи» лежит в таблице character_models игровых данных.
 */
UCLASS()
class CORSAIRSGAME_API ACorsairsPlayerCharacter : public ACorsairsCharacter
{
	GENERATED_BODY()

public:
	ACorsairsPlayerCharacter();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Кому отправлять путь движения. Задаётся режимом игры после входа. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void AttachSession(UCorsairsSession* InSession);

#if !UE_BUILD_SHIPPING
	void ApplyMovementAxisForProbe(FName AxisName, float Value);
#endif

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	/** Привязывает персонажа к карте высот указанной карты. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool UseTerrainHeights(const FString& MapName);

protected:

	/** Камера на кронштейне: обзор от третьего лица, как в оригинале. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Corsairs")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Corsairs")
	TObjectPtr<UCameraComponent> FollowCamera;

private:
	/** Передаёт сессии текущую predicted position.
	 *
	 *  Сессия сама строит путь от последней подтверждённой сервером точки и
	 *  применяет порог смещения. Результат socket send не меняет authoritative
	 *  baseline до terminal event. */
	void ReportMovement();

	/** Передаёт нажатие экрану входа, пока тот принимает ввод.
	 *
	 *  Символы приходят через KeyPressed компонента ввода, а не через привязки
	 *  действий: под каждую букву заводить действие бессмысленно, а UMG с его
	 *  полями ввода потребовал бы ассета, который из кода не создать. */
	void HandleTypedKey(FKey Key);

	void MoveForward(float Value);
	void MoveRight(float Value);
	void TurnCamera(float Value);
	void PitchCamera(float Value);
	void UpdateMovementPredictionState();
	void ReportMovementSpeedProtocolError();

	UFUNCTION()
	void HandleMovementChanged(const FCorsairsMovementEvent& Event);

	UPROPERTY()
	TObjectPtr<UCorsairsSession> Session;

	FCorsairsMovementInputGate MovementInputGate;

	/** Карта высот текущей карты. Персонаж удерживается на ней вручную:
	 *  рельеф пришёл из glTF без физических данных, и провалиться сквозь
	 *  землю иначе — вопрос одного кадра. Оригинальный движок делал так же и
	 *  физикой рельефа не пользовался вовсе. */
	UPROPERTY()
	TObjectPtr<class UCorsairsTerrainHeights> TerrainHeights;

	/** Разовая диагностика вида: копит время и срабатывает один раз. */
	float DiagnosticTimer = 0.0f;
	bool bDiagnosticLogged = false;
	bool bHasValidMovementSpeed = false;
	bool bUsesEventMovementSpeedFallback = false;
	bool bMovementSpeedProtocolErrorReported = false;

	float TimeSinceReport = 0.0f;
};
