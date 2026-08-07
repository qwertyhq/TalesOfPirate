# Server-Authoritative Movement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make local and remote movement follow GameServer acknowledgements, recover deterministically from `BLOCK` and `FAILEDACTION`, preserve skill-driven movement, and stop the held-input packet flood without replacing the already-working input stack.

**Architecture:** Put all `CM_BEGINACTION` ownership and protocol correlation in a pure `FCorsairsActionReducer` inside `CorsairsNet`. Reserve actions before socket send, distinguish confirmed and predicted position, emit typed movement events, and let the pawn apply exact reconciliation plus a neutral/re-press latch. Use a separate half-meter `FCorsairsCharacterGround` sampler for character centers and a pure path follower for remote actors. Finish with a deterministic, recoverable live Garner probe.

**Tech Stack:** Unreal Engine 5.8.1 C++, UE Automation Tests, existing Corsairs binary protocol, C++23 AssetConverter/CTest, Python 3 `unittest`, local SQL Server fixture.

## Global Constraints

- Work only in `/Users/ivan/code/TalesOfPirate/codex-ue-parity` on branch `qwertyhq/codex-ue-parity`.
- The approved design is `docs/superpowers/specs/2026-08-07-garner-reference-zone-parity-design.md`.
- Follow strict TDD. Every production change except the initial characterization test must first have a focused RED caused by the missing or incorrect behavior.
- Do not replace `DefaultInput.ini`, `EnhancedPlayerInput`, legacy axis bindings, possession, or `AddMovementInput`; the live log already proved those paths work.
- Socket-send success never advances the authoritative baseline.
- At most one `ActiveBeginAction` exists. Reserve it before every `CM_BEGINACTION` socket send and roll it back atomically on transport failure.
- At most one manual `PendingMove` exists. A new predicted endpoint while it is active replaces `QueuedEndpoint` and does not send another packet.
- No packet-ID correlation assumption may bypass the action reservation: `MC_FAILEDACTION` has no packet ID and GameServer owns one mutable `m_ulPacketID`.
- `BLOCK`, negative MOVE terminals, skill-path interruption, and `FAILEDACTION(MOVE, reason)` require neutral/re-press before local prediction resumes.
- A held axis does not count as new input. Both movement axes must be observed at zero before the next nonzero edge.
- Characters use the half-meter block raster; scene objects use the separate triangular surface sampler.
- `databases/game.db` is unrelated and must not be modified or staged.
- The live probe may change only the isolated local fixture rows, must snapshot them first, and must restore them in `finally` on every exit path.
- Each GREEN task receives an implementation review and a separate commit.

## Common Verification Commands

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Movement" \
  -TestExit="Automation Test Queue Empty"
```

Run the Unreal command in a separate invocation from Character and Camera suites; do not join multiple `Automation RunTests` commands with semicolons.

---

### Task 1: Characterize and freeze the working legacy input path

**Files:**

- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsLegacyInputTests.cpp`

- [ ] **Step 1: Add the characterization test**

Register `Corsairs.Movement.LegacyInput.PossessedPawnMoves`.

The test must:

1. create a game world with `UWorld::CreateWorld(EWorldType::Game, false)`;
2. create a `UGameInstance` plus `ULocalPlayer`, spawn `APlayerController`, call `SetPlayer` and `InitInputSystem`, and assert its real `PlayerInput` is `UEnhancedPlayerInput` from `DefaultInput.ini`;
3. spawn and possess `ACorsairsPlayerCharacter`, allowing the controller to create/push the normal input component;
4. inject `EKeys::W` with `IE_Pressed` through `APlayerController::InputKey(FInputKeyEventArgs)`, never invoke an axis delegate directly;
5. tick the controller/world at `1/60` for 120 iterations so `UEnhancedPlayerInput` evaluates the configured legacy `MoveForward` mapping;
6. inject `EKeys::W` with `IE_Released`;
7. assert `FVector::Dist2D(Start, End) > 100.0` and that no direct test call touched `MoveForward`.

- [ ] **Step 2: Run it before production changes**

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Movement.LegacyInput" \
  -TestExit="Automation Test Queue Empty"
```

Expected: GREEN on the current production code. A failure is a baseline problem to diagnose before continuing, not permission to replace input configuration.

- [ ] **Step 3: Commit**

```bash
git add \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsLegacyInputTests.cpp
git commit -m "test(ue): characterize legacy movement input"
```

---

### Task 2: Reserve all BeginAction transactions before socket send

**Files:**

- Create: `CorsairsUE/Source/CorsairsNet/Public/CorsairsActionReducer.h`
- Create: `CorsairsUE/Source/CorsairsNet/Private/CorsairsActionReducer.cpp`
- Create: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsActionReducerTests.cpp`

**Public interface:**

`CorsairsActionReducer.h` includes `CoreMinimal.h` and its own generated header. The public enums and movement event are reflected because they cross `UFUNCTION`/dynamic-delegate boundaries:

```cpp
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
    bool IsMovementAuthorityLocked() const;
};
```

- [ ] **Step 1: Add RED reservation tests**

Register these tests:

```text
Corsairs.Movement.Reducer.ReservesBeforeSend
Corsairs.Movement.Reducer.RollsBackFailedSend
Corsairs.Movement.Reducer.ItemLifecycle
Corsairs.Movement.Reducer.SkillLifecycle
Corsairs.Movement.Reducer.ResetOnDisconnect
```

`ReservesBeforeSend` calls outer `Begin(S, Skill, none, lambda)`. Inside the send lambda, call nested `Begin(M, Move, Pending, innerLambda)`. Require outer `Sent`, nested `Busy`, send counts `1/0`, and active packet `S`.

