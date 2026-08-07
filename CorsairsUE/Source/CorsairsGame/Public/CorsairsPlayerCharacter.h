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

	/** Ставит телу анимацию и включает её проигрывание по кругу.
	 *
	 *  Дорожка применима, только если её скелет совпадает со скелетом тела:
	 *  оба приходят из одного `.lab`, и связывает их совпадение имён и
	 *  структуры костей. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool SetBodyAnimation(const FString& AssetPath);

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
	virtual void BeginPlay() override;

public:
	/** Добавляет часть тела поверх основной модели.
	 *
	 *  Персонаж собирается из пяти моделей: тело, голова, руки, одежда.
	 *  Дополнительные части — отдельные компоненты, разделяющие позу с
	 *  основным: скелет у них общий, и двигаться они обязаны как одно целое. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool AddBodyPart(const FString& AssetPath);

	/** Завершает сборку тела: приводит фигуру к росту капсулы.
	 *
	 *  Вызывается после того, как добавлены все части. Раньше нельзя: рост
	 *  определяется по всей сборке, а первая часть — это голова, и мерка по
	 *  ней растягивала персонажа впятеро. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void FinishBody();

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
	/** Отправляет серверу путь от предыдущего сообщённого положения к текущему.
	 *
	 *  Сервер ждёт путь, а не мгновенную позицию: он сам проигрывает
	 *  перемещение по времени и проверяет проходимость. Отправка идёт не
	 *  каждый кадр, а по накоплению смещения — иначе канал забивается
	 *  сообщениями о сдвиге в сантиметр. */
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

	UPROPERTY()
	TObjectPtr<UCorsairsSession> Session;

	/** Положение, о котором серверу уже сообщено, в координатах карты. */
	FIntPoint ReportedPosition = FIntPoint::ZeroValue;

	/** Дополнительные части тела. Хранятся, чтобы снимать их при смене
	 *  внешности — иначе прежние остались бы висеть поверх новых. */
	UPROPERTY()
	TArray<TObjectPtr<class USkeletalMeshComponent>> BodyParts;

	/** Карта высот текущей карты. Персонаж удерживается на ней вручную:
	 *  рельеф пришёл из glTF без физических данных, и провалиться сквозь
	 *  землю иначе — вопрос одного кадра. Оригинальный движок делал так же и
	 *  физикой рельефа не пользовался вовсе. */
	UPROPERTY()
	TObjectPtr<class UCorsairsTerrainHeights> TerrainHeights;

	/** Камеру нужно навести на модель — замером в первом кадре. */
	bool bCameraNeedsAiming = false;

	/** Разовая диагностика вида: копит время и срабатывает один раз. */
	float DiagnosticTimer = 0.0f;
	bool bDiagnosticLogged = false;
	bool bHasReported = false;

	float TimeSinceReport = 0.0f;
};
