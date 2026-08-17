#include "CorsairsWorldClickResolver.h"

namespace
{
constexpr int32 PlayerCtrlType = 1;
constexpr int32 NpcCtrlType = 2;
constexpr int32 NpcEventCtrlType = 3;
constexpr int32 MonsterCtrlType = 5;
constexpr int32 TreeCtrlType = 6;
constexpr int32 MineCtrlType = 7;
constexpr int32 FishCtrlType = 8;
constexpr int32 SalvageCtrlType = 9;
constexpr int32 PetCtrlType = 10;
constexpr int32 RepairableCtrlType = 17;

FCorsairsClickIntent Reject(const TCHAR* Reason)
{
	FCorsairsClickIntent Result;
	Result.Type = ECorsairsClickIntentType::Rejected;
	Result.Reason = Reason;
	return Result;
}

FCorsairsClickIntent Select(const TCHAR* Reason)
{
	FCorsairsClickIntent Result;
	Result.Type = ECorsairsClickIntentType::SelectOnly;
	Result.Reason = Reason;
	return Result;
}

bool IsIdentityValid(const FCorsairsWorldActor& Actor)
{
	return Actor.WorldId != 0 && Actor.Handle != 0;
}

bool IsNpc(const int32 CtrlType)
{
	return CtrlType == NpcCtrlType || CtrlType == NpcEventCtrlType;
}

bool IsCombatActor(const int32 CtrlType)
{
	return CtrlType == PlayerCtrlType || CtrlType == MonsterCtrlType ||
		CtrlType == PetCtrlType;
}

bool HasSkillAtUsableLevel(const FCorsairsClickInput& Input, const int64 SkillId)
{
	for (const FCorsairsSkillEntry& Entry : Input.SkillBag)
	{
		if (Entry.SkillId == SkillId && Entry.Level > 0)
		{
			return true;
		}
	}
	return false;
}

const FCorsairsSkillDefinition* FindSkill(
	const FCorsairsClickInput& Input,
	const int64 SkillId,
	const TOptional<FCorsairsSkillDefinition>& Candidate)
{
	if (SkillId == 0 || !Candidate.IsSet() ||
		Candidate.GetValue().SkillId != SkillId ||
		!HasSkillAtUsableLevel(Input, SkillId))
	{
		return nullptr;
	}
	return &Candidate.GetValue();
}

bool HasEntityTargetType(const FCorsairsSkillDefinition& Skill)
{
	return (Skill.ApplyType == 1 || Skill.ApplyType == 3) &&
		Skill.TargetMode == ECorsairsSkillTargetMode::Entity;
}

bool HasGroundTargetType(const FCorsairsSkillDefinition& Skill)
{
	return Skill.ApplyType == 2 && Skill.TargetMode == ECorsairsSkillTargetMode::Ground;
}

bool SameIdentity(const FCorsairsWorldActor& Left, const FCorsairsWorldActor& Right)
{
	return Left.WorldId == Right.WorldId && Left.Handle == Right.Handle;
}

bool SameKnownTeam(const FCorsairsWorldActor& Local, const FCorsairsWorldActor& Target)
{
	return Local.TargetPolicy.TeamLeaderId != 0 &&
		Local.TargetPolicy.TeamLeaderId == Target.TargetPolicy.TeamLeaderId;
}

bool SameKnownSide(const FCorsairsWorldActor& Local, const FCorsairsWorldActor& Target)
{
	return Local.TargetPolicy.SideId != 0 && Target.TargetPolicy.SideId != 0 &&
		Local.TargetPolicy.SideId == Target.TargetPolicy.SideId;
}

bool RelationKnown(const FCorsairsWorldActor& Local, const FCorsairsWorldActor& Target)
{
	return (Local.TargetPolicy.TeamLeaderId != 0 &&
			Target.TargetPolicy.TeamLeaderId != 0) ||
		(Local.TargetPolicy.SideId != 0 && Target.TargetPolicy.SideId != 0);
}

bool IsFriendly(const FCorsairsWorldActor& Local, const FCorsairsWorldActor& Target)
{
	return SameIdentity(Local, Target) || SameKnownTeam(Local, Target) ||
		SameKnownSide(Local, Target);
}

bool IsResourceTargetAllowed(const int32 ApplyTarget, const int32 CtrlType)
{
	switch (ApplyTarget)
	{
	case 17:
		return CtrlType == RepairableCtrlType;
	case 18:
		return CtrlType == TreeCtrlType;
	case 19:
		return CtrlType == MineCtrlType;
	case 28:
		return CtrlType == FishCtrlType;
	case 29:
		return CtrlType == SalvageCtrlType;
	default:
		return false;
	}
}

bool PassesEntityTargetGate(
	const FCorsairsSkillDefinition& Skill,
	const FCorsairsWorldActor& Local,
	const FCorsairsWorldActor& Target)
{
	if (!IsIdentityValid(Local) || !IsIdentityValid(Target) || IsNpc(Target.CtrlType))
	{
		return false;
	}

	const bool bFriendly = IsFriendly(Local, Target);
	switch (Skill.ApplyTarget)
	{
	case 0:
		return false;
	case 1:
		return SameIdentity(Local, Target);
	case 2:
		return bFriendly && (SameIdentity(Local, Target) || SameKnownTeam(Local, Target));
	case 3:
		return IsCombatActor(Target.CtrlType) && Target.Hp > 0;
	case 4:
		if (Target.CtrlType == MonsterCtrlType)
		{
			return true;
		}
		return Target.CtrlType == PlayerCtrlType && RelationKnown(Local, Target) && !bFriendly;
	case 5:
		if (!IsCombatActor(Target.CtrlType))
		{
			return false;
		}
		if (Target.CtrlType == MonsterCtrlType)
		{
			return !Skill.bHelpful;
		}
		if (!RelationKnown(Local, Target))
		{
			return false;
		}
		return Skill.bHelpful ? bFriendly : !bFriendly;
	case 6:
		return Target.CtrlType == PlayerCtrlType && Target.Hp == 0;
	case 7:
		return IsCombatActor(Target.CtrlType) && !SameIdentity(Local, Target);
	case 17:
	case 18:
	case 19:
	case 28:
	case 29:
		return IsResourceTargetAllowed(Skill.ApplyTarget, Target.CtrlType);
	default:
		return false;
	}
}

FCorsairsClickIntent MakeEntityIntent(
	const int64 SkillId,
	const FCorsairsWorldActor& Target)
{
	FCorsairsClickIntent Result;
	Result.Type = ECorsairsClickIntentType::EntitySkill;
	Result.SkillId = SkillId;
	Result.TargetWorldId = Target.WorldId;
	Result.TargetHandle = Target.Handle;
	return Result;
}
}

