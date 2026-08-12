#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsSkillCatalog.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString WriteFixture(const FString& Name, const FString& Json)
{
	const FString Path = FPaths::ProjectSavedDir() / Name;
	FFileHelper::SaveStringToFile(Json, *Path);
	return Path;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSkillCatalogLoadsAndLooksUpTest,
	"Corsairs.Skill.Catalog.LoadsAndLooksUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSkillCatalogLoadsAndLooksUpTest::RunTest(const FString&)
{
	const FString Path = WriteFixture(
		TEXT("SkillCatalogValid.json"),
		TEXT(R"json({"schemaVersion":1,"skills":[{"skillId":1,"name":"Windslash","applyDistance":400,"applyTarget":4,"applyType":1,"helpful":false,"habitatMask":1,"radius":0,"shape":0,"targetMode":"entity"}]})json"));
	FCorsairsSkillCatalog Catalog;
	FString Error;
	TestTrue(TEXT("catalog loads"), Catalog.Load(Path, Error));
	const FCorsairsSkillDefinition* Definition = Catalog.Find(1);
	TestNotNull(TEXT("skill is found"), Definition);
	if (Definition != nullptr)
	{
		TestEqual(TEXT("skill name"), Definition->Name, FString(TEXT("Windslash")));
		TestEqual(TEXT("entity target mode"), Definition->TargetMode, ECorsairsSkillTargetMode::Entity);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSkillCatalogRejectsInvalidJsonTest,
	"Corsairs.Skill.Catalog.RejectsInvalidJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSkillCatalogRejectsInvalidJsonTest::RunTest(const FString&)
{
	const TArray<TPair<FString, FString>> Fixtures = {
		{TEXT("SkillCatalogDuplicate.json"), TEXT(R"json({"schemaVersion":1,"skills":[{"skillId":1,"name":"A","applyDistance":1,"applyTarget":1,"applyType":1,"helpful":false,"habitatMask":1,"radius":0,"shape":0,"targetMode":"entity"},{"skillId":1,"name":"B","applyDistance":1,"applyTarget":1,"applyType":1,"helpful":false,"habitatMask":1,"radius":0,"shape":0,"targetMode":"entity"}]})json")},
		{TEXT("SkillCatalogBool.json"), TEXT(R"json({"schemaVersion":1,"skills":[{"skillId":true,"name":"A","applyDistance":1,"applyTarget":1,"applyType":1,"helpful":false,"habitatMask":1,"radius":0,"shape":0,"targetMode":"entity"}]})json")},
		{TEXT("SkillCatalogTarget.json"), TEXT(R"json({"schemaVersion":1,"skills":[{"skillId":1,"name":"A","applyDistance":1,"applyTarget":1,"applyType":1,"helpful":false,"habitatMask":1,"radius":0,"shape":0,"targetMode":"future"}]})json")},
		{TEXT("SkillCatalogOverflow.json"), TEXT(R"json({"schemaVersion":1,"skills":[{"skillId":9223372036854775808,"name":"A","applyDistance":1,"applyTarget":1,"applyType":1,"helpful":false,"habitatMask":1,"radius":0,"shape":0,"targetMode":"entity"}]})json")},
	};
	for (const TPair<FString, FString>& Fixture : Fixtures)
	{
		const FString Path = WriteFixture(Fixture.Key, Fixture.Value);
		FCorsairsSkillCatalog Catalog;
		FString Error;
		TestFalse(FString::Printf(TEXT("rejects %s"), *Fixture.Key), Catalog.Load(Path, Error));
		TestFalse(FString::Printf(TEXT("error for %s"), *Fixture.Key), Error.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSkillCatalogRejectsMissingFileTest,
	"Corsairs.Skill.Catalog.RejectsMissingFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSkillCatalogRejectsMissingFileTest::RunTest(const FString&)
{
	FCorsairsSkillCatalog Catalog;
	FString Error;
	TestFalse(
		TEXT("missing file rejected"),
		Catalog.Load(FPaths::ProjectSavedDir() / TEXT("SkillCatalogMissing.json"), Error));
	TestFalse(TEXT("missing file reports error"), Error.IsEmpty());
	return true;
}

#endif
