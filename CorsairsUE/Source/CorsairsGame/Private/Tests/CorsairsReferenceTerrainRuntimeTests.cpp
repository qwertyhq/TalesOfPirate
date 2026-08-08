#if WITH_DEV_AUTOMATION_TESTS

#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Internationalization/Regex.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "StaticMeshResources.h"
#include "UObject/SoftObjectPath.h"

#if PLATFORM_MAC
#include <CommonCrypto/CommonDigest.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsReferenceTerrainRuntime, Log, All);

namespace
{
	constexpr TCHAR MapPackage[] = TEXT("/Game/Maps/Garner");
	constexpr TCHAR WorldObject[] = TEXT("/Game/Maps/Garner.Garner");
	constexpr TCHAR GameModeObject[] =
		TEXT("/Script/CorsairsGame.CorsairsGameMode");
	constexpr TCHAR ActorObject[] =
		TEXT("/Game/Maps/Garner.Garner:PersistentLevel.ReferenceTerrain_Garner_17_21");
	constexpr TCHAR MeshObject[] =
		TEXT("/Game/Terrain/Reference/Garner/SM_Garner_17_21.SM_Garner_17_21");
	constexpr TCHAR TextureObject[] =
		TEXT("/Game/Terrain/Reference/Garner/T_Garner_17_21.T_Garner_17_21");
	constexpr TCHAR MaterialObject[] =
		TEXT("/Game/Terrain/Reference/M_TerrainReference.M_TerrainReference");
	constexpr TCHAR InstanceObject[] =
		TEXT("/Game/Terrain/Reference/Garner/MI_Garner_17_21.MI_Garner_17_21");

	struct FRuntimeInput
	{
		FString RelativePath;
		FString Sha256;
		int64 SizeBytes = 0;
	};

