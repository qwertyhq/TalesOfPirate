#include "CorsairsSession.h"

#include "CorsairsLookAdapter.h"
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

	/** Номер атрибута здоровья (ChaAttrType.h). Массив атрибутов плотный, без
	 *  дыр, поэтому номера заданы числами. */
	constexpr int64 kAttrHp = 1;

	/** Действия внутри разговора с NPC (BuildNpcActionTable в NpcScript.cpp).
	 *  Сервер читает код действия вторым полем и по нему выбирает ветку
	 *  скрипта; без кода скрипт разбирает мусор и молчит. */
	constexpr int64 kNpcActionTalkPage = 302;
	constexpr int64 kNpcActionFuncItem = 303;
	constexpr int64 kNpcActionTradeItem = 309;

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
	LocalActor = FCorsairsWorldActor{};
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

const FCorsairsWorldActor* UCorsairsSession::FindActor(int64 TargetWorldId) const
{
	return VisibleActors.FindByPredicate(
		[TargetWorldId](const FCorsairsWorldActor& A) { return A.WorldId == TargetWorldId; });
}

bool UCorsairsSession::UseSkillOn(int64 SkillId, int64 TargetWorldId)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::InWorld)
	{
		return false;
	}
	const FCorsairsWorldActor* Target = FindActor(TargetWorldId);
	if (Target == nullptr)
	{
		// Цели нет в поле зрения — сервер всё равно ответил бы отказом
		// «цели не существует», и разбирать его пришлось бы вслепую.
		return false;
	}

	// Путь ведёт от персонажа к цели. Сервер сам проводит по нему персонажа и
	// бьёт по прибытии; путь из одной точки он не разыгрывает вовсе.
	const int32 Coordinates[4] = {
		SpawnPosition.X, SpawnPosition.Y,
		Target->Position.X, Target->Position.Y };
	TArray<uint8> Blob;
	Blob.Append(reinterpret_cast<const uint8*>(Coordinates), sizeof(Coordinates));

	WPacket Packet(128 + Blob.Num());
	Packet.WriteCmd(CMD_CM_BEGINACTION);
	Packet.WriteInt64(WorldId);
	Packet.WriteInt64(++ActionPacketId);
	Packet.WriteInt64(Corsairs::Net::Msg::ActionType::SKILL);
	// Признак движения строго 2 — «подойти и ударить». С нулём сервер молчит.
	Packet.WriteInt64(2);
	Packet.WriteInt64(ActionPacketId);
	Packet.WriteSequence(Blob.GetData(), static_cast<uint16>(Blob.Num()));
	Packet.WriteInt64(SkillId);
	Packet.WriteInt64(Target->WorldId);
	Packet.WriteInt64(Target->Handle);

	return Connection->Send(Packet);
}

bool UCorsairsSession::EquipItem(int64 FromGrid, int64 ToSlot)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::InWorld)
	{
		return false;
	}
	WPacket Packet(64);
	Packet.WriteCmd(CMD_CM_BEGINACTION);
	Packet.WriteInt64(WorldId);
	Packet.WriteInt64(++ActionPacketId);
	Packet.WriteInt64(Corsairs::Net::Msg::ActionType::ITEM_USE);
	Packet.WriteInt64(FromGrid);
	Packet.WriteInt64(ToSlot);
	return Connection->Send(Packet);
}

bool UCorsairsSession::PickUpItem(int64 ItemWorldId, int64 ItemHandle)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::InWorld)
	{
		return false;
	}
	WPacket Packet(64);
	Packet.WriteCmd(CMD_CM_BEGINACTION);
	Packet.WriteInt64(WorldId);
	Packet.WriteInt64(++ActionPacketId);
	Packet.WriteInt64(Corsairs::Net::Msg::ActionType::ITEM_PICK);
	Packet.WriteInt64(ItemWorldId);
	Packet.WriteInt64(ItemHandle);
	return Connection->Send(Packet);
}

bool UCorsairsSession::TalkToNpc(int64 NpcWorldId)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::InWorld)
	{
		return false;
	}
	WPacket Packet(64);
	Packet.WriteCmd(CMD_CM_REQUESTNPC);
	Packet.WriteInt64(NpcWorldId);
	// Код действия и номер страницы обязательны: без них скрипт NPC разберёт
	// мусор и промолчит.
	Packet.WriteInt64(kNpcActionTalkPage);
	Packet.WriteInt64(0);
	return Connection->Send(Packet);
}

bool UCorsairsSession::OpenNpcPage(int64 NpcWorldId, int64 Page, int64 Item)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::InWorld)
	{
		return false;
	}
	WPacket Packet(64);
	Packet.WriteCmd(CMD_CM_REQUESTNPC);
	Packet.WriteInt64(NpcWorldId);
	Packet.WriteInt64(kNpcActionFuncItem);
	Packet.WriteInt64(Page);
	Packet.WriteInt64(Item);
	return Connection->Send(Packet);
}

bool UCorsairsSession::SellItemToNpc(int64 NpcWorldId, int64 Grid, int64 Count)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::InWorld)
	{
		return false;
	}
	WPacket Packet(96);
	Packet.WriteCmd(CMD_CM_REQUESTNPC);
	Packet.WriteInt64(NpcWorldId);
	Packet.WriteInt64(kNpcActionTradeItem);
	Packet.WriteInt64(0);              // ROLE_TRADE_SALE
	Packet.WriteInt64(Grid);
	Packet.WriteInt64(Count);
	return Connection->Send(Packet);
}

