#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCharacterCatalog.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterCatalogTest,
	"Corsairs.Character.Catalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterCatalogTest::RunTest(const FString&)
{
	FCorsairsCharacterCatalog Catalog;
	FString Error;
	TestTrue(TEXT("catalog loads"),
		Catalog.Load(FPaths::ProjectDir() /
			TEXT("Data/character_map.json"), Error));
	TestTrue(TEXT("load error empty"), Error.IsEmpty());

	FCorsairsCharacterLook Look;
	Look.TypeId = 1;
	Look.HairId = 2000;
	Look.EquipIds.SetNumZeroed(CorsairsEquipSlotCount);

	FCorsairsResolvedAppearance Default;
	TestTrue(TEXT("Lambert resolves"),
		Catalog.Resolve(1, Look, Default, Error));
	TestTrue(TEXT("modular"), Default.bModular);
	TestEqual(TEXT("head"),
		Default.PartMeshes[0].ToString(),
		FString(TEXT("/Game/All/0000000001/SkeletalMeshes/0000000001")));
	TestEqual(TEXT("face"),
		Default.PartMeshes[1].ToString(),
		FString(TEXT("/Game/All/0000000000/SkeletalMeshes/0000000000")));
	TestEqual(TEXT("default body"),
		Default.PartMeshes[2].ToString(),
		FString(TEXT("/Game/All/0000610002/SkeletalMeshes/0000610002")));

	Look.EquipIds[2] = 289;
	FCorsairsResolvedAppearance Equipped;
	TestTrue(TEXT("equipped resolves"),
		Catalog.Resolve(1, Look, Equipped, Error));
	TestEqual(TEXT("equipped body"),
		Equipped.PartMeshes[2].ToString(),
		FString(TEXT("/Game/All/0000000002/SkeletalMeshes/0000000002")));

	Look.EquipIds[21] = 464;
	FCorsairsResolvedAppearance Apparel;
	TestTrue(TEXT("apparel resolves"),
		Catalog.Resolve(1, Look, Apparel, Error));
	TestEqual(TEXT("apparel overrides equipment"),
		Apparel.PartMeshes[2].ToString(),
		FString(TEXT("/Game/All/0000610002/SkeletalMeshes/0000610002")));

	Look.EquipIds[21] = 999999;
	FCorsairsResolvedAppearance Missing;
	TestTrue(TEXT("missing apparel falls back"),
		Catalog.Resolve(1, Look, Missing, Error));
	TestEqual(TEXT("fallback body"),
		Missing.PartMeshes[2].ToString(),
		FString(TEXT("/Game/All/0000610002/SkeletalMeshes/0000610002")));
	TestTrue(TEXT("fallback warning recorded"), Missing.Warnings.Num() > 0);

	FCorsairsCharacterLook NpcLook;
	FCorsairsResolvedAppearance Npc;
	TestTrue(TEXT("NPC resolves"),
		Catalog.Resolve(5, NpcLook, Npc, Error));
	TestFalse(TEXT("NPC is single mesh"), Npc.bModular);
	TestEqual(TEXT("NPC mesh"),
		Npc.StaticMesh.ToString(),
		FString(TEXT("/Game/All/0005000000/SkeletalMeshes/0005000000")));
	return true;
}

#endif
