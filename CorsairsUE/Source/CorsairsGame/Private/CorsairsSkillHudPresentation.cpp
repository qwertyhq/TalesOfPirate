#include "CorsairsSkillHudPresentation.h"

namespace
{
constexpr int32 SkillShortcutType = 2;
constexpr int32 ItemShortcutType = 1;
constexpr int32 LifeSkillShortcutType = 3;
constexpr int32 SailSkillShortcutType = 4;
constexpr int32 SkillHudSlotCount = 12;

const FCorsairsShortcutEntry* FindShortcut(
	const TArray<FCorsairsShortcutEntry>& Shortcuts,
	const int32 Slot)
{
	for (const FCorsairsShortcutEntry& Shortcut : Shortcuts)
	{
		if (Shortcut.Slot == Slot)
		{
			return &Shortcut;
		}
	}
	return nullptr;
}

const FCorsairsSkillEntry* FindSkill(
	const TArray<FCorsairsSkillEntry>& SkillBag,
	const int64 SkillId)
{
	return SkillBag.FindByPredicate(
		[SkillId](const FCorsairsSkillEntry& Skill)
		{
			return Skill.SkillId == SkillId;
		});
}

bool IsSupportedSkill(const FCorsairsSkillDefinition* Definition)
{
	return Definition != nullptr &&
		(Definition->TargetMode == ECorsairsSkillTargetMode::Entity ||
			Definition->TargetMode == ECorsairsSkillTargetMode::Ground);
}

FString TargetTypeLabel(const ECorsairsSkillTargetMode TargetMode)
{
	switch (TargetMode)
	{
	case ECorsairsSkillTargetMode::Entity:
		return TEXT("сущность");
	case ECorsairsSkillTargetMode::Ground:
		return TEXT("земля");
	default:
		return FString();
	}
}

FCorsairsSkillHudSlot MakeUnsupportedSlot(
	const int32 Slot,
	const FString& Reason)
{
	FCorsairsSkillHudSlot Result;
	Result.Slot = Slot;
	Result.Name = TEXT("Неподдерживается");
	Result.bOccupied = true;
	Result.Reason = Reason;
	return Result;
}

FCorsairsSkillHudSlot MakeSkillSlot(
	const int32 Slot,
	const FCorsairsShortcutEntry& Shortcut,
	const TArray<FCorsairsSkillEntry>& SkillBag,
	const FCorsairsSkillCatalog& SkillCatalog)
{
	const FCorsairsSkillEntry* Entry = FindSkill(SkillBag, Shortcut.GridId);
	const FCorsairsSkillDefinition* Definition = SkillCatalog.Find(Shortcut.GridId);
	if (Entry == nullptr)
	{
		return MakeUnsupportedSlot(Slot, TEXT("Навык отсутствует в skill bag"));
	}
	if (!IsSupportedSkill(Definition))
	{
		return MakeUnsupportedSlot(Slot, TEXT("Навык не поддерживается"));
	}

	FCorsairsSkillHudSlot Result;
	Result.Slot = Slot;
	Result.Name = Definition->Name;
	Result.Level = Entry->Level;
	Result.State = Entry->State;
	Result.bSupported = true;
	Result.bOccupied = true;
	return Result;
}
} // namespace

FCorsairsSkillHudPresentation MakeCorsairsSkillHudPresentation(
	const TArray<FCorsairsShortcutEntry>& Shortcuts,
	const TArray<FCorsairsSkillEntry>& SkillBag,
	const FCorsairsSkillCatalog& SkillCatalog,
	const int64 PreparedSkillId,
	const FString& SelectedTargetName,
	const FString& RejectionReason)
{
	FCorsairsSkillHudPresentation Result;
	Result.Slots.Reserve(SkillHudSlotCount);
	Result.TargetName = SelectedTargetName;
	Result.RejectionReason = RejectionReason;

	for (int32 Slot = 0; Slot < SkillHudSlotCount; ++Slot)
	{
		const FCorsairsShortcutEntry* Shortcut = FindShortcut(Shortcuts, Slot);
		if (Shortcut == nullptr || Shortcut->Type == 0 || Shortcut->GridId == 0)
		{
			FCorsairsSkillHudSlot Empty;
			Empty.Slot = Slot;
			Empty.Name = TEXT("Пусто");
			Result.Slots.Add(MoveTemp(Empty));
			continue;
		}

		FCorsairsSkillHudSlot DisplaySlot;
		switch (Shortcut->Type)
		{
		case SkillShortcutType:
			DisplaySlot = MakeSkillSlot(Slot, *Shortcut, SkillBag, SkillCatalog);
			break;
		case ItemShortcutType:
			DisplaySlot = MakeUnsupportedSlot(Slot, TEXT("Предметы не поддерживаются"));
			break;
		case LifeSkillShortcutType:
			DisplaySlot = MakeUnsupportedSlot(Slot, TEXT("Life-навыки не поддерживаются"));
			break;
		case SailSkillShortcutType:
			DisplaySlot = MakeUnsupportedSlot(Slot, TEXT("Sail-навыки не поддерживаются"));
			break;
		default:
			DisplaySlot = MakeUnsupportedSlot(Slot, TEXT("Тип shortcut не поддерживается"));
			break;
		}
		Result.Slots.Add(MoveTemp(DisplaySlot));
	}

	if (PreparedSkillId != 0)
	{
		const FCorsairsSkillEntry* Entry = FindSkill(SkillBag, PreparedSkillId);
		const FCorsairsSkillDefinition* Definition = SkillCatalog.Find(PreparedSkillId);
		if (IsSupportedSkill(Definition) && Entry != nullptr)
		{
			Result.PreparedSkillName = Definition->Name;
			Result.PreparedTargetType = TargetTypeLabel(Definition->TargetMode);
		}
		else
		{
			Result.PreparedSkillName = TEXT("Неподдерживается");
		}
	}

	return Result;
}
