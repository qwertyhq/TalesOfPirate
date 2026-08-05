#pragma once

#include "CoreMinimal.h"
#include "CorsairsSession.h"
#include "GameFramework/HUD.h"

#include "CorsairsLoginHud.generated.h"

/**
 * Экран входа и состояния сессии.
 *
 * Рисуется через Canvas, а не UMG. Виджет UMG существует только как ассет
 * Blueprint, создать который из кода нельзя — понадобился бы ручной шаг в
 * редакторе, и вход перестал бы работать на чистом чекауте. Canvas обходится
 * без ассетов и решает главную задачу: показать, на какой стадии находится
 * подключение и почему оно не удалось.
 *
 * Полноценный экран с полями ввода — задача UMG и следующего этапа. Здесь
 * важно, чтобы отказ сервера был виден игроку, а не только в журнале.
 */
UCLASS()
class CORSAIRSGAME_API ACorsairsLoginHud : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

	/** Показывать ли сведения о сессии. Выключается, когда уровень открывают
	 *  ради осмотра геометрии. */
	UPROPERTY(EditDefaultsOnly, Category = "Corsairs")
	bool bShowSessionState = true;

	/** Ввод символа в активное поле. Вызывается контроллером. */
	void AppendCharacter(const FString& Character);
	void EraseCharacter();

	/** Переключает поле ввода: учётная запись, пароль, вход. */
	void NextField();

	/** Отправляет введённые данные на вход. */
	void SubmitLogin();

	bool IsAcceptingInput() const;

private:
	FString DescribeStage(ECorsairsLoginStage Stage) const;
	void DrawLoginForm(class UFont* Font);

	/** Что редактируется сейчас. Полей всего два, поэтому хватает флага. */
	bool bEditingPassword = false;

	FString AccountInput;
	FString PasswordInput;
};
