#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsSkillHudPresentation.h"

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString WriteCatalogFixture()
{
	const FString Path = FPaths::ProjectSavedDir() / TEXT("SkillHudPresentation.json");
	FFileHelper::SaveStringToFile(
		TEXT(R"json({"schemaVersion":1,"skills":[{"skillId":101,"name":"Windslash","applyDistance":400,"applyTarget":4,"applyType":1,"helpful":false,"habitatMask":1,"radius":0,"shape":0,"targetMode":"entity"},{"skillId":202,"name":"Rain","applyDistance":500,"applyTarget":0,"applyType":2,"helpful":true,"habitatMask":1,"radius":250,"shape":1,"targetMode":"ground"}]})json"),
		*Path);
	return Path;
}

FCorsairsShortcutEntry MakeShortcut(
	const int32 Slot,
	const int32 Type,
	const int64 GridId)
{
	return FCorsairsShortcutEntry{Slot, Type, GridId};
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSkillHudPresentationFormatsTwelveSlotsTest,
	"Corsairs.Hud.SkillPresentation.FormatsTwelveSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSkillHudPresentationFormatsTwelveSlotsTest::RunTest(const FString&)
{
	FCorsairsSkillCatalog Catalog;
	FString Error;
	TestTrue(TEXT("каталог навыков загружен"), Catalog.Load(WriteCatalogFixture(), Error));

	TArray<FCorsairsShortcutEntry> Shortcuts;
	Shortcuts.Reserve(12);
	Shortcuts.Add(MakeShortcut(0, 2, 101));
	Shortcuts.Add(MakeShortcut(1, 1, 77));
	Shortcuts.Add(MakeShortcut(2, 3, 88));
	Shortcuts.Add(MakeShortcut(3, 4, 99));
	Shortcuts.Add(MakeShortcut(4, 2, 202));
	for (int32 Slot = 5; Slot < 12; ++Slot)
	{
		Shortcuts.Add(MakeShortcut(Slot, 0, 0));
	}

	TArray<FCorsairsSkillEntry> SkillBag;
	FCorsairsSkillEntry Windslash;
	Windslash.SkillId = 101;
	Windslash.Level = 7;
	Windslash.State = 3;
	SkillBag.Add(Windslash);
	FCorsairsSkillEntry Rain;
	Rain.SkillId = 202;
	Rain.Level = 2;
	Rain.State = 1;
	SkillBag.Add(Rain);

	const FCorsairsSkillHudPresentation Presentation =
		MakeCorsairsSkillHudPresentation(
			Shortcuts,
			SkillBag,
			Catalog,
			101,
			TEXT("Капитан Немо"),
			TEXT("цель вне дистанции"));

	TestEqual(TEXT("ровно 12 display slots"), Presentation.Slots.Num(), 12);
	TestTrue(TEXT("боевой skill поддержан"), Presentation.Slots[0].bSupported);
	TestEqual(TEXT("имя skill в слоте"), Presentation.Slots[0].Name, FString(TEXT("Windslash")));
	TestEqual(TEXT("уровень skill в слоте"), Presentation.Slots[0].Level, 7);
	TestEqual(TEXT("state skill в слоте"), Presentation.Slots[0].State, 3);
	TestEqual(TEXT("item отображается как неподдерживаемый"),
		Presentation.Slots[1].Name,
		FString(TEXT("Неподдерживается")));
	TestFalse(TEXT("item не исполняется"), Presentation.Slots[1].bSupported);
	TestTrue(TEXT("item остаётся занятой ячейкой"),
		Presentation.Slots[1].bOccupied);
	TestEqual(TEXT("life skill отображается как неподдерживаемый"),
		Presentation.Slots[2].Name,
		FString(TEXT("Неподдерживается")));
	TestEqual(TEXT("sail skill отображается как неподдерживаемый"),
		Presentation.Slots[3].Name,
		FString(TEXT("Неподдерживается")));
	TestTrue(TEXT("life skill остаётся занятой ячейкой"),
		Presentation.Slots[2].bOccupied);
	TestTrue(TEXT("sail skill остаётся занятой ячейкой"),
		Presentation.Slots[3].bOccupied);
	TestTrue(TEXT("ground fight skill поддержан"), Presentation.Slots[4].bSupported);
	TestFalse(TEXT("пустая ячейка не занята"),
		Presentation.Slots[5].bOccupied);
	TestEqual(TEXT("prepared skill"), Presentation.PreparedSkillName, FString(TEXT("Windslash")));
	TestEqual(TEXT("prepared entity target type"),
		Presentation.PreparedTargetType,
		FString(TEXT("сущность")));
	TestEqual(TEXT("exact selected target"), Presentation.TargetName, FString(TEXT("Капитан Немо")));
	TestEqual(TEXT("rejection reason"), Presentation.RejectionReason, FString(TEXT("цель вне дистанции")));

	const FCorsairsSkillHudPresentation GroundPresentation =
		MakeCorsairsSkillHudPresentation(
			Shortcuts,
			SkillBag,
			Catalog,
			202,
			FString(),
			FString());
	TestEqual(TEXT("prepared ground target type"),
		GroundPresentation.PreparedTargetType,
		FString(TEXT("земля")));
	TestNotEqual(TEXT("entity and ground labels differ"),
		Presentation.PreparedTargetType,
		GroundPresentation.PreparedTargetType);
	return true;
}

#endif
