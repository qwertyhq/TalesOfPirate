#include "CorsairsSession.h"

#include "common/src/Crypto/Blake2s.h"
#include "common/src/Network/NetCommand.h"

#include "CorsairsNet/include/Packet.h"

using Corsairs::Net::RPacket;
using Corsairs::Net::WPacket;

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsSession, Log, All);

namespace
{
	/** Версия клиента. Gate сверяет её с настройкой ClientVersion и отвергает
	 *  вход при несовпадении. */
	constexpr int64 ClientVersion = 32125;

	/** Сколько ячеек снаряжения перечислено для каждого персонажа в ответе на
	 *  вход. Значение зафиксировано протоколом. */
	constexpr int32 EquipSlotNum = 34;

	FString ToFString(const std::string& Value)
	{
		return FString(UTF8_TO_TCHAR(Value.c_str()));
	}
}

void UCorsairsSession::Login(const FString& Host, int32 Port,
							 const FString& Account, const FString& Password)
{
	Logout();

	Characters.Reset();
	bHasPassword2 = false;
	PendingAccount = Account;

	// Сервер хранит и сравнивает хеш как строку, поэтому клиент передаёт
	// шестнадцатеричное представление, а не сам пароль.
	const std::string PasswordUtf8(TCHAR_TO_UTF8(*Password));
	PendingPasswordHash = ToFString(Corsairs::Common::Crypto::Blake2sHex(PasswordUtf8));

	Connection = NewObject<UCorsairsConnection>(this);
	Connection->OnPacket.AddUObject(this, &UCorsairsSession::HandlePacket);
	Connection->OnStateChanged.AddUObject(this, &UCorsairsSession::HandleConnectionState);

	SetStage(ECorsairsLoginStage::Connecting,
			 FString::Printf(TEXT("подключение к %s:%d"), *Host, Port));

	if (!Connection->Connect(Host, Port))
	{
		// Причина уже пришла через HandleConnectionState.
		return;
	}
}

void UCorsairsSession::EnterWorld(int32 SlotIndex)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::SelectingCha)
	{
		return;
	}
	if (!Characters.IsValidIndex(SlotIndex) || !Characters[SlotIndex].Valid)
	{
		SetStage(ECorsairsLoginStage::Failed,
				 FString::Printf(TEXT("в ячейке %d нет персонажа"), SlotIndex));
		return;
	}

	WPacket Packet(32);
	Packet.WriteCmd(CMD_CM_BGNPLAY);
	Packet.WriteInt64(SlotIndex);
	Connection->Send(Packet);

	SetStage(ECorsairsLoginStage::EnteringWorld,
			 FString::Printf(TEXT("вход персонажем %s"), *Characters[SlotIndex].Name));
}

void UCorsairsSession::Poll()
{
	if (Connection != nullptr)
	{
		Connection->Poll();
	}
}

void UCorsairsSession::Logout()
{
	if (Connection != nullptr)
	{
		Connection->Disconnect();
		Connection = nullptr;
	}
	if (Stage != ECorsairsLoginStage::Idle)
	{
		SetStage(ECorsairsLoginStage::Idle, FString());
	}
}

void UCorsairsSession::HandleConnectionState(ECorsairsConnectionState NewState,
											 const FString& Reason)
{
	switch (NewState)
	{
	case ECorsairsConnectionState::Failed:
		SetStage(ECorsairsLoginStage::Failed, Reason);
		break;

	case ECorsairsConnectionState::Disconnected:
		// Разрыв после входа в мир — это выход из игры, а не сбой.
		if (Stage != ECorsairsLoginStage::Idle && Stage != ECorsairsLoginStage::Failed)
		{
			SetStage(ECorsairsLoginStage::Failed, TEXT("соединение закрыто сервером"));
		}
		break;

	default:
		break;
	}
}

void UCorsairsSession::SendLogin()
{
	WPacket Packet(256);
	Packet.WriteCmd(CMD_CM_LOGIN);
	Packet.WriteString(std::string(TCHAR_TO_UTF8(*PendingAccount)));
	Packet.WriteString(std::string(TCHAR_TO_UTF8(*PendingPasswordHash)));
	// MAC сервер только записывает в журнал.
	Packet.WriteString(std::string("00-00-00-00-00-00"));
	Packet.WriteInt64(0);                 // CheatMarker
	Packet.WriteInt64(ClientVersion);
	Connection->Send(Packet);

	SetStage(ECorsairsLoginStage::Authenticating, TEXT("проверка учётной записи"));
}

