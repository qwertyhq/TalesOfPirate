#pragma once

#include "CoreMinimal.h"

enum class ECorsairsSkillTargetMode : uint8
{
	Self,
	Entity,
	Ground,
	Unsupported,
};

struct FCorsairsSkillDefinition
{
	int64 SkillId = 0;
	FString Name;
	int32 ApplyDistance = 0;
	int32 ApplyTarget = 0;
	int32 ApplyType = 0;
	bool bHelpful = false;
	int32 HabitatMask = 0;
	int32 Radius = 0;
	int32 Shape = 0;
	ECorsairsSkillTargetMode TargetMode = ECorsairsSkillTargetMode::Unsupported;
};

class CORSAIRSGAME_API FCorsairsSkillCatalog
{
public:
	bool Load(const FString& JsonPath, FString& OutError);
	const FCorsairsSkillDefinition* Find(int64 SkillId) const;
	int32 Num() const;

private:
	TMap<int64, FCorsairsSkillDefinition> Skills;
};
