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
	constexpr int64 kAttrMovementSpeed = 44;
	constexpr double kMovementReportDistanceCm = 100.0;

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

	bool IsBelowMovementReportDistance(
		const FIntPoint From,
		const FIntPoint To)
	{
		const double DeltaX = static_cast<double>(To.X) - From.X;
		const double DeltaY = static_cast<double>(To.Y) - From.Y;
		return DeltaX * DeltaX + DeltaY * DeltaY <
			kMovementReportDistanceCm * kMovementReportDistanceCm;
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

ECorsairsActionRequestResult UCorsairsSession::SendMovePath(
	const TArray<FIntPoint>& Path)
{
	if (Stage != ECorsairsLoginStage::InWorld)
	{
		return ECorsairsActionRequestResult::Invalid;
	}
	if (!CanSendBeginActionPacket())
	{
		return ECorsairsActionRequestResult::TransportFailed;
	}
	if (Path.Num() < 2)
	{
		// Путь из одной точки сервер трактует как отсутствие движения.
		return ECorsairsActionRequestResult::Invalid;
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
	const int64 PacketId = ++ActionPacketId;
	Packet.WriteInt64(PacketId);
	Packet.WriteInt64(Corsairs::Net::Msg::ActionType::MOVE);
	Packet.WriteSequence(Blob.GetData(), static_cast<uint16>(Blob.Num()));

	const FCorsairsPendingMove PendingMove{
		PacketId,
		Path[0],
		Path.Last(),
	};
	const uint64 BeginGeneration = ActionReducerGeneration;
	ECorsairsActionRequestResult Result = ActionReducer.Begin(
		PacketId,
		ECorsairsBeginActionType::Move,
		PendingMove,
		[&]() { return SendBeginActionPacket(Packet); });
	if (ActionReducerGeneration != BeginGeneration)
	{
		if (Stage == ECorsairsLoginStage::InWorld)
		{
			ActionReducer.EnterWorld(WorldId, SpawnPosition);
		}
		else
		{
			ActionReducer.Reset();
		}
		Result = ECorsairsActionRequestResult::TransportFailed;
	}
	if (Result == ECorsairsActionRequestResult::Sent)
	{
		++MovementBeginSendCount;
	}
	return Result;
}

ECorsairsActionRequestResult UCorsairsSession::SubmitPredictedPosition(
	const FIntPoint Endpoint)
{
	const TOptional<FCorsairsActiveBeginAction>& ActiveAction =
		ActionReducer.GetActiveAction();
	if (!ActiveAction.IsSet())
	{
		if (IsBelowMovementReportDistance(
			ActionReducer.GetConfirmedPosition(), Endpoint))
		{
			return ECorsairsActionRequestResult::Invalid;
		}
		return SendMoveFromConfirmed(Endpoint);
	}

	const TOptional<FCorsairsPendingMove>& PendingMove =
		ActionReducer.GetPendingMove();
	if (!PendingMove.IsSet())
	{
		return ECorsairsActionRequestResult::Busy;
	}

	const FIntPoint Baseline = ActionReducer.GetQueuedEndpoint().IsSet()
		? ActionReducer.GetQueuedEndpoint().GetValue()
		: PendingMove->RequestedEndpoint;
	if (IsBelowMovementReportDistance(Baseline, Endpoint))
	{
		return ECorsairsActionRequestResult::Busy;
	}
	ActionReducer.QueueEndpoint(Endpoint);
	return ECorsairsActionRequestResult::Busy;
}

FIntPoint UCorsairsSession::GetConfirmedPosition() const
{
	return ActionReducer.GetConfirmedPosition();
}

bool UCorsairsSession::IsMovementAuthorityLocked() const
{
	return ActionReducer.IsMovementAuthorityLocked();
}

int64 UCorsairsSession::GetMovementAuthorityEpoch() const
{
	return MovementAuthorityEpoch;
}

double UCorsairsSession::GetMovementSpeedCmPerSecond() const
{
	const int64* Speed = Attributes.Find(kAttrMovementSpeed);
	return Speed != nullptr ? static_cast<double>(*Speed) : 0.0;
}

const FCorsairsWorldActor* UCorsairsSession::FindActor(
	const int64 TargetWorldId) const
{
	return VisibleActors.FindByPredicate(
		[TargetWorldId](const FCorsairsWorldActor& A) { return A.WorldId == TargetWorldId; });
}

FCorsairsWorldActor* UCorsairsSession::FindMutableActor(
	const int64 TargetWorldId)
{
	return VisibleActors.FindByPredicate(
		[TargetWorldId](const FCorsairsWorldActor& A)
		{
			return A.WorldId == TargetWorldId;
		});
}

ECorsairsActionRequestResult UCorsairsSession::UseSkillOn(
	const int64 SkillId,
	const int64 TargetWorldId)
{
	if (Stage != ECorsairsLoginStage::InWorld)
	{
		return ECorsairsActionRequestResult::Invalid;
	}
	if (!CanSendBeginActionPacket())
	{
		return ECorsairsActionRequestResult::TransportFailed;
	}
	const FCorsairsWorldActor* Target = FindActor(TargetWorldId);
	if (Target == nullptr)
	{
		// Цели нет в поле зрения — сервер всё равно ответил бы отказом
		// «цели не существует», и разбирать его пришлось бы вслепую.
		return ECorsairsActionRequestResult::Invalid;
	}

	// Путь ведёт от персонажа к цели. Сервер сам проводит по нему персонажа и
	// бьёт по прибытии; путь из одной точки он не разыгрывает вовсе.
	const FIntPoint ConfirmedPosition =
		ActionReducer.GetConfirmedPosition();
	const int32 Coordinates[4] = {
		ConfirmedPosition.X, ConfirmedPosition.Y,
		Target->Position.X, Target->Position.Y };
	TArray<uint8> Blob;
	Blob.Append(reinterpret_cast<const uint8*>(Coordinates), sizeof(Coordinates));

	WPacket Packet(128 + Blob.Num());
	Packet.WriteCmd(CMD_CM_BEGINACTION);
	Packet.WriteInt64(WorldId);
	const int64 PacketId = ++ActionPacketId;
	Packet.WriteInt64(PacketId);
	Packet.WriteInt64(Corsairs::Net::Msg::ActionType::SKILL);
	// Признак движения строго 2 — «подойти и ударить». С нулём сервер молчит.
	Packet.WriteInt64(2);
	Packet.WriteInt64(PacketId);
	Packet.WriteSequence(Blob.GetData(), static_cast<uint16>(Blob.Num()));
	Packet.WriteInt64(SkillId);
	Packet.WriteInt64(Target->WorldId);
	Packet.WriteInt64(Target->Handle);

	const uint64 BeginGeneration = ActionReducerGeneration;
	bool bReservationSuperseded = false;
	const auto IsExpectedReservation = [this, PacketId]()
	{
		const TOptional<FCorsairsActiveBeginAction>& Active =
			ActionReducer.GetActiveAction();
		return Active.IsSet() &&
			Active->PacketId == PacketId &&
			Active->ActionType == ECorsairsBeginActionType::Skill &&
			Active->Phase == ECorsairsActionPhase::Requested;
	};
	ECorsairsActionRequestResult Result = ActionReducer.Begin(
		PacketId,
		ECorsairsBeginActionType::Skill,
		TOptional<FCorsairsPendingMove>(),
		[&]()
		{
			// Begin уже зарезервировал Skill. Pawn должен увидеть lock до
			// transport callback, который может синхронно завершить действие.
			PublishMovementAuthorityIfChanged();
			if (ActionReducerGeneration != BeginGeneration)
			{
				return false;
			}
			if (!IsExpectedReservation())
			{
				// Reentrant listener уже завершил или заменил reservation.
				// true нужен только внутреннему Begin: false восстановил бы его
				// старый snapshot поверх нового reducer state.
				bReservationSuperseded = true;
				return true;
			}

			const bool bSent = SendBeginActionPacket(Packet);
			if (ActionReducerGeneration == BeginGeneration &&
				!IsExpectedReservation())
			{
				bReservationSuperseded = true;
				return true;
			}
			return bSent;
		});
	PublishMovementAuthorityIfChanged();
	if (ActionReducerGeneration != BeginGeneration)
	{
		if (Stage == ECorsairsLoginStage::InWorld)
		{
			ActionReducer.EnterWorld(WorldId, SpawnPosition);
		}
		else
		{
			ActionReducer.Reset();
		}
		PublishMovementAuthorityIfChanged();
		Result = ECorsairsActionRequestResult::TransportFailed;
	}
	else if (bReservationSuperseded)
	{
		Result = ECorsairsActionRequestResult::TransportFailed;
	}
	return Result;
}

ECorsairsActionRequestResult UCorsairsSession::EquipItem(
	const int64 FromGrid,
	const int64 ToSlot)
{
	if (Stage != ECorsairsLoginStage::InWorld)
	{
		return ECorsairsActionRequestResult::Invalid;
	}
	if (!CanSendBeginActionPacket())
	{
		return ECorsairsActionRequestResult::TransportFailed;
	}
	WPacket Packet(64);
	Packet.WriteCmd(CMD_CM_BEGINACTION);
	Packet.WriteInt64(WorldId);
	const int64 PacketId = ++ActionPacketId;
	Packet.WriteInt64(PacketId);
	Packet.WriteInt64(Corsairs::Net::Msg::ActionType::ITEM_USE);
	Packet.WriteInt64(FromGrid);
	Packet.WriteInt64(ToSlot);
	const uint64 BeginGeneration = ActionReducerGeneration;
	ECorsairsActionRequestResult Result = ActionReducer.Begin(
		PacketId,
		ECorsairsBeginActionType::ItemUse,
		TOptional<FCorsairsPendingMove>(),
		[&]() { return SendBeginActionPacket(Packet); });
	if (ActionReducerGeneration != BeginGeneration)
	{
		if (Stage == ECorsairsLoginStage::InWorld)
		{
			ActionReducer.EnterWorld(WorldId, SpawnPosition);
		}
		else
		{
			ActionReducer.Reset();
		}
		Result = ECorsairsActionRequestResult::TransportFailed;
	}
	return Result;
}

ECorsairsActionRequestResult UCorsairsSession::PickUpItem(
	const int64 ItemWorldId,
	const int64 ItemHandle)
{
	if (Stage != ECorsairsLoginStage::InWorld)
	{
		return ECorsairsActionRequestResult::Invalid;
	}
	if (!CanSendBeginActionPacket())
	{
		return ECorsairsActionRequestResult::TransportFailed;
	}
	WPacket Packet(64);
	Packet.WriteCmd(CMD_CM_BEGINACTION);
	Packet.WriteInt64(WorldId);
	const int64 PacketId = ++ActionPacketId;
	Packet.WriteInt64(PacketId);
	Packet.WriteInt64(Corsairs::Net::Msg::ActionType::ITEM_PICK);
	Packet.WriteInt64(ItemWorldId);
	Packet.WriteInt64(ItemHandle);
	const uint64 BeginGeneration = ActionReducerGeneration;
	ECorsairsActionRequestResult Result = ActionReducer.Begin(
		PacketId,
		ECorsairsBeginActionType::ItemPick,
		TOptional<FCorsairsPendingMove>(),
		[&]() { return SendBeginActionPacket(Packet); });
	if (ActionReducerGeneration != BeginGeneration)
	{
		if (Stage == ECorsairsLoginStage::InWorld)
		{
			ActionReducer.EnterWorld(WorldId, SpawnPosition);
		}
		else
		{
			ActionReducer.Reset();
		}
		Result = ECorsairsActionRequestResult::TransportFailed;
	}
	return Result;
}

ECorsairsActionRequestResult UCorsairsSession::SendMoveFromConfirmed(
	const FIntPoint Endpoint)
{
	return SendMovePath({ActionReducer.GetConfirmedPosition(), Endpoint});
}

bool UCorsairsSession::CanSendBeginActionPacket() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (TestSendOverride)
	{
		return true;
	}
#endif
	return Connection != nullptr &&
		Connection->GetState() == ECorsairsConnectionState::Connected;
}

bool UCorsairsSession::SendBeginActionPacket(WPacket& Packet)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (TestSendOverride)
	{
		return TestSendOverride(Packet);
	}
#endif
	return Connection != nullptr && Connection->Send(Packet);
}

