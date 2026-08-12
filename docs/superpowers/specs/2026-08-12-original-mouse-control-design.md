# Управление мышью, камерой и ручное наведение навыков в UE-клиенте

**Дата:** 2026-08-12
**Статус:** на согласовании
**Область:** `CorsairsUE`; серверный протокол и GameServer не изменяются

## Цель

Вернуть управление, определяющее игровой ритм оригинального клиента:

- ЛКМ по земле строит путь и перемещает персонажа;
- ЛКМ по конкретному персонажу или монстру адресует именно его;
- навык по цели ждёт точного клика по актору;
- площадной навык ждёт точного клика по земле;
- поиск ближайшей цели, автоподхват цели и автонаведение запрещены;
- ПКМ управляет камерой, колесо меняет дистанцию, камера следует за героем;
- NPC открывается кликом, при необходимости герой сначала подходит к нему.

Результат должен позволить проверять перемещение, NPC и PvP-наведение на живом
сервере, не подменяя серверную авторитетность клиентской логикой.

## Подтверждённое поведение оригинала

Оригинальный клиент сначала отдаёт мышь GUI, затем выполняет точный hit-test мира.
Выбранный актор сохраняется как конкретная цель; перебора окружающих акторов и
поиска ближайшего подходящего объекта нет.

Маршрутизация ЛКМ:

1. Подготовлен площадной навык — используются точные координаты земли.
2. Подготовлен навык по сущности — используется только допустимый актор под
   курсором; недопустимый актор не превращает click в разговор или движение.
3. Навык не подготовлен, под курсором NPC — подход и разговор.
4. Навык не подготовлен, под курсором допустимая боевая сущность — default skill
   по этой сущности.
5. Иначе — движение к точке земли.

Проводной контракт навыка различает две формы цели:

- `apply_type` 1 или 3: `tarInfo1=WorldId`, `tarInfo2=Handle`;
- `apply_type` 2: `tarInfo1=X`, `tarInfo2=Y`.

Обе формы отправляются как `CMD_CM_BEGINACTION / ACTION_SKILL`, `chMove=2`, с
путём в source-координатах. Сервер повторно проверяет цель, дистанцию, путь,
экипировку, cooldown, safe zone, PvP, команду и состояние сущностей.

Камера в оригинале следует за главным персонажем. Пока зажата ПКМ, движение мыши
по горизонтали вращает камеру; колесо меняет дистанцию. Короткая ПКМ отменяет
подготовленный навык. Обычный режим вертикального вращения не использует.

## Неподвижные правила

1. **Никакого автонаведения.** Нельзя выбирать ближайшего актора, цель из
   предыдущего кадра или первую сущность из registry, если курсор её не задел.
2. **Identity важнее UObject.** Долгое состояние хранит `WorldId+Handle`, а не
   сырой указатель на актора. Перед отправкой identity проверяется повторно.
3. **Сервер остаётся авторитетным.** Клиент блокирует структурно невозможные и
   заведомо недопустимые по имеющимся данным запросы. Полную проверку
   PvP/team/guild/safe-zone повторяет GameServer.
4. **Движение не телепортирует.** Локальный pawn проигрывает принятый путь и
   сверяется с подтверждённой сервером позицией.
5. **Координатный базис единственный:** source `(X,Y)` ↔ UE `(-Y,+X)`, обратное
   преобразование — `(UE.Y,-UE.X)`.
6. **Fail closed.** Без карты высот, block/region raster, skill catalog или
   валидной identity команда не отправляется; причина видна в HUD/логе.

## Архитектура

### PlayerController владеет мышью

Добавляется `ACorsairsPlayerController`. `ACorsairsGameMode` назначает его через
`PlayerControllerClass`.

Контроллер владеет:

- видимым курсором и режимом захвата мыши;
- точным actor hit-test;
- ground pick;
- подготовленным навыком;
- выбранной identity;
- отложенным подходом к NPC;
- запросами движения и замены текущего маршрута;
- RMB-состоянием камеры.

`ACorsairsPlayerCharacter` остаётся исполнителем пути и владельцем SpringArm.
`UCorsairsSession` остаётся единственной границей протокола и reducer-а.

```text
ЛКМ/ПКМ/колесо
      │
      ▼
ACorsairsPlayerController
  ├─ exact actor trace ───────────────┐
  ├─ deterministic ground picker      │
  ├─ click resolver                   │
  └─ path/target intent               │
                                      ▼
                             UCorsairsSession
                                      │
                              reducer + transport
                                      │
                                      ▼
                                 GameServer
                                      │
                                      ▼
                         FCorsairsServerPathFollower
                                      │
                                      ▼
                        pawn + attached SpringArm
```

