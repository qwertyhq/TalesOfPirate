#include "CorsairsLoginHud.h"

#include "CorsairsGameMode.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"

namespace
{
	/** Отступ от края экрана и шаг между строками. */
	constexpr float MarginX = 24.0f;
	constexpr float MarginY = 24.0f;
	constexpr float LineHeight = 20.0f;

	const FLinearColor NormalColour(0.85f, 0.9f, 1.0f, 1.0f);
	const FLinearColor FailureColour(1.0f, 0.45f, 0.35f, 1.0f);
	const FLinearColor SuccessColour(0.55f, 1.0f, 0.6f, 1.0f);
}

void ACorsairsLoginHud::DrawHUD()
{
	Super::DrawHUD();

	if (!bShowSessionState || Canvas == nullptr)
	{
		return;
	}

	const ACorsairsGameMode* GameMode = GetWorld() != nullptr
		? GetWorld()->GetAuthGameMode<ACorsairsGameMode>()
		: nullptr;
	if (GameMode == nullptr)
	{
		return;
	}

	const UCorsairsSession* Session = GameMode->GetSession();
	if (Session == nullptr)
	{
		return;
	}

	UFont* Font = GEngine != nullptr ? GEngine->GetMediumFont() : nullptr;
	if (Font == nullptr)
	{
		return;
	}

	const ECorsairsLoginStage Stage = Session->GetStage();

	// Форма показывается, пока сессия не начата или отклонена: в остальное
	// время вводить нечего, а перекрывать игру полем ввода не нужно.
	if (Stage == ECorsairsLoginStage::Idle || Stage == ECorsairsLoginStage::Failed)
	{
		DrawLoginForm(Font);
	}

	FLinearColor Colour = NormalColour;
	if (Stage == ECorsairsLoginStage::Failed)
	{
		Colour = FailureColour;
	}
	else if (Stage == ECorsairsLoginStage::InWorld)
	{
		Colour = SuccessColour;
	}

	float Y = MarginY;

	FCanvasTextItem Title(FVector2D(MarginX, Y),
						  FText::FromString(DescribeStage(Stage)), Font, Colour);
	Title.EnableShadow(FLinearColor::Black);
	Canvas->DrawItem(Title);
	Y += LineHeight;

	if (Stage == ECorsairsLoginStage::SelectingCha)
	{
		// Список персонажей полезен даже без возможности выбрать: видно, что
		// именно вернул сервер.
		for (const FCorsairsCharacterSlot& Slot : Session->GetCharacters())
		{
			if (!Slot.Valid)
			{
				continue;
			}
			const FString Line = FString::Printf(TEXT("  %s, уровень %d"),
												 *Slot.Name, Slot.Level);
			FCanvasTextItem Item(FVector2D(MarginX, Y), FText::FromString(Line),
								 Font, NormalColour);
			Item.EnableShadow(FLinearColor::Black);
			Canvas->DrawItem(Item);
			Y += LineHeight;
		}
	}

	if (Stage == ECorsairsLoginStage::InWorld)
	{
		const FIntPoint Spawn = Session->GetSpawnPosition();
		const FString Line = FString::Printf(
			TEXT("карта %s, старт (%d, %d), id в мире %lld"),
			*Session->GetMapName(), Spawn.X, Spawn.Y, Session->GetWorldId());
		FCanvasTextItem Item(FVector2D(MarginX, Y), FText::FromString(Line),
							 Font, NormalColour);
		Item.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(Item);
	}
}

bool ACorsairsLoginHud::IsAcceptingInput() const
{
	const ACorsairsGameMode* GameMode = GetWorld() != nullptr
		? GetWorld()->GetAuthGameMode<ACorsairsGameMode>()
		: nullptr;
	const UCorsairsSession* Session = GameMode != nullptr ? GameMode->GetSession() : nullptr;
	if (Session == nullptr)
	{
		return false;
	}

	const ECorsairsLoginStage Stage = Session->GetStage();
	return Stage == ECorsairsLoginStage::Idle || Stage == ECorsairsLoginStage::Failed;
}

void ACorsairsLoginHud::AppendCharacter(const FString& Character)
{
	if (!IsAcceptingInput())
	{
		return;
	}
	(bEditingPassword ? PasswordInput : AccountInput) += Character;
}

void ACorsairsLoginHud::EraseCharacter()
{
	if (!IsAcceptingInput())
	{
		return;
	}
	FString& Field = bEditingPassword ? PasswordInput : AccountInput;
	Field.LeftChopInline(1);
}

void ACorsairsLoginHud::NextField()
{
	bEditingPassword = !bEditingPassword;
}

void ACorsairsLoginHud::SubmitLogin()
{
	ACorsairsGameMode* GameMode = GetWorld() != nullptr
		? GetWorld()->GetAuthGameMode<ACorsairsGameMode>()
		: nullptr;
	if (GameMode == nullptr || !IsAcceptingInput())
	{
		return;
	}

	// Пустые поля означают «войти с тем, что записано в настройках»: так
	// проверочные запуски не требуют печатать одно и то же каждый раз.
	if (!AccountInput.IsEmpty())
	{
		GameMode->Account = AccountInput;
	}
	if (!PasswordInput.IsEmpty())
	{
		GameMode->Password = PasswordInput;
	}

	GameMode->StartLogin();
}

void ACorsairsLoginHud::DrawLoginForm(UFont* Font)
{
	if (Canvas == nullptr || Font == nullptr)
	{
		return;
	}

	const float FormY = Canvas->SizeY * 0.4f;
	const float FormX = Canvas->SizeX * 0.5f - 160.0f;

	const TArray<FString> Lines = {
		FString::Printf(TEXT("%s Учётная запись: %s"),
						bEditingPassword ? TEXT(" ") : TEXT(">"), *AccountInput),
		// Пароль показывается звёздочками: экран может попасть в запись
		// или на скриншот.
		FString::Printf(TEXT("%s Пароль: %s"),
						bEditingPassword ? TEXT(">") : TEXT(" "),
						*FString::ChrN(PasswordInput.Len(), TEXT('*'))),
		TEXT(""),
		TEXT("Tab — следующее поле, Enter — войти"),
		TEXT("Пустые поля — значения из настроек режима"),
	};

	float Y = FormY;
	for (const FString& Line : Lines)
	{
		FCanvasTextItem Item(FVector2D(FormX, Y), FText::FromString(Line),
							 Font, NormalColour);
		Item.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(Item);
		Y += LineHeight;
	}
}

FString ACorsairsLoginHud::DescribeStage(ECorsairsLoginStage Stage) const
{
	switch (Stage)
	{
	case ECorsairsLoginStage::Idle:           return TEXT("Не подключён");
	case ECorsairsLoginStage::Connecting:     return TEXT("Подключение к серверу...");
	case ECorsairsLoginStage::Authenticating: return TEXT("Проверка учётной записи...");
	case ECorsairsLoginStage::SelectingCha:   return TEXT("Выбор персонажа");
	case ECorsairsLoginStage::EnteringWorld:  return TEXT("Вход в мир...");
	case ECorsairsLoginStage::InWorld:        return TEXT("В мире");
	case ECorsairsLoginStage::Failed:         return TEXT("Ошибка подключения");
	}
	return TEXT("Неизвестная стадия");
}
