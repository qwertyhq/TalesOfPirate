#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCharacter.h"
#include "CorsairsCharacterCatalog.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
	UWorld* CreateCharacterTestWorld()
	{
		const UWorld::InitializationValues InitializationValues =
			UWorld::InitializationValues()
				.AllowAudioPlayback(false)
				.RequiresHitProxies(false)
				.CreatePhysicsScene(false)
				.CreateNavigation(false)
				.CreateAISystem(false)
				.ShouldSimulatePhysics(false)
				.SetTransactional(false);
		return UWorld::CreateWorld(
			EWorldType::Game,
			false,
			NAME_None,
			nullptr,
			true,
			ERHIFeatureLevel::Num,
			&InitializationValues);
	}

	FCorsairsCharacterLook MakeEquippedLook()
	{
		FCorsairsCharacterLook Look;
		Look.TypeId = 1;
		Look.HairId = 2000;
		Look.EquipIds.SetNumZeroed(CorsairsEquipSlotCount);
		Look.EquipIds[1] = 255;
		Look.EquipIds[2] = 289;
		Look.EquipIds[3] = 465;
		Look.EquipIds[4] = 641;
		return Look;
	}
}

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

	FCorsairsCharacterLook Look = MakeEquippedLook();
	FCorsairsResolvedAppearance EquippedAppearance;
	TestTrue(
		TEXT("equipped appearance resolves"),
		Catalog.Resolve(1, Look, EquippedAppearance, Error));

	UWorld* World = CreateCharacterTestWorld();
	ACorsairsCharacter* Actor = World->SpawnActor<ACorsairsCharacter>();
	TestNotNull(TEXT("actor spawned"), Actor);
	if (Actor == nullptr)
	{
		World->DestroyWorld(false);
		return false;
	}

	TestTrue(
		TEXT("equipped appearance applied"),
		Actor->ApplyAppearance(EquippedAppearance));
	TestEqual(TEXT("five parts"), Actor->GetVisiblePartCount(), 5);
	TestFalse(TEXT("modular driver hidden"), Actor->GetMesh()->IsVisible());
	TestEqual(
		TEXT("driver always refreshes bones"),
		Actor->GetMesh()->VisibilityBasedAnimTickOption,
		EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);

	const FVector ModularScale = Actor->GetMesh()->GetRelativeScale3D();
	TestEqual(TEXT("uniform scale X/Y"), ModularScale.X, ModularScale.Y);
	TestEqual(TEXT("uniform scale X/Z"), ModularScale.X, ModularScale.Z);
	TestTrue(
		TEXT("driver scale derived from bounds"),
		!FMath::IsNearlyEqual(ModularScale.X, 1.0));

	UAnimSingleNodeInstance* DriverAnimation =
		Actor->GetMesh()->GetSingleNodeInstance();
	TestNotNull(TEXT("driver single-node animation"), DriverAnimation);
	if (DriverAnimation != nullptr)
	{
		TestTrue(TEXT("driver animation looping"), DriverAnimation->IsLooping());
		TestNotNull(
			TEXT("driver animation asset"),
			DriverAnimation->GetCurrentAsset());
	}

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
		TestEqual(
			FString::Printf(TEXT("part scale %d"), Slot),
			Part->GetRelativeScale3D(),
			FVector::OneVector);
		TestNull(
			FString::Printf(TEXT("part animation %d"), Slot),
			Part->GetSingleNodeInstance());
	}

	USkeletalMesh* EquippedBody =
		Actor->GetPartComponent(2)->GetSkeletalMeshAsset();
	Look.EquipIds[2] = 464;
	FCorsairsResolvedAppearance DefaultBodyAppearance;
	TestTrue(
		TEXT("default body appearance resolves"),
		Catalog.Resolve(1, Look, DefaultBodyAppearance, Error));
	TestTrue(
		TEXT("default body appearance applied"),
		Actor->ApplyAppearance(DefaultBodyAppearance));
	TestTrue(
		TEXT("body mesh changed"),
		Actor->GetPartComponent(2)->GetSkeletalMeshAsset() != EquippedBody);
	for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
	{
		USkeletalMeshComponent* Part = Actor->GetPartComponent(Slot);
		TestEqual(
			FString::Printf(TEXT("reapplied leader %d"), Slot),
			Part->LeaderPoseComponent.Get(),
			static_cast<USkinnedMeshComponent*>(Actor->GetMesh()));
	}

	FCorsairsResolvedAppearance MissingPartAppearance =
		DefaultBodyAppearance;
	MissingPartAppearance.PartMeshes[1] = FSoftObjectPath();
	TestTrue(
		TEXT("missing optional part does not fail"),
		Actor->ApplyAppearance(MissingPartAppearance));
	TestNull(
		TEXT("missing optional part cleared"),
		Actor->GetPartComponent(1)->GetSkeletalMeshAsset());
	TestTrue(
		TEXT("missing optional part remains visible"),
		Actor->GetPartComponent(1)->IsVisible());
	TestEqual(
		TEXT("missing optional part keeps leader"),
		Actor->GetPartComponent(1)->LeaderPoseComponent.Get(),
		static_cast<USkinnedMeshComponent*>(Actor->GetMesh()));

	USkeletalMesh* PreviousDriver =
		Actor->GetMesh()->GetSkeletalMeshAsset();
	const FVector PreviousScale =
		Actor->GetMesh()->GetRelativeScale3D();
	UAnimationAsset* PreviousAnimation =
		Actor->GetMesh()->GetSingleNodeInstance()->GetCurrentAsset();
	TStaticArray<USkeletalMesh*, 5> PreviousPartMeshes;
	TStaticArray<bool, 5> PreviousPartVisibility;
	for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
	{
		PreviousPartMeshes[Slot] =
			Actor->GetPartComponent(Slot)->GetSkeletalMeshAsset();
		PreviousPartVisibility[Slot] =
			Actor->GetPartComponent(Slot)->IsVisible();
	}

	FCorsairsResolvedAppearance InvalidSingleAppearance;
	InvalidSingleAppearance.StaticMesh =
		FSoftObjectPath(TEXT("/Game/Missing/RequiredSingleMesh"));
	InvalidSingleAppearance.Animation = EquippedAppearance.Animation;
	TestFalse(
		TEXT("missing required single mesh fails"),
		Actor->ApplyAppearance(InvalidSingleAppearance));
	TestEqual(
		TEXT("failed single keeps driver"),
		Actor->GetMesh()->GetSkeletalMeshAsset(),
		PreviousDriver);
	TestEqual(
		TEXT("failed single keeps scale"),
		Actor->GetMesh()->GetRelativeScale3D(),
		PreviousScale);
	TestEqual(
		TEXT("failed single keeps animation"),
		Actor->GetMesh()->GetSingleNodeInstance()->GetCurrentAsset(),
		PreviousAnimation);
	for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
	{
		USkeletalMeshComponent* Part = Actor->GetPartComponent(Slot);
		TestEqual(
			FString::Printf(TEXT("failed single keeps mesh %d"), Slot),
			Part->GetSkeletalMeshAsset(),
			PreviousPartMeshes[Slot]);
		TestEqual(
			FString::Printf(TEXT("failed single keeps visibility %d"), Slot),
			Part->IsVisible(),
			PreviousPartVisibility[Slot]);
		TestEqual(
			FString::Printf(TEXT("failed single keeps leader %d"), Slot),
			Part->LeaderPoseComponent.Get(),
			static_cast<USkinnedMeshComponent*>(Actor->GetMesh()));
	}

	TestTrue(
		TEXT("equipped appearance restored"),
		Actor->ApplyAppearance(EquippedAppearance));
	PreviousDriver = Actor->GetMesh()->GetSkeletalMeshAsset();
	USkeletalMesh* PreviousBody =
		Actor->GetPartComponent(2)->GetSkeletalMeshAsset();
	const FVector ScaleBeforeInvalidAnimation =
		Actor->GetMesh()->GetRelativeScale3D();
	PreviousAnimation =
		Actor->GetMesh()->GetSingleNodeInstance()->GetCurrentAsset();
	for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
	{
		PreviousPartMeshes[Slot] =
			Actor->GetPartComponent(Slot)->GetSkeletalMeshAsset();
		PreviousPartVisibility[Slot] =
			Actor->GetPartComponent(Slot)->IsVisible();
	}

	FCorsairsResolvedAppearance InvalidDriverAppearance =
		DefaultBodyAppearance;
	InvalidDriverAppearance.DriverMesh =
		FSoftObjectPath(TEXT("/Game/Missing/RequiredDriver"));
	TestFalse(
		TEXT("missing required driver fails"),
		Actor->ApplyAppearance(InvalidDriverAppearance));
	TestEqual(
		TEXT("failed driver keeps driver"),
		Actor->GetMesh()->GetSkeletalMeshAsset(),
		PreviousDriver);
	TestEqual(
		TEXT("failed driver keeps body"),
		Actor->GetPartComponent(2)->GetSkeletalMeshAsset(),
		PreviousBody);

	FCorsairsResolvedAppearance InvalidAnimationAppearance =
		DefaultBodyAppearance;
	InvalidAnimationAppearance.Animation =
		FSoftObjectPath(TEXT("/Game/Missing/RequiredAnimation"));
	TestFalse(
		TEXT("missing required animation fails"),
		Actor->ApplyAppearance(InvalidAnimationAppearance));
	TestEqual(
		TEXT("failed animation keeps driver"),
		Actor->GetMesh()->GetSkeletalMeshAsset(),
		PreviousDriver);
	TestEqual(
		TEXT("failed animation keeps body"),
		Actor->GetPartComponent(2)->GetSkeletalMeshAsset(),
		PreviousBody);
	TestEqual(
		TEXT("failed animation keeps scale"),
		Actor->GetMesh()->GetRelativeScale3D(),
		ScaleBeforeInvalidAnimation);
	TestEqual(
		TEXT("failed animation keeps prior animation"),
		Actor->GetMesh()->GetSingleNodeInstance()->GetCurrentAsset(),
		PreviousAnimation);
	for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
	{
		USkeletalMeshComponent* Part = Actor->GetPartComponent(Slot);
		TestEqual(
			FString::Printf(TEXT("failed animation keeps mesh %d"), Slot),
			Part->GetSkeletalMeshAsset(),
			PreviousPartMeshes[Slot]);
		TestEqual(
			FString::Printf(
				TEXT("failed animation keeps visibility %d"),
				Slot),
			Part->IsVisible(),
			PreviousPartVisibility[Slot]);
		TestEqual(
			FString::Printf(TEXT("failed animation keeps leader %d"), Slot),
			Part->LeaderPoseComponent.Get(),
			static_cast<USkinnedMeshComponent*>(Actor->GetMesh()));
	}

	FCorsairsResolvedAppearance NpcAppearance;
	FCorsairsCharacterLook NpcLook;
	TestTrue(
		TEXT("NPC appearance resolves"),
		Catalog.Resolve(5, NpcLook, NpcAppearance, Error));
	TestTrue(
		TEXT("single-mesh appearance applied"),
		Actor->ApplyAppearance(NpcAppearance));
	TestTrue(TEXT("single-mesh driver visible"), Actor->GetMesh()->IsVisible());
	TestEqual(
		TEXT("single-mesh asset"),
		Actor->GetMesh()->GetSkeletalMeshAsset(),
		Cast<USkeletalMesh>(NpcAppearance.StaticMesh.TryLoad()));
	const FVector SingleScale = Actor->GetMesh()->GetRelativeScale3D();
	TestEqual(TEXT("single uniform scale X/Y"), SingleScale.X, SingleScale.Y);
	TestEqual(TEXT("single uniform scale X/Z"), SingleScale.X, SingleScale.Z);
	DriverAnimation = Actor->GetMesh()->GetSingleNodeInstance();
	TestNotNull(TEXT("single driver animation"), DriverAnimation);
	if (DriverAnimation != nullptr)
	{
		TestTrue(TEXT("single animation looping"), DriverAnimation->IsLooping());
	}
	for (int32 Slot = 0; Slot < Actor->GetVisiblePartCount(); ++Slot)
	{
		USkeletalMeshComponent* Part = Actor->GetPartComponent(Slot);
		TestNull(
			FString::Printf(TEXT("single clears part %d"), Slot),
			Part->GetSkeletalMeshAsset());
		TestFalse(
			FString::Printf(TEXT("single hides part %d"), Slot),
			Part->IsVisible());
		TestNull(
			FString::Printf(TEXT("single clears leader %d"), Slot),
			Part->LeaderPoseComponent.Get());
		TestEqual(
			FString::Printf(TEXT("single part scale %d"), Slot),
			Part->GetRelativeScale3D(),
			FVector::OneVector);
		TestNull(
			FString::Printf(TEXT("single part animation %d"), Slot),
			Part->GetSingleNodeInstance());
	}

	FCorsairsResolvedAppearance PappaAppearance;
	TestTrue(
		TEXT("Pappa appearance resolves"),
		Catalog.Resolve(260, NpcLook, PappaAppearance, Error));
	TestEqual(
		TEXT("Pappa requests reference pose"),
		PappaAppearance.AnimationPolicy,
		ECorsairsAnimationPolicy::StaticReferencePose);
	TestTrue(
		TEXT("Pappa reference pose applied"),
		Actor->ApplyAppearance(PappaAppearance));
	TestEqual(
		TEXT("Pappa mesh"),
		Actor->GetMesh()->GetSkeletalMeshAsset(),
		Cast<USkeletalMesh>(PappaAppearance.StaticMesh.TryLoad()));
	UAnimSingleNodeInstance* PappaAnimation =
		Actor->GetMesh()->GetSingleNodeInstance();
	TestNotNull(TEXT("Pappa single-node state"), PappaAnimation);
	if (PappaAnimation != nullptr)
	{
		TestNull(
			TEXT("Pappa incompatible animation is cleared"),
			PappaAnimation->GetCurrentAsset());
		TestFalse(TEXT("Pappa reference pose is not playing"),
			PappaAnimation->IsPlaying());
	}
	const FVector PappaScale = Actor->GetMesh()->GetRelativeScale3D();
	TestTrue(
		TEXT("Pappa scale remains finite"),
		!PappaScale.ContainsNaN());
	TestTrue(
		TEXT("Pappa scale remains plausible"),
		PappaScale.GetAbsMax() < 10.0);

	World->DestroyWorld(false);
	return true;
}

#endif
