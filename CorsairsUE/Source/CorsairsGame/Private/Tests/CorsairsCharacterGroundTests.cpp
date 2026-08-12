#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCharacterGround.h"

#include "Components/CapsuleComponent.h"
#include "CorsairsCharacter.h"
#include "CorsairsGameMode.h"
#include "CorsairsPlayerCharacter.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"

namespace
{
	TArray<uint8> MakeRectangularRaster()
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(3 * 2 * 4);
		Bytes[0] = 0x05;
		Bytes[1] = 0x45;
		Bytes[2] = 0x85;
		Bytes[3] = 0x00;
		Bytes[23] = 0x06;
		return Bytes;
	}

	TArray<uint8> EncodeHeightR16(
		const std::initializer_list<int32> RawHeights)
	{
		TArray<uint8> Bytes;
		Bytes.Reserve(static_cast<int32>(RawHeights.size()) * 2);
		for (const int32 RawHeight : RawHeights)
		{
			const uint16 Encoded = static_cast<uint16>((RawHeight + 128) * 256);
			Bytes.Add(static_cast<uint8>(Encoded));
			Bytes.Add(static_cast<uint8>(Encoded >> 8));
		}
		return Bytes;
	}

	FActorSpawnParameters AlwaysSpawnParameters()
	{
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Parameters;
	}

	struct FCharacterGroundDiskFixture
	{
		FCharacterGroundDiskFixture()
		{
			MapName = TEXT("__corsairs_ground_") +
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Directory =
				FPaths::ProjectDir() / TEXT("Data/Heights");
			MetadataPath = Directory /
				(MapName + TEXT(".terrain.json"));
			BlockPath = Directory /
				(MapName + TEXT(".block.raw"));
			RegionPath = Directory /
				(MapName + TEXT(".region.raw"));
			HeightPath = Directory /
				(MapName + TEXT(".height.r16"));
			RuntimePath = Directory /
				(MapName + TEXT(".runtime.json"));
		}

		~FCharacterGroundDiskFixture()
		{
			IFileManager::Get().Delete(*MetadataPath, false, true);
			IFileManager::Get().Delete(*BlockPath, false, true);
			IFileManager::Get().Delete(*RegionPath, false, true);
			IFileManager::Get().Delete(*HeightPath, false, true);
			IFileManager::Get().Delete(*RuntimePath, false, true);
		}

		bool WriteMetadata(const FString& Text) const
		{
			return FFileHelper::SaveStringToFile(Text, *MetadataPath);
		}

		bool WriteBlock(const TArray<uint8>& Bytes) const
		{
			return FFileHelper::SaveArrayToFile(Bytes, *BlockPath);
		}

		bool WriteNavigationContract(
			const TArray<uint8>& BlockBytes,
			const TArray<uint8>& RegionBytes,
			const TArray<uint8>& HeightBytes,
			const FString& MetadataText) const
		{
			const FString Contract = FString::Printf(
				TEXT("{\"schemaVersion\":1,\"map\":\"%s\",\"gridWidth\":1,")
				TEXT("\"gridHeight\":1,\"files\":{")
				TEXT("\"height\":{\"name\":\"%s.height.r16\",\"sha256\":\"%s\",\"sizeBytes\":%d},")
				TEXT("\"block\":{\"name\":\"%s.block.raw\",\"sha256\":\"%s\",\"sizeBytes\":%d},")
				TEXT("\"region\":{\"name\":\"%s.region.raw\",\"sha256\":\"%s\",\"sizeBytes\":%d},")
				TEXT("\"terrainMetadata\":{\"name\":\"%s.terrain.json\",\"sha256\":\"%s\",\"sizeBytes\":%d}}}"),
				*MapName,
				*MapName,
				TEXT("96a296d224f285c67bee93c30f8a309157f0daa35dc5b87e410b78630a09cfc7"),
				HeightBytes.Num(),
				*MapName,
				TEXT("65b6f0dc207f3311afb8d1c50ccb270a8c48992e7c8bb71104df95213e1a32bc"),
				BlockBytes.Num(),
				*MapName,
				TEXT("47dc540c94ceb704a23875c11273e16bb0b8a87aed84de911f2133568115f254"),
				RegionBytes.Num(),
				*MapName,
				TEXT("2a7017327338a5c1e04e8fdf0dce3eb4b2cb9ef8d483fd6d476f2f5a48dafe50"),
				MetadataText.Len());
			return FFileHelper::SaveArrayToFile(RegionBytes, *RegionPath) &&
				FFileHelper::SaveArrayToFile(HeightBytes, *HeightPath) &&
				FFileHelper::SaveStringToFile(
					Contract,
					*RuntimePath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		void DeleteBlock() const
		{
			IFileManager::Get().Delete(*BlockPath, false, true);
		}

		bool ReplaceContractText(
			const FString& Before,
			const FString& After) const
		{
			FString Contract;
			if (!FFileHelper::LoadFileToString(Contract, *RuntimePath))
			{
				return false;
			}
			Contract = Contract.Replace(*Before, *After);
			return FFileHelper::SaveStringToFile(
				Contract,
				*RuntimePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		FString MapName;
		FString MetadataPath;
		FString BlockPath;
		FString RegionPath;
		FString HeightPath;
		FString RuntimePath;
	};

	int32 CountMovementBindings(
		const UCorsairsSession* Session,
		const ACorsairsPlayerCharacter* Pawn)
	{
		int32 Count = 0;
		for (const UObject* Object :
			Session->OnMovementChanged.GetAllObjects())
		{
			if (Object == Pawn)
			{
				++Count;
			}
		}
		return Count;
	}

	int32 CountAuthorityBindings(
		const UCorsairsSession* Session,
		const ACorsairsPlayerCharacter* Pawn)
	{
		int32 Count = 0;
		for (const UObject* Object :
			Session->OnMovementAuthorityChanged.GetAllObjects())
		{
			if (Object == Pawn)
			{
				++Count;
			}
		}
		return Count;
	}

	int32 CountCharacters(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<ACorsairsCharacter> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundNavigationTest,
	"Corsairs.Movement.Ground.NavigationIsStrictAndTraversalAware",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundNavigationTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	const TArray<uint8> BlockBytes = MakeRectangularRaster();
	const TArray<uint8> HeightBytes = EncodeHeightR16({
		0, 10, 20,
		20, 0, 0,
	});
	const TArray<uint8> RegionBytes = {
		0x01, 0x00,
		0x00, 0x00,
		0x08, 0x00,
		0x02, 0x00,
		0x04, 0x00,
		0x09, 0x00,
	};
	TestTrue(
		TEXT("navigation byte rasters load together"),
		Ground.LoadRuntimeFromBytes(
			3, 2, HeightBytes, BlockBytes, RegionBytes, Error));
	TestTrue(TEXT("navigation is exposed only after both rasters"),
		Ground.IsNavigationLoaded());
	TestEqual(
		TEXT("source bounds use 100-unit tiles"),
		Ground.GetSourceBounds(),
		FIntRect(0, 0, 300, 200));

	double HeightCm = 777.0;
	TestTrue(
		TEXT("strict surface samples an in-bounds quadrant"),
		Ground.TrySampleSurface(FVector2d(25.0, 25.0), HeightCm));
	TestEqual(
		TEXT("surface uses first source triangle, not block height"),
		HeightCm,
		75.0);
	TestTrue(
		TEXT("second source triangle is deterministic"),
		Ground.TrySampleSurface(FVector2d(75.0, 75.0), HeightCm));
	TestEqual(TEXT("second source triangle height"), HeightCm, 75.0);
	TestFalse(
		TEXT("strict surface rejects right edge without four vertices"),
		Ground.TrySampleSurface(FVector2d(200.0, 0.0), HeightCm));
	TestFalse(
		TEXT("strict surface rejects negative fractional coordinate"),
		Ground.TrySampleSurface(FVector2d(-0.1, 0.0), HeightCm));

	FCorsairsNavigationCell Cell;
	TestTrue(
		TEXT("little-endian LAND region is readable"),
		Ground.TrySampleNavigation(
			FIntPoint(0, 0), ECorsairsTraversalKind::Land, Cell));
	TestEqual(TEXT("LAND mask is exact"), Cell.RegionMask, static_cast<uint16>(0x0001));
	TestFalse(TEXT("LAND traversal passes LAND"), Cell.bBlocked);

	TestTrue(
		TEXT("sea traversal can inspect LAND"),
		Ground.TrySampleNavigation(
			FIntPoint(0, 0), ECorsairsTraversalKind::Sea, Cell));
	TestTrue(TEXT("sea traversal blocks LAND"), Cell.bBlocked);

	TestTrue(
		TEXT("sea tile is readable"),
		Ground.TrySampleNavigation(
			FIntPoint(100, 0), ECorsairsTraversalKind::Sea, Cell));
	TestFalse(TEXT("sea traversal passes a region without LAND"), Cell.bBlocked);

	TestTrue(
		TEXT("bridge tile is readable"),
		Ground.TrySampleNavigation(
			FIntPoint(200, 0), ECorsairsTraversalKind::Land, Cell));
	TestEqual(TEXT("BRIDGE mask is little-endian"), Cell.RegionMask, static_cast<uint16>(0x0008));
	TestFalse(TEXT("land traversal passes BRIDGE"), Cell.bBlocked);

	TestTrue(
		TEXT("discretionary traversal reads any known region"),
		Ground.TrySampleNavigation(
			FIntPoint(0, 100), ECorsairsTraversalKind::Discretionary, Cell));
	TestFalse(
		TEXT("navigation rejects out of bounds"),
		Ground.TrySampleNavigation(
			FIntPoint(300, 0), ECorsairsTraversalKind::Land, Cell));

	TArray<uint8> ShortRegion = RegionBytes;
	ShortRegion.Pop();
	TestFalse(
		TEXT("short region raster is rejected"),
		Ground.LoadRuntimeFromBytes(
			3, 2, HeightBytes, BlockBytes, ShortRegion, Error));
	TestFalse(TEXT("failed reload hides all navigation"),
		Ground.IsNavigationLoaded());

	TArray<uint8> UnknownRegion = RegionBytes;
	UnknownRegion[8] = 0x80;
	TestFalse(
		TEXT("unknown region bits reject the whole raster"),
		Ground.LoadRuntimeFromBytes(
			3, 2, HeightBytes, BlockBytes, UnknownRegion, Error));
	TestFalse(TEXT("unknown region keeps navigation unavailable"),
		Ground.IsNavigationLoaded());

	TArray<uint8> NonCanonicalHeight = HeightBytes;
	NonCanonicalHeight[0] = 1;
	TestFalse(
		TEXT("non-canonical r16 height is rejected"),
		Ground.LoadRuntimeFromBytes(
			3, 2, NonCanonicalHeight, BlockBytes, RegionBytes, Error));
	TestFalse(TEXT("invalid height keeps runtime surface unavailable"),
		Ground.IsNavigationLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundSamplingTest,
	"Corsairs.Movement.Ground.SamplesRectangularBlockRaster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundSamplingTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	const TArray<uint8> Bytes = MakeRectangularRaster();
	TestTrue(
		TEXT("3x2 block raster loads"),
		Ground.LoadFromBytes(3, 2, Bytes, Error));
	TestTrue(TEXT("loaded raster has no error"), Error.IsEmpty());
	TestTrue(TEXT("loaded state is visible"), Ground.IsLoaded());

	const FCorsairsCharacterCell TopLeft = Ground.Sample(FIntPoint(0, 0));
	TestEqual(TEXT("top-left height"), TopLeft.HeightCm, 25.0);
	TestFalse(TEXT("top-left is free"), TopLeft.bBlocked);

	const FCorsairsCharacterCell TopRight = Ground.Sample(FIntPoint(50, 0));
	TestEqual(TEXT("sign bit makes height negative"), TopRight.HeightCm, -25.0);
	TestFalse(TEXT("sign bit is not a block bit"), TopRight.bBlocked);

	const FCorsairsCharacterCell BottomLeft = Ground.Sample(FIntPoint(0, 50));
	TestEqual(TEXT("bottom-left height"), BottomLeft.HeightCm, 25.0);
	TestTrue(TEXT("block bit is reported independently"), BottomLeft.bBlocked);

	const FCorsairsCharacterCell BottomRight = Ground.Sample(FIntPoint(50, 50));
	TestEqual(TEXT("bottom-right height"), BottomRight.HeightCm, 0.0);
	TestFalse(TEXT("zero quadrant is free"), BottomRight.bBlocked);

	const FCorsairsCharacterCell RectangularLast =
		Ground.Sample(FIntPoint(250, 150));
	TestEqual(
		TEXT("rectangular row stride reaches final tile quadrant"),
		RectangularLast.HeightCm,
		30.0);

	TestEqual(
		TEXT("top-left actor center"),
		Ground.ActorCenter(FIntPoint(0, 0), 88.0),
		FVector(0.0, 0.0, 113.0));
	TestEqual(
		TEXT("negative-height actor center"),
		Ground.ActorCenter(FIntPoint(50, 0), 88.0),
		FVector(0.0, 50.0, 63.0));
	TestEqual(
		TEXT("source XY is rotated into the rigid UE basis"),
		Ground.ActorCenter(FIntPoint(0, 50), 88.0),
		FVector(-50.0, 0.0, 113.0));

	const TArray<FIntPoint> OutOfRange = {
		FIntPoint(-1, 0),
		FIntPoint(0, -1),
		FIntPoint(300, 0),
		FIntPoint(0, 200),
	};
	for (const FIntPoint Position : OutOfRange)
	{
		const FCorsairsCharacterCell Cell = Ground.Sample(Position);
		TestEqual(
			FString::Printf(TEXT("out-of-range height at %s"), *Position.ToString()),
			Cell.HeightCm,
			0.0);
		TestFalse(
			FString::Printf(TEXT("out-of-range block at %s"), *Position.ToString()),
			Cell.bBlocked);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundRejectsInvalidRasterTest,
	"Corsairs.Movement.Ground.RejectsInvalidBlockRaster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundRejectsInvalidRasterTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(23);
	TestFalse(
		TEXT("3x2 raster rejects 23 bytes"),
		Ground.LoadFromBytes(3, 2, Bytes, Error));
	TestTrue(TEXT("size rejection is explicit"), !Error.IsEmpty());
	TestFalse(TEXT("rejected raster is not loaded"), Ground.IsLoaded());

	Error.Empty();
	TArray<uint8> Empty;
	TestFalse(
		TEXT("max dimensions reject without signed overflow"),
		Ground.LoadFromBytes(MAX_int32, MAX_int32, Empty, Error));
	TestTrue(
		TEXT("overflow rejection names the excessive raster"),
		Error.Contains(TEXT("слишком велик")));
	TestFalse(TEXT("overflow rejection stays unloaded"), Ground.IsLoaded());

	Error.Empty();
	TestFalse(
		TEXT("source bounds reject multiplication overflow"),
		Ground.LoadFromBytes(MAX_int32 / 100 + 1, 1, Empty, Error));
	TestTrue(
		TEXT("source bounds overflow is explicit"),
		Error.Contains(TEXT("source bounds")));

	Error.Empty();
	AddExpectedError(
		TEXT("метаданные character ground не найдены"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("missing production metadata fails"),
		Ground.Load(TEXT("__corsairs_missing_character_ground__"), Error));
	TestTrue(TEXT("missing production data reports an error"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundProductionFilesTest,
	"Corsairs.Movement.Ground.ValidatesProductionFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundProductionFilesTest::RunTest(const FString&)
{
	FCharacterGroundDiskFixture Fixture;
	FCorsairsCharacterGround Ground;
	FString Error;
	const TArray<uint8> ValidBytes = {0x05, 0x45, 0x85, 0x00};
	const TArray<uint8> ValidRegion = {0x01, 0x00};
	const TArray<uint8> ValidHeight = {0x00, 0x00};
	const FString ValidMetadata =
		TEXT("{\"gridWidth\":1,\"gridHeight\":1}");
	TestTrue(
		TEXT("valid metadata is written"),
		Fixture.WriteMetadata(ValidMetadata));
	TestTrue(
		TEXT("valid block raster is written"),
		Fixture.WriteBlock(ValidBytes));
	TestTrue(
		TEXT("valid strict navigation contract is written"),
		Fixture.WriteNavigationContract(
			ValidBytes, ValidRegion, ValidHeight, ValidMetadata));
	TestTrue(
		TEXT("real disk load succeeds"),
		Ground.Load(Fixture.MapName, Error));
	TestTrue(
		TEXT("real disk load exposes navigation"),
		Ground.IsNavigationLoaded());
	TestEqual(
		TEXT("real disk load decodes block byte"),
		Ground.Sample(FIntPoint(0, 0)).HeightCm,
		25.0);

	const TArray<uint8> CorruptRegion = {0x02, 0x00};
	TestTrue(
		TEXT("same-size corrupt region is written"),
		FFileHelper::SaveArrayToFile(CorruptRegion, *Fixture.RegionPath));
	AddExpectedError(
		Fixture.MapName,
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("same-size region hash mismatch is rejected"),
		Ground.Load(Fixture.MapName, Error));
	TestFalse(
		TEXT("hash mismatch keeps navigation unavailable"),
		Ground.IsNavigationLoaded());
	TestTrue(
		TEXT("valid region is restored"),
		FFileHelper::SaveArrayToFile(ValidRegion, *Fixture.RegionPath));
	TArray<uint8> MetadataWithBom = {0xef, 0xbb, 0xbf};
	FTCHARToUTF8 ValidMetadataUtf8(*ValidMetadata);
	MetadataWithBom.Append(
		reinterpret_cast<const uint8*>(ValidMetadataUtf8.Get()),
		ValidMetadataUtf8.Length());
	TestTrue(
		TEXT("same JSON with different raw BOM bytes is written"),
		FFileHelper::SaveArrayToFile(MetadataWithBom, *Fixture.MetadataPath));
	TestTrue(
		TEXT("fixture keeps size equal so raw SHA is the rejecting gate"),
		Fixture.ReplaceContractText(
			TEXT("\"sizeBytes\":30}}}"),
			TEXT("\"sizeBytes\":33}}}")));
	AddExpectedError(
		Fixture.MapName,
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("raw metadata hash mismatch is rejected"),
		Ground.Load(Fixture.MapName, Error));
	TestTrue(
		TEXT("metadata rejection reports exact SHA gate"),
		Error.Contains(TEXT("SHA-256")));
	TestFalse(
		TEXT("metadata hash mismatch keeps navigation unavailable"),
		Ground.IsNavigationLoaded());
	TestTrue(
		TEXT("valid raw metadata bytes are restored"),
		Fixture.WriteMetadata(ValidMetadata));
	TestTrue(
		TEXT("valid metadata contract size is restored"),
		Fixture.ReplaceContractText(
			TEXT("\"sizeBytes\":33}}}"),
			TEXT("\"sizeBytes\":30}}}")));

	TestTrue(
		TEXT("malformed metadata is written"),
		Fixture.WriteMetadata(TEXT("{")));
	AddExpectedError(
		Fixture.MapName,
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("malformed metadata is rejected"),
		Ground.Load(Fixture.MapName, Error));
	TestTrue(
		TEXT("malformed metadata error is explicit"),
		Error.Contains(TEXT("некорректный JSON")));

	const TArray<FString> InvalidNumberMetadata = {
		TEXT("{\"gridWidth\":true,\"gridHeight\":1}"),
		TEXT("{\"gridWidth\":\"1\",\"gridHeight\":1}"),
		TEXT("{\"gridWidth\":1.5,\"gridHeight\":1}"),
		TEXT("{\"gridWidth\":2147483648,\"gridHeight\":1}"),
		TEXT("{\"gridWidth\":1e999,\"gridHeight\":1}"),
		TEXT("{\"gridWidth\":1,\"gridHeight\":false}"),
	};
	for (int32 Index = 0; Index < InvalidNumberMetadata.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("invalid numeric metadata %d is written"), Index),
			Fixture.WriteMetadata(InvalidNumberMetadata[Index]));
		AddExpectedError(
			Fixture.MapName,
			EAutomationExpectedErrorFlags::Contains,
			1);
		TestFalse(
			FString::Printf(TEXT("invalid numeric metadata %d is rejected"), Index),
			Ground.Load(Fixture.MapName, Error));
		TestTrue(
			FString::Printf(TEXT("invalid numeric metadata %d names integer contract"), Index),
			Error.Contains(TEXT("положительным целым")));
		TestFalse(
			FString::Printf(TEXT("invalid numeric metadata %d stays unloaded"), Index),
			Ground.IsLoaded());
	}

	TestTrue(
		TEXT("metadata for missing raw is written"),
		Fixture.WriteMetadata(
			TEXT("{\"gridWidth\":1,\"gridHeight\":1}")));
	Fixture.DeleteBlock();
	AddExpectedError(
		Fixture.MapName,
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("missing raw raster is rejected"),
		Ground.Load(Fixture.MapName, Error));
	TestTrue(
		TEXT("missing raw error identifies raster"),
		Error.Contains(TEXT("raster не найден")));

	const TArray<uint8> WrongSizeBytes = {0x05, 0x45, 0x85};
	TestTrue(
		TEXT("wrong-size raw is written"),
		Fixture.WriteBlock(WrongSizeBytes));
	AddExpectedError(
		Fixture.MapName,
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("wrong-size raw raster is rejected"),
		Ground.Load(Fixture.MapName, Error));
	TestTrue(
		TEXT("wrong-size raw error states exact expected size"),
		Error.Contains(TEXT("ожидалось 4 bytes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundRejectsUnloadedActorsTest,
	"Corsairs.Movement.Ground.RejectsUnloadedActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundRejectsUnloadedActorsTest::RunTest(
	const FString&)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	TestNotNull(TEXT("fail-closed world created"), World);
	if (World == nullptr)
	{
		return false;
	}

	ACorsairsGameMode* GameMode = World->SpawnActor<ACorsairsGameMode>(
		ACorsairsGameMode::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		AlwaysSpawnParameters());
	ACorsairsPlayerCharacter* Local =
		World->SpawnActor<ACorsairsPlayerCharacter>(
			ACorsairsPlayerCharacter::StaticClass(),
			FVector(1000.0, 1000.0, 1000.0),
			FRotator::ZeroRotator,
			AlwaysSpawnParameters());
	TestNotNull(TEXT("fail-closed game mode spawned"), GameMode);
	TestNotNull(TEXT("fail-closed local actor spawned"), Local);
	if (GameMode == nullptr || Local == nullptr)
	{
		World->DestroyWorld(false);
		return false;
	}

	const FVector InitialLocation = Local->GetActorLocation();
	const float InitialGravity = Local->GetCharacterMovement()->GravityScale;
	const EMovementMode InitialMode =
		Local->GetCharacterMovement()->MovementMode;
	GameMode->GroundCharacterForTests(Local, FIntPoint(0, 0));
	TestEqual(
		TEXT("unloaded ground does not move local actor to sea Z"),
		Local->GetActorLocation(),
		InitialLocation);
	TestEqual(
		TEXT("unloaded ground does not disable local gravity"),
		Local->GetCharacterMovement()->GravityScale,
		InitialGravity);
	TestEqual(
		TEXT("unloaded ground does not change local movement mode"),
		Local->GetCharacterMovement()->MovementMode,
		InitialMode);

	ACorsairsCharacter* Remote =
		GameMode->SpawnRemoteCharacterForTests(
			FIntPoint(0, 0),
			FRotator::ZeroRotator);
	TestNull(TEXT("unloaded ground refuses remote spawn"), Remote);

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundActivationFailureTest,
	"Corsairs.Movement.Ground.ActivationFailureIsFailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundActivationFailureTest::RunTest(const FString&)
{
	FTestWorldWrapper TestWorld;
	if (!TestTrue(
		TEXT("activation world created"),
		TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = TestWorld.GetTestWorld();
	ACorsairsGameMode* GameMode = World->SpawnActor<ACorsairsGameMode>(
		ACorsairsGameMode::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		AlwaysSpawnParameters());
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	ACorsairsPlayerCharacter* Pawn =
		World->SpawnActor<ACorsairsPlayerCharacter>(
			ACorsairsPlayerCharacter::StaticClass(),
			FVector(1000.0, 1000.0, 1000.0),
			FRotator::ZeroRotator,
			AlwaysSpawnParameters());
	TestNotNull(TEXT("activation game mode spawned"), GameMode);
	TestNotNull(TEXT("activation controller spawned"), Controller);
	TestNotNull(TEXT("activation pawn spawned"), Pawn);
	if (GameMode == nullptr || Controller == nullptr || Pawn == nullptr)
	{
		return false;
	}
	GameMode->bAutoLogin = false;
	Controller->Possess(Pawn);
	if (!TestTrue(
		TEXT("activation world begins play"),
		TestWorld.BeginPlayInTestWorld()))
	{
		TestWorld.ForwardErrorMessages(this);
		return false;
	}
	TestTrue(
		TEXT("catalog is ready before ground activation"),
		GameMode->IsStartupReady());

	UCorsairsSession* Session = GameMode->GetSession();
	TestNotNull(TEXT("activation session exists"), Session);
	if (Session == nullptr)
	{
		return false;
	}
	Session->SetInWorldForTests(77, FIntPoint(0, 0));
	Session->SetMovementSpeedForTests(450);

	FString PreviousGroundError;
	const TArray<uint8> PreviousGroundBytes = {0x01, 0x00, 0x00, 0x00};
	TestTrue(
		TEXT("previous character ground loads before retry"),
		GameMode->LoadCharacterGroundFromBytesForTests(
			1,
			1,
			PreviousGroundBytes,
			PreviousGroundError));
	GameMode->GroundCharacterForTests(Pawn, FIntPoint(0, 0));
	Pawn->AttachSession(Session);
	Pawn->AddMovementInput(FVector::ForwardVector, 1.0f);
	Pawn->GetCharacterMovement()->Velocity = FVector(120.0, 0.0, 0.0);
	TestEqual(
		TEXT("previous activation binds movement delegate"),
		CountMovementBindings(Session, Pawn),
		1);
	TestEqual(
		TEXT("previous activation binds authority delegate"),
		CountAuthorityBindings(Session, Pawn),
		1);
	TestEqual(
		TEXT("previous activation selects flying movement"),
		Pawn->GetCharacterMovement()->MovementMode,
		MOVE_Flying);
	TestFalse(
		TEXT("previous activation leaves pending prediction input"),
		Pawn->GetPendingMovementInputVector().IsNearlyZero());

	FCorsairsWorldActor Remote;
	Remote.WorldId = 88;
	Remote.Name = TEXT("GroundFailureRemote");
	Remote.Position = FIntPoint(0, 0);
	Remote.TypeId = 1;
	Session->AddVisibleActorForTests(Remote);
	const int32 CharactersBeforeRemote = CountCharacters(World);
	GameMode->HandleActorSeenForTests(Remote);
	TestEqual(
		TEXT("previous world registers a real remote actor"),
		CountCharacters(World),
		CharactersBeforeRemote + 1);
	TestEqual(
		TEXT("all previous remote registries contain the actor"),
		GameMode->GetRemoteRegistryCountsForTests(),
		FIntVector(1, 1, 1));
	TestEqual(
		TEXT("session cache contains the previous remote"),
		Session->GetVisibleActors().Num(),
		1);
	ACorsairsCharacter* RegisteredRemote = nullptr;
	for (TActorIterator<ACorsairsCharacter> It(World); It; ++It)
	{
		if (*It != Pawn)
		{
			RegisteredRemote = *It;
			break;
		}
	}
	TestNotNull(TEXT("registered remote actor is discoverable"), RegisteredRemote);

	const FString MapName = TEXT("__corsairs_missing_activation_ground__");
	const FString MetadataPath =
		FPaths::ProjectDir() / TEXT("Data/Heights") /
		(MapName + TEXT(".terrain.json"));
	const FString LoadReason = FString::Printf(
		TEXT("метаданные character ground не найдены: %s"),
		*MetadataPath);
	const FString ExpectedStartupError = FString::Printf(
		TEXT("character ground карты %s не загрузился: %s"),
		*MapName,
		*LoadReason);
	const FVector InitialLocation = Pawn->GetActorLocation();
	AddExpectedError(
		TEXT("появление персонажа 88 заблокировано"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		MapName,
		EAutomationExpectedErrorFlags::Contains,
		3);
	FCorsairsWorldActor LocalActor;
	LocalActor.WorldId = 77;
	LocalActor.Name = TEXT("GroundFailureLocal");
	LocalActor.Position = FIntPoint(0, 0);
	LocalActor.TypeId = 1;
	LocalActor.Look.TypeId = 1;
	LocalActor.Look.HairId = 2000;
	LocalActor.Look.EquipIds.SetNumZeroed(CorsairsEquipSlotCount);
	LocalActor.Look.EquipIds[1] = 255;
	LocalActor.Look.EquipIds[2] = 289;
	LocalActor.Look.EquipIds[3] = 465;
	LocalActor.Look.EquipIds[4] = 641;
	Session->SetInWorldAndBroadcastForTests(LocalActor, MapName);
	TestEqual(
		TEXT("startup error preserves exact path and load reason"),
		GameMode->GetStartupError(),
		ExpectedStartupError);
	TestEqual(
		TEXT("failed activation leaves local transform unchanged"),
		Pawn->GetActorLocation(),
		InitialLocation);
	TestTrue(
		TEXT("failed activation never selects flying movement"),
		Pawn->GetCharacterMovement()->MovementMode != MOVE_Flying);
	TestTrue(
		TEXT("failed activation keeps gravity enabled"),
		Pawn->GetCharacterMovement()->GravityScale > 0.0f);
	TestTrue(
		TEXT("failed reactivation stops predicted velocity"),
		Pawn->GetCharacterMovement()->Velocity.IsNearlyZero());
	TestTrue(
		TEXT("failed reactivation consumes pending prediction input"),
		Pawn->GetPendingMovementInputVector().IsNearlyZero());
	TestEqual(
		TEXT("failed activation does not bind movement delegate"),
		CountMovementBindings(Session, Pawn),
		0);
	TestEqual(
		TEXT("failed activation does not bind authority delegate"),
		CountAuthorityBindings(Session, Pawn),
		0);
	TestTrue(
		TEXT("failed activation destroys a previously registered remote"),
		RegisteredRemote != nullptr && RegisteredRemote->IsActorBeingDestroyed());
	TestEqual(
		TEXT("failed activation empties all remote registries"),
		GameMode->GetRemoteRegistryCountsForTests(),
		FIntVector::ZeroValue);
	TestEqual(
		TEXT("failed activation removes the remote actor immediately"),
		CountCharacters(World),
		1);
	TestEqual(
		TEXT("new ENTERMAP immediately clears stale session cache"),
		Session->GetVisibleActors().Num(),
		0);
	TestEqual(
		TEXT("logout is not reentrant inside stage callback"),
		Session->GetStage(),
		ECorsairsLoginStage::InWorld);

	const int32 CharactersBeforeSeen = CountCharacters(World);
	GameMode->HandleActorSeenForTests(Remote);
	TestEqual(
		TEXT("ActorSeen after ground failure spawns nothing"),
		CountCharacters(World),
		CharactersBeforeSeen);

	TestWorld.TickTestWorld(1.0f / 60.0f);
	TestEqual(
		TEXT("failed activation logs out on the next safe tick"),
		Session->GetStage(),
		ECorsairsLoginStage::Idle);
	TestEqual(
		TEXT("logout clears the session visible-actor cache"),
		Session->GetVisibleActors().Num(),
		0);
	TestEqual(
		TEXT("logout does not repopulate remote registries"),
		GameMode->GetRemoteRegistryCountsForTests(),
		FIntVector::ZeroValue);
	TestWorld.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundActorsTest,
	"Corsairs.Movement.Ground.GroundsLocalAndRemoteActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundActorsTest::RunTest(const FString&)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	TestNotNull(TEXT("grounding world created"), World);
	if (World == nullptr)
	{
		return false;
	}

	ACorsairsGameMode* GameMode = World->SpawnActor<ACorsairsGameMode>(
		ACorsairsGameMode::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		AlwaysSpawnParameters());
	TestNotNull(TEXT("game mode spawned"), GameMode);
	if (GameMode == nullptr)
	{
		World->DestroyWorld(false);
		return false;
	}

	FString Error;
	const TArray<uint8> Bytes = {0x0c, 0x00, 0x00, 0x00};
	TestTrue(
		TEXT("game mode owns the test block raster"),
		GameMode->LoadCharacterGroundFromBytesForTests(
			1,
			1,
			Bytes,
			Error));

	ACorsairsPlayerCharacter* Local =
		World->SpawnActor<ACorsairsPlayerCharacter>(
			ACorsairsPlayerCharacter::StaticClass(),
			FVector(1000.0, 1000.0, 1000.0),
			FRotator::ZeroRotator,
			AlwaysSpawnParameters());
	TestNotNull(TEXT("local actor spawned"), Local);
	if (Local == nullptr)
	{
		World->DestroyWorld(false);
		return false;
	}
	GameMode->GroundCharacterForTests(Local, FIntPoint(0, 0));

	ACorsairsCharacter* Overlap = World->SpawnActor<ACorsairsCharacter>(
		ACorsairsCharacter::StaticClass(),
		FVector(0.0, 0.0, 148.0),
		FRotator::ZeroRotator,
		AlwaysSpawnParameters());
	TestNotNull(TEXT("overlap actor spawned at remote center"), Overlap);

	ACorsairsCharacter* Remote =
		GameMode->SpawnRemoteCharacterForTests(
			FIntPoint(0, 0),
			FRotator::ZeroRotator);
	TestNotNull(TEXT("remote actor survives occupied spawn"), Remote);
	if (Remote != nullptr)
	{
		TestEqual(
			TEXT("remote remains at the requested occupied center"),
			Remote->GetActorLocation(),
			FVector(0.0, 0.0, 148.0));
		TestEqual(
			TEXT("remote capsule bottom rests on character ground"),
			Remote->GetActorLocation().Z -
				Remote->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
			60.0);
	}

	TestEqual(
		TEXT("local uses the same actor center"),
		Local->GetActorLocation(),
		FVector(0.0, 0.0, 148.0));
	TestEqual(
		TEXT("local capsule bottom rests on character ground"),
		Local->GetActorLocation().Z -
			Local->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
		60.0);

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundEndPlayTest,
	"Corsairs.Movement.Ground.EndPlayDetachesOwnedSampler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundEndPlayTest::RunTest(const FString&)
{
	FTestWorldWrapper TestWorld;
	if (!TestTrue(
		TEXT("end-play world created"),
		TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	UWorld* World = TestWorld.GetTestWorld();
	ACorsairsGameMode* GameMode = World->SpawnActor<ACorsairsGameMode>(
		ACorsairsGameMode::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		AlwaysSpawnParameters());
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	ACorsairsPlayerCharacter* Local =
		World->SpawnActor<ACorsairsPlayerCharacter>(
			ACorsairsPlayerCharacter::StaticClass(),
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			AlwaysSpawnParameters());
	TestNotNull(TEXT("end-play game mode spawned"), GameMode);
	TestNotNull(TEXT("end-play controller spawned"), Controller);
	TestNotNull(TEXT("end-play local pawn spawned"), Local);
	if (GameMode == nullptr || Controller == nullptr || Local == nullptr)
	{
		return false;
	}
	GameMode->bAutoLogin = false;
	Controller->Possess(Local);
	if (!TestTrue(
		TEXT("end-play world begins play"),
		TestWorld.BeginPlayInTestWorld()))
	{
		TestWorld.ForwardErrorMessages(this);
		return false;
	}

	FString Error;
	const TArray<uint8> InitialBytes = {0x01, 0x00, 0x00, 0x00};
	TestTrue(
		TEXT("end-play fixture loads initial owned sampler"),
		GameMode->LoadCharacterGroundFromBytesForTests(
			1,
			1,
			InitialBytes,
			Error));
	GameMode->GroundCharacterForTests(Local, FIntPoint(0, 0));

	FCorsairsWorldActor RemoteState;
	RemoteState.WorldId = 99;
	RemoteState.Name = TEXT("EndPlayRemote");
	RemoteState.Position = FIntPoint(0, 0);
	RemoteState.TypeId = 1;
	GameMode->HandleActorSeenForTests(RemoteState);
	ACorsairsCharacter* Remote = nullptr;
	for (TActorIterator<ACorsairsCharacter> It(World); It; ++It)
	{
		if (*It != Local)
		{
			Remote = *It;
			break;
		}
	}
	TestNotNull(TEXT("end-play registered remote exists"), Remote);
	TestEqual(
		TEXT("end-play precondition fills all registries"),
		GameMode->GetRemoteRegistryCountsForTests(),
		FIntVector(1, 1, 1));

	const TArray<uint8> ReloadedBytes = {0x04, 0x00, 0x00, 0x00};
	TestTrue(
		TEXT("game mode reloads its sampler without replacing owner"),
		GameMode->LoadCharacterGroundFromBytesForTests(
			1,
			1,
			ReloadedBytes,
			Error));
	Local->SetActorLocation(FVector(0.0, 0.0, 1000.0));
	TestWorld.TickTestWorld(1.0f / 60.0f);
	TestEqual(
		TEXT("attached pawn samples successful reload through stable address"),
		Local->GetActorLocation().Z,
		108.0);

	TestTrue(TEXT("game mode accepts explicit destroy"), GameMode->Destroy());
	TestTrue(
		TEXT("EndPlay restores local gravity"),
		Local->GetCharacterMovement()->GravityScale > 0.0f);
	TestTrue(
		TEXT("EndPlay leaves local outside flying mode"),
		Local->GetCharacterMovement()->MovementMode != MOVE_Flying);
	TestTrue(
		TEXT("EndPlay destroys registered remote"),
		Remote != nullptr && Remote->IsActorBeingDestroyed());
	TestEqual(
		TEXT("EndPlay empties all remote registries"),
		GameMode->GetRemoteRegistryCountsForTests(),
		FIntVector::ZeroValue);
	TestWorld.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsCharacterGroundPreservesFlyingTest,
	"Corsairs.Movement.Ground.PreservesFlyingMovementMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsCharacterGroundPreservesFlyingTest::RunTest(const FString&)
{
	FTestWorldWrapper TestWorld;
	if (!TestTrue(
		TEXT("flying world created"),
		TestWorld.CreateTestWorld(EWorldType::Game)))
	{
		return false;
	}
	if (!TestTrue(
		TEXT("flying world begins play"),
		TestWorld.BeginPlayInTestWorld()))
	{
		TestWorld.ForwardErrorMessages(this);
		return false;
	}

	FCorsairsCharacterGround Ground;
	FString Error;
	const TArray<uint8> Bytes = {0x0c, 0x00, 0x00, 0x00};
	TestTrue(
		TEXT("flying fixture loads"),
		Ground.LoadFromBytes(1, 1, Bytes, Error));

	ACorsairsPlayerCharacter* Pawn =
		TestWorld.GetTestWorld()->SpawnActor<ACorsairsPlayerCharacter>();
	TestNotNull(TEXT("flying pawn spawned"), Pawn);
	if (Pawn == nullptr)
	{
		return false;
	}

	Pawn->AttachCharacterGround(&Ground);
	TestEqual(
		TEXT("attachment disables gravity"),
		Pawn->GetCharacterMovement()->GravityScale,
		0.0f);
	TestEqual(
		TEXT("attachment selects flying movement"),
		Pawn->GetCharacterMovement()->MovementMode,
		MOVE_Flying);

	TestWorld.TickTestWorld(1.0f / 60.0f);
	TestEqual(
		TEXT("world tick keeps gravity disabled"),
		Pawn->GetCharacterMovement()->GravityScale,
		0.0f);
	TestEqual(
		TEXT("world tick keeps flying movement"),
		Pawn->GetCharacterMovement()->MovementMode,
		MOVE_Flying);
	TestEqual(
		TEXT("tick grounds pawn center from attached raster"),
		Pawn->GetActorLocation().Z,
		148.0);

	const TArray<uint8> ReloadedBytes = {0x01, 0x02, 0x03, 0x04};
	TestTrue(
		TEXT("attached sampler reloads at the same address"),
		Ground.LoadFromBytes(1, 1, ReloadedBytes, Error));
	Pawn->SetActorLocation(FVector(-49.6, 49.6, 1000.0));
	TestWorld.TickTestWorld(1.0f / 60.0f);
	TestEqual(
		TEXT("tick keeps rigid-basis fractional X after sampling"),
		Pawn->GetActorLocation().X,
		-49.6);
	TestEqual(
		TEXT("tick keeps rigid-basis fractional Y after sampling"),
		Pawn->GetActorLocation().Y,
		49.6);
	TestEqual(
		TEXT("tick applies inverse rigid basis before ground sampling"),
		Pawn->GetActorLocation().Z,
		108.0);

	TestWorld.ForwardErrorMessages(this);
	return true;
}

#endif
