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
 * Наследуется от ACharacter ради CharacterMovementComponent, капсулы и
 * стандартного input path. Высота берётся напрямую из half-meter block
 * raster без гравитации: сервер остаётся источником истины о положении, а
 * локальное движение служит предсказанием.
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
	virtual void AttachCharacterGround(
		const FCorsairsCharacterGround* InGround) override;

#if !UE_BUILD_SHIPPING
	void ApplyMovementAxisForProbe(FName AxisName, float Value);
#endif

protected:
	virtual void BeginPlay() override;
	virtual void PawnClientRestart() override;
	virtual void OnRep_Controller() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:

	/** Камера на кронштейне: обзор от третьего лица, как в оригинале. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Corsairs")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Corsairs")
	TObjectPtr<UCameraComponent> FollowCamera;

private:
	/** Один раз применяет профиль камеры после появления локального controller. */
	void ApplyInitialCameraControlRotation();

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
	void ApplyMovementPredictionLock(bool bLocked);
	void ReportMovementSpeedProtocolError();

	UFUNCTION()
	void HandleMovementChanged(const FCorsairsMovementEvent& Event);

	UFUNCTION()
	void HandleMovementAuthorityChanged(bool bLocked, int64 Epoch);

	UPROPERTY()
	TObjectPtr<UCorsairsSession> Session;

	FCorsairsMovementInputGate MovementInputGate;

	/** Разовая диагностика вида: копит время и срабатывает один раз. */
	float DiagnosticTimer = 0.0f;
	bool bDiagnosticLogged = false;
	bool bHasValidMovementSpeed = false;
	bool bSessionMovementAuthorityLocked = false;
	/** Explicit detach is fail-closed; an unattached test pawn keeps the
	 *  characterized legacy-input behavior until a session is assigned. */
	bool bPredictionDisabledWithoutSession = false;
	bool bPredictionLocked = false;
	bool bMovementSpeedProtocolErrorReported = false;
	bool bInitialCameraControlRotationApplied = false;
	int64 LastMovementAuthorityEpoch = 0;

	float TimeSinceReport = 0.0f;
};
