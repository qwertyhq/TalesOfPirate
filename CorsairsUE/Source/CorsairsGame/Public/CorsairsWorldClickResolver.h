#pragma once

#include "CoreMinimal.h"

#include "CorsairsSession.h"
#include "CorsairsSkillCatalog.h"

enum class ECorsairsClickIntentType : uint8
{
	Move,
	Talk,
	EntitySkill,
	GroundSkill,
	SelectOnly,
	Rejected,
};

/** Входной снимок одного exact click. Списка целей здесь намеренно нет. */
struct FCorsairsClickInput
{
	bool bUiConsumed = false;
	bool bLoginGate = false;

	int64 PreparedSkillId = 0;
	TOptional<FCorsairsSkillDefinition> PreparedSkill;
	int64 DefaultSkillId = 0;
	TOptional<FCorsairsSkillDefinition> DefaultSkill;
	TArray<FCorsairsSkillEntry> SkillBag;

	FCorsairsWorldActor LocalActor;
	bool bHasExactActor = false;
	FCorsairsWorldActor ExactActor;
	/** Отдельный identity-снимок позволяет отклонить stale UObject/handle. */
	bool bHasExactActorIdentity = false;
	int64 ExactActorWorldId = 0;
	int64 ExactActorHandle = 0;

	bool bHasGroundPoint = false;
	FIntPoint GroundPoint = FIntPoint::ZeroValue;
};

struct FCorsairsClickIntent
{
	ECorsairsClickIntentType Type = ECorsairsClickIntentType::Rejected;
	int64 SkillId = 0;
	int64 TargetWorldId = 0;
	int64 TargetHandle = 0;
	FIntPoint GroundPoint = FIntPoint::ZeroValue;
	FString Reason;
};

class CORSAIRSGAME_API FCorsairsWorldClickResolver
{
public:
	static FCorsairsClickIntent Resolve(const FCorsairsClickInput& Input);
};
