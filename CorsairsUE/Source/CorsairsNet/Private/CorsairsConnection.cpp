#include "CorsairsConnection.h"

#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "CorsairsNet/include/Packet.h"

using Corsairs::Net::RPacket;
using Corsairs::Net::WPacket;

namespace
{
	/** Заголовок пакета: размер (uint16), счётчик (uint32), команда (uint16). */
	constexpr int32 HeaderSize = 8;

	/** Сколько байт вычитывать за один вызов recv. */
	constexpr int32 ChunkSize = 64 * 1024;

	/** Размер в заголовке — это длина всего пакета вместе с заголовком.
	 *  Порядок байтов сетевой, как и во всём протоколе. */
	uint16 ReadPacketSize(const uint8* Data)
	{
		return static_cast<uint16>((Data[0] << 8) | Data[1]);
	}
}

bool UCorsairsConnection::Connect(const FString& Host, int32 Port)
{
	Disconnect();

	ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Subsystem == nullptr)
	{
		SetState(ECorsairsConnectionState::Failed, TEXT("подсистема сокетов недоступна"));
		return false;
	}

	TSharedRef<FInternetAddr> Address = Subsystem->CreateInternetAddr();
	bool bAddressValid = false;
	Address->SetIp(*Host, bAddressValid);
	if (!bAddressValid)
	{
		// Имя вместо адреса: пробуем разрешить через подсистему.
		const ESocketErrors Result = Subsystem->GetHostByName(TCHAR_TO_ANSI(*Host), *Address);
		if (Result != SE_NO_ERROR)
		{
			SetState(ECorsairsConnectionState::Failed,
					 FString::Printf(TEXT("не разрешается адрес %s"), *Host));
			return false;
		}
	}
	Address->SetPort(Port);

	Socket = FTcpSocketBuilder(TEXT("CorsairsClient"))
				 .AsBlocking()
				 .WithReceiveBufferSize(ChunkSize)
				 .Build();
	if (Socket == nullptr)
	{
		SetState(ECorsairsConnectionState::Failed, TEXT("сокет не создался"));
		return false;
	}

	SetState(ECorsairsConnectionState::Connecting, FString());

	if (!Socket->Connect(*Address))
	{
		Subsystem->DestroySocket(Socket);
		Socket = nullptr;
		SetState(ECorsairsConnectionState::Failed,
				 FString::Printf(TEXT("не удалось подключиться к %s:%d"), *Host, Port));
		return false;
	}

	// Дальше сокет работает без блокировок: приём идёт в Tick, и ожидание
	// данных остановило бы игровой поток.
	Socket->SetNonBlocking(true);

	SendCounter = 0;
	Incoming.Reset();
	SetState(ECorsairsConnectionState::Connected, FString());
	return true;
}

void UCorsairsConnection::Disconnect()
{
	if (Socket != nullptr)
	{
		Socket->Close();
		if (ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			Subsystem->DestroySocket(Socket);
		}
		Socket = nullptr;
	}

	Incoming.Reset();
	SendCounter = 0;

	if (State != ECorsairsConnectionState::Disconnected)
	{
		SetState(ECorsairsConnectionState::Disconnected, FString());
	}
}

bool UCorsairsConnection::Send(WPacket& Packet)
{
	if (Socket == nullptr || State != ECorsairsConnectionState::Connected)
	{
		return false;
	}

	Packet.WriteSess(SendCounter);

	const uint8* Data = Packet.Data();
	const int32 Size = Packet.GetPacketSize();

	int32 TotalSent = 0;
	while (TotalSent < Size)
	{
		int32 Sent = 0;
		if (!Socket->Send(Data + TotalSent, Size - TotalSent, Sent) || Sent <= 0)
		{
			SetState(ECorsairsConnectionState::Failed, TEXT("отправка не удалась"));
			return false;
		}
		TotalSent += Sent;
	}

	++SendCounter;
	return true;
}

void UCorsairsConnection::Tick(float DeltaTime)
{
	if (Socket == nullptr)
	{
		return;
	}

	if (!ReceiveAvailable())
	{
		Disconnect();
		return;
	}

	DispatchComplete();
}

TStatId UCorsairsConnection::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCorsairsConnection, STATGROUP_Tickables);
}

void UCorsairsConnection::BeginDestroy()
{
	Disconnect();
	Super::BeginDestroy();
}

void UCorsairsConnection::SetState(ECorsairsConnectionState NewState, const FString& Reason)
{
	State = NewState;
	OnStateChanged.Broadcast(NewState, Reason);
}

bool UCorsairsConnection::ReceiveAvailable()
{
	uint8 Buffer[ChunkSize];

	for (;;)
	{
		uint32 Pending = 0;
		if (!Socket->HasPendingData(Pending) || Pending == 0)
		{
			// Данных нет — это норма для неблокирующего сокета, а не разрыв.
			return true;
		}

		int32 Read = 0;
		if (!Socket->Recv(Buffer, FMath::Min<int32>(ChunkSize, static_cast<int32>(Pending)), Read))
		{
			return false;
		}
		if (Read <= 0)
		{
			// Ноль байт при наличии ожидающих данных означает закрытие с той
			// стороны.
			return false;
		}

		Incoming.Append(Buffer, Read);
	}
}

void UCorsairsConnection::DispatchComplete()
{
	int32 Offset = 0;

	while (Incoming.Num() - Offset >= HeaderSize)
	{
		const uint16 PacketSize = ReadPacketSize(Incoming.GetData() + Offset);
		if (PacketSize < HeaderSize)
		{
			// Заявленный размер меньше заголовка — поток рассинхронизирован, и
			// продолжать разбор бессмысленно: дальше пойдёт мусор.
			SetState(ECorsairsConnectionState::Failed,
					 FString::Printf(TEXT("размер пакета %d меньше заголовка"), PacketSize));
			Incoming.Reset();
			return;
		}
		if (Incoming.Num() - Offset < PacketSize)
		{
			// Пакет пришёл не целиком, ждём остаток.
			break;
		}

		// RPacket не владеет буфером: он живёт до конца обработчика, а память
		// принадлежит накопителю.
		RPacket Packet(Incoming.GetData() + Offset, PacketSize, false);
		OnPacket.Broadcast(Packet);

		Offset += PacketSize;
	}

	if (Offset > 0)
	{
		Incoming.RemoveAt(0, Offset, EAllowShrinking::No);
	}
}
