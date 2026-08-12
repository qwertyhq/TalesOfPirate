#pragma once

#include "CoreMinimal.h"

struct FCorsairsCharacterCell
{
	double HeightCm = 0.0;
	bool bBlocked = false;
};

enum class ECorsairsTraversalKind : uint8
{
	Land,
	Sea,
	Discretionary,
};

struct FCorsairsNavigationCell
{
	int32 HeightCm = 0;
	uint16 RegionMask = 0;
	bool bBlocked = true;
};

class CORSAIRSGAME_API FCorsairsCharacterGround
{
public:
	bool Load(const FString& MapName, FString& OutError);
	bool LoadFromBytes(
		int32 TileWidth,
		int32 TileHeight,
		TConstArrayView<uint8> Bytes,
		FString& OutError);
	bool LoadRuntimeFromBytes(
		int32 TileWidth,
		int32 TileHeight,
		TConstArrayView<uint8> HeightBytes,
		TConstArrayView<uint8> BlockBytes,
		TConstArrayView<uint8> RegionBytes,
		FString& OutError);
	FCorsairsCharacterCell Sample(FIntPoint SourcePosition) const;
	bool TrySampleSurface(
		FVector2d SourcePoint,
		double& OutHeightCm) const;
	bool TrySampleNavigation(
		FIntPoint SourcePoint,
		ECorsairsTraversalKind Traversal,
		FCorsairsNavigationCell& OutCell) const;
	FVector ActorCenter(
		FIntPoint SourcePosition,
		double ScaledCapsuleHalfHeight) const;
	bool IsLoaded() const;
	bool IsNavigationLoaded() const;
	FIntRect GetSourceBounds() const;

private:
	void Reset();

	TArray<uint8> Cells;
	TArray<uint8> SurfaceHeights;
	TArray<uint8> Regions;
	int32 Width = 0;
	int32 Height = 0;
};
