#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	constexpr TCHAR MapPackage[] = TEXT("/Game/Maps/Garner");
	constexpr TCHAR WorldObject[] = TEXT("/Game/Maps/Garner.Garner");
	constexpr TCHAR GameModeObject[] =
		TEXT("/Script/CorsairsGame.CorsairsGameMode");
	constexpr TCHAR MeshObject[] =
		TEXT("/Game/Terrain/Reference/Garner/SM_Garner_17_21.SM_Garner_17_21");
	constexpr TCHAR TextureObject[] =
		TEXT("/Game/Terrain/Reference/Garner/T_Garner_17_21.T_Garner_17_21");
	constexpr TCHAR MaterialObject[] =
		TEXT("/Game/Terrain/Reference/M_TerrainReference.M_TerrainReference");
	constexpr TCHAR InstanceObject[] =
		TEXT("/Game/Terrain/Reference/Garner/MI_Garner_17_21.MI_Garner_17_21");
	constexpr TCHAR ActorObject[] =
		TEXT("/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21");
	constexpr TCHAR MarkerObject[] =
		TEXT("/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrainBuildRoot");

	UWorld* LoadGarnerWorld(FAutomationTestBase& Test)
	{
		UPackage* Package = LoadPackage(nullptr, MapPackage, LOAD_None);
		if (!Test.TestNotNull(TEXT("Garner package loads"), Package))
		{
			return nullptr;
		}
		UWorld* World = FindObject<UWorld>(Package, TEXT("Garner"));
		Test.TestNotNull(TEXT("exact Garner world object loads"), World);
		if (World != nullptr)
		{
			Test.TestEqual(TEXT("world path"), World->GetPathName(), FString(WorldObject));
		}
		return World;
	}

	template<typename T>
	T* LoadExact(FAutomationTestBase& Test, const TCHAR* Path, const TCHAR* Label)
	{
		T* Value = Cast<T>(FSoftObjectPath(Path).TryLoad());
		Test.TestNotNull(Label, Value);
		return Value;
	}

	FString MetadataValue(const UObject* Object, const TCHAR* Key)
	{
		if (Object == nullptr || Object->GetOutermost() == nullptr)
		{
			return FString();
		}
		const UMetaData* Metadata = Object->GetOutermost()->GetMetaData();
		return Metadata != nullptr ? Metadata->GetValue(Object, Key) : FString();
	}

	bool StrictOverlap(const FBox& First, const FBox& Second)
	{
		return First.Max.X > Second.Min.X && First.Min.X < Second.Max.X &&
			First.Max.Y > Second.Min.Y && First.Min.Y < Second.Max.Y;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReferenceTerrainDefaultGameModeTest,
	"Corsairs.Terrain.ReferenceAssets.DefaultGameMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReferenceTerrainDefaultGameModeTest::RunTest(const FString&)
{
	UWorld* World = LoadGarnerWorld(*this);
	if (World == nullptr)
	{
		return false;
	}
	UClass* Expected = FSoftClassPath(GameModeObject).TryLoadClass<AGameModeBase>();
	TestNotNull(TEXT("exact CorsairsGameMode soft-loads"), Expected);
	TestEqual(
		TEXT("saved default GameMode"),
		World->GetWorldSettings()->DefaultGameMode.Get(),
		Expected);
	TestEqual(
		TEXT("saved GameMode path"),
		Expected != nullptr ? Expected->GetPathName() : FString(),
		FString(GameModeObject));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReferenceTerrainActorTest,
	"Corsairs.Terrain.ReferenceAssets.ReferenceActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReferenceTerrainActorTest::RunTest(const FString&)
{
	UWorld* World = LoadGarnerWorld(*this);
	if (World == nullptr)
	{
		return false;
	}
	UStaticMesh* Mesh = LoadExact<UStaticMesh>(*this, MeshObject, TEXT("mesh"));
	UTexture2D* Texture = LoadExact<UTexture2D>(*this, TextureObject, TEXT("texture"));
	UMaterial* Material = LoadExact<UMaterial>(*this, MaterialObject, TEXT("material"));
	UMaterialInstanceConstant* Instance = LoadExact<UMaterialInstanceConstant>(
		*this, InstanceObject, TEXT("material instance"));
	if (Mesh == nullptr || Texture == nullptr || Material == nullptr || Instance == nullptr)
	{
		return false;
	}

	TestEqual(TEXT("texture source width"), Texture->Source.GetSizeX(), 4096);
	TestEqual(TEXT("texture source height"), Texture->Source.GetSizeY(), 4096);
	TestEqual(TEXT("texture source format"), Texture->Source.GetFormat(), TSF_BGRA8);
	TestTrue(TEXT("texture source hash metadata"),
		MetadataValue(Texture, TEXT("Corsairs.SourceSha256")).Len() == 64);
	TestEqual(TEXT("texture format metadata"),
		MetadataValue(Texture, TEXT("Corsairs.SourceFormat")), FString(TEXT("RGBA8")));
	TestTrue(TEXT("texture is sRGB"), Texture->SRGB);
	TestEqual(TEXT("texture compression"), Texture->CompressionSettings, TC_Default);
	TestEqual(TEXT("texture filter"), Texture->Filter, TF_Bilinear);
	TestEqual(TEXT("texture address X"), Texture->AddressX, TA_Clamp);
	TestEqual(TEXT("texture address Y"), Texture->AddressY, TA_Clamp);
	TestEqual(TEXT("texture mip generation"),
		Texture->MipGenSettings, TMGS_FromTextureGroup);
	TestEqual(TEXT("texture group"), Texture->LODGroup, TEXTUREGROUP_World);
	TestEqual(TEXT("texture LOD bias"), Texture->LODBias, 0);
	TestFalse(TEXT("texture streams normally"), Texture->NeverStream);

	TestEqual(TEXT("material blend"), Material->BlendMode, BLEND_Masked);
	TestEqual(TEXT("material shading"), Material->GetShadingModels(),
		FMaterialShadingModelField(MSM_Unlit));
	TestTrue(TEXT("material static-mesh usage"),
		Material->GetUsageByFlag(MATUSAGE_StaticMesh));
	TestTrue(TEXT("material Nanite usage"),
		Material->GetUsageByFlag(MATUSAGE_Nanite));
	UMaterialExpressionTextureSampleParameter2D* Sample = nullptr;
	int32 SampleCount = 0;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (UMaterialExpressionTextureSampleParameter2D* Candidate =
			Cast<UMaterialExpressionTextureSampleParameter2D>(Expression))
		{
			if (Candidate->ParameterName == TEXT("BaseColorTexture"))
			{
				Sample = Candidate;
				++SampleCount;
			}
		}
	}
	TestEqual(TEXT("one BaseColorTexture sample"), SampleCount, 1);
	if (Sample != nullptr)
	{
		TestEqual(TEXT("sample type"), Sample->SamplerType, SAMPLERTYPE_Color);
		TestEqual(TEXT("sample source"), Sample->SamplerSource, SSM_FromTextureAsset);
		const FExpressionInput* Emissive =
			Material->GetExpressionInputForProperty(MP_EmissiveColor);
		const FExpressionInput* Opacity =
			Material->GetExpressionInputForProperty(MP_OpacityMask);
		TestTrue(TEXT("RGB drives emissive"), Emissive != nullptr &&
			Emissive->Expression == Sample && Emissive->MaskR &&
			Emissive->MaskG && Emissive->MaskB);
		TestTrue(TEXT("A drives opacity mask"), Opacity != nullptr &&
			Opacity->Expression == Sample && Opacity->MaskA);
	}
	TestEqual(TEXT("instance parent"), Instance->Parent.Get(), Material);
	UTexture* BoundTexture = nullptr;
	TestTrue(TEXT("instance texture parameter exists"),
		Instance->GetTextureParameterValue(
			FMaterialParameterInfo(TEXT("BaseColorTexture")), BoundTexture));
	TestEqual(TEXT("instance texture binding"), BoundTexture, Texture);
	TestTrue(TEXT("mesh Nanite enabled"), Mesh->GetNaniteSettings().bEnabled);
	TestEqual(TEXT("mesh material slot 0"), Mesh->GetMaterial(0), Instance);
	TestTrue(TEXT("mesh glTF hash metadata"),
		MetadataValue(Mesh, TEXT("Corsairs.SourceGltfSha256")).Len() == 64);
	TestTrue(TEXT("mesh bin hash metadata"),
		MetadataValue(Mesh, TEXT("Corsairs.SourceBinSha256")).Len() == 64);

	AStaticMeshActor* Reference = nullptr;
	int32 ReferenceCount = 0;
	bool bMarkerFound = false;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		bMarkerFound |= Actor->GetPathName() == MarkerObject;
		if (Actor->GetPathName() == ActorObject)
		{
			Reference = Cast<AStaticMeshActor>(Actor);
			++ReferenceCount;
		}
	}
	TestTrue(TEXT("build marker exists"), bMarkerFound);
	TestEqual(TEXT("one exact reference actor"), ReferenceCount, 1);
	TestNotNull(TEXT("reference actor is StaticMeshActor"), Reference);
	if (Reference == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("reference actor label"), Reference->GetActorLabel(),
		FString(TEXT("ReferenceTerrain_Garner_17_21")));
	TestTrue(TEXT("reference actor tag"),
		Reference->Tags.Contains(TEXT("CorsairsReferenceTerrain")));
	TestTrue(TEXT("reference actor location"), Reference->GetActorLocation().Equals(
		FVector(217600.0, -268800.0, 0.0), 0.01));
	UStaticMeshComponent* Component = Reference->GetStaticMeshComponent();
	TestEqual(TEXT("component mesh"), Component->GetStaticMesh(), Mesh);
	TestEqual(TEXT("component material slot 0"), Component->GetMaterial(0), Instance);
	FVector Origin;
	FVector Extent;
	Reference->GetActorBounds(false, Origin, Extent);
	const FBox ReferenceBox(Origin - Extent, Origin + Extent);
	TestTrue(TEXT("reference min X"), FMath::IsNearlyEqual(
		ReferenceBox.Min.X, 217600.0, 1.0));
	TestTrue(TEXT("reference max X"), FMath::IsNearlyEqual(
		ReferenceBox.Max.X, 230400.0, 1.0));
	TestTrue(TEXT("reference min Y"), FMath::IsNearlyEqual(
		ReferenceBox.Min.Y, -281600.0, 1.0));
	TestTrue(TEXT("reference max Y"), FMath::IsNearlyEqual(
		ReferenceBox.Max.Y, -268800.0, 1.0));
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == Reference || !Actor->GetActorLabel().StartsWith(TEXT("Terrain_")))
		{
			continue;
		}
		FVector LegacyOrigin;
		FVector LegacyExtent;
		Actor->GetActorBounds(false, LegacyOrigin, LegacyExtent);
		if (StrictOverlap(ReferenceBox, FBox(
			LegacyOrigin - LegacyExtent, LegacyOrigin + LegacyExtent)))
		{
			TestTrue(TEXT("overlap is tagged"),
				Actor->Tags.Contains(TEXT("CorsairsReferenceTerrainHiddenOverlap")));
			TestTrue(TEXT("overlap is hidden in editor"), Actor->IsHiddenEd());
			TestTrue(TEXT("overlap is hidden in game"), Actor->IsHidden());
		}
	}
	return !HasAnyErrors();
}

#endif
