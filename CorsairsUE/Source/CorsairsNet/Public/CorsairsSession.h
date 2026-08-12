#pragma once

#include "CoreMinimal.h"
#include "CorsairsActionReducer.h"
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

inline constexpr int32 CorsairsEquipSlotCount = 34;

USTRUCT(BlueprintType)
struct FCorsairsCharacterLook
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 SynType = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 TypeId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int32 HairId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	bool bIsBoat = false;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	TArray<int32> EquipIds;
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

	/** Угол поворота в целых градусах. Оригинал кладёт пришедшее значение
	 *  прямо в персонажа (`pCha->setYaw(sAngle)` в NetProtocol.cpp) и
	 *  переводит в радианы умножением на пи и делением на сто восемьдесят —
	 *  делителя в этом пути нет. Та же единица у построек и предметов на
	 *  земле; отдельно стоят только эффекты, где угол хранится в сотых долях
	 *  радиана. */
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

	/** Ключ сущности на сервере. Идёт в паре с идентификатором: одного
	 *  идентификатора серверу мало, он сверяет обе половины. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 Handle = 0;

	/** Здоровье. Обновляется итогами ударов — по нему и видно урон. */
	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 Hp = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	double MovementSpeedCmPerSecond = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FCorsairsCharacterLook Look;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCorsairsActorSeen,
											const FCorsairsWorldActor&, Actor);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCorsairsActorLeft, int64, WorldId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCorsairsActorLookChanged,
											 int64, WorldId,
											 const FCorsairsCharacterLook&, Look);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FCorsairsMovementChanged,
	const FCorsairsMovementEvent&,
	Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FCorsairsProtocolError,
	const FString&,
	Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FCorsairsMovementAuthorityChanged,
	bool,
	bLocked,
	int64,
	Epoch);

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

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	FCorsairsWorldActor GetLocalActor() const { return LocalActor; }

	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsLoginStageChanged OnStageChanged;

	/** Персонаж попал в поле зрения. Сервер шлёт это и для NPC, и для
	 *  монстров, и для других игроков — на garner одних только NPC 452. */
	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsActorSeen OnActorSeen;

	/** Персонаж вышел из поля зрения. */
	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsActorLeft OnActorLeft;

	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsActorLookChanged OnActorLookChanged;

	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsMovementChanged OnMovementChanged;

	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsProtocolError OnProtocolError;

	UPROPERTY(BlueprintAssignable, Category = "Corsairs")
	FCorsairsMovementAuthorityChanged OnMovementAuthorityChanged;

	/** Отправляет серверу путь движения.
	 *
	 *  Путь — список точек в координатах карты (100 единиц на клетку). Сервер
	 *  ждёт именно путь, а не мгновенное положение: он сам проигрывает
	 *  перемещение по времени и проверяет проходимость, поэтому телепорт в
	 *  произвольную точку им не принимается.
	 *
	 *  Первой точкой должно идти текущее положение персонажа. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	ECorsairsActionRequestResult SendMovePath(
		const TArray<FIntPoint>& Path);

	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	ECorsairsActionRequestResult SubmitPredictedPosition(
		FIntPoint Endpoint);

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	FIntPoint GetConfirmedPosition() const;

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	bool IsMovementAuthorityLocked() const;

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	int64 GetMovementAuthorityEpoch() const;

	UFUNCTION(BlueprintPure, Category = "Corsairs")
	double GetMovementSpeedCmPerSecond() const;

	/** Применяет умение к цели.
	 *
	 *  Признак движения в пакете жёстко равен двум — «подойти и ударить».
	 *  С нулём сервер не отвечает вовсе, а путь из одной точки не
	 *  разыгрывает: подводить персонажа к цели обязан он сам.
	 *
	 *  Учтите: без оружия в руках сервер подтвердит действие, но удара не
	 *  случится — по цели не придёт ничего. Молчание здесь неотличимо от
	 *  поломки, поэтому оружие надевается заранее. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	ECorsairsActionRequestResult UseSkillOn(
		int64 SkillId, int64 TargetWorldId);

	/** Надевает вещь: перекладывает её из ячейки сумки в слот экипировки.
	 *
	 *  Слоты — из enumEQUIP_* (правая рука 9). Действие адресуется ячейками,
	 *  а не номерами предметов, поэтому содержимое сумки нужно знать заранее;
	 *  оно приходит внутри входа в карту. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	ECorsairsActionRequestResult EquipItem(
		int64 FromGrid, int64 ToSlot);

	/** Поднимает лежащий на земле предмет. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	ECorsairsActionRequestResult PickUpItem(
		int64 ItemWorldId, int64 ItemHandle);

	/** Начинает разговор с NPC.
	 *
	 *  Сервер ищет NPC вокруг положения персонажа, а не по полю зрения: с
	 *  дальней дистанции запрос не найдёт цели и останется без ответа. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool TalkToNpc(int64 NpcWorldId);

	/** Открывает у торговца страницу с товарами.
	 *
	 *  Сделке это обязано предшествовать: она ссылается на уже начатый
	 *  разговор, и без открытой лавки торговец её не рассматривает. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool OpenNpcPage(int64 NpcWorldId, int64 Page, int64 Item);

	/** Продаёт торговцу вещь из ячейки сумки.
	 *
	 *  Сделке должно предшествовать открытие лавки: она ссылается на уже
	 *  начатый разговор. */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool SellItemToNpc(int64 NpcWorldId, int64 Grid, int64 Count);

	/** Отправляет реплику в чат. GM-команды идут туда же с префиксом «&». */
	UFUNCTION(BlueprintCallable, Category = "Corsairs")
	bool Say(const FString& Text);

	/** Характеристики персонажа по номеру атрибута (ChaAttrType.h).
	 *
	 *  Пополняются и обновлениями характеристик, и результатами ударов:
	 *  опыт с расходом маны приходят внутри уведомления о действии. */
	UFUNCTION(BlueprintPure, Category = "Corsairs")
	TMap<int64, int64> GetAttributes() const { return Attributes; }

	/** Содержимое сумки: номер ячейки → номер предмета. */
	UFUNCTION(BlueprintPure, Category = "Corsairs")
	TMap<int64, int64> GetKitbag() const { return Kitbag; }

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