`RollsBackFailedSend` returns false from the send lambda and requires `TransportFailed`, no active/pending action, and unchanged confirmed position.

`ItemLifecycle` requires:

- ItemUse closes only on matching local `KITBAG=6`, `LOOK=5`, or `ITEM_FAILED=15`;
- ItemPick closes only on matching local `KITBAG=6` or `ITEM_FAILED=15`;
- wrong world ID, packet ID, or notification type leaves the action busy and marks protocol error;
- a second BeginAction never calls its send lambda before the first terminal.

`SkillLifecycle` requires:

- `Skill S -> MOVE ON S -> MOVE INRANGE S` enters Fight and closes only the server-move phase;
- `SKILL_SRC S, state=0` remains active;
- terminal `SKILL_SRC S, state=1` clears the skill;
- MOVE `ARRIVE/BLOCK/CANCEL/NOTARGET/CANTMOVE` without `INRANGE` clears skill and server move, emits exact reconciliation, and requires neutral;
- `FAILEDACTION(SKILL, any)` and `FAILEDACTION(MOVE, any)` during the skill path clear the skill;
- unrelated failure does not clear it.

- [ ] **Step 2: Verify RED**

Build must fail because `CorsairsActionReducer.h` does not exist.

- [ ] **Step 3: Implement the reservation state machine**

`Begin` must set `ActiveBeginAction` and optional `PendingMove` before invoking `Send`. On false, restore the exact pre-call state. On an existing active action, return `Busy` without invoking `Send`.

Use protocol values:

```text
MOVE=1, SKILL=2, ITEM_PICK=8, ITEM_USE=11
KITBAG=6, LOOK=5, ITEM_FAILED=15
```

Do not add a timeout unlock. Disconnect and `Reset` are the only non-protocol cleanup paths.

- [ ] **Step 4: Verify GREEN**

Run build and `Corsairs.Movement.Reducer`; then run the whole Movement suite.

- [ ] **Step 5: Commit**

```bash
git add \
  CorsairsUE/Source/CorsairsNet/Public/CorsairsActionReducer.h \
  CorsairsUE/Source/CorsairsNet/Private/CorsairsActionReducer.cpp \
  CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsActionReducerTests.cpp
git commit -m "feat(net): reserve begin actions atomically"
```

---

### Task 3: Reduce MOVE acknowledgements, failures, queues, and stale packets

**Files:**

- Modify: `CorsairsUE/Source/CorsairsNet/Public/CorsairsActionReducer.h`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/CorsairsActionReducer.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsActionReducerTests.cpp`

**Literal constants in tests:**

```text
MOVE=1
ON=0
ARRIVE=1
BLOCK=2
CANCEL=4
INRANGE=8
NOTARGET=16
CANTMOVE=32
ACTFORBID=0
EXISTACT=1
MOVEPATH=2
```

- [ ] **Step 1: Add RED tests**

Register and cover:

```text
Corsairs.Movement.Reducer.OnArrive
Corsairs.Movement.Reducer.DirectTerminals
Corsairs.Movement.Reducer.QueuesOnce
Corsairs.Movement.Reducer.DuplicateTerminal
Corsairs.Movement.Reducer.StaleTerminal
Corsairs.Movement.Reducer.RejectsMismatchedLocalPacket
Corsairs.Movement.Reducer.SkillOpensServerDrivenWithoutPending
Corsairs.Movement.Reducer.MalformedWaypoints
Corsairs.Movement.Reducer.FailedMove
Corsairs.Movement.Reducer.RemoteMove
```

Required scenarios:

- Start `(223325,278475)`, manual packet 10 requests `(223825,278475)`, `ON` then `ARRIVE` confirms the endpoint and clears pending/active.
- Direct `ARRIVE` and direct `BLOCK` close a matching pending without prior `ON`; `BLOCK` emits `Rejected` and requires neutral.
- Queue A, then B while pending; `ARRIVE` returns only B and the caller can send exactly once from the newly confirmed endpoint.
- Replaying the same terminal sets `bDuplicate=true` and emits no event/queue.
- After packet 10 completes and packet 11 begins, terminal 10 cannot change confirmed position or pending 11.
- Pending packet 11 rejects local MOVE packet 12 without reconciliation.
- Active skill S with no pending manual move accepts local server-driven MOVE S, sets `bServerDriven=true`, and rejects any other packet as stale.
- Empty, 7-byte, and 9-byte waypoint blobs are protocol errors with byte-for-byte unchanged reducer state. A valid blob is a nonzero multiple of two `int32`.
- `FAILEDACTION(local, MOVE, EXISTACT)` clears pending/queue, emits rejection to the prior confirmed endpoint, and never resends.
- Remote movement ignores local reservation correlation, preserves all waypoints, emits `bServerDriven=true` terminal endpoint/facing data, and deduplicates by world ID plus packet/state/endpoint. Manual pending events set `bServerDriven=false`.

- [ ] **Step 2: Verify RED**

Expected: focused automation failures because terminal, malformed, and queue behavior are not implemented.

- [ ] **Step 3: Implement MOVE reduction**

Every accepted terminal, including `ARRIVE`, sets confirmed position to the final waypoint. Only `ARRIVE` exposes a queued endpoint. Negative terminals clear the queue and require neutral. Track `LastCompletedMove` as `(worldId, packetId, state, endpoint)`.

While a manual pending exists, only its packet ID may affect local state. With no manual pending, a matching active skill may open `ServerDrivenMove`. `INRANGE` moves the skill to Fight; other MOVE terminals interrupt and clear it.

- [ ] **Step 4: Verify GREEN and commit**

```bash
git add \
  CorsairsUE/Source/CorsairsNet/Public/CorsairsActionReducer.h \
  CorsairsUE/Source/CorsairsNet/Private/CorsairsActionReducer.cpp \
  CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsActionReducerTests.cpp
