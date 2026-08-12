#pragma once

#include "CoreMinimal.h"

#include "CorsairsWorldActor.generated.h"

inline constexpr int32 CorsairsEquipSlotCount = 34;

USTRUCT(BlueprintType)
struct FCorsairsCharacterLook
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 SynType = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 TypeId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 HairId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	bool bIsBoat = false;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	TArray<int32> EquipIds;
};

/** Серверные признаки, ограничивающие выбор цели. */
USTRUCT(BlueprintType)
struct FCorsairsTargetPolicy
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 GuildId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 TeamLeaderId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 SideId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 PkControl = 0;
};

/** Персонаж, попавший в поле зрения: другой игрок, NPC или монстр. */
USTRUCT(BlueprintType)
struct FCorsairsWorldActor
{
	GENERATED_BODY()

	/** Идентификатор в мире. По нему приходят все дальнейшие сообщения. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 WorldId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FString Name;

	/** Положение в координатах карты. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FIntPoint Position = FIntPoint::ZeroValue;

	/** Угол поворота в целых градусах. Оригинал кладёт пришедшее значение
	 *  прямо в персонажа (`pCha->setYaw(sAngle)` в NetProtocol.cpp) и
	 *  переводит в радианы умножением на пи и делением на сто восемьдесят —
	 *  делителя в этом пути нет. Та же единица у построек и предметов на
	 *  земле; отдельно стоят только эффекты, где угол хранится в сотых долях
	 *  радиана. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 Angle = 0;

	/** Тип модели. Разворачивается в тело через таблицу персонажей. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 TypeId = 0;

	/** Управляющий тип: игрок, NPC, монстр. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 CtrlType = 0;

	/** Запись в таблице персонажей. У NPC именно она задаёт модель: поле
	 *  внешности, которым пользуются игроки, у них пустое. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 ChaId = 0;

	/** Ключ сущности на сервере. Идёт в паре с идентификатором: одного
	 *  идентификатора серверу мало, он сверяет обе половины. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 Handle = 0;

	/** Здоровье. Обновляется итогами ударов — по нему и видно урон. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 Hp = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	double MovementSpeedCmPerSecond = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FCorsairsCharacterLook Look;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FCorsairsTargetPolicy TargetPolicy;

	/** Идентификатор основного персонажа человека. Общий для его human/boat
	 *  representations и не обязан совпадать с WorldId текущей сущности. Поле
	 *  добавлено в хвост DTO, чтобы не менять смысл старых aggregate fixtures. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 HumanId = 0;
};
