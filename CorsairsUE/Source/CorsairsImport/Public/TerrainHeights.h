#pragma once

#include "CoreMinimal.h"
#include "TerrainHeights.generated.h"

/** Карта высот одной игровой карты.
 *
 *  Оригинальный движок не опирался на физику: высота персонажа бралась прямо
 *  из карты высот, той же, из которой построена видимая земля. Здесь то же
 *  самое — и по той же причине. Коллизия рельефа в Unreal у нас не строится
 *  (меши приходят из glTF без физических данных), а держать персонажа на
 *  земле надо.
 */
UCLASS(BlueprintType)
class CORSAIRSIMPORT_API UCorsairsTerrainHeights : public UObject
{
	GENERATED_BODY()

public:
	/** Загружает высоты карты из `Data/Heights/<карта>.height.r16`.
	 *
	 *  Файл — квадратная сетка беззнаковых 16-битных значений, по одному на
	 *  клетку карты. Исходная высота восьмибитная и растянута при экспорте,
	 *  поэтому обратное преобразование — деление на 256 со сдвигом. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool Load(const FString& MapName);

	/** Высота земли под мировой точкой, в сантиметрах Unreal.
	 *
	 *  Сто сантиметров мира на клетку. Сетка проиндексирована исходными
	 *  координатами карты, а мир получен поворотом Q(x, y) = (-y, x), поэтому
	 *  обратный переход берёт source X из WorldY, а source Y из -WorldX — так
	 *  же, как при размещении объектов. */
	UFUNCTION(BlueprintPure, Category = "Corsairs")
	double HeightAt(double WorldX, double WorldY) const;

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	bool IsLoaded() const { return Side > 0; }

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	FString GetMapName() const { return LoadedMap; }

private:
	TArray<uint16> Cells;
	int32 Side = 0;
	FString LoadedMap;
};
