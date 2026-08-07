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
	if (!Root->TryGetNumberField(TEXT("gridWidth"), TileWidth) ||
		!Root->TryGetNumberField(TEXT("gridHeight"), TileHeight) ||
		TileWidth <= 0 || TileHeight <= 0)
	{
		OutError = FString::Printf(
			TEXT("%s: gridWidth/gridHeight должны быть положительными целыми числами"),
			*JsonPath);
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

	const int64 ExpectedSize =
		static_cast<int64>(TileWidth) *
		static_cast<int64>(TileHeight) *
		QuadrantsPerTile;
	if (ExpectedSize > MAX_int32 || Bytes.Num() != ExpectedSize)
	{
		OutError = FString::Printf(
			TEXT("ожидалось %lld bytes для raster %dx%d, получено %d"),
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
		static_cast<double>(SourcePosition.X),
		-static_cast<double>(SourcePosition.Y),
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
