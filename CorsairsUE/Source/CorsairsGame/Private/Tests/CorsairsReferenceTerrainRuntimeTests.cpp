#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/StringConv.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"

#if PLATFORM_MAC
#include <CommonCrypto/CommonDigest.h>
#endif

#if PLATFORM_MAC
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

	bool NormalizeProbeDirectory(const FString& Raw, FString& Out)
	{
		Out.Reset();
		if (Raw.IsEmpty() || FPaths::IsRelative(Raw))
		{
			return false;
		}
		Out = FPaths::ConvertRelativePathToFull(Raw);
		FPaths::RemoveDuplicateSlashes(Out);
		FPaths::NormalizeDirectoryName(Out);
		return !Out.IsEmpty() && !FPaths::IsRelative(Out) && Out != TEXT("/");
	}

	bool SandboxDirectoryComponents(
		const FString& ContainerDataRoot,
		const FString& AutomationReportsRoot,
		TArray<FString>& OutComponents)
	{
		OutComponents.Reset();
		const FString Prefix = ContainerDataRoot + TEXT("/");
		if (!AutomationReportsRoot.StartsWith(
			Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FString RelativeSuffix = AutomationReportsRoot.Mid(Prefix.Len());
		RelativeSuffix.ParseIntoArray(OutComponents, TEXT("/"), false);
		if (OutComponents.IsEmpty())
		{
			return false;
		}

		FString ReconstructedRoot = ContainerDataRoot;
		for (const FString& Component : OutComponents)
		{
			if (Component.IsEmpty() || Component == TEXT(".") ||
				Component == TEXT("..") || Component.Contains(TEXT("\\")) ||
				Component.Contains(TEXT("/")))
			{
				return false;
			}
			ReconstructedRoot = FPaths::Combine(ReconstructedRoot, Component);
		}
		if (ReconstructedRoot != AutomationReportsRoot)
		{
			return false;
		}
		return true;
	}

#if PLATFORM_MAC
	struct FSandboxDirectoryIdentity
	{
		uint64 Device = 0;
		uint64 Inode = 0;
		uint32 Owner = 0;
		uint32 Mode = 0;
	};

	bool SameSandboxDirectoryIdentity(
		const FSandboxDirectoryIdentity& Left,
		const FSandboxDirectoryIdentity& Right)
	{
		return Left.Device == Right.Device && Left.Inode == Right.Inode &&
			Left.Owner == Right.Owner && Left.Mode == Right.Mode;
	}

	bool IsSafeSandboxDirectory(
		const int Descriptor,
		const bool bRequireExactOwnerMode,
		FSandboxDirectoryIdentity* OutIdentity = nullptr)
	{
		struct stat DirectoryInfo = {};
		if (::fstat(Descriptor, &DirectoryInfo) != 0 ||
			!S_ISDIR(DirectoryInfo.st_mode) ||
			DirectoryInfo.st_uid != ::geteuid() ||
			(DirectoryInfo.st_mode & (S_IWGRP | S_IWOTH)) != 0)
		{
			return false;
		}
		if (bRequireExactOwnerMode &&
			(DirectoryInfo.st_mode & static_cast<mode_t>(ALLPERMS)) !=
				static_cast<mode_t>(0700))
		{
			return false;
		}
		if (OutIdentity != nullptr)
		{
			OutIdentity->Device = static_cast<uint64>(DirectoryInfo.st_dev);
			OutIdentity->Inode = static_cast<uint64>(DirectoryInfo.st_ino);
			OutIdentity->Owner = static_cast<uint32>(DirectoryInfo.st_uid);
			OutIdentity->Mode = static_cast<uint32>(
				DirectoryInfo.st_mode & static_cast<mode_t>(ALLPERMS));
		}
		return true;
	}

	int OpenVerifiedSandboxDirectory(
		const FString& Path,
		FSandboxDirectoryIdentity* OutIdentity = nullptr)
	{
		const FTCHARToUTF8 PathUtf8(*Path);
		const int Descriptor = ::open(
			PathUtf8.Get(),
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
		if (Descriptor < 0 ||
			!IsSafeSandboxDirectory(Descriptor, false, OutIdentity))
		{
			if (Descriptor >= 0)
			{
				::close(Descriptor);
			}
			return -1;
		}
		return Descriptor;
	}

	int OpenVerifiedSandboxDirectoryAt(
		const int ParentDescriptor,
		const FString& Component,
		const bool bRequireExactOwnerMode,
		FSandboxDirectoryIdentity* OutIdentity = nullptr)
	{
		const FTCHARToUTF8 ComponentUtf8(*Component);
		const int Descriptor = ::openat(
			ParentDescriptor,
			ComponentUtf8.Get(),
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (Descriptor < 0 ||
			!IsSafeSandboxDirectory(
				Descriptor, bRequireExactOwnerMode, OutIdentity))
		{
			if (Descriptor >= 0)
			{
				::close(Descriptor);
			}
			return -1;
		}
		return Descriptor;
	}

	bool VerifySandboxDirectoryChain(
		const FString& ContainerDataRoot,
		const TArray<FString>& Components,
		const TArray<FSandboxDirectoryIdentity>& ExpectedIdentities)
	{
		if (ExpectedIdentities.Num() != Components.Num() + 1)
		{
			return false;
		}
		FSandboxDirectoryIdentity ObservedIdentity;
		int ParentDescriptor = OpenVerifiedSandboxDirectory(
			ContainerDataRoot, &ObservedIdentity);
		if (ParentDescriptor < 0 || !SameSandboxDirectoryIdentity(
			ObservedIdentity, ExpectedIdentities[0]))
		{
			if (ParentDescriptor >= 0)
			{
				::close(ParentDescriptor);
			}
			return false;
		}
		for (int32 Index = 0; Index < Components.Num(); ++Index)
		{
			ObservedIdentity = {};
			const int ChildDescriptor = OpenVerifiedSandboxDirectoryAt(
				ParentDescriptor, Components[Index], false, &ObservedIdentity);
			if (ChildDescriptor < 0 || !SameSandboxDirectoryIdentity(
				ObservedIdentity, ExpectedIdentities[Index + 1]))
			{
				if (ChildDescriptor >= 0)
				{
					::close(ChildDescriptor);
				}
				::close(ParentDescriptor);
				return false;
			}
			::close(ParentDescriptor);
			ParentDescriptor = ChildDescriptor;
		}
		::close(ParentDescriptor);
		return true;
	}
#endif

	bool BootstrapSandboxAutomationReportsDirectory(
		const FString& ContainerDataRoot,
		const FString& AutomationReportsRoot)
	{
		TArray<FString> Components;
		if (!SandboxDirectoryComponents(
			ContainerDataRoot, AutomationReportsRoot, Components))
		{
			return false;
		}
#if PLATFORM_MAC
		TArray<FSandboxDirectoryIdentity> ExpectedIdentities;
		ExpectedIdentities.Reserve(Components.Num() + 1);
		FSandboxDirectoryIdentity ParentIdentity;
		int ParentDescriptor = OpenVerifiedSandboxDirectory(
			ContainerDataRoot, &ParentIdentity);
		if (ParentDescriptor < 0)
		{
			return false;
		}
		ExpectedIdentities.Add(ParentIdentity);
		for (const FString& Component : Components)
		{
			const FTCHARToUTF8 ComponentUtf8(*Component);
			bool bCreated = false;
			FSandboxDirectoryIdentity ChildIdentity;
			int ChildDescriptor = ::openat(
				ParentDescriptor,
				ComponentUtf8.Get(),
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (ChildDescriptor < 0)
			{
				const int OpenError = errno;
				if (OpenError != ENOENT)
				{
					::close(ParentDescriptor);
					return false;
				}
				if (::mkdirat(
						ParentDescriptor,
						ComponentUtf8.Get(),
						static_cast<mode_t>(0700)) != 0)
				{
					::close(ParentDescriptor);
					return false;
				}
				bCreated = true;
				ChildDescriptor = OpenVerifiedSandboxDirectoryAt(
					ParentDescriptor, Component, false, &ChildIdentity);
			}
			else if (!IsSafeSandboxDirectory(
				ChildDescriptor, false, &ChildIdentity))
			{
				::close(ChildDescriptor);
				::close(ParentDescriptor);
				return false;
			}
			if (ChildDescriptor < 0)
			{
				::close(ParentDescriptor);
				return false;
			}
			if (bCreated &&
				(::fchmod(ChildDescriptor, static_cast<mode_t>(0700)) != 0 ||
				 !IsSafeSandboxDirectory(
					 ChildDescriptor, true, &ChildIdentity)))
			{
				::close(ChildDescriptor);
				::close(ParentDescriptor);
				return false;
			}
			const bool bChildSynced = ::fsync(ChildDescriptor) == 0;
			const bool bParentSynced = ::fsync(ParentDescriptor) == 0;
			if (!bChildSynced || !bParentSynced)
			{
				::close(ChildDescriptor);
				::close(ParentDescriptor);
				return false;
			}
			::close(ParentDescriptor);
			ParentDescriptor = ChildDescriptor;
			ExpectedIdentities.Add(ChildIdentity);
		}
		::close(ParentDescriptor);
		return VerifySandboxDirectoryChain(
			ContainerDataRoot, Components, ExpectedIdentities);
#else
		IPlatformFile& PhysicalPlatformFile = IPlatformFile::GetPlatformPhysical();
		if (!PhysicalPlatformFile.DirectoryExists(*ContainerDataRoot))
		{
			return false;
		}
		if (!PhysicalPlatformFile.CreateDirectoryTree(*AutomationReportsRoot) &&
			!PhysicalPlatformFile.DirectoryExists(*AutomationReportsRoot))
		{
			return false;
		}
		return PhysicalPlatformFile.DirectoryExists(*AutomationReportsRoot);
#endif
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
	const bool bSandboxProbe = FParse::Param(
		FCommandLine::Get(), TEXT("CorsairsTerrainSandboxProbe"));
	if (bSandboxProbe)
	{
		if (HasAnyErrors())
		{
			return false;
		}
		const FString BundleIdentifier = FPlatformProcess::GetGameBundleId();
		FString ContainerDataRoot;
		FString AutomationReportsRoot;
		const bool bContainerValid = NormalizeProbeDirectory(
			FString(FPlatformProcess::UserHomeDir()), ContainerDataRoot);
		const bool bReportsValid = NormalizeProbeDirectory(
			FPaths::AutomationReportsDir(), AutomationReportsRoot);
		TestTrue(TEXT("sandbox bundle identifier is nonempty"),
			!BundleIdentifier.IsEmpty());
		TestTrue(TEXT("sandbox container root is normalized absolute"),
			bContainerValid);
		TestTrue(TEXT("sandbox container root has Data leaf"),
			bContainerValid &&
			FPaths::GetCleanFilename(ContainerDataRoot).Equals(
				TEXT("Data"), ESearchCase::CaseSensitive));
		TestTrue(TEXT("automation reports root is normalized absolute"),
			bReportsValid);
		TestTrue(TEXT("automation reports root is strictly below container Data"),
			bContainerValid && bReportsValid &&
			AutomationReportsRoot != ContainerDataRoot &&
			FPaths::IsUnderDirectory(AutomationReportsRoot, ContainerDataRoot));
		if (HasAnyErrors())
		{
			return false;
		}
		if (!BootstrapSandboxAutomationReportsDirectory(
			ContainerDataRoot, AutomationReportsRoot))
		{
			AddError(TEXT("sandbox automation reports bootstrap failed"));
			return false;
		}
		const FString Json = FString::Printf(
			TEXT("{\"automationReportsRoot\":%s,\"bundleIdentifier\":%s,")
			TEXT("\"containerDataRoot\":%s,\"issues\":[],")
			TEXT("\"reportType\":\"garner-terrain-packaged-sandbox\",")
			TEXT("\"schemaVersion\":1,\"sourceHead\":%s,\"status\":\"PASS\",")
			TEXT("\"transactionId\":%s}"),
			*EscapeJsonString(AutomationReportsRoot),
			*EscapeJsonString(BundleIdentifier),
			*EscapeJsonString(ContainerDataRoot),
			*EscapeJsonString(SourceHead),
			*EscapeJsonString(TransactionId));
		UE_LOG(
			LogCorsairsReferenceTerrainRuntime,
			Display,
			TEXT("CORSAIRS_TERRAIN_SANDBOX_JSON=%s"),
			*Json);
		return true;
	}
	else
	{
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
}

#endif
