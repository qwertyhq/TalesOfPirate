#include "CorsairsCharacterCatalog.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr int32 ApparelOffset = 19;
	constexpr int32 VisiblePartCount = 5;

	bool ReadRequiredInt(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		const TCHAR* Field,
		int32& OutValue,
		FString& OutError)
	{
		if (!Object->TryGetNumberField(Field, OutValue))
		{
			OutError = FString::Printf(TEXT("%s.%s: expected integer"), *Path, Field);
			return false;
		}

		return true;
	}

	bool ReadRequiredString(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		const TCHAR* Field,
		FString& OutValue,
		FString& OutError)
	{
		if (!Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s.%s: expected non-empty string"), *Path, Field);
			return false;
		}

		return true;
	}

	bool ReadAnimationPolicy(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		ECorsairsAnimationPolicy& OutPolicy,
		FString& OutError)
	{
		FString Value;
		if (!ReadRequiredString(
			Object,
			Path,
			TEXT("animationPolicy"),
			Value,
			OutError))
		{
			return false;
		}
		if (Value == TEXT("loop"))
		{
			OutPolicy = ECorsairsAnimationPolicy::Loop;
			return true;
		}
		if (Value == TEXT("staticReferencePose"))
		{
			OutPolicy = ECorsairsAnimationPolicy::StaticReferencePose;
			return true;
		}

		OutError = FString::Printf(
			TEXT("%s.animationPolicy: unsupported value %s"),
			*Path,
			*Value);
		return false;
	}

	const FSoftObjectPath* FindItemMesh(
		const TMap<int32, TMap<int32, FSoftObjectPath>>& ItemMeshes,
		int32 ItemId,
		int32 ModuleIndex)
	{
		const TMap<int32, FSoftObjectPath>* MeshesByModule = ItemMeshes.Find(ItemId);
		if (MeshesByModule == nullptr)
		{
			return nullptr;
		}

		const FSoftObjectPath* Mesh = MeshesByModule->Find(ModuleIndex);
		if (Mesh == nullptr || Mesh->IsNull())
		{
			return nullptr;
		}

		return Mesh;
	}
}

bool FCorsairsCharacterCatalog::Load(const FString& JsonPath, FString& OutError)
{
	Characters.Empty();
	ItemMeshes.Empty();
	OutError.Empty();

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *JsonPath))
	{
		OutError = FString::Printf(TEXT("%s: could not read file"), *JsonPath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("%s: invalid JSON"), *JsonPath);
		return false;
	}

	const TSharedPtr<FJsonObject>* CharactersObject = nullptr;
	if (!Root->TryGetObjectField(TEXT("characters"), CharactersObject) || CharactersObject == nullptr)
	{
		OutError = TEXT("characters: expected object");
		return false;
	}

	const TSharedPtr<FJsonObject>* ItemsObject = nullptr;
	if (!Root->TryGetObjectField(TEXT("items"), ItemsObject) || ItemsObject == nullptr)
	{
		OutError = TEXT("items: expected object");
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& CharacterPair : (*CharactersObject)->Values)
	{
		int32 CharacterId = 0;
		const FString CharacterPath = FString::Printf(TEXT("characters.%s"), *CharacterPair.Key);
		if (!LexTryParseString(CharacterId, *CharacterPair.Key))
		{
			OutError = FString::Printf(TEXT("%s: expected numeric key"), *CharacterPath);
			return false;
		}

		const TSharedPtr<FJsonObject>* CharacterObject = nullptr;
		if (!CharacterPair.Value->TryGetObject(CharacterObject) || CharacterObject == nullptr)
		{
			OutError = FString::Printf(TEXT("%s: expected object"), *CharacterPath);
			return false;
		}

		FCharacterEntry Entry;
		FString DriverMesh;
		FString Animation;
		FString StaticMesh;
		if (!ReadRequiredInt(*CharacterObject, CharacterPath, TEXT("modalType"), Entry.ModalType, OutError) ||
			!ReadRequiredInt(*CharacterObject, CharacterPath, TEXT("moduleIndex"), Entry.ModuleIndex, OutError) ||
			!ReadAnimationPolicy(*CharacterObject, CharacterPath, Entry.AnimationPolicy, OutError) ||
			!ReadRequiredString(*CharacterObject, CharacterPath, TEXT("driverMesh"), DriverMesh, OutError) ||
			!ReadRequiredString(*CharacterObject, CharacterPath, TEXT("animation"), Animation, OutError) ||
			!ReadRequiredString(*CharacterObject, CharacterPath, TEXT("staticMesh"), StaticMesh, OutError))
		{
			return false;
		}
		if (Entry.ModalType == 1 &&
			Entry.AnimationPolicy != ECorsairsAnimationPolicy::Loop)
		{
			OutError = FString::Printf(
				TEXT("%s.animationPolicy: modular character must loop"),
				*CharacterPath);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* DefaultItemIds = nullptr;
		if (!(*CharacterObject)->TryGetArrayField(TEXT("defaultItemIds"), DefaultItemIds) ||
			DefaultItemIds == nullptr || DefaultItemIds->Num() != VisiblePartCount)
		{
			OutError = FString::Printf(TEXT("%s.defaultItemIds: expected exactly five integers"), *CharacterPath);
			return false;
		}

		for (int32 Slot = 0; Slot < VisiblePartCount; ++Slot)
		{
			if (!(*DefaultItemIds)[Slot]->TryGetNumber(Entry.DefaultItemIds[Slot]))
			{
				OutError = FString::Printf(
					TEXT("%s.defaultItemIds[%d]: expected integer"),
					*CharacterPath,
					Slot);
				return false;
			}
		}

		Entry.DriverMesh = FSoftObjectPath(DriverMesh);
		Entry.Animation = FSoftObjectPath(Animation);
		Entry.StaticMesh = FSoftObjectPath(StaticMesh);
		Characters.Add(CharacterId, MoveTemp(Entry));
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& ItemPair : (*ItemsObject)->Values)
	{
		int32 ItemId = 0;
		const FString ItemPath = FString::Printf(TEXT("items.%s"), *ItemPair.Key);
		if (!LexTryParseString(ItemId, *ItemPair.Key))
		{
			OutError = FString::Printf(TEXT("%s: expected numeric key"), *ItemPath);
			return false;
		}

		const TSharedPtr<FJsonObject>* ItemObject = nullptr;
		if (!ItemPair.Value->TryGetObject(ItemObject) || ItemObject == nullptr)
		{
			OutError = FString::Printf(TEXT("%s: expected object"), *ItemPath);
			return false;
		}

		const TSharedPtr<FJsonObject>* MeshesByModuleObject = nullptr;
		if (!(*ItemObject)->TryGetObjectField(TEXT("meshesByModule"), MeshesByModuleObject) ||
			MeshesByModuleObject == nullptr)
		{
			OutError = FString::Printf(TEXT("%s.meshesByModule: expected object"), *ItemPath);
			return false;
		}

		TMap<int32, FSoftObjectPath> MeshesByModule;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& ModulePair : (*MeshesByModuleObject)->Values)
		{
			int32 ModuleIndex = 0;
			const FString ModulePath = FString::Printf(
				TEXT("%s.meshesByModule.%s"),
				*ItemPath,
				*ModulePair.Key);
			if (!LexTryParseString(ModuleIndex, *ModulePair.Key))
			{
				OutError = FString::Printf(TEXT("%s: expected numeric key"), *ModulePath);
				return false;
			}

			FString MeshPath;
			if (!ModulePair.Value->TryGetString(MeshPath) || MeshPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s: expected non-empty string"), *ModulePath);
				return false;
			}

			MeshesByModule.Add(ModuleIndex, FSoftObjectPath(MeshPath));
		}

		ItemMeshes.Add(ItemId, MoveTemp(MeshesByModule));
	}

	if (Characters.IsEmpty() || ItemMeshes.IsEmpty())
	{
		OutError = TEXT("characters and items must both be non-empty");
		Characters.Empty();
		ItemMeshes.Empty();
		return false;
	}

	return true;
}

