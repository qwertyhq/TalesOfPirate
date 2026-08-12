# Original Mouse Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** заменить тестовое WASD-управление на оригинальную схему: точный клик по земле/сущности, серверный путь и скилл по выбранной цели, камера за героем с RMB-поворотом и колесом масштаба, 12 горячих клавиш из серверного shortcut bag.

**Architecture:** `ACorsairsPlayerController` становится единственным владельцем мыши, курсора, подготовленного скилла и очереди намерений. Чистые сервисы `FCorsairsGroundPicker`, `FCorsairsMapPathfinder` и `FCorsairsWorldClickResolver` переводят ввод в детерминированный запрос; `UCorsairsSession` остаётся владельцем сетевого состояния, точной identity сущностей и action lifecycle. Сервер не меняется и остаётся авторитетом.

**Tech Stack:** Unreal Engine 5.8 C++23, Automation Tests, Python 3 standard library, SQLite read-only export, MessagePack protocol from `CorsairsNet`.

**Canonical design:** `docs/superpowers/specs/2026-08-12-original-mouse-control-design.md`.

## Неподвижные ограничения

- Все игровые комментарии — на русском языке.
- Никакого ближайшего таргета, автодоводки или замены кликнутого `WorldId`.
- Для сущности wire identity — точные `WorldId + Handle`; перед отправкой identity сверяется повторно.
- Для земли wire target — точные source coordinates; путь может использовать центры raster cells.
- Навигация использует source basis; UE → source: `(round(Y), round(-X))`.
- Путь содержит от 2 до 32 `Point`, не более 256 bytes.
- Новый клик во время MOVE сначала посылает ровно один `CMD_CM_ENDACTION`, затем ждёт terminal и перепланирует от подтверждённой позиции.
- Production WASD и периодическая отправка predicted position удаляются.
- Клиент не рассчитывает урон, попадание, cooldown или PvP-исход.
- До финального live gate UE-команды выполняются строго по одному процессу; серверный стек не перезапускается.

## Task 1: Детерминированный каталог скиллов

**Files:**

- Create: `CorsairsUE/Scripts/build_skill_catalog.py`
- Create: `CorsairsUE/Scripts/tests/test_build_skill_catalog.py`
- Create: `CorsairsUE/Data/skills.json`
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsSkillCatalog.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsSkillCatalog.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsSkillCatalogTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs`

**Contract:**

```cpp
enum class ECorsairsSkillTargetMode : uint8
{
    Entity,
    Ground,
    Unsupported,
};

struct FCorsairsSkillDefinition
{
    int64 SkillId = 0;
    FString Name;
    int32 ApplyDistance = 0;
    int32 ApplyTarget = 0;
    int32 ApplyType = 0;
    bool bHelpful = false;
    int32 HabitatMask = 0;
    int32 Radius = 0;
    int32 Shape = 0;
    ECorsairsSkillTargetMode TargetMode = ECorsairsSkillTargetMode::Unsupported;
};
```

- Экспорт читать SQLite URI `mode=ro` и выполнять ровно один ordered query к `skills`.
- JSON — `schemaVersion=1`, записи отсортированы по `skillId`, UTF-8, конечный LF, атомарная публикация.
- Literal fixtures: skill 1 — entity, skill 4 — ground; полный текущий census — 411 уникальных IDs.
- Runtime loader отклоняет duplicate, unknown fields, неверные типы/диапазоны и неподдерживаемый schema version.

**TDD steps:**

1. Добавить Python RED на deterministic bytes, skill 1/4 и duplicate rejection.
2. Запустить:

   ```bash
   python3 -B -m unittest CorsairsUE.Scripts.tests.test_build_skill_catalog -v
   ```

   Ожидается RED из-за отсутствующего production script.
3. Реализовать exporter, получить Python GREEN, сгенерировать `skills.json` в новый temp и атомарно опубликовать.
4. Добавить C++ RED loader tests: valid lookup, duplicate, bool-as-int, unknown target mode, missing file.
5. Реализовать loader и зарегистрировать `skills.json` как UFS RuntimeDependency.
6. Self-review: убедиться, что runtime не открывает SQLite и не содержит fallback-каталога.

## Task 2: Серверное состояние skill bag, shortcuts и target policy

**Files:**

- Modify: `CorsairsUE/Source/CorsairsNet/Public/CorsairsSession.h`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/CorsairsSession.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionActorStateTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionMovementTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Public/CorsairsWorldActor.h`

