#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "UObject/Object.h"

#include "CorsairsConnection.generated.h"

class FSocket;

// Классы протокола живут в Corsairs::Net. Объявление их в глобальной области
// создаёт другой тип, и компилятор сообщает о неполном типе там, где всё
// подключено.
namespace Corsairs::Net
{
	class WPacket;
	class RPacket;
}

/**
 * Состояние соединения с GateServer.
 */
UENUM(BlueprintType)
enum class ECorsairsConnectionState : uint8
{
	Disconnected UMETA(DisplayName = "Отключён"),
	Connecting   UMETA(DisplayName = "Подключается"),
	Connected    UMETA(DisplayName = "Подключён"),
	Failed       UMETA(DisplayName = "Ошибка"),
};

/** Пакет от сервера. Обработчик обязан прочитать его до возврата: буфер
 *  переиспользуется. */
DECLARE_MULTICAST_DELEGATE_OneParam(FCorsairsPacketReceived, Corsairs::Net::RPacket&);

/** Смена состояния соединения; строка описывает причину при ошибке. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FCorsairsStateChanged, ECorsairsConnectionState, const FString&);

/**
 * Клиентское соединение с GateServer поверх FSocket.
 *
 * Транспорт написан заново: реализация из sources/Libraries/CorsairsNet живёт
 * на сырых сокетах и собственных потоках, что в UE неуместно. Сам протокол —
 * WPacket и RPacket из той же библиотеки — переиспользуется дословно, поэтому
 * раскладка байтов совпадает с серверами по построению, а не по договорённости.
 *
 * Приём идёт в Tick: сокет неблокирующий, за кадр вычитывается всё готовое.
 * Отдельный поток не нужен — трафик клиента невелик, а обработка пакетов всё
 * равно должна попадать в игровой поток.
 */
UCLASS()
class CORSAIRSNET_API UCorsairsConnection : public UObject, public FTickableGameObject
{
	GENERATED_BODY()

public:
	/** Подключается к серверу. Возвращает false, если сокет не создался или
	 *  адрес не разобрался; сетевые ошибки приходят через OnStateChanged. */
	bool Connect(const FString& Host, int32 Port);

	void Disconnect();

	/** Отправляет пакет, проставив счётчик защиты WPE.
	 *
	 *  Gate сверяет поле Sess со своим счётчиком и разрывает соединение при
	 *  расхождении, поэтому нумерацию ведёт само соединение, а не вызывающий. */
	bool Send(Corsairs::Net::WPacket& Packet);

	ECorsairsConnectionState GetState() const { return State; }

	FCorsairsPacketReceived OnPacket;
	FCorsairsStateChanged OnStateChanged;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return Socket != nullptr; }
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }

	virtual void BeginDestroy() override;

private:
	void SetState(ECorsairsConnectionState NewState, const FString& Reason);

	/** Вычитывает готовые данные в буфер. false — соединение закрылось. */
	bool ReceiveAvailable();

	/** Разбирает из буфера все целые пакеты. */
	void DispatchComplete();

	FSocket* Socket = nullptr;
	ECorsairsConnectionState State = ECorsairsConnectionState::Disconnected;

	/** Счётчик защиты WPE. Начинается с нуля и растёт на каждый принятый
	 *  сервером пакет. */
	uint32 SendCounter = 0;

	/** Накопитель входящих: TCP не сохраняет границы сообщений, и пакет может
	 *  прийти по частям либо несколько сразу. */
	TArray<uint8> Incoming;
};
