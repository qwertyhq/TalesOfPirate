#include "CorsairsActionReducer.h"

namespace
{
constexpr int64 MoveOn = 0;
constexpr int64 MoveArrive = 1;
constexpr int64 MoveBlock = 2;
constexpr int64 MoveCancel = 4;
constexpr int64 MoveInRange = 8;
constexpr int64 MoveNoTarget = 16;
constexpr int64 MoveCantMove = 32;

constexpr int64 MoveAction = 1;
constexpr int64 SkillAction = 2;
constexpr int64 KitbagAction = 6;
constexpr int64 LookAction = 5;
constexpr int64 ItemFailedAction = 15;

bool IsSupportedAction(const ECorsairsBeginActionType ActionType)
{
	return ActionType == ECorsairsBeginActionType::Move ||
		ActionType == ECorsairsBeginActionType::Skill ||
		ActionType == ECorsairsBeginActionType::ItemPick ||
		ActionType == ECorsairsBeginActionType::ItemUse;
}

bool IsSkillInterrupt(const int64 MoveState)
{
	return MoveState == MoveArrive ||
		MoveState == MoveBlock ||
		MoveState == MoveCancel ||
		MoveState == MoveNoTarget ||
		MoveState == MoveCantMove;
}

bool DecodeWaypoints(
	const TConstArrayView<uint8> WaypointBytes,
	TArray<FIntPoint>& Waypoints)
{
	constexpr int32 WaypointSize = sizeof(int32) * 2;
	if (WaypointBytes.IsEmpty() ||
		WaypointBytes.Num() % WaypointSize != 0)
	{
		return false;
	}

	Waypoints.Reserve(WaypointBytes.Num() / WaypointSize);
	for (int32 Offset = 0;
		Offset < WaypointBytes.Num();
		Offset += WaypointSize)
	{
		int32 X = 0;
		int32 Y = 0;
		FMemory::Memcpy(&X, WaypointBytes.GetData() + Offset, sizeof(X));
		FMemory::Memcpy(
			&Y,
			WaypointBytes.GetData() + Offset + sizeof(X),
			sizeof(Y));
		Waypoints.Emplace(X, Y);
	}
	return true;
}
} // namespace

void FCorsairsActionReducer::EnterWorld(
	const int64 LocalWorldId,
	const FIntPoint SpawnPosition)
{
	Reset();
	_localWorldId = LocalWorldId;
	_confirmedPosition = SpawnPosition;
}

void FCorsairsActionReducer::Reset()
{
	_localWorldId = 0;
	_confirmedPosition = FIntPoint::ZeroValue;
	_activeAction.Reset();
	_pendingMove.Reset();
	_queuedEndpoint.Reset();
}

ECorsairsActionRequestResult FCorsairsActionReducer::Begin(
	const int64 PacketId,
	const ECorsairsBeginActionType ActionType,
	const TOptional<FCorsairsPendingMove>& Move,
	TFunctionRef<bool()> Send)
{
	if (_activeAction.IsSet())
	{
		return ECorsairsActionRequestResult::Busy;
	}

	const bool bIsMove = ActionType == ECorsairsBeginActionType::Move;
	if (PacketId == 0 ||
		!IsSupportedAction(ActionType) ||
		bIsMove != Move.IsSet() ||
		(Move.IsSet() && Move->PacketId != PacketId))
	{
		return ECorsairsActionRequestResult::Invalid;
	}

	const TOptional<FCorsairsActiveBeginAction> PreviousActiveAction =
		_activeAction;
	const TOptional<FCorsairsPendingMove> PreviousPendingMove = _pendingMove;
	const TOptional<FIntPoint> PreviousQueuedEndpoint = _queuedEndpoint;

	_activeAction = FCorsairsActiveBeginAction{
		PacketId,
		ActionType,
		ECorsairsActionPhase::Requested,
	};
	_pendingMove = Move;
	if (bIsMove)
	{
		_queuedEndpoint.Reset();
	}

	if (!Send())
	{
		_activeAction = PreviousActiveAction;
		_pendingMove = PreviousPendingMove;
		_queuedEndpoint = PreviousQueuedEndpoint;
		return ECorsairsActionRequestResult::TransportFailed;
	}

	return ECorsairsActionRequestResult::Sent;
}

bool FCorsairsActionReducer::QueueEndpoint(const FIntPoint Endpoint)
{
	if (!_pendingMove.IsSet())
	{
		return false;
	}
	_queuedEndpoint = Endpoint;
	return true;
}

