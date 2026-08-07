#pragma once

#include "CoreMinimal.h"

struct FCorsairsCharacterCell
{
	double HeightCm = 0.0;
	bool bBlocked = false;
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
	FCorsairsCharacterCell Sample(FIntPoint SourcePosition) const;
	FVector ActorCenter(
		FIntPoint SourcePosition,
		double ScaledCapsuleHalfHeight) const;
	bool IsLoaded() const;

private:
	void Reset();

	TArray<uint8> Cells;
	int32 Width = 0;
	int32 Height = 0;
};