### Чистый resolver

`FCorsairsWorldClickResolver` получает снимок состояния, exact hit и модификаторы
и возвращает одно намерение без доступа к UE World и сети:

- `MOVE_TO_POINT`;
- `APPROACH_AND_TALK`;
- `USE_ENTITY_SKILL`;
- `USE_GROUND_SKILL`;
- `SELECT_ONLY`;
- `REJECTED` с причиной.

Resolver не имеет API для списка ближайших акторов. Этим архитектурно исключается
случайное появление автонаведения.

### Состояние targeting

```text
NONE
  ├─ hotkey entity skill ──> ENTITY_SKILL_READY
  └─ hotkey area skill ────> GROUND_SKILL_READY

ENTITY_SKILL_READY
  ├─ exact valid actor LMB ─> send ─> NONE
  ├─ invalid/ground LMB ────> остаётся READY
  └─ short RMB/reset ───────> NONE

GROUND_SKILL_READY
  ├─ valid ground LMB ──────> send ─> NONE
  ├─ invalid ground LMB ────> остаётся READY
  └─ short RMB/reset ───────> NONE
```

Подготовленный навык имеет приоритет над обычным NPC/attack-routing, как в
оригинале. Он очищается только после успешной постановки пакета в transport,
явной отмены, удаления навыка из skill bag, logout/reset или потери мира.

## Режим мыши и приоритет UI

В обычном игровом режиме:

- `bShowMouseCursor=true`;
- viewport не захватывает и не блокирует курсор постоянно;
- UI получает событие первым;
- gameplay-click исполняется только если UI событие не обработал.

На зажатии ПКМ контроллер временно захватывает и скрывает курсор. На отпускании
возвращает видимость и прежнюю экранную позицию.

Actor hit-test выполняется по отдельному gameplay trace channel. На нём находятся
капсулы серверных персонажей, NPC и монстров, но не декоративные mesh-компоненты
города. Точный miss остаётся miss: никакой screen-space proximity fallback не
допускается.

## Ground pick и проходимость

Ground point определяется детерминированным пересечением camera ray с runtime
height field, а не первым `WorldStatic` mesh: здания не должны перехватывать
земляной клик. Решатель ограничен bounds текущей карты и использует тот же
`FCorsairsCharacterGround`, что и привязка персонажей к поверхности.

Runtime-набор карты расширяется:

- `<map>.height.r16`;
- `<map>.block.raw`;
- `<map>.region.raw`;
- manifest с размерами, bounds и хешами.

Отсутствующий raster, выход за bounds, неверный размер или неизвестная region
считаются непроходимыми. Текущее поведение `Sample()` «OOB = свободно» для
маршрутизации не используется.

## Click-to-move

Путь строится в source-координатах на half-cell raster:

- размер клетки — 50 source units;
- каждая передаваемая точка — центр `50*k+25`;
- сначала проверяется прямая видимость;
- если она закрыта, normal click использует legacy 8-connected FIFO BFS;
- порядок соседей: N, S, W, E, NE, SE, SW, NW;
- путь сжимается по сменам направления;
- blocked target отступает по лучу к start до ближайшей свободной клетки;
- legacy BFS corner-cutting сохраняется буквально; сервер остаётся финальным
  арбитром коллизии;
- если полный путь не найден, допускается только legacy straight-prefix до
  последней свободной клетки, без прыжка через препятствие.

Пакет MOVE содержит строго 2–32 точки и не более 256 байт path blob. Значения и
количество проверяются до сериализации, чтобы исключить серверное narrowing и
signed-char overflow.

Если сжатый маршрут длиннее 32 точек, отправляется безопасный префикс. После
server terminal путь пересчитывается от новой подтверждённой позиции к исходной
цели. Последняя точка не подменяется недостижимым прямым endpoint.

Повторный ЛКМ во время активного действия:

1. сохраняет только последнее намерение;
2. отправляет `CMD_CM_ENDACTION` один раз;
3. ждёт terminal/reset reducer-а;
4. заново строит путь от подтверждённой позиции;
5. отправляет новый action.

Одинаковый target в пределах 100 мс подавляется. Все интервалы считаются через
`std::chrono::steady_clock`, не по кадрам.