void UCorsairsSession::PublishMovementAuthorityIfChanged()
{
	const bool bLocked = ActionReducer.IsMovementAuthorityLocked();
	if (bLocked == bPublishedMovementAuthorityLocked)
	{
		return;
	}

	bPublishedMovementAuthorityLocked = bLocked;
	const int64 Epoch = ++MovementAuthorityEpoch;
	OnMovementAuthorityChanged.Broadcast(bLocked, Epoch);
#if WITH_DEV_AUTOMATION_TESTS
	if (TestMovementAuthorityObserver)
	{
		TestMovementAuthorityObserver(bLocked, Epoch);
	}
#endif
}

void UCorsairsSession::ApplyReducerEffects(
	const FCorsairsReducerEffects& Effects)
{
	const uint64 EffectsGeneration = ActionReducerGeneration;
	PublishMovementAuthorityIfChanged();
	if (ActionReducerGeneration != EffectsGeneration)
	{
		return;
	}

	const auto ReportProtocolError =
		[this](const FString& Message)
		{
			OnProtocolError.Broadcast(Message);
#if WITH_DEV_AUTOMATION_TESTS
			if (TestProtocolErrorObserver)
			{
				TestProtocolErrorObserver(Message);
			}
#endif
		};

	if (Effects.bProtocolError)
	{
		ReportProtocolError(TEXT("некорректное уведомление о действии"));
		return;
	}

	if (Effects.QueuedEndpoint.IsSet() &&
		!IsBelowMovementReportDistance(
			ActionReducer.GetConfirmedPosition(),
			Effects.QueuedEndpoint.GetValue()))
	{
		SendMoveFromConfirmed(Effects.QueuedEndpoint.GetValue());
	}

	if (Effects.Movement.IsSet())
	{
		FCorsairsMovementEvent Event = Effects.Movement.GetValue();
		FCorsairsWorldActor* RemoteActor = nullptr;
		if (Event.bLocal)
		{
			Event.MovementSpeedCmPerSecond =
				GetMovementSpeedCmPerSecond();
		}
		else
		{
			RemoteActor = FindMutableActor(Event.WorldId);
			if (RemoteActor != nullptr)
			{
				Event.MovementSpeedCmPerSecond =
					RemoteActor->MovementSpeedCmPerSecond;
			}
		}

		if (Event.bServerDriven &&
			Event.MovementSpeedCmPerSecond <= 0.0)
		{
			ReportProtocolError(FString::Printf(
				TEXT("для движения персонажа %lld отсутствует ATTR_MSPD"),
				Event.WorldId));
		}
		else
		{
			// Позиция должна измениться до broadcast: обработчик мира уже
			// начнёт визуальное движение, а следующий UseSkillOn обязан видеть
			// тот же подтверждённый сервером endpoint, а не точку появления.
			if (RemoteActor != nullptr)
			{
				RemoteActor->Position = Event.Endpoint;
			}
			OnMovementChanged.Broadcast(Event);
#if WITH_DEV_AUTOMATION_TESTS
			if (TestMovementObserver)
			{
				TestMovementObserver(Event);
			}
#endif
		}
	}
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
	VisibleActors.Reset();
	++ActionReducerGeneration;
	ActionReducer.Reset();
	PublishMovementAuthorityIfChanged();
	ActionPacketId = 0;
	MovementBeginSendCount = 0;
	if (Stage != ECorsairsLoginStage::Idle)
	{
		SetStage(ECorsairsLoginStage::Idle, FString());
	}
}