void UCorsairsSession::SendCreatePassword2()
{
	WPacket Packet(64);
	Packet.WriteCmd(CMD_CM_CREATE_PASSWORD2);
	Packet.WriteString(std::string(TCHAR_TO_UTF8(*Password2)));
	Connection->Send(Packet);
}

void UCorsairsSession::HandlePacket(RPacket& Packet)
{
	const uint16 Cmd = Packet.GetCmd();

	// Рукопожатие. При выключенном RSA-AES оно пустое, но приходит всегда, и
	// именно оно означает готовность сервера принимать команды.
	if (Cmd == CMD_MC_SEND_SERVER_PUBLIC_KEY)
	{
		SendLogin();
		return;
	}

	if (Cmd == CMD_MC_LOGIN)
	{
		const int64 ErrCode = Packet.ReadInt64();
		if (ErrCode != 0)
		{
			SetStage(ECorsairsLoginStage::Failed,
					 FString::Printf(TEXT("вход отклонён, код %lld"), ErrCode));
			return;
		}

		Packet.ReadInt64();                              // maxChaNum
		const int64 Count = Packet.ReadInt64();

		Characters.Reset();
		for (int64 i = 0; i < Count; ++i)
		{
			FCorsairsCharacterSlot Slot;
			Slot.Valid = Packet.ReadInt64() != 0;
			if (Slot.Valid)
			{
				Slot.Name = ToFString(Packet.ReadString());
				Slot.Job = ToFString(Packet.ReadString());
				Slot.Level = static_cast<int32>(Packet.ReadInt64());
				Slot.TypeId = static_cast<int32>(Packet.ReadInt64());
				for (int32 e = 0; e < EquipSlotNum; ++e)
				{
					Packet.ReadInt64();                  // идентификаторы снаряжения
				}
			}
			Characters.Add(Slot);
		}
		bHasPassword2 = Packet.ReadInt64() != 0;

		// Второй пароль обязателен: GroupServer отвергает вход в мир, пока он
		// пуст, и сообщает об этом кодом ERR_PT_INVALID_PW2.
		if (!bHasPassword2)
		{
			SendCreatePassword2();
			return;
		}

		SetStage(ECorsairsLoginStage::SelectingCha,
				 FString::Printf(TEXT("персонажей: %d"), Characters.Num()));
		return;
	}

	if (Cmd == CMD_MC_CREATE_PASSWORD2)
	{
		const int64 ErrCode = Packet.ReadInt64();
		if (ErrCode != 0)
		{
			SetStage(ECorsairsLoginStage::Failed,
					 FString::Printf(TEXT("второй пароль отклонён, код %lld"), ErrCode));
			return;
		}
		bHasPassword2 = true;
		SetStage(ECorsairsLoginStage::SelectingCha,
				 FString::Printf(TEXT("персонажей: %d"), Characters.Num()));
		return;
	}

	if (Cmd == CMD_MC_BGNPLAY)
	{
		const int64 ErrCode = Packet.ReadInt64();
		if (ErrCode != 0)
		{
			SetStage(ECorsairsLoginStage::Failed,
					 FString::Printf(TEXT("вход в мир отклонён, код %lld"), ErrCode));
		}
		// При успехе ждём ENTERMAP: подтверждение приходит уже от GameServer.
		return;
	}

	if (Cmd == CMD_MC_ENTERMAP)
	{
		SetStage(ECorsairsLoginStage::InWorld, TEXT("в мире"));
		return;
	}

	UE_LOG(LogCorsairsSession, Verbose, TEXT("необработанная команда %u"), Cmd);
}

void UCorsairsSession::SetStage(ECorsairsLoginStage NewStage, const FString& Message)
{
	Stage = NewStage;
	UE_LOG(LogCorsairsSession, Log, TEXT("стадия %d: %s"),
		   static_cast<int32>(NewStage), *Message);
	OnStageChanged.Broadcast(NewStage, Message);
}
