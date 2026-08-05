#pragma once

#include "CoreMinimal.h"
#include "CorsairsConnection.h"
#include "UObject/Object.h"

#include "CorsairsSession.generated.h"

/** Персонаж в списке выбора. */
USTRUCT(BlueprintType)
struct FCorsairsCharacterSlot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	bool Valid = false;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FString Job;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 Level = 0;

	/** Тип модели персонажа. Разворачивается в имя скелета `{:04}.lab`. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 TypeId = 0;
};

/** Стадия входа. Именно она определяет, что показывать на экране. */
UENUM(BlueprintType)
enum class ECorsairsLoginStage : uint8
{
	Idle           UMETA(DisplayName = "Не начат"),
	Connecting     UMETA(DisplayName = "Подключение"),
	Authenticating UMETA(DisplayName = "Проверка учётной записи"),
	SelectingCha   UMETA(DisplayName = "Выбор персонажа"),
	EnteringWorld  UMETA(DisplayName = "Вход в мир"),
	InWorld        UMETA(DisplayName = "В мире"),
	Failed         UMETA(DisplayName = "Ошибка"),
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCorsairsLoginStageChanged,
											 ECorsairsLoginStage, Stage,
											 const FString&, Message);

/**
 * Сессия игрока: соединение с GateServer и последовательность входа.
 *
 * Порядок обмена продиктован серверами и проверен консольным клиентом
 * `sources/Dotnet/Tools/Corsairs.TestClient`:
 *
 *   сервер -> MC_SEND_SERVER_PUBLIC_KEY  (пустой, если RSA-AES выключен)
 *   CM_LOGIN            -> MC_LOGIN            (список персонажей)
 *   CM_CREATE_PASSWORD2 -> MC_CREATE_PASSWORD2 (если второго пароля ещё нет)
 *   CM_BGNPLAY          -> MC_ENTERMAP         (ответ уже от GameServer)
 *
 * Второй пароль обязателен: GroupServer отвергает вход в мир, пока он пуст.
 *
 * Класс живёт в сетевом модуле, а не в игровом, хотя описывает ход входа.
 * Причина техническая и жёсткая: WPacket и RPacket приходят из
 * sources/Libraries без макроса экспорта, поэтому за пределами своего модуля
 * их символы не видны компоновщику. Всё, что трогает пакеты напрямую, обязано
 * оставаться здесь; наружу отдаётся API на типах UE.
 */
UCLASS(BlueprintType)
class CORSAIRSNET_API UCorsairsSession : public UObject
{
	GENERATED_BODY()

public:
	/** Начинает вход. Дальнейший ход отслеживается через OnStageChanged. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void Login(const FString& Host, int32 Port, const FString& Account, const FString& Password);

	/** Входит в мир выбранным персонажем. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void EnterWorld(int32 SlotIndex);

	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void Logout();

	/** Прокачивает соединение. Нужен там, где нет игрового цикла. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	void Poll();

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	ECorsairsLoginStage GetStage() const { return Stage; }

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	const TArray<FCorsairsCharacterSlot>& GetCharacters() const { return Characters; }

	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsLoginStageChanged OnStageChanged;

	/** Второй пароль учётной записи. Задаётся заранее: экрана для его ввода
	 *  пока нет, а без него сервер не пускает в мир. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Corsairs")
	FString Password2 = TEXT("test1234");

private:
	void HandlePacket(Corsairs::Net::RPacket& Packet);
	void HandleConnectionState(ECorsairsConnectionState NewState, const FString& Reason);

	void SendLogin();
	void SendCreatePassword2();

	void SetStage(ECorsairsLoginStage NewStage, const FString& Message);

	UPROPERTY()
	TObjectPtr<UCorsairsConnection> Connection;

	ECorsairsLoginStage Stage = ECorsairsLoginStage::Idle;
	TArray<FCorsairsCharacterSlot> Characters;

	FString PendingAccount;
	FString PendingPasswordHash;
	bool bHasPassword2 = false;
};
