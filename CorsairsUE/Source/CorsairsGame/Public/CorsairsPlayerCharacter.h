#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

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
class CORSAIRSGAME_API ACorsairsPlayerCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ACorsairsPlayerCharacter();

	/** Ставит телу скелетный меш по пути ассета. Пустой путь оставляет тело
	 *  невидимым, что лучше подстановки чужой модели: так сразу видно, что
	 *  соответствие не разрешилось. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool SetBodyMesh(const FString& AssetPath);

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Кому отправлять путь движения. Задаётся режимом игры после входа. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void AttachSession(UCorsairsSession* InSession);

protected:
	/** Камера на кронштейне: обзор от третьего лица, как в оригинале. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Corsairs")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Corsairs")
	TObjectPtr<UCameraComponent> FollowCamera;

private:
	/** Отправляет серверу путь от предыдущего сообщённого положения к текущему.
	 *
	 *  Сервер ждёт путь, а не мгновенную позицию: он сам проигрывает
	 *  перемещение по времени и проверяет проходимость. Отправка идёт не
	 *  каждый кадр, а по накоплению смещения — иначе канал забивается
	 *  сообщениями о сдвиге в сантиметр. */
	void ReportMovement();

	void MoveForward(float Value);
	void MoveRight(float Value);
	void TurnCamera(float Value);
	void PitchCamera(float Value);

	UPROPERTY()
	TObjectPtr<UCorsairsSession> Session;

	/** Положение, о котором серверу уже сообщено, в координатах карты. */
	FIntPoint ReportedPosition = FIntPoint::ZeroValue;
	bool bHasReported = false;

	float TimeSinceReport = 0.0f;
};
