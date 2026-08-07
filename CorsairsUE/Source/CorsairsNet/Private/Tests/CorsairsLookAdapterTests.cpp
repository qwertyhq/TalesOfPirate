#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CorsairsLookAdapter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsLookAdapterTest,
	"Corsairs.Character.LookAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsLookAdapterTest::RunTest(const FString&)
{
	Corsairs::Net::Msg::ChaLookInfo Source;
	Source.synType = 1;
	Source.typeId = 3;
	Source.hairId = 2124;
	Source.isBoat = false;
	for (int32 Index = 0; Index < Corsairs::Net::Msg::EQUIP_NUM; ++Index)
	{
		Source.equips[Index].id = 1000 + Index;
	}

	const FCorsairsCharacterLook Result = MakeCharacterLook(Source);
	TestEqual(TEXT("syn type"), Result.SynType, 1);
	TestEqual(TEXT("type"), Result.TypeId, 3);
	TestEqual(TEXT("hair"), Result.HairId, 2124);
	TestFalse(TEXT("human"), Result.bIsBoat);
	TestEqual(TEXT("all slots preserved"), Result.EquipIds.Num(), 34);
	for (int32 Index = 0; Index < Result.EquipIds.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("slot %d"), Index),
			Result.EquipIds[Index], 1000 + Index);
	}
	return true;
}

#endif
