#pragma once

#include "CoreMinimal.h"

#include "CorsairsSession.h"
#include "CorsairsSkillCatalog.h"

/** Данные одного слота Canvas HUD без зависимости от мира или сессии. */
struct FCorsairsSkillHudSlot
{
	int32 Slot = 0;
	FString Name;
	int32 Level = 0;
	int32 State = 0;
	bool bSupported = false;
	bool bOccupied = false;
	FString Reason;
};

/** Чистый снимок текста и слотов для отрисовки панели навыков. */
struct FCorsairsSkillHudPresentation
{
	TArray<FCorsairsSkillHudSlot> Slots;
	FString PreparedSkillName;
	FString PreparedTargetType;
	FString TargetName;
	FString RejectionReason;
};

/**
 * Форматирует ровно первый ряд из 12 shortcut entries для Canvas HUD.
 * Функция только читает входные снимки и не обращается к Session или World.
 */
CORSAIRSGAME_API FCorsairsSkillHudPresentation MakeCorsairsSkillHudPresentation(
	const TArray<FCorsairsShortcutEntry>& Shortcuts,
	const TArray<FCorsairsSkillEntry>& SkillBag,
	const FCorsairsSkillCatalog& SkillCatalog,
	int64 PreparedSkillId,
	const FString& SelectedTargetName,
	const FString& RejectionReason);