FCorsairsReducerEffects FCorsairsActionReducer::OnMove(
	const int64 WorldId,
	const int64 PacketId,
	const int64 MoveState,
	const TConstArrayView<uint8> WaypointBytes)
{
	FCorsairsReducerEffects Effects;
	TArray<FIntPoint> Waypoints;
	if (!DecodeWaypoints(WaypointBytes, Waypoints))
	{
		Effects.bProtocolError = true;
		return Effects;
	}

	if (WorldId != _localWorldId ||
		!_activeAction.IsSet() ||
		_activeAction->ActionType != ECorsairsBeginActionType::Skill ||
		_activeAction->PacketId != PacketId)
	{
		Effects.bProtocolError = true;
		return Effects;
	}

	const bool bOn = MoveState == MoveOn;
	const bool bInRange = MoveState == MoveInRange;
	const bool bInterrupt = IsSkillInterrupt(MoveState);
	if (!bOn && !bInRange && !bInterrupt)
	{
		Effects.bProtocolError = true;
		return Effects;
	}

	FCorsairsMovementEvent Movement;
	Movement.WorldId = WorldId;
	Movement.PacketId = PacketId;
	Movement.Type = bOn
		? ECorsairsMovementEventType::AcceptedPath
		: bInRange
			? ECorsairsMovementEventType::Terminal
			: ECorsairsMovementEventType::Rejected;
	Movement.MoveState = static_cast<uint8>(MoveState);
	Movement.Waypoints = MoveTemp(Waypoints);
	Movement.Endpoint = Movement.Waypoints.Last();
	Movement.bLocal = true;
	Movement.bServerDriven = true;
	Movement.bRequireNeutral = bInterrupt;

	_confirmedPosition = Movement.Endpoint;
	if (bOn)
	{
		_activeAction->Phase = ECorsairsActionPhase::ServerMove;
	}
	else if (bInRange)
	{
		_activeAction->Phase = ECorsairsActionPhase::Fight;
	}
	else
	{
		_activeAction.Reset();
	}

	Effects.Movement = MoveTemp(Movement);
	return Effects;
}

FCorsairsReducerEffects FCorsairsActionReducer::OnSkillSource(
	const int64 WorldId,
	const int64 PacketId,
	const int64 FightState)
{
	FCorsairsReducerEffects Effects;
	if (WorldId != _localWorldId ||
		!_activeAction.IsSet() ||
		_activeAction->ActionType != ECorsairsBeginActionType::Skill ||
		_activeAction->PacketId != PacketId)
	{
		Effects.bProtocolError = true;
		return Effects;
	}

	if (FightState == 0)
	{
		_activeAction->Phase = ECorsairsActionPhase::Fight;
	}
	else
	{
		_activeAction.Reset();
	}
	return Effects;
}

FCorsairsReducerEffects FCorsairsActionReducer::OnItemNotification(
	const int64 WorldId,
	const int64 PacketId,
	const int64 NotificationActionType)
{
	FCorsairsReducerEffects Effects;
	if (WorldId != _localWorldId ||
		!_activeAction.IsSet() ||
		_activeAction->PacketId != PacketId)
	{
		Effects.bProtocolError = true;
		return Effects;
	}

	const bool bItemUseTerminal =
		_activeAction->ActionType == ECorsairsBeginActionType::ItemUse &&
		(NotificationActionType == KitbagAction ||
			NotificationActionType == LookAction ||
			NotificationActionType == ItemFailedAction);
	const bool bItemPickTerminal =
		_activeAction->ActionType == ECorsairsBeginActionType::ItemPick &&
		(NotificationActionType == KitbagAction ||
			NotificationActionType == ItemFailedAction);
	if (!bItemUseTerminal && !bItemPickTerminal)
	{
		Effects.bProtocolError = true;
		return Effects;
	}

	_activeAction.Reset();
	return Effects;
}

FCorsairsReducerEffects FCorsairsActionReducer::OnFailedAction(
	const int64 WorldId,
	const int64 ActionType,
	const int64 /*Reason*/)
{
	FCorsairsReducerEffects Effects;
	if (WorldId != _localWorldId || !_activeAction.IsSet())
	{
		Effects.bProtocolError = true;
		return Effects;
	}

	const bool bSkillFailure =
		_activeAction->ActionType == ECorsairsBeginActionType::Skill &&
		(ActionType == SkillAction || ActionType == MoveAction);
	if (!bSkillFailure)
	{
		Effects.bProtocolError = true;
		return Effects;
	}

	_activeAction.Reset();
	return Effects;
}

FIntPoint FCorsairsActionReducer::GetConfirmedPosition() const
{
	return _confirmedPosition;
}

const TOptional<FCorsairsActiveBeginAction>&
	FCorsairsActionReducer::GetActiveAction() const
{
	return _activeAction;
}

const TOptional<FCorsairsPendingMove>&
	FCorsairsActionReducer::GetPendingMove() const
{
	return _pendingMove;
}

const TOptional<FIntPoint>& FCorsairsActionReducer::GetQueuedEndpoint() const
{
	return _queuedEndpoint;
}

bool FCorsairsActionReducer::IsMovementAuthorityLocked() const
{
	return _activeAction.IsSet() &&
		_activeAction->ActionType == ECorsairsBeginActionType::Skill;
}