Удержание ЛКМ дольше 400 мс включает continuous walk; point пересчитывается не
чаще одного раза в 500 мс и использует straight-only режим. В отличие от
очевидной legacy-ошибки continuous walk прекращается при отпускании ЛКМ, а не на
следующем нажатии.

Production WASD и периодический `SubmitPredictedPosition` выключаются. Они не
работают параллельно с click-to-move. Тестовые probe-методы могут остаться, но не
привязываются к игровому input.

## NPC

Без подготовленного навыка точный ЛКМ по `NPC` или `NPC_EVENT` создаёт intent
`APPROACH_AND_TALK`.

- Если подтверждённая дистанция не больше 300 source units, сразу вызывается
  `TalkToNpc(WorldId)`.
- Иначе вычисляется точка примерно в 200 units от NPC по линии игрок→NPC,
  строится путь, а `WorldId+Handle` сохраняются как pending interaction.
- После terminal pending identity повторно проверяется; если NPC ещё видим и
  дистанция допустима, отправляется `TalkToNpc`.
- Новый click, short RMB, logout, `ActorLeft`, смена Handle или ошибка движения
  отменяют pending interaction.

Другой NPC никогда не подставляется вместо исчезнувшего.

## Навыки и ручное PvP-наведение

### Состояние skill bag

`UCorsairsSession` начинает хранить и публиковать:

- `defSkillId` из `ENTERMAP.skillBag`;
- полный список навыков из `ChaSkillBagInfo`;
- 36 shortcut entries из `ENTERMAP.shortcut`;
- обновления `CMD_MC_SYNSKILLBAG`;
- обновления `CMD_MC_SYNDEFAULTSKILL`.

На logout/reset состояние обнуляется. Обновление bag, удалившее подготовленный
навык, отменяет targeting.

### Skill catalog

Runtime не читает SQLite. Отдельный build script создаёт проверяемый
`CorsairsUE/Data/skills.json` из `databases/gamedata.sqlite`. Для targeting нужны:

- `id`, `name`;
- `applyType`, `applyTarget`, `helpful`;
- `applyDistance`, `targetHabitat`;
- форма и радиус range.

`applyType` 1/3 означает entity target, 2 — ground/area target. Неизвестный тип,
отсутствующая запись, non-finite/overflow или противоречивые поля запрещают
подготовку навыка.

### Hotkeys и HUD

Первая реализация использует исходные 12 слотов текущей строки:

- `F1`–`F12` подготавливают fight skill из shortcut;
- повторное нажатие той же клавиши оставляет тот же навык подготовленным, как в
  оригинальном клиенте; явная отмена выполняется короткой ПКМ;
- Canvas HUD показывает 12 слотов, имя подготовленного навыка и тип цели;
- предметы, life/sail skills и редактирование панели пока отображаются, но не
  исполняются этим gameplay-router;
- number-row режим добавляется позже как настройка, а не дублируется скрыто.

Обычный ЛКМ по боевой сущности использует `defSkillId`, но цель всё равно должна
быть exact hit под курсором.

### Entity skill

Перед отправкой проверяются:

- навык есть в актуальном bag и catalog;
- `applyType` равен 1 или 3;
- actor hit точный;
- `WorldId+Handle` совпадают с текущей server identity;
- идентификаторы входят в проводной диапазон;
- obvious target class не противоречит `applyTarget`.

Client-side недостаточно данных для полной проверки team/guild/side/PK. Перед
живым PvP-acceptance эти поля добавляются в world DTO из уже получаемых server
messages; obvious invalid target блокируется локально, а GameServer повторяет
всю проверку. Клиент никогда не ищет «другую допустимую цель»: отказ оставляет
тот же targeting state и отображается без смены цели.

Добавляется протокольный путь, принимающий exact entity target и заранее
построенный approach path. Существующий reducer резервирует ровно одну action.

### Ground/area skill

Добавляется `UseSkillAtPoint` с exact source `(X,Y)` и заранее построенным путём.
В packet `tarInfo1=X`, `tarInfo2=Y`; актор под курсором не заменяет ground point.
Путь обязателен и содержит 2–32 валидных points.

### Приоритет обычного клика

Если skill не подготовлен:

1. NPC/NPC_EVENT — approach-talk;
2. MONS и допустимый resource target — default skill по exact actor;
3. PLAYER — default skill по exact actor; сервер решает PvP/team/safe-zone;
4. неподдерживаемая сущность — `SELECT_ONLY`;
5. земля — движение.

