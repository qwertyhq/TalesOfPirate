#include "CorsairsGameMode.h"

#include "CorsairsPlayerCharacter.h"

#include "Dom/JsonObject.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsGameMode, Log, All);

namespace
{
	/** Таблица «тип персонажа -> модель тела». Готовится скриптом
	 *  Scripts/build_character_map.py по таблице characters игровых данных:
	 *  внутри игры нет ни sqlite3, ни доступа к исходникам. */
	const TCHAR* CharacterMapRelativePath = TEXT("Data/character_map.json");
}

ACorsairsGameMode::ACorsairsGameMode()
{
	DefaultPawnClass = ACorsairsPlayerCharacter::StaticClass();
}

void ACorsairsGameMode::BeginPlay()
{
	Super::BeginPlay();

	if (!bAutoLogin)
	{
		UE_LOG(LogCorsairsGameMode, Log, TEXT("автоматический вход выключен"));
		return;
	}

	Session = NewObject<UCorsairsSession>(this);
	Session->OnStageChanged.AddDynamic(this, &ACorsairsGameMode::HandleStageChanged);
	Session->Login(Host, Port, Account, Password);
}

void ACorsairsGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Session != nullptr)
	{
		Session->Logout();
		Session = nullptr;
	}
	Super::EndPlay(Reason);
}

void ACorsairsGameMode::HandleStageChanged(ECorsairsLoginStage Stage, const FString& Message)
{
	UE_LOG(LogCorsairsGameMode, Log, TEXT("вход: %s"), *Message);

	switch (Stage)
	{
	case ECorsairsLoginStage::SelectingCha:
	{
		// Пока экрана выбора нет — берём первого пригодного персонажа.
		const TArray<FCorsairsCharacterSlot>& Characters = Session->GetCharacters();
		for (int32 Index = 0; Index < Characters.Num(); ++Index)
		{
			if (Characters[Index].Valid)
			{
				Session->EnterWorld(Index);
				return;
			}
		}
		UE_LOG(LogCorsairsGameMode, Warning,
			   TEXT("в учётной записи нет персонажей — входить некем"));
		break;
	}

	case ECorsairsLoginStage::InWorld:
	{
		// Тело подбирается по типу персонажа, которым вошли.
		const TArray<FCorsairsCharacterSlot>& Characters = Session->GetCharacters();
		for (const FCorsairsCharacterSlot& Slot : Characters)
		{
			if (!Slot.Valid)
			{
				continue;
			}

			APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
			ACorsairsPlayerCharacter* Character =
				Controller != nullptr
					? Cast<ACorsairsPlayerCharacter>(Controller->GetPawn())
					: nullptr;
			if (Character == nullptr)
			{
				UE_LOG(LogCorsairsGameMode, Warning, TEXT("персонаж игрока ещё не создан"));
				break;
			}

			const FString MeshPath = ResolveBodyMesh(Slot.TypeId);
			if (MeshPath.IsEmpty())
			{
				UE_LOG(LogCorsairsGameMode, Warning,
					   TEXT("для типа %d нет модели в таблице"), Slot.TypeId);
			}
			else if (Character->SetBodyMesh(MeshPath))
			{
				UE_LOG(LogCorsairsGameMode, Log, TEXT("тело: %s"), *MeshPath);
			}
			break;
		}
		break;
	}

	default:
		break;
	}
}

FString ACorsairsGameMode::ResolveBodyMesh(int32 TypeId) const
{
	const FString Path = FPaths::ProjectDir() / CharacterMapRelativePath;

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		UE_LOG(LogCorsairsGameMode, Warning,
			   TEXT("нет таблицы персонажей: %s (сначала build_character_map.py)"), *Path);
		return FString();
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogCorsairsGameMode, Warning, TEXT("таблица персонажей не разобралась"));
		return FString();
	}

	const TSharedPtr<FJsonObject>* Characters = nullptr;
	if (!Root->TryGetObjectField(TEXT("characters"), Characters) || Characters == nullptr)
	{
		return FString();
	}

	const TSharedPtr<FJsonObject>* Entry = nullptr;
	if (!(*Characters)->TryGetObjectField(FString::FromInt(TypeId), Entry) || Entry == nullptr)
	{
		return FString();
	}

	FString Mesh;
	(*Entry)->TryGetStringField(TEXT("mesh"), Mesh);
	return Mesh;
}
