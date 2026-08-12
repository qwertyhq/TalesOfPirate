#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsGroundPicker.h"

#include "CorsairsCharacterGround.h"
#include "Misc/AutomationTest.h"

namespace
{
TArray<uint8> FlatBlock(const int32 Width, const int32 Height)
{
	TArray<uint8> Bytes;
	Bytes.Init(0, Width * Height * 4);
	return Bytes;
}

TArray<uint8> FlatHeight(const int32 Width, const int32 Height)
{
	TArray<uint8> Bytes;
	Bytes.Init(0, Width * Height * 2);
	return Bytes;
}

TArray<uint8> RampHeight()
{
	TArray<uint8> Bytes = FlatHeight(4, 4);
	const auto SetRawHeight = [&Bytes](const int32 Index, const int32 RawHeight)
	{
		const uint16 Encoded = static_cast<uint16>(
			(RawHeight + 128) * 256);
		Bytes[Index * 2] = static_cast<uint8>(Encoded);
		Bytes[Index * 2 + 1] = static_cast<uint8>(Encoded >> 8);
	};
	SetRawHeight(0, 0);
	SetRawHeight(1, 2);
	SetRawHeight(4, 4);
	SetRawHeight(5, 6);
	return Bytes;
}

TArray<uint8> LandRegion(const int32 Width, const int32 Height)
{
	TArray<uint8> Bytes;
	Bytes.Init(1, Width * Height * 2);
	for (int32 Index = 1; Index < Bytes.Num(); Index += 2)
	{
		Bytes[Index] = 0;
	}
	return Bytes;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGroundPickerRampTest,
	"Corsairs.Movement.GroundPicker.Ramp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGroundPickerRampTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TestTrue(TEXT("ramp runtime ground loads"), Ground.LoadRuntimeFromBytes(
		4, 4, RampHeight(), FlatBlock(4, 4), LandRegion(4, 4), Error));

	FVector2d SourcePoint = FVector2d::ZeroVector;
	double HeightCm = -1.0;
	TestTrue(TEXT("ramp ray hits interpolated height"),
		FCorsairsGroundPicker::TryPick(
			FVector(-50.0, 50.0, 1000.0),
			FVector(0.0, 0.0, -1.0), Ground, SourcePoint, HeightCm));
	TestEqual(TEXT("ramp height is deterministic"), HeightCm, 30.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGroundPickerRejectsRayMissAndOobTest,
	"Corsairs.Movement.GroundPicker.RayMissAndOob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGroundPickerRejectsRayMissAndOobTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TestTrue(TEXT("flat runtime ground loads"), Ground.LoadRuntimeFromBytes(
		4, 4, FlatHeight(4, 4), FlatBlock(4, 4), LandRegion(4, 4), Error));
	FVector2d SourcePoint = FVector2d::ZeroVector;
	double HeightCm = 0.0;
	TestFalse(TEXT("upward ray misses heightfield"),
		FCorsairsGroundPicker::TryPick(
			FVector(-100.0, 150.0, 1000.0),
			FVector(0.0, 0.0, 1.0), Ground, SourcePoint, HeightCm));
	TestFalse(TEXT("out-of-bounds ray fails closed"),
		FCorsairsGroundPicker::TryPick(
			FVector(-1000.0, 1000.0, 1000.0),
			FVector(0.0, 0.0, -1.0), Ground, SourcePoint, HeightCm));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGroundPickerFlatInverseQTest,
	"Corsairs.Movement.GroundPicker.FlatInverseQ",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGroundPickerFlatInverseQTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FString Error;
	TestTrue(TEXT("flat runtime ground loads"), Ground.LoadRuntimeFromBytes(
		4, 4, FlatHeight(4, 4), FlatBlock(4, 4), LandRegion(4, 4), Error));

	FVector2d SourcePoint = FVector2d::ZeroVector;
	double HeightCm = -1.0;
	TestTrue(TEXT("inverse-Q ray hits heightfield"),
		FCorsairsGroundPicker::TryPick(
			FVector(-100.0, 150.0, 1000.0),
			FVector(0.0, 0.0, -1.0), Ground, SourcePoint, HeightCm));
	TestEqual(TEXT("inverse-Q source X is UE Y"), SourcePoint.X, 150.0);
	TestEqual(TEXT("inverse-Q source Y is negative UE X"), SourcePoint.Y, 100.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsGroundPickerRejectsMissAndUnloadedTest,
	"Corsairs.Movement.GroundPicker.FailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsGroundPickerRejectsMissAndUnloadedTest::RunTest(const FString&)
{
	FCorsairsCharacterGround Ground;
	FVector2d SourcePoint = FVector2d::ZeroVector;
	double HeightCm = 0.0;
	TestFalse(TEXT("unloaded heightfield fails closed"),
		FCorsairsGroundPicker::TryPick(
			FVector::ZeroVector, FVector(0.0, 0.0, -1.0),
			Ground, SourcePoint, HeightCm));
	return true;
}

#endif
