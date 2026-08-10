#include "CorsairsCharacterGround.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsCharacterGround, Log, All);

namespace
{
	constexpr int32 HalfMeterCm = 50;
	constexpr int32 QuadrantsPerTile = 4;
	constexpr uint8 MagnitudeMask = 0x3f;
	constexpr uint8 SignMask = 0x40;
	constexpr uint8 BlockMask = 0x80;
	constexpr double HeightStepCm = 5.0;

	FString MetadataPath(const FString& MapName)
	{
		return FPaths::ProjectDir() /
			TEXT("Data/Heights") /
			(MapName + TEXT(".terrain.json"));
	}

	FString BlockPath(const FString& MapName)
	{
		return FPaths::ProjectDir() /
			TEXT("Data/Heights") /
			(MapName + TEXT(".block.raw"));
	}

	bool ReadPositiveDimension(
		const TSharedPtr<FJsonObject>& Root,
		const FString& JsonPath,
		const TCHAR* Field,
		int32& OutValue,
		FString& OutError)
	{
		const TSharedPtr<FJsonValue>* Value = Root->Values.Find(Field);
		if (Value == nullptr || !Value->IsValid() ||
			(*Value)->Type != EJson::Number)
		{
			OutError = FString::Printf(
				TEXT("%s.%s должен быть положительным целым числом"),
				*JsonPath,
				Field);
			return false;
		}

		const double Number = (*Value)->AsNumber();
		if (!FMath::IsFinite(Number) || Number < 1.0 ||
			Number > static_cast<double>(MAX_int32) ||
			FMath::FloorToDouble(Number) != Number)
		{
			OutError = FString::Printf(
				TEXT("%s.%s должен быть положительным целым числом не больше %d"),
				*JsonPath,
				Field,
				MAX_int32);
			return false;
		}

		OutValue = static_cast<int32>(Number);
		return true;
	}
}

bool FCorsairsCharacterGround::Load(
	const FString& MapName,
	FString& OutError)
{
	Reset();
	OutError.Empty();

	const FString JsonPath = MetadataPath(MapName);
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *JsonPath))
	{
		OutError = FString::Printf(
			TEXT("метаданные character ground не найдены: %s"),
			*JsonPath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(
			TEXT("метаданные character ground содержат некорректный JSON: %s"),
			*JsonPath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}

	int32 TileWidth = 0;
	int32 TileHeight = 0;
	if (!ReadPositiveDimension(
			Root,
			JsonPath,
			TEXT("gridWidth"),
			TileWidth,
			OutError) ||
		!ReadPositiveDimension(
			Root,
			JsonPath,
			TEXT("gridHeight"),
			TileHeight,
			OutError))
	{
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}

	const FString RawPath = BlockPath(MapName);
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *RawPath))
	{
		OutError = FString::Printf(
			TEXT("character ground raster не найден: %s"),
			*RawPath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}

	if (!LoadFromBytes(TileWidth, TileHeight, Bytes, OutError))
	{
		OutError = FString::Printf(
			TEXT("%s: %s"),
			*RawPath,
			*OutError);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}

	UE_LOG(
		LogCorsairsCharacterGround,
		Log,
		TEXT("character ground %s: source grid %dx%d"),
		*MapName,
		Width,
		Height);
	return true;
}

bool FCorsairsCharacterGround::LoadFromBytes(
	const int32 TileWidth,
	const int32 TileHeight,
	const TConstArrayView<uint8> Bytes,
	FString& OutError)
{
	Reset();
	OutError.Empty();

	if (TileWidth <= 0 || TileHeight <= 0)
	{
		OutError = TEXT("размер source-tile raster должен быть положительным");
		return false;
	}

	const uint64 TileCount =
		static_cast<uint64>(TileWidth) *
		static_cast<uint64>(TileHeight);
	constexpr uint64 MaxTileCount =
		static_cast<uint64>(MAX_int32) / QuadrantsPerTile;
	if (TileCount > MaxTileCount)
	{
		OutError = FString::Printf(
			TEXT("raster %dx%d слишком велик: максимум %llu source tiles"),
			TileWidth,
			TileHeight,
			MaxTileCount);
		return false;
	}

	const int32 ExpectedSize = static_cast<int32>(
		TileCount * QuadrantsPerTile);
	if (Bytes.Num() != ExpectedSize)
	{
		OutError = FString::Printf(
			TEXT("ожидалось %d bytes для raster %dx%d, получено %d"),
			ExpectedSize,
			TileWidth,
			TileHeight,
			Bytes.Num());
		return false;
	}

	Cells.Append(Bytes.GetData(), Bytes.Num());
	Width = TileWidth;
	Height = TileHeight;
	return true;
}

FCorsairsCharacterCell FCorsairsCharacterGround::Sample(
	const FIntPoint SourcePosition) const
{
	if (!IsLoaded() || SourcePosition.X < 0 || SourcePosition.Y < 0)
	{
		return {};
	}

	const int32 GridX = SourcePosition.X / HalfMeterCm;
	const int32 GridY = SourcePosition.Y / HalfMeterCm;
	if (GridX >= Width * 2 || GridY >= Height * 2)
	{
		return {};
	}

	const int32 TileX = GridX / 2;
	const int32 TileY = GridY / 2;
	const int32 Quadrant = (GridY % 2) * 2 + (GridX % 2);
	const int32 Index =
		(TileY * Width + TileX) * QuadrantsPerTile + Quadrant;
	const uint8 Encoded = Cells[Index];
	const double Magnitude =
		static_cast<double>(Encoded & MagnitudeMask) * HeightStepCm;

	FCorsairsCharacterCell Cell;
	Cell.HeightCm = (Encoded & SignMask) != 0 ? -Magnitude : Magnitude;
	Cell.bBlocked = (Encoded & BlockMask) != 0;
	return Cell;
}

FVector FCorsairsCharacterGround::ActorCenter(
	const FIntPoint SourcePosition,
	const double ScaledCapsuleHalfHeight) const
{
	const FCorsairsCharacterCell Cell = Sample(SourcePosition);
	return FVector(
		-static_cast<double>(SourcePosition.Y),
		static_cast<double>(SourcePosition.X),
		Cell.HeightCm + ScaledCapsuleHalfHeight);
}

bool FCorsairsCharacterGround::IsLoaded() const
{
	return Width > 0 && Height > 0 &&
		Cells.Num() == Width * Height * QuadrantsPerTile;
}

void FCorsairsCharacterGround::Reset()
{
	Cells.Reset();
	Width = 0;
	Height = 0;
}