FCorsairsClickIntent FCorsairsWorldClickResolver::Resolve(const FCorsairsClickInput& Input)
{
	if (Input.bUiConsumed)
	{
		return Select(TEXT("ui consumed click"));
	}
	if (Input.bLoginGate)
	{
		return Select(TEXT("login gate"));
	}
	if (Input.PreparedSkillId != 0)
	{
		const FCorsairsSkillDefinition* Skill = FindSkill(
			Input, Input.PreparedSkillId, Input.PreparedSkill);
		if (Skill == nullptr)
		{
			return Reject(TEXT("prepared skill is absent or unusable"));
		}
		if (HasGroundTargetType(*Skill))
		{
			if (!Input.bHasGroundPoint)
			{
				return Reject(TEXT("prepared ground skill needs exact ground point"));
			}
			FCorsairsClickIntent Result;
			Result.Type = ECorsairsClickIntentType::GroundSkill;
			Result.SkillId = Skill->SkillId;
			Result.GroundPoint = Input.GroundPoint;
			return Result;
		}
		if (!HasEntityTargetType(*Skill))
		{
			return Reject(TEXT("prepared skill target mode is unsupported"));
		}
		if (!IsIdentityValid(Input.LocalActor))
		{
			return Reject(TEXT("local actor identity is invalid"));
		}
		if (!Input.bHasExactActor)
		{
			return Reject(TEXT("prepared entity skill needs exact actor"));
		}
		if (!Input.bHasExactActorIdentity)
		{
			return Reject(TEXT("prepared entity skill needs exact identity"));
		}
		if (!IsIdentityValid(Input.ExactActor))
		{
			return Reject(TEXT("exact actor identity is invalid"));
		}
		if (Input.ExactActor.WorldId != Input.ExactActorWorldId ||
			Input.ExactActor.Handle != Input.ExactActorHandle)
		{
			return Reject(TEXT("exact actor identity is stale"));
		}
		if (!PassesEntityTargetGate(*Skill, Input.LocalActor, Input.ExactActor))
		{
			return Reject(TEXT("prepared entity target is invalid"));
		}
		return MakeEntityIntent(Skill->SkillId, Input.ExactActor);
	}

	if (!IsIdentityValid(Input.LocalActor))
	{
		return Reject(TEXT("local actor identity is invalid"));
	}
	if (Input.bHasExactActor)
	{
		if (!Input.bHasExactActorIdentity)
		{
			return Reject(TEXT("exact actor needs exact identity"));
		}
		if (!IsIdentityValid(Input.ExactActor))
		{
			return Reject(TEXT("exact actor identity is invalid"));
		}
		if (Input.ExactActor.WorldId != Input.ExactActorWorldId ||
			Input.ExactActor.Handle != Input.ExactActorHandle)
		{
			return Reject(TEXT("exact actor identity is stale"));
		}
	}
	else if (Input.bHasExactActorIdentity)
	{
		return Reject(TEXT("exact actor snapshot is missing"));
	}

	if (Input.bHasExactActor && IsNpc(Input.ExactActor.CtrlType))
	{
		FCorsairsClickIntent Result;
		Result.Type = ECorsairsClickIntentType::Talk;
		Result.TargetWorldId = Input.ExactActor.WorldId;
		Result.TargetHandle = Input.ExactActor.Handle;
		return Result;
	}

	if (Input.bHasExactActor)
	{
		const FCorsairsSkillDefinition* Skill = FindSkill(
			Input, Input.DefaultSkillId, Input.DefaultSkill);
		if (Skill != nullptr && HasEntityTargetType(*Skill) &&
			PassesEntityTargetGate(*Skill, Input.LocalActor, Input.ExactActor))
		{
			return MakeEntityIntent(Skill->SkillId, Input.ExactActor);
		}
		return Select(TEXT("exact combat actor selected"));
	}

	if (!Input.bHasGroundPoint)
	{
		return Reject(TEXT("ground point is missing"));
	}
	FCorsairsClickIntent Result;
	Result.Type = ECorsairsClickIntentType::Move;
	Result.GroundPoint = Input.GroundPoint;
	return Result;
}