**Contract:**

```cpp
struct FCorsairsSkillEntry
{
    int64 SkillId = 0;
    int32 Level = 0;
    int32 State = 0;
};

struct FCorsairsShortcutEntry
{
    int32 Slot = 0;
    int32 Type = 0;
    int64 GridId = 0;
};

struct FCorsairsTargetPolicy
{
    int64 GuildId = 0;
    int64 TeamLeaderId = 0;
    int32 SideId = 0;
    int32 PkControl = 0;
};
```

- `ENTERMAP` заменяет skill bag, default skill, shortcut bag и local policy одним консистентным snapshot.
- `CMD_MC_SYNSKILLBAG`: INIT заменяет, ADD upsert, MODI upsert или удаляет при `Level <= 0`; неизвестный sync type fail-closed и сохраняет прежнее состояние.
- `CMD_MC_SYNDEFAULTSKILL` меняет только local default.
- `CMD_MC_TLEADER_ID`, `CMD_MC_SIDE_INFO`, `CMD_MC_GUILD_INFO`, `PK_CTRL` обновляют policy конкретной сущности по exact WorldId.
- Logout/disconnect очищают bag, shortcuts, prepared prerequisites и policy.
- Добавить read-only getters и отдельные delegates `OnSkillStateChanged` / `OnTargetPolicyChanged`.

**TDD steps:**

1. Добавить RED packet tests через существующие `serialize → RPacket → HandlePacketForTests`.
2. Проверить INIT/ADD/MODI/delete, default, 12 shortcut slots, live policy update, malformed packet, unknown actor и reset.
3. Запустить один focused Editor build; RED должен быть compile/test failure только по новым API.
4. Реализовать DTO/state без дублирования deserializers из `CommandMessages.h`.
5. Запустить `Corsairs.Net.Session` focused suite.
6. Self-review: getters возвращают копии/const view; ни один delegate не вызывается до полного state commit.

## Task 3: Строгий MOVE/SKILL wire и отмена active action

**Files:**

- Modify: `CorsairsUE/Source/CorsairsNet/Public/CorsairsActionReducer.h`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/CorsairsActionReducer.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsActionReducerTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Public/CorsairsSession.h`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/CorsairsSession.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionMovementTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsNet/Private/Tests/CorsairsSessionSkillTests.cpp`

**Contract:**

```cpp
ECorsairsActionRequestResult FCorsairsActionReducer::RequestCancel(
    TFunctionRef<bool()> SendCancel);

bool UCorsairsSession::EndActiveAction();

ECorsairsActionRequestResult UCorsairsSession::UseSkillOn(
    int64 SkillId,
    int64 TargetWorldId,
    int64 TargetHandle,
    const TArray<FIntPoint>& ApproachPath);

ECorsairsActionRequestResult UCorsairsSession::UseSkillAtPoint(
    int64 SkillId,
    FIntPoint TargetPoint,
    const TArray<FIntPoint>& ApproachPath);
```

- `SendMovePath` принимает только 2..32 точки и не более 256 bytes до выделения payload.
- `EndActiveAction` сериализует существующий `serializeCmEndActionCmd()`, резервирует cancel до send, повторный вызов возвращает Busy без второго пакета, send failure откатывает reservation.
- Terminal/reset очищает cancel reservation; новый BEGIN до terminal запрещён.
- Entity SKILL использует `serializeCmBeginActionHeader(..., SKILL)`, `SkillId`, `TargetWorldId` как uint32 wire, `TargetHandle` как int32, затем approach path.
- Ground SKILL использует тот же header, точные X/Y как target и отдельный approach path.
- Bag membership, level, target identity и 2..32 path проверяются до reducer и повторно внутри send callback.

**TDD steps:**

1. RED: MOVE 1/33 reject без send; 2/32 exact bytes.
2. RED: ENDACTION command-only, один send, Busy на повторе, transport failure rollback, terminal release.
3. RED: entity exact ID+handle, ground exact point, stale identity, absent skill, malformed path.
4. Усилить reentrant test: observer вызывает второй action во время synchronous movement notification; новый state уже должен быть виден.
5. Реализовать через общий private `SendSkillAction`, не копируя packet header вручную.
6. Запустить `Corsairs.Net.ActionReducer` и `Corsairs.Net.Session` focused suites.

