#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsWorldClickResolver.h"

#include "Misc/AutomationTest.h"

namespace
{
FCorsairsWorldActor MakeActor(
	const int64 WorldId,
	const int64 Handle,
	const int32 CtrlType,
	const int64 TeamLeaderId,
	const int32 SideId,
	const int64 Hp = 100)
{
	FCorsairsWorldActor Actor;
	Actor.WorldId = WorldId;
	Actor.Handle = Handle;
	Actor.CtrlType = CtrlType;
	Actor.Hp = Hp;
	Actor.TargetPolicy.TeamLeaderId = TeamLeaderId;
	Actor.TargetPolicy.SideId = SideId;
	return Actor;
}

FCorsairsSkillDefinition MakeDefinition(
	const int64 SkillId,
	const int32 ApplyType,
	const int32 ApplyTarget,
	const bool bHelpful)
{
	FCorsairsSkillDefinition Definition;
	Definition.SkillId = SkillId;
	Definition.ApplyType = ApplyType;
	Definition.ApplyTarget = ApplyTarget;
	Definition.bHelpful = bHelpful;
	Definition.TargetMode = ApplyType == 2
		? ECorsairsSkillTargetMode::Ground
		: ECorsairsSkillTargetMode::Entity;
	return Definition;
}

FCorsairsSkillEntry MakeEntry(const int64 SkillId, const int32 Level = 1)
{
	FCorsairsSkillEntry Entry;
	Entry.SkillId = SkillId;
	Entry.Level = Level;
	return Entry;
}

FCorsairsClickInput MakeInput()
{
	FCorsairsClickInput Input;
	Input.LocalActor = MakeActor(1, 10, 1, 100, 1);
	Input.GroundPoint = FIntPoint(50, 75);
	Input.bHasGroundPoint = true;
	return Input;
}

void AddExactActor(FCorsairsClickInput& Input, const FCorsairsWorldActor& Actor)
{
	Input.bHasExactActor = true;
	Input.ExactActor = Actor;
	Input.bHasExactActorIdentity = true;
	Input.ExactActorWorldId = Actor.WorldId;
	Input.ExactActorHandle = Actor.Handle;
}

void AddExactActorWithoutIdentity(
	FCorsairsClickInput& Input,
	const FCorsairsWorldActor& Actor)
{
	Input.bHasExactActor = true;
	Input.ExactActor = Actor;
}

void AddSkill(
	FCorsairsClickInput& Input,
	const FCorsairsSkillDefinition& Definition,
	const bool bPrepared,
	const int32 Level = 1)
{
	Input.SkillBag.Add(MakeEntry(Definition.SkillId, Level));
	if (bPrepared)
	{
		Input.PreparedSkillId = Definition.SkillId;
		Input.PreparedSkill = Definition;
	}
	else
	{
		Input.DefaultSkillId = Definition.SkillId;
		Input.DefaultSkill = Definition;
	}
}

FCorsairsClickInput MakeCopiedInputAfterSkillDefinitionLifetime()
{
	FCorsairsClickInput Input = MakeInput();
	{
		const FCorsairsSkillDefinition Definition = MakeDefinition(119, 1, 3, false);
		Input.SkillBag.Add(MakeEntry(Definition.SkillId));
		Input.PreparedSkillId = Definition.SkillId;
		Input.PreparedSkill = Definition;
	}
	return Input;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsWorldClickResolverTruthTableTest,
	"Corsairs.Input.WorldClickResolver.TruthTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsWorldClickResolverTruthTableTest::RunTest(const FString&)
{
	auto Expect = [this](
		const TCHAR* Name,
		const FCorsairsClickInput& Input,
		const ECorsairsClickIntentType ExpectedType,
		const int64 ExpectedSkillId = 0,
		const int64 ExpectedWorldId = 0,
		const int64 ExpectedHandle = 0) {
		const FCorsairsClickIntent Result = FCorsairsWorldClickResolver::Resolve(Input);
		TestEqual(Name, Result.Type, ExpectedType);
		if (ExpectedSkillId != 0)
		{
			TestEqual(FString::Printf(TEXT("%s skill"), Name), Result.SkillId, ExpectedSkillId);
		}
		if (ExpectedWorldId != 0)
		{
			TestEqual(FString::Printf(TEXT("%s world identity"), Name), Result.TargetWorldId, ExpectedWorldId);
			TestEqual(FString::Printf(TEXT("%s handle identity"), Name), Result.TargetHandle, ExpectedHandle);
		}
		if (ExpectedType == ECorsairsClickIntentType::Rejected)
		{
			TestFalse(FString::Printf(TEXT("%s explains rejection"), Name), Result.Reason.IsEmpty());
		}
	};

	FCorsairsSkillDefinition EntitySkill = MakeDefinition(101, 1, 4, false);
	FCorsairsSkillDefinition GroundSkill = MakeDefinition(102, 2, 0, true);
	FCorsairsSkillDefinition SelfSkill = MakeDefinition(103, 1, 1, true);
	FCorsairsSkillDefinition TeamSkill = MakeDefinition(104, 1, 2, true);
	FCorsairsSkillDefinition HarmfulRelationSkill = MakeDefinition(106, 1, 5, false);
	FCorsairsSkillDefinition DeadPlayerSkill = MakeDefinition(107, 1, 6, false);
	FCorsairsSkillDefinition TreeSkill = MakeDefinition(108, 1, 18, false);
	FCorsairsSkillDefinition UnknownSkill = MakeDefinition(110, 1, 0, false);
	FCorsairsSkillDefinition LiveSkill = MakeDefinition(112, 1, 3, false);
	FCorsairsSkillDefinition NonSelfSkill = MakeDefinition(113, 1, 7, false);
	FCorsairsSkillDefinition RepairSkill = MakeDefinition(114, 1, 17, false);
	FCorsairsSkillDefinition FishSkill = MakeDefinition(115, 1, 28, false);
	FCorsairsSkillDefinition SalvageSkill = MakeDefinition(116, 1, 29, false);

	{
		FCorsairsClickInput Input = MakeInput();
		Input.bUiConsumed = true;
		Expect(TEXT("UI consumed has priority"), Input, ECorsairsClickIntentType::SelectOnly);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		Input.bLoginGate = true;
		Expect(TEXT("login gate has priority"), Input, ECorsairsClickIntentType::SelectOnly);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(2, 20, 5, 200, 2));
		AddSkill(Input, EntitySkill, true);
		Expect(TEXT("prepared entity skill uses exact actor"), Input,
			ECorsairsClickIntentType::EntitySkill, 101, 2, 20);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddSkill(Input, EntitySkill, true);
		Expect(TEXT("prepared entity miss has no fallback"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		Input.LocalActor = FCorsairsWorldActor{};
		Input.bHasExactActor = true;
		Input.ExactActor = FCorsairsWorldActor{};
		Input.bHasExactActorIdentity = true;
		Input.ExactActorWorldId = 2;
		Input.ExactActorHandle = 20;
		AddSkill(Input, GroundSkill, true);
		Expect(TEXT("prepared ground ignores invalid actor fields"), Input,
			ECorsairsClickIntentType::GroundSkill, 102);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActorWithoutIdentity(Input, MakeActor(2, 20, 5, 0, 0));
		AddSkill(Input, EntitySkill, true);
		Expect(TEXT("prepared entity requires exact identity snapshot"), Input,
			ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		Input.bHasExactActorIdentity = true;
		Input.ExactActorWorldId = 2;
		Input.ExactActorHandle = 20;
		AddSkill(Input, EntitySkill, true);
		Expect(TEXT("prepared entity ground click has no fallback"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		const FCorsairsWorldActor StaleActor = MakeActor(2, 99, 5, 200, 2);
		AddExactActor(Input, StaleActor);
		Input.bHasExactActorIdentity = true;
		Input.ExactActorWorldId = 2;
		Input.ExactActorHandle = 20;
		AddSkill(Input, EntitySkill, true);
		Expect(TEXT("stale handle is rejected"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddSkill(Input, GroundSkill, true);
		Expect(TEXT("prepared ground skill uses exact point"), Input,
			ECorsairsClickIntentType::GroundSkill, 102);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(2, 20, 5, 0, 0));
		Input.ExactActorHandle = 99;
		AddSkill(Input, GroundSkill, true);
		Expect(TEXT("prepared ground ignores stale actor identity"), Input,
			ECorsairsClickIntentType::GroundSkill, 102);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		Input.bHasGroundPoint = false;
		AddSkill(Input, GroundSkill, true);
		Expect(TEXT("prepared ground miss has no fallback"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		Input.PreparedSkillId = 999;
		Expect(TEXT("prepared skill absent from bag is rejected"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(3, 30, 2, 0, 0));
		AddSkill(Input, EntitySkill, true);
		Expect(TEXT("prepared skill cannot talk to NPC"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(3, 30, 2, 0, 0));
		Expect(TEXT("NPC click talks"), Input, ECorsairsClickIntentType::Talk, 0, 3, 30);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActorWithoutIdentity(Input, MakeActor(3, 30, 2, 0, 0));
		Expect(TEXT("NPC without exact identity is rejected"), Input,
			ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(4, 40, 3, 0, 0));
		Expect(TEXT("NPC event click talks"), Input, ECorsairsClickIntentType::Talk, 0, 4, 40);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(2, 20, 5, 0, 0));
		AddSkill(Input, EntitySkill, false);
		Expect(TEXT("monster click uses default skill"), Input,
			ECorsairsClickIntentType::EntitySkill, 101, 2, 20);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(5, 50, 1, 300, 2));
		AddSkill(Input, EntitySkill, false);
		Expect(TEXT("player click uses default skill"), Input,
			ECorsairsClickIntentType::EntitySkill, 101, 5, 50);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActorWithoutIdentity(Input, MakeActor(5, 50, 1, 300, 2));
		AddSkill(Input, EntitySkill, false);
		Expect(TEXT("default skill requires exact identity snapshot"), Input,
			ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(2, 20, 5, 200, 2));
		Expect(TEXT("combat click without default only selects"), Input, ECorsairsClickIntentType::SelectOnly);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		Expect(TEXT("ground click moves"), Input, ECorsairsClickIntentType::Move);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		Input.bHasGroundPoint = false;
		Expect(TEXT("ground miss is rejected"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		EntitySkill.ApplyType = 2;
		EntitySkill.TargetMode = ECorsairsSkillTargetMode::Ground;
		AddExactActor(Input, MakeActor(2, 20, 5, 200, 2));
		AddSkill(Input, EntitySkill, true);
		const FCorsairsClickIntent Result = FCorsairsWorldClickResolver::Resolve(Input);
		TestEqual(
			TEXT("ground skill ignores actor under cursor"),
			Result.Type,
			ECorsairsClickIntentType::GroundSkill);
		TestEqual(TEXT("ground skill keeps exact skill"), Result.SkillId, int64{101});
		TestEqual(TEXT("ground skill keeps exact point"), Result.GroundPoint, Input.GroundPoint);
		TestEqual(TEXT("ground skill does not synthesize world identity"), Result.TargetWorldId, int64{0});
		TestEqual(TEXT("ground skill does not synthesize handle"), Result.TargetHandle, int64{0});
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(1, 10, 1, 100, 1));
		AddSkill(Input, UnknownSkill, true);
		Expect(TEXT("unknown apply target is rejected"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(1, 10, 1, 100, 1));
		AddSkill(Input, SelfSkill, true);
		Expect(TEXT("self target uses exact local identity"), Input,
			ECorsairsClickIntentType::EntitySkill, 103, 1, 10);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		FCorsairsSkillDefinition HarmfulSelfSkill = MakeDefinition(111, 1, 5, false);
		AddExactActor(Input, MakeActor(1, 10, 1, 100, 1));
		AddSkill(Input, HarmfulSelfSkill, true);
		Expect(TEXT("harmful self target is rejected"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(6, 60, 1, 100, 2));
		AddSkill(Input, HarmfulRelationSkill, true);
		Expect(TEXT("harmful same team target is rejected"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(2, 20, 5, 200, 2));
		AddSkill(Input, HarmfulRelationSkill, true);
		Expect(TEXT("harmful known enemy target is allowed"), Input,
			ECorsairsClickIntentType::EntitySkill, 106, 2, 20);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(2, 20, 5, 0, 0));
		AddSkill(Input, HarmfulRelationSkill, true);
		Expect(TEXT("harmful monster without relation is allowed"), Input,
			ECorsairsClickIntentType::EntitySkill, 106, 2, 20);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		FCorsairsSkillDefinition HelpfulMonsterSkill = MakeDefinition(117, 1, 5, true);
		AddExactActor(Input, MakeActor(2, 20, 5, 0, 0));
		AddSkill(Input, HelpfulMonsterSkill, true);
		Expect(TEXT("helpful monster without relation is rejected"), Input,
			ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(5, 50, 1, 0, 0));
		AddSkill(Input, HarmfulRelationSkill, true);
		Expect(TEXT("harmful player without relation is rejected"), Input,
			ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(7, 70, 6, 0, 0));
		AddSkill(Input, TreeSkill, true);
		Expect(TEXT("tree resource accepts tree skill"), Input,
			ECorsairsClickIntentType::EntitySkill, 108, 7, 70);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(7, 70, 6, 0, 0));
		AddSkill(Input, TreeSkill, false);
		Expect(TEXT("default skill can target exact tree"), Input,
			ECorsairsClickIntentType::EntitySkill, 108, 7, 70);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(16, 160, 99, 0, 0));
		AddSkill(Input, EntitySkill, false);
		Expect(TEXT("unsupported exact actor is selected, not moved through"), Input,
			ECorsairsClickIntentType::SelectOnly);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(8, 80, 7, 0, 0));
		AddSkill(Input, TreeSkill, true);
		Expect(TEXT("resource cross pair is rejected"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(9, 90, 1, 300, 2, 0));
		AddSkill(Input, DeadPlayerSkill, true);
		Expect(TEXT("dead player accepts dead-player skill"), Input,
			ECorsairsClickIntentType::EntitySkill, 107, 9, 90);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(9, 90, 1, 300, 2, 100));
		AddSkill(Input, DeadPlayerSkill, true);
		Expect(TEXT("live player rejects dead-player skill"), Input, ECorsairsClickIntentType::Rejected);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(10, 100, 5, 300, 2));
		GroundSkill.TargetMode = ECorsairsSkillTargetMode::Ground;
		AddSkill(Input, GroundSkill, false);
		Expect(TEXT("default ground skill does not auto-target actor"), Input, ECorsairsClickIntentType::SelectOnly);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(1, 10, 1, 100, 1));
		AddSkill(Input, TeamSkill, true);
		Expect(TEXT("team skill accepts self"), Input,
			ECorsairsClickIntentType::EntitySkill, 104, 1, 10);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(11, 110, 5, 0, 0));
		AddSkill(Input, LiveSkill, true);
		Expect(TEXT("ordinary live target does not require relation"), Input,
			ECorsairsClickIntentType::EntitySkill, 112, 11, 110);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(17, 170, 1, 0, 0));
		AddSkill(Input, LiveSkill, true);
		Expect(TEXT("unknown player target3 remains valid"), Input,
			ECorsairsClickIntentType::EntitySkill, 112, 17, 170);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(12, 120, 5, 0, 0));
		AddSkill(Input, NonSelfSkill, true);
		Expect(TEXT("nonself target accepts exact other actor"), Input,
			ECorsairsClickIntentType::EntitySkill, 113, 12, 120);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(18, 180, 1, 100, 1, 0));
		AddSkill(Input, NonSelfSkill, true);
		Expect(TEXT("target7 accepts dead nonself actor"), Input,
			ECorsairsClickIntentType::EntitySkill, 113, 18, 180);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(1, 10, 1, 100, 1));
		FCorsairsSkillDefinition HarmfulSelfSkill = MakeDefinition(118, 1, 1, false);
		AddSkill(Input, HarmfulSelfSkill, true);
		Expect(TEXT("target1 harmful self follows target shape"), Input,
			ECorsairsClickIntentType::EntitySkill, 118, 1, 10);
	}
	{
		FCorsairsClickInput Input = MakeCopiedInputAfterSkillDefinitionLifetime();
		FCorsairsClickInput CopiedInput = Input;
		AddExactActor(CopiedInput, MakeActor(17, 170, 5, 0, 0));
		Expect(TEXT("copied input owns immutable skill definition"), CopiedInput,
			ECorsairsClickIntentType::EntitySkill, 119, 17, 170);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(13, 130, 17, 0, 0));
		AddSkill(Input, RepairSkill, true);
		Expect(TEXT("repair skill accepts repairable target"), Input,
			ECorsairsClickIntentType::EntitySkill, 114, 13, 130);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(14, 140, 8, 0, 0));
		AddSkill(Input, FishSkill, true);
		Expect(TEXT("fish skill accepts fish target"), Input,
			ECorsairsClickIntentType::EntitySkill, 115, 14, 140);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		AddExactActor(Input, MakeActor(15, 150, 9, 0, 0));
		AddSkill(Input, SalvageSkill, true);
		Expect(TEXT("salvage skill accepts dbboat target"), Input,
			ECorsairsClickIntentType::EntitySkill, 116, 15, 150);
	}
	{
		FCorsairsClickInput Input = MakeInput();
		Input.LocalActor = FCorsairsWorldActor{};
		Expect(TEXT("invalid local identity fails closed"), Input, ECorsairsClickIntentType::Rejected);
	}
	return true;
}

#endif
