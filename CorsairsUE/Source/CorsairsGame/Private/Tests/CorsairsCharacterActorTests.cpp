#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCharacter.h"
#include "CorsairsCharacterCatalog.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterActorTest,
	"Corsairs.Character.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterActorTest::RunTest(const FString&)
{
	FCorsairsCharacterCatalog Catalog;
	FString Error;
	TestTrue(
		TEXT("catalog loads"),
		Catalog.Load(
			FPaths::ProjectDir() / TEXT("Data/character_map.json"),
			Error));

	FCorsairsCharacterLook Look;
	Look.TypeId = 1;
	Look.HairId = 2000;
	Look.EquipIds.SetNumZeroed(CorsairsEquipSlotCount);
	Look.EquipIds[1] = 255;
	Look.EquipIds[2] = 289;
	Look.EquipIds[3] = 465;
	Look.EquipIds[4] = 641;

	FCorsairsResolvedAppearance Appearance;
	TestTrue(
		TEXT("equipped appearance resolves"),
		Catalog.Resolve(1, Look, Appearance, Error));

	const UWorld::InitializationValues InitializationValues =
		UWorld::InitializationValues()
			.AllowAudioPlayback(false)
			.RequiresHitProxies(false)
			.CreatePhysicsScene(false)
			.CreateNavigation(false)
			.CreateAISystem(false)
			.ShouldSimulatePhysics(false)
			.SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		NAME_None,
		nullptr,
		true,
		ERHIFeatureLevel::Num,
		&InitializationValues);
	ACorsairsCharacter* Actor = World->SpawnActor<ACorsairsCharacter>();
	TestNotNull(TEXT("actor spawned"), Actor);
	if (Actor == nullptr)
	{
		World->DestroyWorld(false);
		return false;
	}

	TestTrue(TEXT("appearance applied"), Actor->ApplyAppearance(Appearance));
	TestEqual(TEXT("five parts"), Actor->GetVisiblePartCount(), 5);
	for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
	{
		USkeletalMeshComponent* Part = Actor->GetPartComponent(Slot);
		TestNotNull(FString::Printf(TEXT("part %d"), Slot), Part);
		TestNotNull(
			FString::Printf(TEXT("mesh %d"), Slot),
			Part->GetSkeletalMeshAsset());
		TestEqual(
			FString::Printf(TEXT("leader %d"), Slot),
			Part->LeaderPoseComponent.Get(),
			static_cast<USkinnedMeshComponent*>(Actor->GetMesh()));
	}

	Look.EquipIds[2] = 464;
	TestTrue(
		TEXT("default body appearance resolves"),
		Catalog.Resolve(1, Look, Appearance, Error));
	TestTrue(
		TEXT("default body appearance applied"),
		Actor->ApplyAppearance(Appearance));
	for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
	{
		USkeletalMeshComponent* Part = Actor->GetPartComponent(Slot);
		TestEqual(
			FString::Printf(TEXT("reapplied leader %d"), Slot),
			Part->LeaderPoseComponent.Get(),
			static_cast<USkinnedMeshComponent*>(Actor->GetMesh()));
	}

	World->DestroyWorld(false);
	return true;
}

#endif