## Task 4: Runtime region и fail-closed ground navigation

**Files:**

- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsCharacterGround.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsCharacterGround.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCharacterGroundTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/CorsairsGame.Build.cs`
- Runtime generated input: `CorsairsUE/Data/Heights/garner.region.raw` (ignored, SHA-verified before install)

**Contract:**

```cpp
enum class ECorsairsTraversalKind : uint8
{
    Land,
    Sea,
    Discretionary,
};

struct FCorsairsNavigationCell
{
    int32 HeightCm = 0;
    uint16 RegionMask = 0;
    bool bBlocked = true;
};

bool TrySampleSurface(FVector2d SourcePoint, double& OutHeightCm) const;
bool TrySampleNavigation(
    FIntPoint SourcePoint,
    ECorsairsTraversalKind Traversal,
    FCorsairsNavigationCell& OutCell) const;
bool IsNavigationLoaded() const;
FIntRect GetSourceBounds() const;
```

- `Load()` читает block и region; отсутствие, неверный размер или unknown region bits делает navigation unavailable и возвращает ошибку.
- `.block.raw`: четыре half-cells на tile; `0x80` blocked, signed 7-bit height ×5 cm.
- `.region.raw`: little-endian uint16 на tile; известная маска `0x007f`.
- Land проходит только `LAND|BRIDGE`; Sea — только без LAND; Discretionary — любой известный region.
- OOB/unloaded всегда fail closed; старый `Sample()` остаётся только для совместимого pawn grounding и не используется pathfinder.
- Runtime dependency включает ignored generated region рядом с уже установленным block.

**TDD steps:**

1. RED на exact quadrants, LE region, land/bridge/sea, unknown bits, short buffers, OOB.
2. Реализовать byte-loader и production file load.
3. Сверить retained region source: размер 33,554,432 bytes, SHA-256 `7d453c8513bfb1338cb5c122c94485ea1a9b5137340421212acbbdbdb8729aac`.
4. Установить файл только после SHA/size check; исходный retained artifact не изменять.
5. Self-review: path API не вызывает permissive `Sample()`.

## Task 5: Детерминированный picker и legacy pathfinder

**Files:**

- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsGroundPicker.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsGroundPicker.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsGroundPickerTests.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsMapPathfinder.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsMapPathfinder.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsMapPathfinderTests.cpp`

**Contract:**

```cpp
enum class ECorsairsPathMode : uint8 { Normal, StraightOnly };
enum class ECorsairsPathStatus : uint8 { Found, Partial, Invalid };

struct FCorsairsPathResult
{
    ECorsairsPathStatus Status = ECorsairsPathStatus::Invalid;
    TArray<FIntPoint> Waypoints;
    FIntPoint RequestedTarget = FIntPoint::ZeroValue;
    FIntPoint ResolvedTarget = FIntPoint::ZeroValue;
    bool bTruncated = false;
};
```

- Picker принимает camera ray, переводит его inverse-Q, пересекает только source bounds/heightfield с шагом не более 25 source units и детерминированной бисекцией; WorldStatic collision не является входом API.
- Pathfinder квантует точки в `50*k+25`.
- Blocked target отступает по лучу к start.
- Сначала straight supercover; diagonal LOS проверяет обе ортогональные клетки.
- Normal fallback — FIFO 8-neighbor в порядке N,S,W,E,NE,SE,SW,NW; для совместимости legacy BFS corner-cut сохраняется.
- Direction-run compression; при no-route — только безопасный straight prefix.
- Search budget — максимум 16,384 посещённых cells; budget exhaustion даёт `Partial`, не зависание.
- Wire prefix не более 32; последний элемент никогда не подменяется недостижимым target; continuation хранит исходный `RequestedTarget`.

**TDD steps:**

1. RED picker: flat/ramp, inverse Q, miss, unloaded/OOB.
2. RED path: centers, LOS, blocked retreat, deterministic tie, corner cut, compression, straight-only, no-route prefix, >32 safe prefix, budget exhaustion.
3. Реализовать чистые классы без UObject/World access.
4. Запустить `Corsairs.Movement.GroundPicker` и `Corsairs.Movement.Pathfinder`.