void UCorsairsSession::HandleConnectionState(ECorsairsConnectionState NewState,
											 const FString& Reason)
{
	if (NewState == ECorsairsConnectionState::Failed ||
		NewState == ECorsairsConnectionState::Disconnected)
	{
		++ActionReducerGeneration;
		ActionReducer.Reset();
		PublishMovementAuthorityIfChanged();
		ActionPacketId = 0;
		MovementBeginSendCount = 0;
	}

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
			else if (Entry.attrId == kAttrMovementSpeed)
			{
				LocalActor.MovementSpeedCmPerSecond =
					static_cast<double>(Entry.attrVal);
			}
		}
		WorldId = LocalActor.WorldId;
		SpawnPosition = LocalActor.Position;
		MapName = ToFString(Data.mapName);
		ActionPacketId = 0;
		MovementBeginSendCount = 0;
		++ActionReducerGeneration;
		ActionReducer.EnterWorld(WorldId, SpawnPosition);
		PublishMovementAuthorityIfChanged();

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

		if (const auto* Move =
				std::get_if<Corsairs::Net::Msg::ActionMoveData>(&Message.data))
		{
			ApplyReducerEffects(ActionReducer.OnMove(
				Message.worldId,
				Message.packetId,
				Move->moveState,
				TConstArrayView<uint8>(
					Move->waypoints.data(),
					static_cast<int32>(Move->waypoints.size()))));
			return;
		}

		if (const auto* SkillSource =
				std::get_if<Corsairs::Net::Msg::ActionSkillSrcData>(
					&Message.data))
		{
			ApplyReducerEffects(ActionReducer.OnSkillSource(
				Message.worldId,
				Message.packetId,
				SkillSource->state));
			return;
		}

		if (const auto* KitbagUpdate =
				std::get_if<Corsairs::Net::Msg::ChaKitbagInfo>(&Message.data))
		{
			if (Message.actionType == Corsairs::Net::Msg::ActionType::KITBAG &&
				Message.worldId == WorldId)
			{
				if (KitbagUpdate->synType ==
					Corsairs::Net::Msg::SYN_KITBAG_INIT)
				{
					Kitbag.Reset();
				}
				for (const auto& Item : KitbagUpdate->items)
				{
					if (Item.itemId > 0)
					{
						Kitbag.Add(Item.gridId, Item.itemId);
					}
					else
					{
						Kitbag.Remove(Item.gridId);
					}
				}
			}
			if (Message.actionType == Corsairs::Net::Msg::ActionType::KITBAG)
			{
				ApplyReducerEffects(ActionReducer.OnItemNotification(
					Message.worldId,
					Message.packetId,
					Message.actionType));
			}
			return;
		}

		if (std::holds_alternative<
				Corsairs::Net::Msg::ActionItemFailedData>(Message.data))
		{
			ApplyReducerEffects(ActionReducer.OnItemNotification(
				Message.worldId,
				Message.packetId,
				Message.actionType));
			return;
		}

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
			ApplyReducerEffects(ActionReducer.OnItemNotification(
				Message.worldId,
				Message.packetId,
				Message.actionType));
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

	if (Cmd == CMD_MC_FAILEDACTION)
	{
		Corsairs::Net::Msg::McFailedActionMessage Message;
		Corsairs::Net::Msg::deserialize(Packet, Message);
		ApplyReducerEffects(ActionReducer.OnFailedAction(
			Message.worldId,
			Message.actionType,
			Message.reason));
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
				if (Entry.attrId == kAttrHp)
				{
					LocalActor.Hp = Entry.attrVal;
				}
				else if (Entry.attrId == kAttrMovementSpeed)
				{
					LocalActor.MovementSpeedCmPerSecond =
						static_cast<double>(Entry.attrVal);
				}
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
				}
				else if (Entry.attrId == kAttrMovementSpeed)
				{
					Actor->MovementSpeedCmPerSecond =
						static_cast<double>(Entry.attrVal);
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
			}
			else if (Entry.attrId == kAttrMovementSpeed)
			{
				Actor.MovementSpeedCmPerSecond =
					static_cast<double>(Entry.attrVal);
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
		Corsairs::Net::Msg::McChaEndSeeMessage Message;
		Corsairs::Net::Msg::deserialize(Packet, Message);
		const int64 WorldIdLeft = Message.worldId;
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
#if WITH_DEV_AUTOMATION_TESTS
	if (TestStageObserver)
	{
		TestStageObserver(NewStage);
	}
#endif
}