git commit -m "feat(net): reduce authoritative move results"
```

---

### Task 4: Wire the reducer into `UCorsairsSession`

**Files:**

- Modify: `CorsairsUE/Source/CorsairsNet/Public/CorsairsSession.h`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/CorsairsSession.cpp`
- Create: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionMovementTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp`
- Modify: `CorsairsUE/Scripts/check_login.py`
- Modify: `CorsairsUE/Scripts/check_combat.py`

**Public additions:**

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FCorsairsMovementChanged,
    const FCorsairsMovementEvent&,
    Event);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FCorsairsProtocolError,
    const FString&,
    Message);

UFUNCTION(BlueprintCallable, Category = "Corsairs")
ECorsairsActionRequestResult SubmitPredictedPosition(
    FIntPoint Endpoint);

UFUNCTION(BlueprintPure, Category = "Corsairs")
FIntPoint GetConfirmedPosition() const;

UFUNCTION(BlueprintPure, Category = "Corsairs")
bool IsMovementAuthorityLocked() const;

UFUNCTION(BlueprintPure, Category = "Corsairs")
double GetMovementSpeedCmPerSecond() const;

UPROPERTY(BlueprintAssignable, Category = "Corsairs")
FCorsairsMovementChanged OnMovementChanged;

UPROPERTY(BlueprintAssignable, Category = "Corsairs")
FCorsairsProtocolError OnProtocolError;
```

Change these four APIs to the exact return signatures:

```cpp
ECorsairsActionRequestResult SendMovePath(
    const TArray<FIntPoint>& Path);
ECorsairsActionRequestResult UseSkillOn(
    int64 SkillId, int64 TargetWorldId);
ECorsairsActionRequestResult EquipItem(
    int64 FromGrid, int64 ToSlot);
ECorsairsActionRequestResult PickUpItem(
    int64 ItemWorldId, int64 ItemHandle);
```

**Private additions:**

```cpp
ECorsairsActionRequestResult SendMoveFromConfirmed(
    FIntPoint Endpoint);
bool SendBeginActionPacket(
    Corsairs::Net::WPacket& Packet);
bool CanSendBeginActionPacket() const;
void ApplyReducerEffects(
    const FCorsairsReducerEffects& Effects);
FCorsairsActionReducer ActionReducer;
int64 MovementBeginSendCount = 0;

#if WITH_DEV_AUTOMATION_TESTS
TFunction<bool(Corsairs::Net::WPacket&)> TestSendOverride;
#endif
```

- [ ] **Step 1: Add RED session integration tests**

Add a `public:` automation section guarded by `WITH_DEV_AUTOMATION_TESTS` so the tests can call these methods without bypassing C++ access control:

```cpp
void SetSendOverrideForTests(
    TFunction<bool(Corsairs::Net::WPacket&)> Override);
void SetInWorldForTests(
    int64 InWorldId, FIntPoint Spawn);
void HandlePacketForTests(
    Corsairs::Net::RPacket& Packet);
```

Expose the same read-only non-shipping diagnostics used by the live probe:

```cpp
#if !UE_BUILD_SHIPPING
int64 GetMovementBeginSendCountForDiagnostics() const;
bool HasPendingMoveForDiagnostics() const;
#endif
```

Tests must:

- recursively call a second public BeginAction from the first send override and prove `Busy` plus one serialized packet;
- feed real serialized `McCharacterActionMessage` MOVE packets through `RPacket`;
- feed real `McFailedActionMessage` command 520;
- verify ENTERMAP initializes confirmed position before `InWorld`;
- verify `UseSkillOn` builds its path from reducer confirmed position, not `SpawnPosition`;
- verify `SubmitPredictedPosition` applies the literal 100 cm distance gate from confirmed position, does not send stationary reports, replaces at most one queued endpoint while a manual MOVE is pending, and never queues manual input behind a non-MOVE action;
- verify repeated pending endpoints below 100 cm do not mutate the queue, `ARRIVE` sends exactly one queued path `[newConfirmed,latestQueued]`, and every negative terminal clears it without a send;
- verify disconnect/logout reset all reducer state;
- verify existing LOOK/KITBAG mutation still runs while also closing the matching reservation.
- verify local `ATTR_MSPD` (`attrId=44`) becomes `MovementSpeedCmPerSecond`, remote actor speed is read from CHABEGINSEE attributes, and every broadcast event receives the matching positive speed;
- verify missing/zero speed marks a protocol error before a server-driven follower can start;
- verify diagnostic MOVE count increments only after an actual `Sent` transaction and pending state clears on all terminals.
- serialize `McChaEndSeeMessage{seeType=1,worldId=77}` and require actor 77, not actor 1, to leave the remote map.

- [ ] **Step 2: Verify RED**

Expected compile failures for missing public result type, delegates, and test seams.

- [ ] **Step 3: Route every BeginAction through one transaction**

Each method first requires `Stage == InWorld` and `CanSendBeginActionPacket()`, increments `ActionPacketId`, constructs the packet, then calls:

```cpp
ActionReducer.Begin(
    ActionPacketId,
    ActionType,
    PendingMove,
    [&]() { return SendBeginActionPacket(Packet); });
```

