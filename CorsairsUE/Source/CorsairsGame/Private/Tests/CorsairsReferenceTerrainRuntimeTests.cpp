#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
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
	const FAssetData MeshAssetData =
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(
			FSoftObjectPath(MeshObject),
			/*bIncludeOnlyOnDiskAssets=*/ true);
	TestTrue(TEXT("cooked mesh disk asset data"), MeshAssetData.IsValid());
	FString NaniteEnabled;
	TestTrue(TEXT("cooked mesh Nanite tag"),
		MeshAssetData.GetTagValue(TEXT("NaniteEnabled"), NaniteEnabled));
	TestEqual(TEXT("cooked mesh Nanite enabled"),
		NaniteEnabled, FString(TEXT("True")));
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
		UStaticMeshComponent* Component = Reference->GetStaticMeshComponent();
		TestNotNull(TEXT("cooked reference component"), Component);
		if (Component == nullptr)
		{
			return false;
		}
		TestTrue(TEXT("cooked reference root component"),
			Reference->GetRootComponent() == Component);
		TestTrue(TEXT("cooked reference root has no attach parent"),
			Component->GetAttachParent() == nullptr);
		const FTransform RelativeTransform =
			Component->GetRelativeTransform();
		TestTrue(TEXT("cooked serialized location"),
			RelativeTransform.GetLocation().Equals(
				FVector(217600.0, -268800.0, 0.0), 0.01));
		TestTrue(TEXT("cooked serialized rotation"),
			RelativeTransform.GetRotation().Equals(
				FQuat::Identity, UE_KINDA_SMALL_NUMBER));
		TestTrue(TEXT("cooked serialized scale"),
			RelativeTransform.GetScale3D().Equals(
				FVector::OneVector, UE_KINDA_SMALL_NUMBER));
		TestTrue(TEXT("cooked component mesh"), Component->GetStaticMesh() == Mesh);
		TestTrue(TEXT("cooked component material"),
			Component->GetMaterial(0) == Instance);
		const FBox Bounds = Component->CalcBounds(RelativeTransform).GetBox();
		TestTrue(TEXT("cooked min X"), FMath::IsNearlyEqual(
			Bounds.Min.X, 217600.0, 1.0));
		TestTrue(TEXT("cooked max X"), FMath::IsNearlyEqual(
			Bounds.Max.X, 230400.0, 1.0));
		TestTrue(TEXT("cooked min Y"), FMath::IsNearlyEqual(
			Bounds.Min.Y, -281600.0, 1.0));
		TestTrue(TEXT("cooked max Y"), FMath::IsNearlyEqual(
			Bounds.Max.Y, -268800.0, 1.0));
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
