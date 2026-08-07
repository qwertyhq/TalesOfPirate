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

/** Персонаж, попавший в поле зрения: другой игрок, NPC или монстр. */
USTRUCT(BlueprintType)
struct FCorsairsWorldActor
{
	GENERATED_BODY()

	/** Идентификатор в мире. По нему приходят все дальнейшие сообщения. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 WorldId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FString Name;

	/** Положение в координатах карты. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FIntPoint Position = FIntPoint::ZeroValue;

	/** Угол поворота в десятых долях градуса, как в протоколе. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 Angle = 0;

	/** Тип модели. Разворачивается в тело через таблицу персонажей. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 TypeId = 0;

	/** Управляющий тип: игрок, NPC, монстр. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 CtrlType = 0;

	/** Запись в таблице персонажей. У NPC именно она задаёт модель: поле
	 *  внешности, которым пользуются игроки, у них пустое. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 ChaId = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCorsairsActorSeen,
											const FCorsairsWorldActor&, Actor);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCorsairsActorLeft, int64, WorldId);

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

	/** Персонаж попал в поле зрения. Сервер шлёт это и для NPC, и для
	 *  монстров, и для других игроков — на garner одних только NPC 452. */
	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsActorSeen OnActorSeen;

	/** Персонаж вышел из поля зрения. */
	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsActorLeft OnActorLeft;

	/** Отправляет серверу путь движения.
	 *
	 *  Путь — список точек в координатах карты (100 единиц на клетку). Сервер
	 *  ждёт именно путь, а не мгновенное положение: он сам проигрывает
	 *  перемещение по времени и проверяет проходимость, поэтому телепорт в
	 *  произвольную точку им не принимается.
	 *
	 *  Первой точкой должно идти текущее положение персонажа. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool SendMovePath(const TArray<FIntPoint>& Path);

	/** Идентификатор персонажа в мире. Приходит при входе в карту и нужен в
	 *  каждой команде действия. */
	UFUNCTION(BlueprintPure, Category = "Corsairs")
	int64 GetWorldId() const { return WorldId; }

	/** Положение, с которого сервер начал персонажа, в координатах карты. */
	UFUNCTION(BlueprintPure, Category = "Corsairs")
	FIntPoint GetSpawnPosition() const { return SpawnPosition; }

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	FString GetMapName() const { return MapName; }

	/** Кого сервер держит в поле зрения прямо сейчас.
	 *
	 *  Список ведёт сама сессия, а не подписчик: делегат-свойство при
	 *  обращении из Python отдаёт копию, и подписка на неё теряется. Хранить
	 *  состояние здесь надёжнее и к тому же полезно — новому подписчику не
	 *  нужно ждать следующего сообщения, чтобы узнать, кто вокруг. */
	UFUNCTION(BlueprintPure, Category = "Corsairs")
	const TArray<FCorsairsWorldActor>& GetVisibleActors() const { return VisibleActors; }

	/** Сколько каких команд пришло от сервера, по их номерам.
	 *
	 *  Нужно для разбора: «персонажей в поле зрения ноль» может означать и
	 *  что рядом никого нет, и что команда не обрабатывается. Счётчик
	 *  различает эти случаи. */
	UFUNCTION(BlueprintPure, Category = "Corsairs")
	TMap<int32, int32> GetReceivedCommands() const { return ReceivedCommands; }

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

	int64 WorldId = 0;
	FIntPoint SpawnPosition = FIntPoint::ZeroValue;
	FString MapName;

	/** Номер пакета действия. Сервер отслеживает порядок команд по нему и
	 *  отбрасывает устаревшие. */
	int64 ActionPacketId = 0;

	TMap<int32, int32> ReceivedCommands;
	TArray<FCorsairsWorldActor> VisibleActors;

	FString PendingAccount;
	FString PendingPasswordHash;
	bool bHasPassword2 = false;
};