`CanSendBeginActionPacket` returns true for a configured test override even when `Connection == nullptr`; otherwise it requires a live connection. `SendBeginActionPacket` calls `TestSendOverride(Packet)` when set and otherwise calls `Connection->Send(Packet)`. This is the single guard/send seam: public BeginAction methods may not pre-reject the test override with a direct null-connection check, and there must be no direct `Connection->Send(Packet)` left for any `CMD_CM_BEGINACTION` public method.

In `CMD_MC_NOTIACTION`:

- MOVE variant goes to `OnMove`;
- SKILL_SRC goes to `OnSkillSource`;
- KITBAG, LOOK, and ITEM_FAILED go to `OnItemNotification` in addition to their existing state mutation.

In `CMD_MC_FAILEDACTION`, deserialize `McFailedActionMessage` and call `OnFailedAction`. It has no packet ID; correlate only through the single active action.

In `CMD_MC_CHAENDSEE`, deserialize the complete `McChaEndSeeMessage` (`seeType` then `worldId`) and remove/broadcast `worldId`. Never read the first `int64` as the actor ID.

Read `ATTR_MSPD=44` from ENTERMAP, attribute updates, and CHABEGINSEE. `ApplyReducerEffects` copies the corresponding local/remote speed into the event before broadcast; a `bServerDriven` event with missing/non-positive speed emits `OnProtocolError` and is not played. Reset the diagnostic MOVE counter on ENTERMAP/disconnect.

`SubmitPredictedPosition` owns the literal `100 cm` report threshold:

```text
no active action:
  distance(ConfirmedPosition, Endpoint) < 100 -> Invalid, no send
  otherwise -> SendMoveFromConfirmed(Endpoint)
manual PendingMove:
  distance(last requested-or-queued endpoint, Endpoint) < 100 -> Busy
  otherwise replace the one QueuedEndpoint -> Busy, no send
active non-MOVE action:
  Busy, no manual queue
```

On manual `ARRIVE`, `ApplyReducerEffects` sends the returned queued endpoint exactly once and only when it remains at least 100 cm from the new confirmed position. Negative terminal/failure never resends. Thus removing pawn `ReportedPosition` cannot create stationary packet spam.

Update the existing `ACorsairsPlayerCharacter::ReportMovement` boolean call in this task to compare `SendMovePath(Path) == ECorsairsActionRequestResult::Sent`; Task 5 then removes that legacy baseline entirely. This keeps every intermediate commit buildable.

- [ ] **Step 4: Update Python checks**

Make check scripts compare the new enum result to `Sent` and fail explicitly on `Busy`, `Invalid`, or `TransportFailed`. Do not treat absence of disconnect as movement acceptance.

- [ ] **Step 5: Verify GREEN and commit**

Run Movement automation, Character automation, Camera automation, Python tests, and Mac build.

```bash
git add \
  CorsairsUE/Source/CorsairsNet/Public/CorsairsSession.h \
  CorsairsUE/Source/CorsairsNet/Private/CorsairsSession.cpp \
  CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionMovementTests.cpp \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp \
  CorsairsUE/Scripts/check_login.py \
  CorsairsUE/Scripts/check_combat.py
git commit -m "feat(net): integrate authoritative movement session"
```

---

### Task 5: Gate pawn prediction until neutral/re-press

**Files:**

- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsMovementInputGate.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsMovementInputGate.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsMovementInputGateTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerCharacter.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsPlayerMovementTests.cpp`

**Pure interface:**

```cpp
class CORSAIRSGAME_API FCorsairsMovementInputGate
{
public:
    void SetAuthorityLocked(bool bLocked);
    void RequireNeutral();
    void SetForward(float Value);
    void SetRight(float Value);
    bool AllowsPrediction() const;
    bool IsWaitingForNeutral() const;

private:
    float Forward = 0.0f;
    float Right = 0.0f;
    bool bAuthorityLocked = false;
    bool bWaitingForNeutral = false;
};
```

Add this non-shipping C++ test/probe seam to `ACorsairsPlayerCharacter`:

```cpp
#if !UE_BUILD_SHIPPING
public:
    void ApplyMovementAxisForProbe(
        FName AxisName, float Value);
#endif
```

It accepts only `MoveForward` and `MoveRight`, dispatches to the same private axis callbacks used by `SetupPlayerInputComponent`, and returns without side effects for any other name. It does not call the session directly.

Add the exact dynamic-delegate receiver to the reflected pawn class:

```cpp
UFUNCTION()
void HandleMovementChanged(
    const FCorsairsMovementEvent& Event);
