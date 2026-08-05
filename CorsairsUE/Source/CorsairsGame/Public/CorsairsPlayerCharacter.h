#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "CorsairsPlayerCharacter.generated.h"

class UCameraComponent;
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

protected:
	/** Камера на кронштейне: обзор от третьего лица, как в оригинале. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Corsairs")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Corsairs")
	TObjectPtr<UCameraComponent> FollowCamera;

private:
	void MoveForward(float Value);
	void MoveRight(float Value);
	void TurnCamera(float Value);
	void PitchCamera(float Value);
};
