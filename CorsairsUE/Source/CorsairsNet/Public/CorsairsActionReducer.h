#pragma once

#include "CoreMinimal.h"

#include "CorsairsActionReducer.generated.h"

UENUM(BlueprintType)
enum class ECorsairsBeginActionType : uint8
{
	None = 0,
	Move = 1,
	Skill = 2,
	ItemPick = 8,
	ItemUse = 11,
};

UENUM(BlueprintType)
enum class ECorsairsActionPhase : uint8
{
	None,
	Requested,
	ServerMove,
	Fight,
};

UENUM(BlueprintType)
enum class ECorsairsActionRequestResult : uint8
{
	Sent,
	Busy,
	Invalid,
	TransportFailed,
};

UENUM(BlueprintType)
enum class ECorsairsMovementEventType : uint8
{
	AcceptedPath,
	Terminal,
	Rejected,
};

struct FCorsairsActiveBeginAction
{
	int64 PacketId = 0;
	ECorsairsBeginActionType ActionType =
		ECorsairsBeginActionType::None;
	ECorsairsActionPhase Phase = ECorsairsActionPhase::None;
};

struct FCorsairsPendingMove
{
	int64 PacketId = 0;
	FIntPoint Start = FIntPoint::ZeroValue;
	FIntPoint RequestedEndpoint = FIntPoint::ZeroValue;
};

struct FCorsairsCompletedMove
{
	int64 WorldId = 0;
	int64 PacketId = 0;
	int64 MoveState = 0;
	FIntPoint Endpoint = FIntPoint::ZeroValue;
};

USTRUCT(BlueprintType)
struct CORSAIRSNET_API FCorsairsMovementEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 WorldId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	int64 PacketId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	ECorsairsMovementEventType Type =
		ECorsairsMovementEventType::AcceptedPath;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	uint8 MoveState = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	TArray<FIntPoint> Waypoints;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	FIntPoint Endpoint = FIntPoint::ZeroValue;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	bool bLocal = false;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	bool bServerDriven = false;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	double MovementSpeedCmPerSecond = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Corsairs")
	bool bRequireNeutral = false;
};

struct FCorsairsReducerEffects
{
	TOptional<FCorsairsMovementEvent> Movement;
	TOptional<FIntPoint> QueuedEndpoint;
	bool bProtocolError = false;
	bool bDuplicate = false;
};

class CORSAIRSNET_API FCorsairsActionReducer
{
public:
	void EnterWorld(int64 LocalWorldId, FIntPoint SpawnPosition);
	void Reset();
	ECorsairsActionRequestResult Begin(
		int64 PacketId,
		ECorsairsBeginActionType ActionType,
		const TOptional<FCorsairsPendingMove>& Move,
		TFunctionRef<bool()> Send);
	ECorsairsActionRequestResult RequestCancel(
		TFunctionRef<bool()> SendCancel);
	bool QueueEndpoint(FIntPoint Endpoint);
	FCorsairsReducerEffects OnMove(
		int64 WorldId,
		int64 PacketId,
		int64 MoveState,
		TConstArrayView<uint8> WaypointBytes);
	FCorsairsReducerEffects OnSkillSource(
		int64 WorldId,
		int64 PacketId,
		int64 FightState);
	FCorsairsReducerEffects OnItemNotification(
		int64 WorldId,
		int64 PacketId,
		int64 NotificationActionType);
	FCorsairsReducerEffects OnFailedAction(
		int64 WorldId,
		int64 ActionType,
		int64 Reason);
	FIntPoint GetConfirmedPosition() const;
	const TOptional<FCorsairsActiveBeginAction>&
		GetActiveAction() const;
	const TOptional<FCorsairsPendingMove>& GetPendingMove() const;
	const TOptional<FIntPoint>& GetQueuedEndpoint() const;
	bool HasActiveAction() const;
	bool IsCancelPending() const;
	bool IsMovementAuthorityLocked() const;

private:
	int64 _localWorldId = 0;
	FIntPoint _confirmedPosition = FIntPoint::ZeroValue;
	TOptional<FCorsairsActiveBeginAction> _activeAction;
	TOptional<FCorsairsPendingMove> _pendingMove;
	TOptional<FIntPoint> _queuedEndpoint;
	TOptional<FCorsairsCompletedMove> _lastCompletedMove;
	uint64 _nextReservationToken = 0;
	TOptional<uint64> _beginReservationToken;
	TOptional<uint64> _cancelReservationToken;
};