```

`AttachSession` uses `AddDynamic`; detach/end play removes that binding. The test asserts one broadcast invokes the handler exactly once after reattachment, not twice.

- [ ] **Step 1: Add RED pure gate tests**

Register:

```text
Corsairs.Movement.InputGate.HeldAxisAfterBlock
Corsairs.Movement.InputGate.HeldAxisAcrossSkill
Corsairs.Movement.InputGate.OneAxisNeutralIsInsufficient
```

Require:

- Forward 1, then `RequireNeutral`, then 120 repeated Forward 1 calls never allows prediction;
- Forward 0 and Right 0 clear the latch; next Forward 1 allows;
- locking with held Forward and unlocking still denies until neutral/re-press;
- Forward 0 with Right 1 remains latched.

- [ ] **Step 2: Add RED pawn integration tests**

Register:

```text
Corsairs.Movement.Pawn.FirstSegmentFromSpawn
Corsairs.Movement.Pawn.HeldInputNoPacketFlood
Corsairs.Movement.Pawn.ReconcilesTerminalExactly
```

The first test attaches an in-world session at `(223325,278475)`, moves before the first 0.5-second report, and requires the first captured path to be `[spawn,current]`, not omitted and not `[current,current]`.

The held-input test injects a rejected event while Forward remains 1, simulates two seconds, and requires zero send calls; neutral/re-press produces exactly one call.

The terminal test requires exact XY teleport to the server endpoint and zero velocity/input vector.

- [ ] **Step 3: Verify RED**

Expected failures: old code discards the first segment and advances `ReportedPosition` on socket send.

- [ ] **Step 4: Implement pawn integration**

Remove `ReportedPosition`, `bHasReported`, and the initial-report discard. Store both axis values, including zeros. Before `AddMovementInput`, update the pure gate with `Session->IsMovementAuthorityLocked()`.

`ReportMovement` calls `SubmitPredictedPosition(ToMapCoordinates(GetActorLocation()))`; no result advances confirmed state.

Bind `OnMovementChanged` in `AttachSession`. For terminal/rejected events, zero velocity, consume the movement vector, require neutral when requested, and teleport exact authoritative XY. Z is supplied by Task 6 ground.

Set `CharacterMovement->MaxFlySpeed` from positive session `ATTR_MSPD=44`; missing/zero speed keeps prediction locked and exposes the protocol error instead of using an arbitrary fallback. Manual (`bServerDriven=false`) AcceptedPath events never start a follower and therefore cannot fight local prediction.

Implement `ApplyMovementAxisForProbe` as the only non-shipping runtime seam used by Task 8; unit-test both accepted axis names and rejection of an unknown name.

- [ ] **Step 5: Verify GREEN and commit**

```bash
git add \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsMovementInputGate.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsMovementInputGate.cpp \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsMovementInputGateTests.cpp \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerCharacter.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsPlayerMovementTests.cpp
git commit -m "fix(ue): reconcile blocked movement input"
```

---

### Task 6: Ground all characters from the half-meter block raster

**Files:**

- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacterGround.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacterGround.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCharacterGroundTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacter.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacter.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerCharacter.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsGameMode.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsGameMode.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs`

**Interface:**

```cpp
struct FCorsairsCharacterCell
{
    double HeightCm = 0.0;
    bool bBlocked = false;
};

class CORSAIRSGAME_API FCorsairsCharacterGround
{
public:
    bool Load(
        const FString& MapName, FString& OutError);
    bool LoadFromBytes(
        int32 TileWidth,
        int32 TileHeight,
        TConstArrayView<uint8> Bytes,
        FString& OutError);
    FCorsairsCharacterCell Sample(
        FIntPoint SourcePosition) const;
    FVector ActorCenter(
        FIntPoint SourcePosition,
        double ScaledCapsuleHalfHeight) const;
    bool IsLoaded() const;
};
```

`ACorsairsGameMode` owns one `TUniquePtr<FCorsairsCharacterGround>`.
`ACorsairsCharacter` stores the non-owning pointer through this exact
runtime-only interface so Task 7 can sample the same raster while advancing
remote paths:

```cpp
virtual void AttachCharacterGround(
    const FCorsairsCharacterGround* InGround);
```

`ACorsairsPlayerCharacter` overrides `AttachCharacterGround`, calls the base
implementation, and configures its `UCharacterMovementComponent`. GameMode
must detach/destroy characters before releasing the owned sampler in
`EndPlay`.

- [ ] **Step 1: Add RED sampler tests**

For a 3×2 source-tile raster, provide exactly 24 bytes. Put `0x05`, `0x45`, `0x85`, and `0x00` in the first tile's quadrants `[top-left, top-right, bottom-left, bottom-right]`. Half-meter samples `(0,0)`, `(50,0)`, and `(0,50)` yield +25, −25, and +25 cm; only the third is blocked. With half-height 88, centers are 113, 63, and 113 cm. Verify rectangular indexing, out-of-range zero/unblocked, and rejection of 23 bytes for a 3×2 source-tile raster.

- [ ] **Step 2: Add RED actor grounding test**

Spawn local and remote characters over a 60 cm cell with capsule half-height 88. Both centers must be Z 148 and both capsule bottoms Z 60. Put an overlap actor at the remote spawn and require the remote actor still exists.

Register
`Corsairs.Movement.Ground.PreservesFlyingMovementMode`. Attach the new
`FCorsairsCharacterGround` to the local pawn and require
`GravityScale == 0.0f` and `MovementMode == MOVE_Flying` before and after a
world tick. The test must use the new attachment path, not call the legacy
`UseTerrainHeights`; it is RED because that path does not exist yet. This
protects the two settings currently applied inside `UseTerrainHeights` from
being lost when that API is removed.

- [ ] **Step 3: Verify RED**

Expected compile failures because the sampler does not exist; existing remote center also lacks the capsule half-height.

- [ ] **Step 4: Implement rectangular block sampling**

Load `Data/Heights/<map>.terrain.json` for source-tile `gridWidth/gridHeight` and the matching generated `.block.raw` installed by Garner terrain Task 7. Require exactly `width * height * 4` bytes. Expand each source tile to four half-meter cells in `[top-left, top-right, bottom-left, bottom-right]` order:

```text
gridX = trunc(sourceX / 50)
gridY = trunc(sourceY / 50)
tileX = gridX / 2
tileY = gridY / 2
quadrant = (gridY % 2) * 2 + (gridX % 2)
byte = bytes[(tileY * width + tileX) * 4 + quadrant]
magnitudeCm = (byte & 63) * 5
heightCm = (byte & 64) ? -magnitudeCm : magnitudeCm
blocked = (byte & 128) != 0
```

Do not clamp. Out of bounds returns sea-height zero and unblocked. `ActorCenter` returns `(X,-Y,height+halfHeight)`.