#if !UE_BUILD_SHIPPING
	int64 GetMovementBeginSendCountForDiagnostics() const;
	bool HasPendingMoveForDiagnostics() const;
#endif

#if WITH_DEV_AUTOMATION_TESTS
	void SetSendOverrideForTests(
		TFunction<bool(Corsairs::Net::WPacket&)> Override);
	void SetInWorldForTests(
		int64 InWorldId, FIntPoint Spawn);
	void SetInWorldAndBroadcastForTests(
		const FCorsairsWorldActor& InLocalActor,
		const FString& InMapName);
	void SetMovementSpeedForTests(int64 Speed);
	void AddVisibleActorForTests(
		const FCorsairsWorldActor& Actor);
	void SetMovementAuthorityObserverForTests(
		TFunction<void(bool, int64)> Observer);
	void HandlePacketForTests(
		Corsairs::Net::RPacket& Packet);
	void HandleConnectionStateForTests(
		ECorsairsConnectionState NewState,
		const FString& Reason);
	void SetEventObserversForTests(
		TFunction<void(const FCorsairsMovementEvent&)> MovementObserver,
		TFunction<void(const FString&)> ProtocolErrorObserver,
		TFunction<void(ECorsairsLoginStage)> StageObserver);
#endif

private:
	void HandlePacket(Corsairs::Net::RPacket& Packet);
	void HandleConnectionState(ECorsairsConnectionState NewState, const FString& Reason);

	void SendLogin();
	void SendCreatePassword2();

	void SetStage(ECorsairsLoginStage NewStage, const FString& Message);
	ECorsairsActionRequestResult SendMoveFromConfirmed(
		FIntPoint Endpoint);
	bool SendBeginActionPacket(
		Corsairs::Net::WPacket& Packet);
	bool CanSendBeginActionPacket() const;
	void ApplyReducerEffects(
		const FCorsairsReducerEffects& Effects);
	void PublishMovementAuthorityIfChanged();

	UPROPERTY()
	TObjectPtr<UCorsairsConnection> Connection;

	ECorsairsLoginStage Stage = ECorsairsLoginStage::Idle;
	TArray<FCorsairsCharacterSlot> Characters;

	int64 WorldId = 0;
	FCorsairsWorldActor LocalActor;
	FIntPoint SpawnPosition = FIntPoint::ZeroValue;
	FString MapName;

	/** Номер пакета действия. Сервер отслеживает порядок команд по нему и
	 *  отбрасывает устаревшие. */
	int64 ActionPacketId = 0;
	FCorsairsActionReducer ActionReducer;
	uint64 ActionReducerGeneration = 0;
	int64 MovementBeginSendCount = 0;
	bool bPublishedMovementAuthorityLocked = false;
	int64 MovementAuthorityEpoch = 0;

	TMap<int32, int32> ReceivedCommands;
	TArray<FCorsairsWorldActor> VisibleActors;

	/** Характеристики персонажа и содержимое сумки — состояние, без которого
	 *  не собрать ни удар, ни экипировку. */
	TMap<int64, int64> Attributes;
	TMap<int64, int64> Kitbag;

	/** Ищет цель в поле зрения. Удар адресуется парой «идентификатор +
	 *  handle», и handle берётся отсюда: у сущностей вроде NPC старший бит
	 *  идентификатора установлен, и одного его серверу мало. */
	const FCorsairsWorldActor* FindActor(int64 TargetWorldId) const;
	FCorsairsWorldActor* FindMutableActor(int64 TargetWorldId);

	FString PendingAccount;
	FString PendingPasswordHash;
	bool bHasPassword2 = false;

#if WITH_DEV_AUTOMATION_TESTS
	TFunction<bool(Corsairs::Net::WPacket&)> TestSendOverride;
	TFunction<void(const FCorsairsMovementEvent&)> TestMovementObserver;
	TFunction<void(const FString&)> TestProtocolErrorObserver;
	TFunction<void(ECorsairsLoginStage)> TestStageObserver;
	TFunction<void(bool, int64)> TestMovementAuthorityObserver;
#endif
};