## Task 6: Чистый resolver точного клика

**Files:**

- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsWorldClickResolver.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsWorldClickResolver.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsWorldClickResolverTests.cpp`

**Contract:**

```cpp
enum class ECorsairsClickIntentType : uint8
{
    Move,
    Talk,
    EntitySkill,
    GroundSkill,
    SelectOnly,
    Rejected,
};

struct FCorsairsClickIntent
{
    ECorsairsClickIntentType Type = ECorsairsClickIntentType::Rejected;
    int64 SkillId = 0;
    int64 TargetWorldId = 0;
    int64 TargetHandle = 0;
    FIntPoint GroundPoint = FIntPoint::ZeroValue;
    FString Reason;
};
```

- Приоритет: UI/login gate → prepared skill → NPC interaction → default combat skill → ground move.
- Prepared entity skill без exact actor отклоняется; prepared ground skill без exact ground point отклоняется.
- Entity skill никогда не переключается на другую сущность.
- Obvious-invalid gate использует catalog + local/remote target policy: NPC не combat target; harmful self/same known team/side отклоняется; resource skill принимает только соответствующий CtrlType; неизвестная политика не объявляется валидной автоматически для явно ограниченного target class.
- Нормальный NPC-клик выдаёт Talk; нормальный monster/player-клик выдаёт default entity skill только если default присутствует в bag.

**TDD steps:**

1. RED truth table минимум из 20 cases, включая no fallback и stale handle.
2. Реализовать resolver как pure value transformation.
3. Mutation check: удаление identity comparison, перестановка prepared/NPC priority и nearest-target stub обязаны ломать тесты.

## Task 7: PlayerController, RMB-камера и очередь намерений

**Files:**

- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerController.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerController.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsPlayerControllerTests.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsMouseControlState.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsMouseControlState.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsMouseControlStateTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsCameraProfile.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsCameraProfile.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsCameraProfileTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsPlayerCharacter.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsPlayerCharacter.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsLegacyInputTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsPlayerMovementTests.cpp`
- Modify: `CorsairsUE/Config/DefaultInput.ini`
- Modify: `CorsairsUE/Config/DefaultEngine.ini`

**Behavior:**

- LMB deprojects cursor; character trace uses owned character channel; ground uses `FCorsairsGroundPicker`.
- Duplicate exact target inside 100 ms is suppressed.
- Held LMB after 400 ms switches to StraightOnly and resamples no faster than 500 ms; release stops hold immediately.
- Active MOVE: latest click overwrites pending intent, exactly one ENDACTION is sent, terminal replans from `Session.GetConfirmedPosition()`.
- Truncated ARRIVE replans to original requested target.
- RMB press stores cursor and captures it; only MouseX changes yaw. Release restores visible cursor and exact prior position.
- RMB shorter/equal 200 ms below drag threshold clears prepared action and requests cancel; drag does not.
- RMB double-click resets legacy camera yaw; wheel applies original small impulse and clamps profile.
- F1..F12 prepare exact shortcut skill; повторное нажатие того же слота
  идемпотентно оставляет его подготовленным, а короткая RMB отменяет targeting;
  unsupported entry reports reason and sends nothing.
- PlayerCharacter no longer binds production MoveForward/MoveRight/Turn/LookUp and no longer calls periodic `SubmitPredictedPosition`.
- Spring arm stays attached to pawn; network Angle/pawn rotation is not changed by camera yaw.

**TDD steps:**

1. Pure RED mouse state tests: 100/400/500/200 ms, latest-wins, cursor restore, zoom clamps.
2. Controller RED with real test world/input: visible cursor, UI gate, entity trace, ground picker, one cancel, terminal replan, F slots.
3. Camera RED: follow pawn, yaw only RMB, MouseY ignored, double reset, zoom extrema.
4. Legacy input RED: W/A/S/D and free MouseX send no MOVE/change; no periodic predicted packets.
5. Реализовать controller/state and remove pawn production bindings.
6. Run `Corsairs.Input`, `Corsairs.Camera`, `Corsairs.Movement.Routing` separately.

## Task 8: GameMode wiring, NPC interaction и Canvas HUD

**Files:**

- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsGameMode.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsGameMode.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsMovementRoutingTests.cpp`
- Modify: `CorsairsUE/Source/CorsairsGame/Public/CorsairsLoginHud.h`
- Modify: `CorsairsUE/Source/CorsairsGame/Private/CorsairsLoginHud.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Public/CorsairsSkillHudPresentation.h`
- Create: `CorsairsUE/Source/CorsairsGame/Private/CorsairsSkillHudPresentation.cpp`
- Create: `CorsairsUE/Source/CorsairsGame/Private/Tests/CorsairsSkillHudPresentationTests.cpp`

**Behavior:**

- `PlayerControllerClass = ACorsairsPlayerController::StaticClass()`.
- GameMode загружает ground navigation и skill catalog до InWorld; controller получает pointers только после успешной активации и отсоединяется в EndPlay/failure.
- Invalid server identity destroys spawned actor before registry publication; test проверяет `IsActorBeingDestroyed()`.
- NPC Talk использует exact clicked WorldId, строит approach path до допустимой дистанции и после ARRIVE вызывает `TalkToNpc`; ActorLeft очищает target/dialogue.
- HUD рисует 12 slots, name/level/state, prepared status, exact target name и причину rejected action. HUD ничего не таргетит и не вызывает Session напрямую.
- Login/character UI consumes input before world resolver.

**TDD steps:**

1. RED GameMode class/wiring and fail-closed actor destruction.
2. RED NPC end-to-end fixture: click exact NPC → path → ARRIVE → exact `CMD_CM_REQUESTNPC`; monster/player/ground не открывают talk.
3. RED pure HUD presentation for 12 slots and prepared/rejected labels.
4. Реализовать wiring/presentation; никаких UMG assets в этом slice.
5. Run `Corsairs.Movement.Routing`, `Corsairs.Targeting`, `Corsairs.Hud`.

## Task 9: Единая проверка и live acceptance

**Files:**

- Modify only if a real failure proves it: files owned by Tasks 1–8.
- Record evidence: `artifacts/gameplay/mouse-control/<run-id>/` (ignored diagnostics).

**Steps:**

1. Run Python focused suites.
2. Build once from final source:

   ```bash
   nice -n 10 "/Users/Shared/Epic Games/UE_5.8/Engine/Build/BatchFiles/Mac/Build.sh" \
     CorsairsUEEditor Mac Development "$PWD/CorsairsUE/CorsairsUE.uproject" \
     -WaitMutex -MaxParallelActions=4
   ```

3. Run focused automation prefixes in separate UE processes: `Corsairs.Net`, `Corsairs.Movement`, `Corsairs.Targeting`, `Corsairs.Input`, `Corsairs.Camera`, `Corsairs.Hud`.
4. Build Game target once.
5. Confirm no stale UE client; keep four existing server processes alive.
6. Launch `/Game/Maps/GarnerSceneProgressPlay` and accept only these facts:
   - cursor visible; W/A/S/D do not move;
   - one ground click moves the player and camera follows;
   - second click during move sends one cancel and follows the new route;
   - RMB rotates camera only, release restores cursor, wheel zooms within clamps;
   - exact click on Pappa approaches and opens server dialogue;
   - F-slot entity skill uses only clicked entity; ground skill uses only clicked ground point;
   - click on an obviously invalid target sends no SKILL;
   - no packet exceeds 32 route points; no crash/assert/default fallback.
7. Inspect client/server logs for MOVE/SKILL/NPC command sequence and absence of protocol errors.
8. Run `git diff --check`, scoped status, artifact/process audit.
9. Request independent code review, fix only verified blockers, then commit in logical slices.

## Execution order and parallelism

- Task 1 may run on Luna independently.
- Tasks 2–3 run sequentially on Sol because they share Session/Reducer.
- Tasks 4–5 run sequentially on Sol because they share Ground.
- Task 6 can run in parallel after Task 1+2 public DTOs are frozen.
- Tasks 7–8 run sequentially in the shared worktree.
- Task 9 is single-owner and never runs multiple UE processes concurrently.

## Done definition

- Не только tests: live client demonstrates click-to-move, following camera, exact NPC interaction and at least one exact entity/ground skill request.
- No auto-targeting path exists in production code.
- Server remains authoritative and unmodified.
- Generated ignored map data and Content are explicitly recorded in handoff; branch alone is not falsely claimed portable.