GameMode owns one runtime sampler and attaches it to the local pawn and every
remote `ACorsairsCharacter`. Local and remote use the same `ActorCenter`;
remote spawn sets `SpawnCollisionHandlingOverride=AlwaysSpawn`.
`ACorsairsPlayerCharacter::AttachCharacterGround` must preserve
`GravityScale=0.0f` and call `SetMovementMode(MOVE_Flying)` after the sampler
is attached; its per-tick Z update uses the attached sampler and never
switches back to walking. Remove `UseTerrainHeights`, all
`TerrainHeights.h`/`UCorsairsTerrainHeights` use from `CorsairsGame`, and the
editor-only `CorsairsImport` dependency from `CorsairsGame.Build.cs`.
Offline scene placement continues to own the separate terrain-surface
sampler.

- [ ] **Step 5: Verify GREEN and commit**

```bash
git add \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacterGround.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacterGround.cpp \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCharacterGroundTests.cpp \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacter.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacter.cpp \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerCharacter.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsGameMode.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsGameMode.cpp \
  CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs
git commit -m "fix(ue): ground characters from block grid"
```

After the Editor tests pass, build the runtime target to prove no Editor module leaked into the game:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUE Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
```

---

### Task 7: Play server waypoints for skill and remote movement

**Files:**

- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsServerPathFollower.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsServerPathFollower.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsServerPathFollowerTests.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsMovementRoutingTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacter.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacter.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsGameMode.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsGameMode.cpp`

**Interface:**

```cpp
class CORSAIRSGAME_API FCorsairsServerPathFollower
{
public:
    void Accept(TConstArrayView<FIntPoint> Waypoints);
    FIntPoint Advance(double DistanceCm);
    void Reconcile(FIntPoint Endpoint);
    void Stop();
    FIntPoint GetPosition() const;
    double GetFacingYaw() const;
    bool IsActive() const;
};
```

Add this exact dynamic-delegate receiver to the reflected GameMode class:

```cpp
UFUNCTION()
void HandleMovementChanged(
    const FCorsairsMovementEvent& Event);
```

`BeginPlay` binds it with
`Session->OnMovementChanged.AddDynamic(this,
&ACorsairsGameMode::HandleMovementChanged)`. `EndPlay` calls the matching
`RemoveDynamic` before `Session->Logout()` and before nulling `Session`.
This GameMode receiver routes only `bLocal == false` events through
`WorldActors`; it returns immediately for local events because Task 5 already
binds the possessed pawn directly. There must be exactly one local receiver,
not a second GameMode forwarding path.

- [ ] **Step 1: Add RED follower tests**

For `(0,0) -> (300,0) -> (300,400)`, advance 200, 200, and 300 cm. Require segment order, endpoint, facing 0° on +X and the correct UE yaw after map-Y inversion on +Y. Terminal reconcile snaps exactly; rejection stops.

- [ ] **Step 2: Add RED routing tests**

Register:

```text
Corsairs.Movement.Routing.GameModeDynamicLifecycle
Corsairs.Movement.Routing.LocalDeliveredOnce
Corsairs.Movement.Routing.RemoteByWorldId
```

`GameModeDynamicLifecycle` creates a game world with `bAutoLogin=false`,
runs `BeginPlay`, and proves a remote `OnMovementChanged.Broadcast` reaches
the GameMode handler exactly once. After `EndPlay`, broadcasting on a saved
session pointer must not reach the handler or move the actor. The test must
exercise the actual dynamic multicast, not call `HandleMovementChanged`
directly.

`LocalDeliveredOnce` attaches the possessed pawn as in Task 5, broadcasts
one local event, and requires one pawn transition: GameMode must not forward
it a second time. `RemoteByWorldId` creates two mapped remote actors and
requires only the actor named by `Event.WorldId` to accept the path. Remote
transforms must change only from accepted server waypoints; local skill
authority lock remains active throughout server-driven MOVE.

Require manual local `bServerDriven=false` AcceptedPath to leave the follower inactive. Require local skill and remote `bServerDriven=true` paths to advance by `MovementSpeedCmPerSecond * DeltaSeconds`; zero/missing speed is rejected before playback.

- [ ] **Step 3: Verify RED**

Expected failures because remote actors are static after spawn and no follower exists.

- [ ] **Step 4: Implement follower and routing**

`ACorsairsCharacter` owns a follower. It accepts only
`bServerDriven=true`, positive-speed events, advances during Tick by the
event's authoritative `MovementSpeedCmPerSecond * DeltaSeconds`, samples the
Task 6 ground pointer for each position, and applies facing. GameMode's
dynamic handler resolves only remote actors and calls that receiver; local
events remain on the Task 5 pawn binding. Extend the existing
`ACorsairsPlayerCharacter::HandleMovementChanged` in this task so a local
`bServerDriven=true` event is passed once to the inherited follower,
whereas a manual `bServerDriven=false` AcceptedPath is not. Manual local
prediction remains owned by `CharacterMovement`; it never receives follower
playback. Terminal uses exact reconcile rather than overshoot.

- [ ] **Step 5: Verify GREEN and commit**

```bash
git add \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsServerPathFollower.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsServerPathFollower.cpp \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsServerPathFollowerTests.cpp \
  CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsMovementRoutingTests.cpp \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacter.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacter.cpp \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsGameMode.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsGameMode.cpp
git commit -m "feat(ue): follow authoritative server paths"
```

---

### Task 8: Prove free and blocked movement on Garner with a recoverable fixture

**Dependencies:** Garner terrain Task 1 provides `MapSectionReader`, Task 4 provides `Sha256File`, and Task 7 installs the runtime block raster and metadata.

**Files:**

