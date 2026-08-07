#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsMovementInputGate.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMovementInputGateHeldAxisAfterBlockTest,
	"Corsairs.Movement.InputGate.HeldAxisAfterBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMovementInputGateHeldAxisAfterBlockTest::RunTest(const FString&)
{
	// Mutation: RequireNeutral либо повторный nonzero axis преждевременно
	// очищает latch и снова разрешает held input без отпускания клавиши.
	FCorsairsMovementInputGate Gate;
	Gate.SetForward(1.0f);
	Gate.RequireNeutral();
	bool bAllHeldSamplesBlocked = true;
	for (int32 Repeat = 0; Repeat < 120; ++Repeat)
	{
		Gate.SetForward(1.0f);
		bAllHeldSamplesBlocked &= !Gate.AllowsPrediction();
	}
	TestTrue(TEXT("120 held Forward samples remain blocked"),
		bAllHeldSamplesBlocked);

	Gate.SetForward(0.0f);
	Gate.SetRight(0.0f);
	TestFalse(TEXT("both neutral axes release latch"), Gate.IsWaitingForNeutral());
	Gate.SetForward(1.0f);
	TestTrue(TEXT("new Forward press predicts"), Gate.AllowsPrediction());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMovementInputGateHeldAxisAcrossSkillTest,
	"Corsairs.Movement.InputGate.HeldAxisAcrossSkill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMovementInputGateHeldAxisAcrossSkillTest::RunTest(const FString&)
{
	// Mutation: SetAuthorityLocked(false) снимает neutral latch вместе с
	// authority lock и пропускает клавишу, удержанную во время skill.
	FCorsairsMovementInputGate Gate;
	Gate.SetForward(1.0f);
	Gate.SetAuthorityLocked(true);
	TestFalse(TEXT("skill authority blocks prediction"), Gate.AllowsPrediction());
	Gate.SetAuthorityLocked(false);
	TestTrue(TEXT("unlock keeps neutral latch"), Gate.IsWaitingForNeutral());
	TestFalse(TEXT("held axis stays blocked after skill"), Gate.AllowsPrediction());

	Gate.SetForward(0.0f);
	Gate.SetRight(0.0f);
	Gate.SetForward(1.0f);
	TestTrue(TEXT("neutral and re-press resume prediction"), Gate.AllowsPrediction());

	FCorsairsMovementInputGate InitiallyNeutralGate;
	InitiallyNeutralGate.SetAuthorityLocked(true);
	InitiallyNeutralGate.SetAuthorityLocked(false);
	TestTrue(
		TEXT("zero-axis authority transition still requires fresh neutral"),
		InitiallyNeutralGate.IsWaitingForNeutral());
	TestFalse(
		TEXT("unlock alone never resumes prediction"),
		InitiallyNeutralGate.AllowsPrediction());
	InitiallyNeutralGate.SetForward(0.0f);
	InitiallyNeutralGate.SetRight(0.0f);
	InitiallyNeutralGate.SetForward(1.0f);
	TestTrue(
		TEXT("fresh neutral and press resume initially neutral gate"),
		InitiallyNeutralGate.AllowsPrediction());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsMovementInputGateOneAxisNeutralIsInsufficientTest,
	"Corsairs.Movement.InputGate.OneAxisNeutralIsInsufficient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsMovementInputGateOneAxisNeutralIsInsufficientTest::RunTest(
	const FString&)
{
	// Mutation: latch использует OR вместо AND и снимается при отпускании
	// только одной из двух одновременно удержанных осей.
	FCorsairsMovementInputGate Gate;
	Gate.SetForward(1.0f);
	Gate.SetRight(1.0f);
	Gate.RequireNeutral();
	Gate.SetForward(0.0f);
	TestTrue(TEXT("held Right keeps neutral latch"), Gate.IsWaitingForNeutral());
	TestFalse(TEXT("one neutral axis is insufficient"), Gate.AllowsPrediction());

	Gate.SetRight(0.0f);
	TestFalse(TEXT("both axes clear neutral latch"), Gate.IsWaitingForNeutral());
	Gate.SetForward(1.0f);
	TestTrue(TEXT("next press predicts"), Gate.AllowsPrediction());
	return true;
}

#endif