bool FCorsairsCharacterCatalog::Resolve(
	int32 ArchetypeId,
	const FCorsairsCharacterLook& Look,
	FCorsairsResolvedAppearance& OutAppearance,
	FString& OutError) const
{
	OutAppearance = FCorsairsResolvedAppearance{};
	OutError.Empty();

	if (Look.bIsBoat)
	{
		OutError = FString::Printf(
			TEXT("boat appearance is not supported for archetype %d"),
			ArchetypeId);
		return false;
	}

	const FCharacterEntry* Entry = Characters.Find(ArchetypeId);
	if (Entry == nullptr)
	{
		OutError = FString::Printf(TEXT("characters.%d: no catalog entry"), ArchetypeId);
		return false;
	}

	OutAppearance.DriverMesh = Entry->DriverMesh;
	OutAppearance.Animation = Entry->Animation;
	OutAppearance.AnimationPolicy = Entry->AnimationPolicy;
	if (Entry->ModalType != 1)
	{
		OutAppearance.StaticMesh = Entry->StaticMesh;
		if (OutAppearance.StaticMesh.IsNull())
		{
			OutError = FString::Printf(TEXT("characters.%d.staticMesh: missing"), ArchetypeId);
			return false;
		}

		return true;
	}

	OutAppearance.bModular = true;
	int32 Requested[VisiblePartCount] = {};
	Requested[0] =
		Look.EquipIds.IsValidIndex(0) && Look.EquipIds[0] != 0
			? Look.EquipIds[0]
			: (Look.HairId != 0 ? Look.HairId : Entry->DefaultItemIds[0]);
	for (int32 Slot = 1; Slot < VisiblePartCount; ++Slot)
	{
		Requested[Slot] =
			Look.EquipIds.IsValidIndex(Slot) && Look.EquipIds[Slot] != 0
				? Look.EquipIds[Slot]
				: Entry->DefaultItemIds[Slot];
	}
	for (int32 Slot = 0; Slot < VisiblePartCount; ++Slot)
	{
		const int32 ApparelSlot = Slot + ApparelOffset;
		if (Look.EquipIds.IsValidIndex(ApparelSlot) && Look.EquipIds[ApparelSlot] != 0)
		{
			Requested[Slot] = Look.EquipIds[ApparelSlot];
		}
	}

	for (int32 Slot = 0; Slot < VisiblePartCount; ++Slot)
	{
		const int32 DefaultItemId = Entry->DefaultItemIds[Slot];
		const FSoftObjectPath* Mesh = FindItemMesh(ItemMeshes, Requested[Slot], Entry->ModuleIndex);
		if (Mesh == nullptr && Requested[Slot] != DefaultItemId)
		{
			OutAppearance.Warnings.Add(FString::Printf(
				TEXT("archetype %d slot %d item %d is missing; using default %d"),
				ArchetypeId,
				Slot,
				Requested[Slot],
				DefaultItemId));
			Mesh = FindItemMesh(ItemMeshes, DefaultItemId, Entry->ModuleIndex);
		}

		if (Mesh == nullptr)
		{
			OutAppearance.Warnings.Add(FString::Printf(
				TEXT("archetype %d slot %d default item %d is missing"),
				ArchetypeId,
				Slot,
				DefaultItemId));
			continue;
		}

		OutAppearance.PartMeshes[Slot] = *Mesh;
	}

	return true;
}