- Create: `tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MovementFixture.h`
- Create: `tools/AssetConverter/src/MovementFixture.cpp`
- Create: `tools/AssetConverter/tests/TestMovementFixture.cpp`
- Modify: `tools/AssetConverter/src/Main.cpp`
- Modify: `tools/AssetConverter/CMakeLists.txt`
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsMovementProbeSubsystem.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsMovementProbeSubsystem.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Public/CorsairsSession.h`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/CorsairsSession.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionMovementTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerCharacter.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp`
- Create: `CorsairsUE/Scripts/run_movement_probe.py`
- Create: `CorsairsUE/Scripts/tests/test_run_movement_probe.py`

**Converter interface:**

```cpp
struct MovementPoint {
    std::int32_t X{0};
    std::int32_t Y{0};
    auto operator<=>(const MovementPoint&) const = default;
};

enum class MovementDirection : std::uint8_t {
    PositiveX,
    PositiveY,
    NegativeX,
    NegativeY,
};

struct MovementFixture {
    MovementPoint Start;
    MovementPoint FreeEnd;
    MovementPoint BlockedEnd;
    MovementDirection FreeDirection{
        MovementDirection::PositiveX};
    MovementDirection BlockedDirection{
        MovementDirection::PositiveX};
    double FreeCameraYaw{0};
    double BlockedCameraYaw{0};
    std::string SourceMapSha256;
};

std::optional<MovementFixture> BuildMovementFixture(
    MapSectionReader& reader,
    MovementPoint start,
    std::int32_t CapsuleRadiusCm,
    std::string SourceMapSha256,
    std::string& diagnostics);
```

**Non-shipping serialized-MOVE diagnostic contract:**

Task 8 extends `CorsairsSession.h` with a C++-only record and getter. They are
not `USTRUCT`/`UFUNCTION` and must not exist in a shipping build:

```cpp
#if !UE_BUILD_SHIPPING
struct CORSAIRSNET_API FCorsairsSerializedMoveDiagnostic
{
    int64 PacketId = 0;
    FIntPoint SerializedStart = FIntPoint::ZeroValue;
    FIntPoint SerializedEnd = FIntPoint::ZeroValue;
    bool bFromQueuedEndpoint = false;
};
#endif
```

```cpp
#if !UE_BUILD_SHIPPING
TConstArrayView<FCorsairsSerializedMoveDiagnostic>
    GetSerializedMoveDiagnostics() const;
#endif
```

The session owns the matching non-shipping array. Task 8 changes the private
Task 4 helper to:

```cpp
ECorsairsActionRequestResult SendMoveFromConfirmed(
    FIntPoint Endpoint,
    bool bFromQueuedEndpoint);
```

`SubmitPredictedPosition` passes `false`; only the `QueuedEndpoint` branch in
`ApplyReducerEffects` passes `true`. A record is appended only after
`ActionReducer.Begin` returns `Sent`. `SerializedStart` and `SerializedEnd`
must be decoded from the completed byte blob passed to
`Packet.WriteSequence`, not copied from the input `Path`, so the diagnostic
describes the actual MOVE wire payload. A rejected/busy/invalid/failed
transaction appends nothing. ENTERMAP, logout, and disconnect reset both the
record array and `MovementBeginSendCount`; at every observable point their
sizes/counts must agree after the public send/packet-handler call returns.

- [ ] **Step 1: Add RED deterministic fixture tests**

Use a synthetic 25×25 half-meter grid with the start at its center, leaving room for the 500 cm segment plus the 34 cm capsule halo. Selection order is exactly `+X,+Y,-X,-Y`. Free path is the first 500 cm cardinal swept disk whose intersected cells exist and are unblocked. Blocked path scans direction order and then distances 1 through 40 cells; all prior swept cells are free and the terminal cell has bit 128. Require literal endpoints/directions. A no-candidate fixture returns `std::nullopt` and non-empty diagnostics; it must not fabricate a default endpoint or throw across the CLI boundary.

- [ ] **Step 2: Implement fixture CLI**

Add:

```bash
./tools/AssetConverter/build/AssetConverter movement-fixture \
  Client/map/garner.map \
  --start 223325,278475 \
  --capsule-radius 34 \
  --output artifacts/movement/garner.fixture.json
```

The CLI computes source-map SHA-256 with the terrain plan's `Sha256File` before opening the reader and passes the digest into `BuildMovementFixture`. JSON contains that digest, literal start/free/blocked endpoints, directions, and camera yaws.

- [ ] **Step 3: Add RED orchestrator safety tests**

Mock SQL and client execution. Require call order:

```text
snapshot -> set free fixture -> run free -> restore
snapshot -> set blocked fixture -> run blocked -> restore
```

Force the client call to fail and still require character-row restore. Refuse non-loopback DB hosts. Require explicit account and character `Test195126`. Read `AccountServer.dbo.account_login.login_status` before any write and abort if it is not zero; never reset or mutate an active login row. Snapshot and restore only `GameDB.dbo.character(map,map_x,map_y,angle)`.

- [ ] **Step 4: Add RED serialized-MOVE and probe-contract tests**

Register
`Corsairs.Movement.Session.SerializedMoveDiagnostics` in the Task 4 session
test file. Use `TestSendOverride` to parse the captured
`CMD_CM_BEGINACTION/MOVE` packet independently of the diagnostic getter.
Require:

- a direct successful send produces one record whose packet ID and decoded
  first/last points exactly equal the captured wire sequence, with
  `bFromQueuedEndpoint=false`;
- replacing `QueuedEndpoint`, then feeding the matching `ARRIVE`, produces
  exactly one second wire packet and one second record with
  `bFromQueuedEndpoint=true`, `SerializedStart` equal to the ARRIVE endpoint,
  and `SerializedEnd` equal to the latest queued endpoint;
