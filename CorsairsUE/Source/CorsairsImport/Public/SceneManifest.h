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

	// Мировая позиция объекта в сантиметрах UE.
	//
	// Исходные координаты карты — целые, 100 единиц на тайл. `UnitsPerTile`
	// задаёт, сколько сантиметров UE занимает один тайл; при 100 масштаб
	// получается один к одному.
	//
	// Координаты переводятся жёстким поворотом Q(x, y, z) = (-y, x, z) —
	// поворотом XY на +90 градусов с определителем +1. Отражения нет:
	// ориентация плоскости сохраняется, поэтому обход треугольников, знаки
	// углов и «право/лево» сцены не переворачиваются. Тот же Q действует на
	// рельеф, персонажей и камеру, эталон — Scripts/scene_coordinate_basis.py.
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	static FVector GetObjectLocation(const FCorsairsPlacedObject& Object,
									 float UnitsPerTile = 100.0f);

	// Поворот объекта вокруг вертикали.
	//
	// Yaw в манифесте — целые градусы (не десятые доли). Результат равен
	// source_yaw - 90: собственный угол легаси-объекта равен θ-180
	// (sources/Engine/Model/lwObjectMethod.cpp:41), а Q добавляет +90.
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	static FRotator GetObjectRotation(const FCorsairsPlacedObject& Object);
};
