#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsLoginHud.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsStartupGateTest,
	"Corsairs.Character.StartupGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsStartupGateTest::RunTest(const FString&)
{
	const FString StartupError =
		TEXT("/full/project/Data/character_map.json: invalid JSON");
	const FCorsairsLoginPresentation Failed =
		MakeCorsairsLoginPresentation(
			false,
			StartupError,
			ECorsairsLoginStage::Idle);

	TestFalse(TEXT("startup failure rejects input"), Failed.bAcceptInput);
	TestFalse(TEXT("startup failure hides login form"), Failed.bShowLoginForm);
	TestEqual(
		TEXT("startup failure title"),
		Failed.Title,
		FString(TEXT("Ошибка запуска")));
	TestEqual(
		TEXT("full catalog error is presented"),
		Failed.Message,
		StartupError);

	const FCorsairsLoginPresentation Ready =
		MakeCorsairsLoginPresentation(
			true,
			FString(),
			ECorsairsLoginStage::Idle);
	TestTrue(TEXT("ready idle state accepts input"), Ready.bAcceptInput);
	TestTrue(TEXT("ready idle state shows login form"), Ready.bShowLoginForm);
	return true;
}

#endif
