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
		}

		~FCharacterGroundDiskFixture()
		{
			IFileManager::Get().Delete(*MetadataPath, false, true);
			IFileManager::Get().Delete(*BlockPath, false, true);
		}

		bool WriteMetadata(const FString& Text) const
		{
			return FFileHelper::SaveStringToFile(Text, *MetadataPath);
		}

		bool WriteBlock(const TArray<uint8>& Bytes) const
		{
			return FFileHelper::SaveArrayToFile(Bytes, *BlockPath);
		}

		void DeleteBlock() const
		{
			IFileManager::Get().Delete(*BlockPath, false, true);
		}

		FString MapName;
		FString MetadataPath;
		FString BlockPath;
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
		FVector(50.0, 0.0, 63.0));
	TestEqual(
		TEXT("source Y is mirrored in actor center"),
		Ground.ActorCenter(FIntPoint(0, 50), 88.0),
		FVector(0.0, -50.0, 113.0));

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
	TestTrue(
		TEXT("valid metadata is written"),
		Fixture.WriteMetadata(
			TEXT("{\"gridWidth\":1,\"gridHeight\":1}")));
	TestTrue(
		TEXT("valid block raster is written"),
		Fixture.WriteBlock(ValidBytes));
	TestTrue(
		TEXT("real disk load succeeds"),
		Ground.Load(Fixture.MapName, Error));
	TestEqual(
		TEXT("real disk load decodes block byte"),
		Ground.Sample(FIntPoint(0, 0)).HeightCm,
		25.0);

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
		2);
	TestFalse(
		TEXT("missing runtime ground rejects local activation"),
		GameMode->ActivateLocalCharacterForTests(
			Pawn,
			MapName,
			FIntPoint(0, 0)));
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
	TestEqual(
		TEXT("logout is not reentrant inside stage callback"),
		Session->GetStage(),
		ECorsairsLoginStage::InWorld);

	const int32 CharactersBeforeSeen = CountCharacters(World);
	FCorsairsWorldActor Remote;
	Remote.WorldId = 88;
	Remote.Position = FIntPoint(0, 0);
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

	TestWorld.ForwardErrorMessages(this);
	return true;
}

#endif