bool UCorsairsSession::Say(const FString& Text)
{
	if (Connection == nullptr || Stage != ECorsairsLoginStage::InWorld)
	{
		return false;
	}
	WPacket Packet(64 + Text.Len() * 2);
	Packet.WriteCmd(CMD_CM_SAY);
	Packet.WriteString(TCHAR_TO_UTF8(*Text));
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
	LocalActor = FCorsairsWorldActor{};
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
		LocalActor.WorldId = Data.baseInfo.worldId;
		LocalActor.Name = ToFString(Data.baseInfo.name);
		LocalActor.Position = FIntPoint(
			static_cast<int32>(Data.baseInfo.posX),
			static_cast<int32>(Data.baseInfo.posY));
		LocalActor.Angle = static_cast<int32>(Data.baseInfo.angle);
		LocalActor.TypeId = static_cast<int32>(Data.baseInfo.look.typeId);
		LocalActor.CtrlType = static_cast<int32>(Data.baseInfo.ctrlType);
		LocalActor.ChaId = static_cast<int32>(Data.baseInfo.chaId);
		LocalActor.Handle = Data.baseInfo.handle;
		LocalActor.Look = MakeCharacterLook(Data.baseInfo.look);

		// Сумка и характеристики приходят вместе со входом. Без содержимого
		// сумки нечем надеть оружие, а без него сервер подтвердит удар, но
		// разыгрывать его не станет.
		Kitbag.Reset();
		for (const auto& Item : Data.kitbag.items)
		{
			if (Item.itemId > 0)
			{
				Kitbag.Add(Item.gridId, Item.itemId);
			}
		}
		Attributes.Reset();
		for (const auto& Entry : Data.attr.attrs)
		{
			Attributes.Add(Entry.attrId, Entry.attrVal);
			if (Entry.attrId == kAttrHp)
			{
				LocalActor.Hp = Entry.attrVal;
			}
		}
		WorldId = LocalActor.WorldId;
		SpawnPosition = LocalActor.Position;
		MapName = ToFString(Data.mapName);

		SetStage(ECorsairsLoginStage::InWorld,
				 FString::Printf(TEXT("карта %s, позиция (%d, %d)"),
								 *MapName, SpawnPosition.X, SpawnPosition.Y));
		return;
	}

	if (Cmd == CMD_MC_NOTIACTION)
	{
		// Итог удара приходит именно здесь, а не отдельным сообщением о
		// характеристиках: новое здоровье цели лежит в перечне изменений
		// действия, а опыт и расход бьющего — в наборе для источника.
		Corsairs::Net::Msg::McCharacterActionMessage Message;
		Corsairs::Net::Msg::deserialize(Packet, Message);

		if (const auto* Look =
				std::get_if<Corsairs::Net::Msg::ChaLookInfo>(&Message.data))
		{
			const FCorsairsCharacterLook Converted = MakeCharacterLook(*Look);
			if (Message.worldId == WorldId)
			{
				LocalActor.Look = Converted;
				LocalActor.TypeId = Converted.TypeId;
			}
			else if (FCorsairsWorldActor* Actor = VisibleActors.FindByPredicate(
						 [&Message](const FCorsairsWorldActor& Candidate)
						 { return Candidate.WorldId == Message.worldId; }))
			{
				Actor->Look = Converted;
				Actor->TypeId = Converted.TypeId;
			}
			OnActorLookChanged.Broadcast(Message.worldId, Converted);
			return;
		}

		if (const auto* TarData =
				std::get_if<Corsairs::Net::Msg::ActionSkillTarData>(&Message.data))
		{
			if (FCorsairsWorldActor* Actor = VisibleActors.FindByPredicate(
					[&Message](const FCorsairsWorldActor& A)
					{ return A.WorldId == Message.worldId; }))
			{
				for (const auto& Entry : TarData->effects)
				{
					if (Entry.attrId == kAttrHp)
					{
						Actor->Hp = Entry.attrVal;
						break;
					}
				}
			}
			if (TarData->srcId == WorldId)
			{
				for (const auto& Entry : TarData->srcEffects)
				{
					Attributes.Add(Entry.attrId, Entry.attrVal);
				}
			}
		}
		return;
	}

	if (Cmd == CMD_MC_SYNATTR)
	{
		Corsairs::Net::Msg::McSynAttributeMessage Message;
		Corsairs::Net::Msg::deserialize(Packet, Message);
		if (Message.worldId == WorldId)
		{
			for (const auto& Entry : Message.attr.attrs)
			{
				Attributes.Add(Entry.attrId, Entry.attrVal);
			}
		}
		else if (FCorsairsWorldActor* Actor = VisibleActors.FindByPredicate(
					 [&Message](const FCorsairsWorldActor& A)
					 { return A.WorldId == Message.worldId; }))
		{
			for (const auto& Entry : Message.attr.attrs)
			{
				if (Entry.attrId == kAttrHp)
				{
					Actor->Hp = Entry.attrVal;
					break;
				}
			}
		}
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
		Actor.Look = MakeCharacterLook(Message.base.look);
		Actor.TypeId = Actor.Look.TypeId;
		Actor.CtrlType = static_cast<int32>(Message.base.ctrlType);
		Actor.ChaId = static_cast<int32>(Message.base.chaId);
		Actor.Handle = Message.base.handle;
		for (const auto& Entry : Message.attr.attrs)
		{
			if (Entry.attrId == kAttrHp)
			{
				Actor.Hp = Entry.attrVal;
				break;
			}
		}

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
