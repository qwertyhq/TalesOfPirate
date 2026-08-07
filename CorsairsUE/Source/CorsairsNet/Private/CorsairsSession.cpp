#include "CorsairsSession.h"

#include "common/src/Crypto/Blake2s.h"
#include "common/src/Network/NetCommand.h"

#include "CorsairsNet/include/CommandMessages.h"
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

bool UCorsairsSession::SendMovePath(const TArray<FIntPoint>& Path)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::InWorld)
	{
		return false;
	}
	if (Path.Num() < 2)
	{
		// Путь из одной точки сервер трактует как отсутствие движения.
		return false;
	}

	// Точки укладываются в двоичный блок ровно так, как их читает сервер:
	// пара 32-битных чисел на точку, без выравнивания. Формат задан
	// структурой Corsairs::Util::Point, которую сервер копирует напрямую.
	TArray<uint8> Blob;
	Blob.Reserve(Path.Num() * 2 * sizeof(int32));
	for (const FIntPoint& Point : Path)
	{
		const int32 Coordinates[2] = {Point.X, Point.Y};
		Blob.Append(reinterpret_cast<const uint8*>(Coordinates), sizeof(Coordinates));
	}

	WPacket Packet(64 + Blob.Num());
	Packet.WriteCmd(CMD_CM_BEGINACTION);
	Packet.WriteInt64(WorldId);
	Packet.WriteInt64(++ActionPacketId);
	Packet.WriteInt64(Corsairs::Net::Msg::ActionType::MOVE);
	Packet.WriteSequence(Blob.GetData(), static_cast<uint16>(Blob.Num()));

	return Connection->Send(Packet);
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
	ReceivedCommands.FindOrAdd(static_cast<int32>(Cmd)) += 1;

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
		// Разбор готовой функцией из CommandMessages: структура входа в карту
		// насчитывает больше сотни полей, включая сумку, навыки и лодки, и
		// разбирать её вручную значит завести вторую копию раскладки, которая
		// разойдётся с серверной при первом же изменении.
		Corsairs::Net::Msg::McEnterMapMessage Message;
		Corsairs::Net::Msg::deserialize(Packet, Message);

		if (Message.errCode != 0 || !Message.data.has_value())
		{
			SetStage(ECorsairsLoginStage::Failed,
					 FString::Printf(TEXT("вход в карту отклонён, код %lld"), Message.errCode));
			return;
		}

		const auto& Data = Message.data.value();
		WorldId = Data.baseInfo.worldId;
		SpawnPosition = FIntPoint(static_cast<int32>(Data.baseInfo.posX),
								  static_cast<int32>(Data.baseInfo.posY));
		MapName = ToFString(Data.mapName);

		SetStage(ECorsairsLoginStage::InWorld,
				 FString::Printf(TEXT("карта %s, позиция (%d, %d)"),
								 *MapName, SpawnPosition.X, SpawnPosition.Y));
		return;
	}

	if (Cmd == CMD_MC_CHABEGINSEE)
	{
		// Сервер сам решает, кто попадает в поле зрения, и присылает это
		// сообщение для игроков, NPC и монстров одинаково. Именно так мир и
		// населяется: запекать NPC в уровень не нужно и неверно — они живут
		// на сервере и могут двигаться, исчезать и появляться.
		Corsairs::Net::Msg::McChaBeginSeeMessage Message;
		Corsairs::Net::Msg::deserialize(Packet, Message);

		FCorsairsWorldActor Actor;
		Actor.WorldId = Message.base.worldId;
		Actor.Name = ToFString(Message.base.name);
		Actor.Position = FIntPoint(static_cast<int32>(Message.base.posX),
								   static_cast<int32>(Message.base.posY));
		Actor.Angle = static_cast<int32>(Message.base.angle);
		// Тип модели лежит в сведениях о внешности, а не в commId: последний —
		// идентификатор сообщества, и в нём приходят отрицательные значения.
		Actor.TypeId = static_cast<int32>(Message.base.look.typeId);
		Actor.CtrlType = static_cast<int32>(Message.base.ctrlType);
		Actor.ChaId = static_cast<int32>(Message.base.chaId);

		// У игроков модель задаёт внешность, у NPC — запись в таблице
		// персонажей: поле внешности у них не заполняется.
		if (Actor.TypeId == 0)
		{
			Actor.TypeId = Actor.ChaId;
		}

		VisibleActors.Add(Actor);
		OnActorSeen.Broadcast(Actor);
		return;
	}

	if (Cmd == CMD_MC_CHAENDSEE)
	{
		const int64 WorldIdLeft = Packet.ReadInt64();
		VisibleActors.RemoveAll([WorldIdLeft](const FCorsairsWorldActor& A)
								{ return A.WorldId == WorldIdLeft; });
		OnActorLeft.Broadcast(WorldIdLeft);
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
