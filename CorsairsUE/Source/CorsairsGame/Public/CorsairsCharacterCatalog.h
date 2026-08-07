#pragma once

#include "CoreMinimal.h"
#include "CorsairsSession.h"

enum class ECorsairsBodyPart : uint8
{
	Head = 0,
	Face,
	Body,
	Gloves,
	Shoes,
	Count
};

struct FCorsairsResolvedAppearance
{
	bool bModular = false;
	FSoftObjectPath DriverMesh;
	FSoftObjectPath Animation;
	TStaticArray<FSoftObjectPath,
		static_cast<int32>(ECorsairsBodyPart::Count)> PartMeshes;
	FSoftObjectPath StaticMesh;
	TArray<FString> Warnings;
};

class CORSAIRSGAME_API FCorsairsCharacterCatalog
{
public:
	bool Load(const FString& JsonPath, FString& OutError);
	bool Resolve(
		int32 ArchetypeId,
		const FCorsairsCharacterLook& Look,
		FCorsairsResolvedAppearance& OutAppearance,
		FString& OutError) const;

private:
	struct FCharacterEntry
	{
		int32 ModalType = 0;
		int32 ModuleIndex = 0;
		FSoftObjectPath DriverMesh;
		FSoftObjectPath Animation;
		TStaticArray<int32, 5> DefaultItemIds = {0, 0, 0, 0, 0};
		FSoftObjectPath StaticMesh;
	};

	TMap<int32, FCharacterEntry> Characters;
	TMap<int32, TMap<int32, FSoftObjectPath>> ItemMeshes;
};
