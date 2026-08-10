#include "TerrainHeights.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsHeights, Log, All);

namespace
{
	/** Сто сантиметров мира на одну клетку карты. */
	constexpr double UnitsPerCell = 100.0;

	/** Исходная высота — знаковый байт в единицах по десять сантиметров.
	 *  При экспорте она растянута в беззнаковое шестнадцатибитное поле
	 *  формулой (raw + 128) * 256, здесь выполняется обратное. */
	constexpr double CentimetersPerUnit = 10.0;
	constexpr int32 HeightBias = 128;
	constexpr int32 HeightScale = 256;
}

bool UCorsairsTerrainHeights::Load(const FString& MapName)
{
	if (LoadedMap == MapName && Side > 0)
	{
		return true;
	}

	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("Data"), TEXT("Heights"),
										 MapName + TEXT(".height.r16"));
	TArray<uint8> Raw;
	if (!FFileHelper::LoadFileToArray(Raw, *Path))
	{
		UE_LOG(LogCorsairsHeights, Warning, TEXT("карта высот не найдена: %s"), *Path);
		return false;
	}

	const int32 Count = Raw.Num() / static_cast<int32>(sizeof(uint16));
	const int32 Root = FMath::RoundToInt(FMath::Sqrt(static_cast<double>(Count)));
	if (Root <= 0 || Root * Root != Count)
	{
		UE_LOG(LogCorsairsHeights, Warning,
			   TEXT("карта высот %s не квадратная: значений %d"), *MapName, Count);
		return false;
	}

	Cells.SetNumUninitialized(Count);
	FMemory::Memcpy(Cells.GetData(), Raw.GetData(), Raw.Num());
	Side = Root;
	LoadedMap = MapName;

	UE_LOG(LogCorsairsHeights, Log, TEXT("карта высот %s: сетка %dx%d"), *MapName, Side, Side);
	return true;
}

double UCorsairsTerrainHeights::HeightAt(double WorldX, double WorldY) const
{
	if (Side <= 0)
	{
		return 0.0;
	}

	// Сетка высот проиндексирована исходными координатами карты: столбец —
	// source X, строка — source Y. Мир построен поворотом Q(x, y) = (-y, x),
	// значит обратный переход — source X = WorldY, source Y = -WorldX.
	// Раньше здесь стояла пара к зеркалу (столбец от WorldX, строка от
	// -WorldY); с Q она давала высоту из точки, отражённой относительно
	// диагонали.
	const int32 Col = FMath::Clamp(FMath::FloorToInt(WorldY / UnitsPerCell), 0, Side - 1);
	const int32 Row = FMath::Clamp(FMath::FloorToInt(-WorldX / UnitsPerCell), 0, Side - 1);

	const uint16 Stored = Cells[Row * Side + Col];
	const int32 Original = static_cast<int32>(Stored) / HeightScale - HeightBias;
	return static_cast<double>(Original) * CentimetersPerUnit;
}
