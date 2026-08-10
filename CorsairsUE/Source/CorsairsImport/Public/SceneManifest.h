#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "SceneManifest.generated.h"

// Один объект, размещённый на карте. Соответствует записи в
// `<карта>.objects.json`, который пишет AssetConverter.
USTRUCT(BlueprintType)
struct CORSAIRSIMPORT_API FCorsairsPlacedObject
{
	GENERATED_BODY()

	// Идентификатор модели. Имя файла модели ищется по нему в таблице
	// `scene_objects` игровых данных — сам манифест имя не содержит.
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 ModelId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 Type = 0;

	// Мировые координаты карты в исходных единицах (1 тайл = 100 единиц).
	// Пересчёт в сантиметры UE делается при расстановке, где известен масштаб.
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 X = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 Y = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 HeightOff = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 Yaw = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 Scale = 0;
};

// Разобранный манифест сцены.
USTRUCT(BlueprintType)
struct CORSAIRSIMPORT_API FCorsairsSceneManifest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 SectionCntX = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 SectionCntY = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 SectionWidth = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 SectionHeight = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	TArray<FCorsairsPlacedObject> Objects;
};

UCLASS()
class CORSAIRSIMPORT_API UCorsairsSceneManifestLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Читает `<карта>.objects.json`. Возвращает false, если файл не открылся
	// или структура не соответствует ожидаемой; причина — в OutError.
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	static bool LoadSceneManifest(const FString& FilePath,
								  FCorsairsSceneManifest& OutManifest,
								  FString& OutError);

	// Точка карты в координатах мира, в сантиметрах.
	//
	// Единственное место, где выполняется этот перевод. Им пользуются и
	// расстановка сцены, и появление сетевых сущностей: раньше формула жила
	// в обеих по отдельности, разъехалась, и персонажи вставали в зеркальном
	// углу мира относительно домов.
	//
	// Оси переносятся один в один. Обе системы левосторонние с высотой по Z,
	// а две перестановки осей на пути модели — в конвертере и в импортёре —
	// гасят друг друга.
	//
	// Исходные координаты карты — целые, 100 единиц на тайл. `UnitsPerTile`
	// задаёт, сколько сантиметров UE занимает один тайл; при 100 масштаб
	// получается один к одному.
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	static FVector MapPointToWorld(int32 MapX, int32 MapY, float HeightCm,
								   float UnitsPerTile = 100.0f);

	// Точка мира обратно в координаты карты — то, что уходит на сервер.
	//
	// Обратная к MapPointToWorld и обязана меняться вместе с ней. Стоит им
	// разойтись, и персонаж будет стоять в одном месте, а серверу сообщать
	// другое: движение начнёт отвергаться проверкой проходимости, причём
	// молча — сервер просто не ответит.
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	static FIntPoint WorldPointToMap(const FVector& WorldLocation,
									 float UnitsPerTile = 100.0f);

	// Угол карты в поворот мира. В протоколе и в файлах карты он хранится
	// одинаково — целыми градусами.
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	static FRotator MapYawToWorld(int32 Yaw);

	// Мировая позиция объекта сцены. Обёртка над MapPointToWorld.
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	static FVector GetObjectLocation(const FCorsairsPlacedObject& Object,
									 float UnitsPerTile = 100.0f);

	// Поворот объекта сцены. Обёртка над MapYawToWorld.
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	static FRotator GetObjectRotation(const FCorsairsPlacedObject& Object);
};