#if !UE_BUILD_SHIPPING
int64 UCorsairsSession::GetMovementBeginSendCountForDiagnostics() const
{
	return MovementBeginSendCount;
}

bool UCorsairsSession::HasPendingMoveForDiagnostics() const
{
	return ActionReducer.GetPendingMove().IsSet();
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
void UCorsairsSession::SetSendOverrideForTests(
	TFunction<bool(WPacket&)> Override)
{
	TestSendOverride = MoveTemp(Override);
}

void UCorsairsSession::SetInWorldForTests(
	const int64 InWorldId,
	const FIntPoint Spawn)
{
	WorldId = InWorldId;
	SpawnPosition = Spawn;
	LocalActor = FCorsairsWorldActor{};
	LocalActor.WorldId = InWorldId;
	LocalActor.Position = Spawn;
	VisibleActors.Reset();
	Attributes.Reset();
	ActionPacketId = 0;
	MovementBeginSendCount = 0;
	++ActionReducerGeneration;
	ActionReducer.EnterWorld(InWorldId, Spawn);
	PublishMovementAuthorityIfChanged();
	Stage = ECorsairsLoginStage::InWorld;
}

void UCorsairsSession::SetInWorldAndBroadcastForTests(
	const FCorsairsWorldActor& InLocalActor,
	const FString& InMapName)
{
	WorldId = InLocalActor.WorldId;
	LocalActor = InLocalActor;
	SpawnPosition = InLocalActor.Position;
	MapName = InMapName;
	ActionPacketId = 0;
	MovementBeginSendCount = 0;
	++ActionReducerGeneration;
	ActionReducer.EnterWorld(WorldId, SpawnPosition);
	PublishMovementAuthorityIfChanged();
	SetStage(
		ECorsairsLoginStage::InWorld,
		FString::Printf(
			TEXT("тестовая карта %s, позиция (%d, %d)"),
			*MapName,
			SpawnPosition.X,
			SpawnPosition.Y));
}

void UCorsairsSession::SetMovementSpeedForTests(const int64 Speed)
{
	Attributes.Add(kAttrMovementSpeed, Speed);
	LocalActor.MovementSpeedCmPerSecond = static_cast<double>(Speed);
}

void UCorsairsSession::AddVisibleActorForTests(
	const FCorsairsWorldActor& Actor)
{
	VisibleActors.Add(Actor);
}

void UCorsairsSession::SetMovementAuthorityObserverForTests(
	TFunction<void(bool, int64)> Observer)
{
	TestMovementAuthorityObserver = MoveTemp(Observer);
}

void UCorsairsSession::HandlePacketForTests(RPacket& Packet)
{
	HandlePacket(Packet);
}

void UCorsairsSession::HandleConnectionStateForTests(
	const ECorsairsConnectionState NewState,
	const FString& Reason)
{
	HandleConnectionState(NewState, Reason);
}

void UCorsairsSession::SetEventObserversForTests(
	TFunction<void(const FCorsairsMovementEvent&)> MovementObserver,
	TFunction<void(const FString&)> ProtocolErrorObserver,
	TFunction<void(ECorsairsLoginStage)> StageObserver)
{
	TestMovementObserver = MoveTemp(MovementObserver);
	TestProtocolErrorObserver = MoveTemp(ProtocolErrorObserver);
	TestStageObserver = MoveTemp(StageObserver);
}
#endif