`Ctrl`-force, `Alt`-follow и `Shift`-repeat будут отдельным parity-slice после
базового ручного targeting. До этого они не получают скрытого альтернативного
поведения.

## Камера

SpringArm остаётся дочерним компонентом pawn, поэтому camera target следует за
героем каждый кадр без копирования позиции в контроллер.

- MouseX без ПКМ ничего не вращает.
- MouseY в parity-режиме ничего не вращает.
- Пока ПКМ удерживается, MouseX меняет только yaw.
- Короткая ПКМ до 200 мс без drag отменяет prepared skill и pending intent.
- Drag определяется порогом экранного смещения, а не только временем.
- Двойная ПКМ возвращает исходный yaw Q-базиса.
- Колесо проходит по существующему legacy camera profile и жёстко ограничивается
  его диапазоном дистанции/FOV.
- Вращение камеры не поворачивает персонажа и не меняет серверный `Angle`.

## Сброс и ошибки

Targeting, selection и pending intents очищаются при:

- выходе из `InWorld`;
- logout/disconnect;
- смене карты;
- `ActorLeft` соответствующей identity;
- полной замене skill bag;
- protocol/reducer reset.

Transport failure не считается успешным использованием. При `Busy` prepared
skill сохраняется; при структурной ошибке сохраняется до явной отмены, а HUD
показывает причину. Серверный action failure завершает reducer и позволяет новый
click, но не выбирает цель автоматически.

## Проверка по фактам

Тесты ограничиваются наблюдаемыми контрактами, а не искусственной матрицей всех
внутренних строк:

1. Pure resolver: exact actor/ground routing и доказательство отсутствия nearest
   fallback.
2. Pathfinder fixtures: grid centers, OOB, blocked target, LOS/BFS, compression,
   safe prefix и предел 32.
3. Protocol behavior: MOVE, ENDACTION, entity skill и ground skill с точными
   полями и повторной identity-проверкой.
4. Session behavior: ENTERMAP/sync skill bag, default skill и reset.
5. World behavior: actor trace, UI precedence, NPC approach, ActorLeft cancel.
6. Camera behavior: visible cursor, RMB-only yaw, short cancel, wheel clamp,
   double-click reset и follow pawn.
7. Один живой acceptance-прогон на текущем сервере.

Живой acceptance считается пройденным, когда `Test195126`:

- кликом по земле идёт по Garner, повторный click заменяет маршрут;
- кликом по Pappa подходит и отправляет разговор именно его `WorldId`;
- кликом по конкретному монстру применяет default skill только к нему;
- `F`-hotkey entity skill ждёт actor click;
- area skill ждёт ground click и отправляет exact coordinates;
- движение курсора без ПКМ не вращает камеру;
- ПКМ вращает camera yaw, колесо меняет дистанцию, камера следует за бегущим
  персонажем;
- сетевой лог не содержит target, которого не было под курсором.

## Этапы реализации

1. **Мышь, camera и click-to-move:** PlayerController, cursor mode, ground picker,
   block+region pathfinder, cancel/replan, отключение production WASD.
2. **NPC:** exact actor trace, approach-and-talk, pending identity/reset.
3. **Skill state:** skill bag/default/shortcut ingestion и generated catalog.
4. **Target policy:** перенос доступных team/guild/side/PK признаков в world DTO
   и локальный obvious-invalid gate без замены серверной проверки.
5. **Manual targeting:** entity/ground packet paths, F1–F12, HUD feedback.
6. **Live acceptance:** один последовательный build, focused automation и
   подключение к уже работающему серверному стеку.

Каждый этап должен давать видимое игровое поведение. Широкие corpus-проверки и
повторные тяжёлые сборки не запускаются между мелкими правками; финальная сборка
делается один раз после focused GREEN.

## Вне текущего объёма

- nearest-target, tab-target и любые варианты autoaim;
- полноценный UMG skill bar, drag-and-drop, cooldown FX и иконки;
- эффекты навыков, combat animation parity и damage presentation;
- локальная копия правил safe-zone и формул боя: эти проверки остаются на
  сервере; клиент получает лишь данные для obvious target gate;
- item/life/sail shortcut execution;
- `Ctrl` force, `Alt` follow, `Shift` repeat;
- генерация region raster для всех карт — первая реализация закрывает Garner,
  общий rollout выполняется после живого acceptance.