- `Busy`, `Invalid`, and `TransportFailed` append no record;
- every terminal leaves the existing history intact, while ENTERMAP,
  logout, and disconnect clear it together with the diagnostic counter.

Extend `test_run_movement_probe.py` with result manifests containing literal
`serialized_moves` records. RED expectations must reject a manifest when a
record is missing, its counter disagrees, a queue-origin record does not
start at the preceding ARRIVE endpoint, a direct record is mislabeled as
queue-origin, or the blocked held/re-press record deltas do not match the
actual record slices.

- [ ] **Step 5: Implement runtime probe subsystem and wire diagnostics**

Activate only with:

```text
-CorsairsMovementProbe=<manifest>
-CorsairsMovementCase=free|blocked
```

Implement `UCorsairsMovementProbeSubsystem` as `UGameInstanceSubsystem` plus `FTickableGameObject`, with `Initialize`, `Deinitialize`, `Tick`, `IsTickable`, and `GetStatId`. It parses the two command-line switches during `Initialize`, remains inactive when absent, and ticks only in a non-shipping build.

Wait for actual session `InWorld`; assert actual confirmed position equals fixture start. Apply fixture camera yaw and drive `ACorsairsPlayerCharacter::ApplyMovementAxisForProbe`, never direct network calls or private-member access. Save all movement/failure events and packet counts to JSON; exit nonzero on timeout or contract violation.

Read `GetMovementBeginSendCountForDiagnostics()` and `HasPendingMoveForDiagnostics()` from Session. Store counter snapshots before input, after every terminal, after the two-second held interval, and after release/re-press. Assert:

- no second outbound MOVE occurs while one is pending;
- total free-run sends equal `1 +` the number of ARRIVE events that actually exposed one queued endpoint;
- both cases finish with pending empty;
- blocked held interval has counter delta exactly 0;
- blocked release/re-press has counter delta exactly 1.

Read `GetSerializedMoveDiagnostics()` after each snapshot and save this exact
JSON shape:

```json
{
  "serialized_moves": [
    {
      "packet_id": 1,
      "start": [223325, 278475],
      "end": [223825, 278475],
      "from_queued_endpoint": false
    }
  ]
}
```

The runtime subsystem copies the non-shipping records; it must not infer
start/end from actor transforms or movement events. The Python orchestrator
independently rechecks that the final counter equals the record count, the
first free MOVE starts at fixture start, every queue-origin MOVE starts at
the endpoint of the ARRIVE that released it, and its end is the latest
queued intent. For the blocked case it slices records at the saved
post-BLOCK, post-held, and post-re-press indices: held adds zero records,
re-press adds exactly one direct (`from_queued_endpoint=false`) record whose
start is the BLOCK endpoint. Missing diagnostics are a hard failure, not a
reason to trust counter-only assertions.

- [ ] **Step 6: Run live gates**

```bash
cmake --build tools/AssetConverter/build \
  --target AssetConverter AssetConverterTests -j8
ctest --test-dir tools/AssetConverter/build \
  --output-on-failure
"/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
  CorsairsUEEditor Mac Development \
  "$PWD/CorsairsUE/CorsairsUE.uproject" -WaitMutex
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/CorsairsUE/CorsairsUE.uproject" \
  -unattended -nop4 -NullRHI -NoSound \
  -ExecCmds="Automation RunTests Corsairs.Movement" \
  -TestExit="Automation Test Queue Empty"
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  CorsairsUE.Scripts.tests.test_run_movement_probe -v
python3 CorsairsUE/Scripts/run_movement_probe.py \
  --map Client/map/garner.map \
  --start 223325,278475 \
  --account admin \
  --character Test195126 \
  --output artifacts/movement
```

Free case: at least 500 cm, server/client endpoint delta at most 20 cm, no `FAILEDACTION`.

Blocked case: terminal `BLOCK`, pending empty, held W for two seconds creates zero MOVE packets, release/re-press creates exactly one.

- [ ] **Step 7: Verify restoration and commit**

Query the character fixture row after the run and compare it with the saved snapshot. Query `login_status` again and require zero; the orchestrator must never have written that table.

```bash
git add \
  tools/AssetConverter/CMakeLists.txt \
  tools/AssetConverter/include/Corsairs/Tools/AssetConverter/MovementFixture.h \
  tools/AssetConverter/src/MovementFixture.cpp \
  tools/AssetConverter/src/Main.cpp \
  tools/AssetConverter/tests/TestMovementFixture.cpp \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsMovementProbeSubsystem.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsMovementProbeSubsystem.cpp \
  CorsairsUE/Source/CorsairsNet/Public/CorsairsSession.h \
  CorsairsUE/Source/CorsairsNet/Private/CorsairsSession.cpp \
  CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionMovementTests.cpp \
  CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerCharacter.h \
  CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp \
  CorsairsUE/Scripts/run_movement_probe.py \
  CorsairsUE/Scripts/tests/test_run_movement_probe.py
git commit -m "test(ue): verify authoritative Garner movement"
```

## Dependency Order

```text
Task 1
Task 2 -> Task 3 -> Task 4 -> Task 5 -> Task 6 -> Task 7
Garner terrain Tasks 1 + 4 + 7 + Movement Task 7 -> Movement Task 8
```

The first implementation milestone is Tasks 1 through 6: it fixes the user-visible BLOCK desynchronization, initial 0.5-second loss, held-input flood, and incorrect local/remote character height. Tasks 7 and 8 then prove server-driven movement and live behavior.
