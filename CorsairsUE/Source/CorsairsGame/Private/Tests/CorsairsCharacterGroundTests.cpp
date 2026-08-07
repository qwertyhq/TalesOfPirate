#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsCharacterGround.h"

#include "Components/CapsuleComponent.h"
#include "CorsairsCharacter.h"
#include "CorsairsGameMode.h"
#include "CorsairsPlayerCharacter.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/AutomationTest.h"
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
