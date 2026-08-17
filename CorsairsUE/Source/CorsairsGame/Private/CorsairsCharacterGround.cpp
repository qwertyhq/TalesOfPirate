#include "CorsairsCharacterGround.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <openssl/sha.h>

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsCharacterGround, Log, All);

namespace
{
	constexpr int32 HalfMeterCm = 50;
	constexpr int32 QuadrantsPerTile = 4;
	constexpr uint8 MagnitudeMask = 0x3f;
	constexpr uint8 SignMask = 0x40;
	constexpr uint8 BlockMask = 0x80;
	constexpr double HeightStepCm = 5.0;
	constexpr uint16 KnownRegionMask = 0x007f;
	constexpr uint16 LandRegionMask = 0x0001;
	constexpr uint16 BridgeRegionMask = 0x0008;

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

	FString RegionPath(const FString& MapName)
	{
		return FPaths::ProjectDir() /
			TEXT("Data/Heights") /
			(MapName + TEXT(".region.raw"));
	}

	FString HeightPath(const FString& MapName)
	{
		return FPaths::ProjectDir() /
			TEXT("Data/Heights") /
			(MapName + TEXT(".height.r16"));
	}

	FString RuntimePath(const FString& MapName)
	{
		return FPaths::ProjectDir() /
			TEXT("Data/Heights") /
			(MapName + TEXT(".runtime.json"));
	}

	bool ReadExpectedFile(
		const TSharedPtr<FJsonObject>& Files,
		const FString& JsonPath,
		const TCHAR* Field,
		const FString& ExpectedName,
		FString& OutSha256,
		int32& OutSize,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (!Files->TryGetObjectField(Field, Value) ||
			Value == nullptr || !Value->IsValid())
		{
			OutError = FString::Printf(
				TEXT("%s.files.%s должен быть объектом"),
				*JsonPath,
				Field);
			return false;
		}
		FString Name;
		double Size = 0.0;
		if (!(*Value)->TryGetStringField(TEXT("name"), Name) ||
			Name != ExpectedName ||
			!(*Value)->TryGetStringField(TEXT("sha256"), OutSha256) ||
			OutSha256.Len() != 64 ||
			!(*Value)->TryGetNumberField(TEXT("sizeBytes"), Size) ||
			!FMath::IsFinite(Size) || Size < 1.0 ||
			Size > static_cast<double>(MAX_int32) ||
			FMath::FloorToDouble(Size) != Size)
		{
			OutError = FString::Printf(
				TEXT("%s.files.%s нарушает runtime contract"),
				*JsonPath,
				Field);
			return false;
		}
		for (const TCHAR Character : OutSha256)
		{
			if (!FChar::IsHexDigit(Character) || FChar::IsUpper(Character))
			{
				OutError = FString::Printf(
					TEXT("%s.files.%s содержит некорректный SHA-256"),
					*JsonPath,
					Field);
				return false;
			}
		}
		OutSize = static_cast<int32>(Size);
		return true;
	}