	bool IsLowerHex(const FString& Value, const int32 Length)
	{
		if (Value.Len() != Length)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!FChar::IsDigit(Character) &&
				(Character < TEXT('a') || Character > TEXT('f')))
			{
				return false;
			}
		}
		return true;
	}

	const TCHAR* BoolJson(const bool Value)
	{
		return Value ? TEXT("true") : TEXT("false");
	}

	FString VectorJson(const FVector& Value)
	{
		return FString::Printf(
			TEXT("[%.17g,%.17g,%.17g]"),
			Value.X,
			Value.Y,
			Value.Z);
	}

	FString TransformJson(const FTransform& Value)
	{
		const FQuat Rotation = Value.GetRotation();
		return FString::Printf(
			TEXT("{\"location\":%s,\"rotationQuat\":[%.17g,%.17g,%.17g,%.17g],")
			TEXT("\"scale\":%s}"),
			*VectorJson(Value.GetLocation()),
			Rotation.X,
			Rotation.Y,
			Rotation.Z,
			Rotation.W,
			*VectorJson(Value.GetScale3D()));
	}

	FString BoxJson(const FBox& Value)
	{
		return FString::Printf(
			TEXT("{\"isValid\":%s,\"max\":%s,\"min\":%s}"),
			BoolJson(Value.IsValid != 0),
			*VectorJson(Value.Max),
			*VectorJson(Value.Min));
	}

	void LogReferenceDiagnostics(
		AStaticMeshActor* Reference,
		UStaticMesh* Mesh,
		UStaticMeshComponent* Component)
	{
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		const int32 LodCount = RenderData != nullptr ? RenderData->LODResources.Num() : 0;
		const int32 Lod0Vertices = LodCount > 0
			? RenderData->LODResources[0].GetNumVertices()
			: 0;
		const int32 Lod0Triangles = LodCount > 0
			? RenderData->LODResources[0].GetNumTriangles()
			: 0;
		const IConsoleVariable* NaniteProject =
			IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.ProjectEnabled"));
		const int32 NaniteProjectEnabled = NaniteProject != nullptr
			? NaniteProject->GetInt()
			: -1;

		const FTransform ActorTransform = Reference->GetActorTransform();
		const FTransform RelativeTransform = Component->GetRelativeTransform();
		const FTransform WorldTransform = Component->GetComponentTransform();
		const FBox MeshLocalBounds = Mesh->GetBoundingBox();
		const FBox MeshBoundsByRelativeTransform =
			MeshLocalBounds.TransformBy(RelativeTransform);
		const FBox ComponentCalcBoundsByRelativeTransform =
			Component->CalcBounds(RelativeTransform).GetBox();
		FVector ActorBoundsOrigin;
		FVector ActorBoundsExtent;
		Reference->GetActorBounds(false, ActorBoundsOrigin, ActorBoundsExtent);
		const FBox ActorBounds(
			ActorBoundsOrigin - ActorBoundsExtent,
			ActorBoundsOrigin + ActorBoundsExtent);

		UE_LOG(
			LogCorsairsReferenceTerrainRuntime,
			Display,
			TEXT("CORSAIRS_TERRAIN_RUNTIME_DIAGNOSTIC={")
			TEXT("\"actorBounds\":%s,\"actorTransform\":%s,")
			TEXT("\"componentCalcBoundsByRelativeTransform\":%s,")
			TEXT("\"componentRegistered\":%s,\"lod0Triangles\":%d,")
			TEXT("\"lod0Vertices\":%d,\"lodCount\":%d,")
			TEXT("\"meshBoundsByRelativeTransform\":%s,")
			TEXT("\"meshHasValidNaniteData\":%s,\"meshHasValidRenderData\":%s,")
			TEXT("\"meshLocalBounds\":%s,\"naniteProjectEnabled\":%d,")
			TEXT("\"relativeTransform\":%s,\"renderDataInitialized\":%s,")
			TEXT("\"renderDataPresent\":%s,\"rootIsStaticMeshComponent\":%s,")
			TEXT("\"worldTransform\":%s}"),
			*BoxJson(ActorBounds),
			*TransformJson(ActorTransform),
			*BoxJson(ComponentCalcBoundsByRelativeTransform),
			BoolJson(Component->IsRegistered()),
			Lod0Triangles,
			Lod0Vertices,
			LodCount,
			*BoxJson(MeshBoundsByRelativeTransform),
			BoolJson(Mesh->HasValidNaniteData()),
			BoolJson(Mesh->HasValidRenderData(false)),
			*BoxJson(MeshLocalBounds),
			NaniteProjectEnabled,
			*TransformJson(RelativeTransform),
			BoolJson(RenderData != nullptr && RenderData->IsInitialized()),
			BoolJson(RenderData != nullptr),
			BoolJson(Reference->GetRootComponent() == Component),
			*TransformJson(WorldTransform));
	}

	bool Sha256Bytes(const uint8* Data, const int64 SizeBytes, FString& OutSha256)
	{
		if (Data == nullptr || SizeBytes <= 0 ||
			SizeBytes > static_cast<int64>(MAX_uint32))
		{
			return false;
		}

		FSHA256Signature Signature;
#if PLATFORM_MAC
		const bool bComputed = CC_SHA256(
			Data,
			static_cast<CC_LONG>(SizeBytes),
			Signature.Signature) != nullptr;
#else
		const bool bComputed = FPlatformMisc::GetSHA256Signature(
			Data,
			static_cast<uint32>(SizeBytes),
			Signature);
#endif
		if (!bComputed)
		{
			return false;
		}

		OutSha256 = Signature.ToString().ToLower();
		return IsLowerHex(OutSha256, 64);
	}

	bool ReadRuntimeInput(const TCHAR* RelativePath, FRuntimeInput& Out)
	{
		Out.RelativePath = RelativePath;
		const FString FullPath = FPaths::ProjectDir() / RelativePath;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FullPath) || Bytes.IsEmpty())
		{
			return false;
		}
		if (!Sha256Bytes(Bytes.GetData(), Bytes.Num(), Out.Sha256))
		{
			return false;
		}
		Out.SizeBytes = Bytes.Num();
		return true;
	}

	FString RuntimeInputJson(const FRuntimeInput& Input)
	{
		return FString::Printf(
			TEXT("{\"projectRelativePath\":\"%s\",\"sha256\":\"%s\",\"sizeBytes\":%lld}"),
			*Input.RelativePath,
			*Input.Sha256,
			Input.SizeBytes);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsReferenceTerrainRuntimeTest,
	"Corsairs.Terrain.ReferenceRuntime.CookedWorld",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsReferenceTerrainRuntimeTest::RunTest(const FString&)
{
	FString TransactionId;
	FString SourceHead;
	TestTrue(TEXT("transaction argument exists"), FParse::Value(
		FCommandLine::Get(), TEXT("CorsairsTerrainTransaction="), TransactionId));
	TestTrue(TEXT("source-head argument exists"), FParse::Value(
		FCommandLine::Get(), TEXT("CorsairsTerrainSourceHead="), SourceHead));
	TestTrue(TEXT("transaction is 32 lowercase hex"), IsLowerHex(TransactionId, 32));
	TestTrue(TEXT("source head is 40 lowercase hex"), IsLowerHex(SourceHead, 40));

	UWorld* World = LoadObject<UWorld>(nullptr, WorldObject);
	TestNotNull(TEXT("cooked Garner world loads without play"), World);
	if (World == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("cooked map package"), World->GetOutermost()->GetName(),
		FString(MapPackage));
	TestEqual(TEXT("cooked world object"), World->GetPathName(), FString(WorldObject));
	UClass* DefaultGameMode = World->GetWorldSettings()->DefaultGameMode.Get();
	TestNotNull(TEXT("cooked default GameMode exists"), DefaultGameMode);
	TestEqual(TEXT("cooked default GameMode path"),
		DefaultGameMode != nullptr ? DefaultGameMode->GetPathName() : FString(),
		FString(GameModeObject));

	UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(MeshObject).TryLoad());
	UTexture2D* Texture = Cast<UTexture2D>(FSoftObjectPath(TextureObject).TryLoad());
	UMaterial* Material = Cast<UMaterial>(FSoftObjectPath(MaterialObject).TryLoad());
	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(
		FSoftObjectPath(InstanceObject).TryLoad());
	TestNotNull(TEXT("cooked mesh"), Mesh);
	TestNotNull(TEXT("cooked texture"), Texture);
	TestNotNull(TEXT("cooked material"), Material);
	TestNotNull(TEXT("cooked instance"), Instance);
	if (Mesh == nullptr || Texture == nullptr || Material == nullptr || Instance == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("cooked texture sRGB"), Texture->SRGB);
	TestEqual(TEXT("cooked texture filter"), Texture->Filter, TF_Bilinear);
	TestEqual(TEXT("cooked texture address X"), Texture->AddressX, TA_Clamp);
	TestEqual(TEXT("cooked texture address Y"), Texture->AddressY, TA_Clamp);
	TestEqual(TEXT("cooked material blend"), Material->BlendMode, BLEND_Masked);
	TestEqual(TEXT("cooked material shading"), Material->GetShadingModels(),
		FMaterialShadingModelField(MSM_Unlit));
	TestTrue(TEXT("cooked mesh Nanite"), Mesh->HasValidNaniteData());
	TestTrue(TEXT("cooked mesh material slot"), Mesh->GetMaterial(0) == Instance);
	TestTrue(TEXT("cooked instance parent"), Instance->Parent.Get() == Material);
	UTexture* BoundTexture = nullptr;
	TestTrue(TEXT("cooked BaseColorTexture parameter"),
		Instance->GetTextureParameterValue(
			FMaterialParameterInfo(TEXT("BaseColorTexture")), BoundTexture));
	TestTrue(TEXT("cooked texture binding"), BoundTexture == Texture);

	AStaticMeshActor* Reference = nullptr;
	int32 ReferenceCount = 0;
	if (World->PersistentLevel != nullptr)
	{
		for (AActor* Actor : World->PersistentLevel->Actors)
		{
			if (Actor != nullptr && Actor->GetPathName() == ActorObject)
			{
				Reference = Cast<AStaticMeshActor>(Actor);
				++ReferenceCount;
			}
		}
	}
	TestEqual(TEXT("one cooked reference actor"), ReferenceCount, 1);
	TestNotNull(TEXT("cooked reference actor type"), Reference);
	if (Reference != nullptr)
	{
		TestTrue(TEXT("cooked actor location"), Reference->GetActorLocation().Equals(
			FVector(217600.0, -268800.0, 0.0), 0.01));
		UStaticMeshComponent* Component = Reference->GetStaticMeshComponent();
		LogReferenceDiagnostics(Reference, Mesh, Component);
		TestTrue(TEXT("cooked component mesh"), Component->GetStaticMesh() == Mesh);
		TestTrue(TEXT("cooked component material"),
			Component->GetMaterial(0) == Instance);
		FVector Origin;
		FVector Extent;
		Reference->GetActorBounds(false, Origin, Extent);
		TestTrue(TEXT("cooked min X"), FMath::IsNearlyEqual(
			Origin.X - Extent.X, 217600.0, 1.0));
		TestTrue(TEXT("cooked max X"), FMath::IsNearlyEqual(
			Origin.X + Extent.X, 230400.0, 1.0));
		TestTrue(TEXT("cooked min Y"), FMath::IsNearlyEqual(
			Origin.Y - Extent.Y, -281600.0, 1.0));
		TestTrue(TEXT("cooked max Y"), FMath::IsNearlyEqual(
			Origin.Y + Extent.Y, -268800.0, 1.0));
	}

	TArray<FRuntimeInput> Inputs;
	Inputs.SetNum(3);
	TestTrue(TEXT("Data/character_map.json is readable"),
		ReadRuntimeInput(TEXT("Data/character_map.json"), Inputs[0]));
	TestTrue(TEXT("Data/Heights/garner.block.raw is readable"),
		ReadRuntimeInput(TEXT("Data/Heights/garner.block.raw"), Inputs[1]));
	TestTrue(TEXT("Data/Heights/garner.terrain.json is readable"),
		ReadRuntimeInput(TEXT("Data/Heights/garner.terrain.json"), Inputs[2]));
	if (HasAnyErrors())
	{
		return false;
	}
	const FString Json = FString::Printf(
		TEXT("{\"actorObject\":\"%s\",\"gameModeClass\":\"%s\",\"issues\":[],")
		TEXT("\"mapPackage\":\"%s\",\"reportType\":\"garner-terrain-packaged-runtime\",")
		TEXT("\"runtimeInputs\":[%s,%s,%s],\"schemaVersion\":1,\"sourceHead\":\"%s\",")
		TEXT("\"status\":\"PASS\",\"transactionId\":\"%s\",\"worldObject\":\"%s\"}"),
		ActorObject,
		GameModeObject,
		MapPackage,
		*RuntimeInputJson(Inputs[0]),
		*RuntimeInputJson(Inputs[1]),
		*RuntimeInputJson(Inputs[2]),
		*SourceHead,
		*TransactionId,
		WorldObject);
	UE_LOG(
		LogCorsairsReferenceTerrainRuntime,
		Display,
		TEXT("CORSAIRS_TERRAIN_RUNTIME_JSON=%s"),
		*Json);
	return true;
}

#endif