	bool ValidateBytes(
		const TConstArrayView<uint8> Bytes,
		const int32 ExpectedSize,
		const FString& ExpectedSha256,
		const FString& Path,
		FString& OutError)
	{
		if (Bytes.Num() != ExpectedSize)
		{
			OutError = FString::Printf(
				TEXT("%s: ожидалось %d bytes, получено %d"),
				*Path,
				ExpectedSize,
				Bytes.Num());
			return false;
		}
		uint8 Digest[SHA256_DIGEST_LENGTH];
		if (SHA256(Bytes.GetData(), Bytes.Num(), Digest) == nullptr)
		{
			OutError = FString::Printf(
				TEXT("%s: SHA-256 не удалось вычислить"),
				*Path);
			return false;
		}
		FString ActualSha256;
		ActualSha256.Reserve(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			ActualSha256 += FString::Printf(TEXT("%02x"), Byte);
		}
		if (ActualSha256 != ExpectedSha256)
		{
			OutError = FString::Printf(
				TEXT("%s: SHA-256 не совпадает с runtime contract"),
				*Path);
			return false;
		}
		return true;
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
	TArray<uint8> JsonBytes;
	if (!FFileHelper::LoadFileToArray(JsonBytes, *JsonPath))
	{
		OutError = FString::Printf(
			TEXT("метаданные character ground не найдены: %s"),
			*JsonPath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
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

	const FString ContractPath = RuntimePath(MapName);
	FString ContractText;
	if (!FFileHelper::LoadFileToString(ContractText, *ContractPath))
	{
		OutError = FString::Printf(
			TEXT("runtime contract character ground не найден: %s"),
			*ContractPath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
	TSharedPtr<FJsonObject> Contract;
	const TSharedRef<TJsonReader<>> ContractReader =
		TJsonReaderFactory<>::Create(ContractText);
	if (!FJsonSerializer::Deserialize(ContractReader, Contract) ||
		!Contract.IsValid())
	{
		OutError = FString::Printf(
			TEXT("runtime contract содержит некорректный JSON: %s"),
			*ContractPath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
	const TSharedPtr<FJsonObject>* Files = nullptr;
	FString ContractMap;
	double SchemaVersion = 0.0;
	int32 ContractWidth = 0;
	int32 ContractHeight = 0;
	if (!Contract->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion) ||
		SchemaVersion != 1.0 ||
		!Contract->TryGetStringField(TEXT("map"), ContractMap) ||
		ContractMap != MapName ||
		!ReadPositiveDimension(
			Contract, ContractPath, TEXT("gridWidth"), ContractWidth, OutError) ||
		!ReadPositiveDimension(
			Contract, ContractPath, TEXT("gridHeight"), ContractHeight, OutError) ||
		ContractWidth != TileWidth || ContractHeight != TileHeight ||
		!Contract->TryGetObjectField(TEXT("files"), Files) ||
		Files == nullptr || !Files->IsValid())
	{
		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("runtime contract не соответствует карте %s"),
				*MapName);
		}
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}

	FString BlockSha256;
	FString RegionSha256;
	FString MetadataSha256;
	FString HeightSha256;
	int32 BlockSize = 0;
	int32 RegionSize = 0;
	int32 MetadataSize = 0;
	int32 HeightSize = 0;
	if (!ReadExpectedFile(
			*Files, ContractPath, TEXT("height"),
			MapName + TEXT(".height.r16"),
			HeightSha256, HeightSize, OutError) ||
		!ReadExpectedFile(
			*Files, ContractPath, TEXT("block"),
			MapName + TEXT(".block.raw"),
			BlockSha256, BlockSize, OutError) ||
		!ReadExpectedFile(
			*Files, ContractPath, TEXT("region"),
			MapName + TEXT(".region.raw"),
			RegionSha256, RegionSize, OutError) ||
		!ReadExpectedFile(
			*Files, ContractPath, TEXT("terrainMetadata"),
			MapName + TEXT(".terrain.json"),
			MetadataSha256, MetadataSize, OutError))
	{
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
	const FString SurfacePath = HeightPath(MapName);
	TArray<uint8> HeightBytes;
	if (!FFileHelper::LoadFileToArray(HeightBytes, *SurfacePath))
	{
		OutError = FString::Printf(
			TEXT("height raster не найден: %s"),
			*SurfacePath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
	if (!ValidateBytes(
			HeightBytes, HeightSize, HeightSha256, SurfacePath, OutError))
	{
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
	if (!ValidateBytes(
			JsonBytes, MetadataSize, MetadataSha256, JsonPath, OutError))
	{
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}

	const FString RawPath = BlockPath(MapName);
	TArray<uint8> BlockBytes;
	if (!FFileHelper::LoadFileToArray(BlockBytes, *RawPath))
	{
		OutError = FString::Printf(
			TEXT("character ground raster не найден: %s"),
			*RawPath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
	if (!ValidateBytes(
			BlockBytes, BlockSize, BlockSha256, RawPath, OutError))
	{
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
	const FString NavigationPath = RegionPath(MapName);
	TArray<uint8> RegionBytes;
	if (!FFileHelper::LoadFileToArray(RegionBytes, *NavigationPath))
	{
		OutError = FString::Printf(
			TEXT("character region raster не найден: %s"),
			*NavigationPath);
		UE_LOG(LogCorsairsCharacterGround, Error, TEXT("%s"), *OutError);
		return false;
	}
	if (!ValidateBytes(
			RegionBytes, RegionSize, RegionSha256, NavigationPath, OutError) ||
		!LoadRuntimeFromBytes(
			TileWidth,
			TileHeight,
			HeightBytes,
			BlockBytes,
			RegionBytes,
			OutError))
	{
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
	if (TileWidth > MAX_int32 / 100 || TileHeight > MAX_int32 / 100)
	{
		OutError = TEXT("source bounds выходят за диапазон int32");
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

bool FCorsairsCharacterGround::LoadRuntimeFromBytes(
	const int32 TileWidth,
	const int32 TileHeight,
	const TConstArrayView<uint8> HeightBytes,
	const TConstArrayView<uint8> BlockBytes,
	const TConstArrayView<uint8> RegionBytes,
	FString& OutError)
{
	if (!LoadFromBytes(TileWidth, TileHeight, BlockBytes, OutError))
	{
		return false;
	}
	const uint64 TileCount =
		static_cast<uint64>(TileWidth) * static_cast<uint64>(TileHeight);
	if (TileCount > static_cast<uint64>(MAX_int32) / 2)
	{
		Reset();
		OutError = TEXT("region raster слишком велик");
		return false;
	}
	const int32 ExpectedSize = static_cast<int32>(TileCount * 2);
	if (HeightBytes.Num() != ExpectedSize)
	{
		Reset();
		OutError = FString::Printf(
			TEXT("ожидалось %d bytes для height raster %dx%d, получено %d"),
			ExpectedSize,
			TileWidth,
			TileHeight,
			HeightBytes.Num());
		return false;
	}
	for (int32 Index = 0; Index < HeightBytes.Num(); Index += 2)
	{
		if (HeightBytes[Index] != 0)
		{
			Reset();
			OutError = FString::Printf(
				TEXT("height raster содержит неканонический r16 sample %d"),
				Index / 2);
			return false;
		}
	}
	if (RegionBytes.Num() != ExpectedSize)
	{
		Reset();
		OutError = FString::Printf(
			TEXT("ожидалось %d bytes для region raster %dx%d, получено %d"),
			ExpectedSize,
			TileWidth,
			TileHeight,
			RegionBytes.Num());
		return false;
	}
	for (int32 Index = 0; Index < RegionBytes.Num(); Index += 2)
	{
		const uint16 RegionMask =
			static_cast<uint16>(RegionBytes[Index]) |
			(static_cast<uint16>(RegionBytes[Index + 1]) << 8);
		if ((RegionMask & ~KnownRegionMask) != 0)
		{
			Reset();
			OutError = FString::Printf(
				TEXT("region raster содержит неизвестные биты 0x%04x"),
				RegionMask & ~KnownRegionMask);
			return false;
		}
	}
	SurfaceHeights.Append(HeightBytes.GetData(), HeightBytes.Num());
	Regions.Append(RegionBytes.GetData(), RegionBytes.Num());
	return true;
}

bool FCorsairsCharacterGround::TrySampleSurface(
	const FVector2d SourcePoint,
	double& OutHeightCm) const
{
	if (!IsNavigationLoaded() || !FMath::IsFinite(SourcePoint.X) ||
		!FMath::IsFinite(SourcePoint.Y) ||
		SourcePoint.X < 0.0 || SourcePoint.Y < 0.0 ||
		Width < 2 || Height < 2 ||
		SourcePoint.X >= static_cast<double>((Width - 1) * 100) ||
		SourcePoint.Y >= static_cast<double>((Height - 1) * 100))
	{
		return false;
	}
	const int32 TileX = FMath::FloorToInt(SourcePoint.X / 100.0);
	const int32 TileY = FMath::FloorToInt(SourcePoint.Y / 100.0);
	const double FractionX = SourcePoint.X / 100.0 - TileX;
	const double FractionY = SourcePoint.Y / 100.0 - TileY;
	const auto ReadHeightCm = [this](const int32 X, const int32 Y)
	{
		const int32 Index = (Y * Width + X) * 2;
		const uint16 Encoded =
			static_cast<uint16>(SurfaceHeights[Index]) |
			(static_cast<uint16>(SurfaceHeights[Index + 1]) << 8);
		const int32 RawHeight = static_cast<int32>(Encoded / 256) - 128;
		return static_cast<double>(RawHeight) * 10.0;
	};
	const double TopLeft = ReadHeightCm(TileX, TileY);
	const double TopRight = ReadHeightCm(TileX + 1, TileY);
	const double BottomLeft = ReadHeightCm(TileX, TileY + 1);
	const double BottomRight = ReadHeightCm(TileX + 1, TileY + 1);
	if (FractionX + FractionY <= 1.0)
	{
		OutHeightCm = TopLeft +
			FractionX * (TopRight - TopLeft) +
			FractionY * (BottomLeft - TopLeft);
	}
	else
	{
		OutHeightCm = BottomRight +
			(1.0 - FractionX) * (BottomLeft - BottomRight) +
			(1.0 - FractionY) * (TopRight - BottomRight);
	}
	OutHeightCm = FMath::Max(OutHeightCm, 0.0);
	return true;
}

bool FCorsairsCharacterGround::TrySampleNavigation(
	const FIntPoint SourcePoint,
	const ECorsairsTraversalKind Traversal,
	FCorsairsNavigationCell& OutCell) const
{
	OutCell = {};
	if (!IsNavigationLoaded() || SourcePoint.X < 0 || SourcePoint.Y < 0 ||
		SourcePoint.X >= Width * 100 || SourcePoint.Y >= Height * 100)
	{
		return false;
	}
	const int32 TileX = SourcePoint.X / 100;
	const int32 TileY = SourcePoint.Y / 100;
	const int32 RegionIndex = (TileY * Width + TileX) * 2;
	const uint16 RegionMask =
		static_cast<uint16>(Regions[RegionIndex]) |
		(static_cast<uint16>(Regions[RegionIndex + 1]) << 8);
	if ((RegionMask & ~KnownRegionMask) != 0)
	{
		return false;
	}

	const int32 GridX = SourcePoint.X / HalfMeterCm;
	const int32 GridY = SourcePoint.Y / HalfMeterCm;
	const int32 Quadrant = (GridY % 2) * 2 + (GridX % 2);
	const int32 BlockIndex =
		(TileY * Width + TileX) * QuadrantsPerTile + Quadrant;
	const uint8 Encoded = Cells[BlockIndex];
	const int32 Magnitude =
		static_cast<int32>(Encoded & MagnitudeMask) * 5;
	OutCell.HeightCm = (Encoded & SignMask) != 0 ? -Magnitude : Magnitude;
	OutCell.RegionMask = RegionMask;
	bool bTraversalBlocked = false;
	switch (Traversal)
	{
	case ECorsairsTraversalKind::Land:
		bTraversalBlocked =
			(RegionMask & (LandRegionMask | BridgeRegionMask)) == 0;
		break;
	case ECorsairsTraversalKind::Sea:
		bTraversalBlocked = (RegionMask & LandRegionMask) != 0;
		break;
	case ECorsairsTraversalKind::Discretionary:
		break;
	default:
		return false;
	}
	OutCell.bBlocked = (Encoded & BlockMask) != 0 || bTraversalBlocked;
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

bool FCorsairsCharacterGround::IsNavigationLoaded() const
{
	return IsLoaded() &&
		SurfaceHeights.Num() == Width * Height * 2 &&
		Regions.Num() == Width * Height * 2;
}

FIntRect FCorsairsCharacterGround::GetSourceBounds() const
{
	if (!IsNavigationLoaded())
	{
		return {};
	}
	return FIntRect(0, 0, Width * 100, Height * 100);
}

void FCorsairsCharacterGround::Reset()
{
	Cells.Reset();
	SurfaceHeights.Reset();
	Regions.Reset();
	Width = 0;
	Height = 0;
}
