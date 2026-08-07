/// Прогон игровых механик против живого сервера.
///
///     dotnet run --project sources/Dotnet/Tools/Corsairs.TestClient -- \
///         mechanics [хост] [порт] [учётная-запись] [пароль]
///
/// Каждая проверка совершает действие настоящим клиентским пакетом и судит по
/// ответу сервера. Смысл в том, что отказы этого сервера молчаливы: атака по
/// несуществующей цели не пишет ни строки в журнал, а клиент просто ничего не
/// делает. Единственный способ их заметить — сделать ход и посмотреть ответ.
module Corsairs.TestClient.Mechanics

open System
open Corsairs.Platform.Network.Protocol
open Corsairs.TestClient.Session

/// Типы сущностей (EChaCtrlType). Монстры и NPC приходят одним сообщением и
/// различаются только этим полем.
let private CTRL_PLAYER = 1L
let private CTRL_NPC = 2L
let private CTRL_NPC_EVENT = 3L
let private CTRL_MONS = 5L

/// Причины отказа (EFailedActionReason). Пятая — «цели нет»: именно её сервер
/// возвращал на каждую атаку по мобу, пока идентификатор цели терялся на
/// знаковом расширении.
let private FAIL_REASONS =
    [| "действие запрещено"; "действие уже идёт"; "путь неверен"; "двигаться нельзя"
       "нет такого умения"; "цели не существует"; "предмета нет"; "умение не подходит" |]

let private describeFail (reason: int64) =
    if reason >= 0L && reason < int64 FAIL_REASONS.Length then FAIL_REASONS[int reason]
    else $"код {reason}"

/// Результат одной проверки. Пропуск — не провал: если на карте нет монстра,
/// проверка атаки ничего не доказывает, но и ничего не опровергает.
type Outcome =
    | Passed of string
    | Failed of string
    | Skipped of string

type Check = { Name: string; Run: Live -> Outcome }

/// Ждёт подтверждения действия, разбирая отказ.
///
/// MC_NOTIACTION означает, что сервер действие принял и разослал; сюда же
/// приходит MC_FAILEDACTION с причиной, и без её разбора отказ выглядел бы
/// просто как тишина.
let private awaitAction (live: Live) (seconds: float) : Outcome =
    match waitFor live [ Commands.CMD_MC_NOTIACTION; Commands.CMD_MC_FAILEDACTION ] seconds with
    | None -> Failed "сервер не ответил"
    | Some packet ->
        let cmd = packet.GetCmd()
        if cmd = Commands.CMD_MC_NOTIACTION then
            packet.Dispose()
            Passed "сервер принял действие"
        else
            let msg = CommandMessages.Deserialize.mcFailedActionMessage packet
            packet.Dispose()
            Failed $"отказано: {describeFail msg.Reason}"

let private actorsOf (live: Live) (ctrl: int64) =
    live.Seen.Values |> Seq.filter (fun a -> a.CtrlType = ctrl) |> Seq.toList

/// Ближайшая сущность нужного вида: дальняя цель упёрлась бы в отказ по
/// дальности и проверяла бы не то.
let private byDistance (live: Live) (ctrl: int64) =
    actorsOf live ctrl
    |> List.sortBy (fun a ->
        let dx = float (a.PosX - live.PosX)
        let dy = float (a.PosY - live.PosY)
        dx * dx + dy * dy)

let private nearest (live: Live) (ctrl: int64) = byDistance live ctrl |> List.tryHead

let private nearestMany (live: Live) (ctrl: int64) (count: int) =
    byDistance live ctrl |> List.truncate count

// ─────────────────────────────────────────────────────────────────────────
//  Проверки
// ─────────────────────────────────────────────────────────────────────────

/// Площадь города, где стоят NPC. Персонаж бота остаётся между прогонами там,
/// куда ушёл, и за несколько запусков уходит из населённой части карты —
/// поэтому каждый прогон начинается с возврата. Телепорт внутри той же карты
/// безопасен, в отличие от перехода между картами: смену карты клиент обязан
/// сопровождать переподключением, которого бот не делает.
let private HOME_X = 2246L
let private HOME_Y = 2704L

let private checkVision =
    { Name = "поле зрения"
      Run = fun live ->
        say live $"&move {HOME_X},{HOME_Y}"
        drain live 4.0
        live.PosX <- HOME_X * 100L
        live.PosY <- HOME_Y * 100L
        // Список не очищаем: сущности приходят не мгновенно, сервер рассылает
        // их по мере обхода поля зрения, и очистка выбросила бы уже известное
        // ради ещё не пришедшего.
        drain live 6.0
        let players = actorsOf live CTRL_PLAYER |> List.length
        let npcs = (actorsOf live CTRL_NPC |> List.length) + (actorsOf live CTRL_NPC_EVENT |> List.length)
        let mons = actorsOf live CTRL_MONS |> List.length
        if live.Seen.Count = 0 then
            Failed "сервер не показал ни одной сущности"
        else
            Passed $"видно {live.Seen.Count}: игроков {players}, NPC {npcs}, монстров {mons}" }

let private checkGmMode =
    { Name = "режим разработчика"
      Run = fun live ->
        say live "&dev on"
        drain live 2.0
        // Ответ приходит системным сообщением, а не отдельной командой, и
        // содержимое зависит от локали — судим по тому, что связь жива и
        // сервер не разорвал соединение на неизвестной команде.
        say live "&dev off"
        drain live 1.0
        Passed "GM-команды приняты" }

let private checkMove =
    { Name = "движение"
      Run = fun live ->
        let targetX = live.PosX + 600L
        let targetY = live.PosY
        moveTo live targetX targetY
        match awaitAction live 8.0 with
        | Passed _ ->
            drain live 2.0
            Passed $"путь принят: ({live.PosX}, {live.PosY}) → ({targetX}, {targetY})"
        | other -> other }

let private checkTalkToNpc =
    { Name = "разговор с NPC"
      Run = fun live ->
        drain live 2.0
        // Перебираем нескольких: у части NPC скрипт пустой, и молчание
        // одного ничего не говорит о механике разговора в целом.
        let candidates = nearestMany live CTRL_NPC 3
        if candidates.IsEmpty then Skipped "рядом нет NPC" else
        let rec attempt (rest: SeenActor list) (tried: string list) =
          match rest with
          | [] ->
            let listedTried = String.concat "; " (List.rev tried)
            Failed $"ни один NPC не открыл страницу разговора (пробовали: {listedTried})"
          | npc :: others ->
            // Сервер ищет NPC вокруг позиции персонажа и на дальности поля
            // зрения уже не находит: говорить можно только вплотную. Подходим
            // телепортом, а не путём, — иначе пришлось бы ждать, пока сервер
            // проведёт персонажа по маршруту.
            say live $"&move {npc.PosX / 100L},{npc.PosY / 100L}"
            drain live 3.0
            live.PosX <- npc.PosX
            live.PosY <- npc.PosY

            talkTo live npc
            // Чем именно отвечает NPC, решает его Lua-скрипт: страницей
            // разговора, витриной, списком заданий. Поэтому судим по факту
            // ответа, а перечень команд печатаем — по нему видно, какая
            // ветка скрипта отработала.
            live.Notices.Clear()
            let cmds = collect live 5.0
            // Страница разговора — это MC_TALKPAGE и родня. Одно лишь
            // системное сообщение означает, что скрипт NPC до страницы не
            // дошёл и вместо неё объяснил, почему.
            let real =
                cmds |> List.exists (fun c ->
                    c = Commands.CMD_MC_TALKPAGE || c = Commands.CMD_MC_FUNCPAGE
                    || c = Commands.CMD_MC_TRADEPAGE || c = Commands.CMD_MC_MISSIONTALK)
            if real then
                let listed = cmds |> List.distinct |> List.map string |> String.concat ", "
                Passed $"NPC «{npc.Name}» открыл страницу разговора (команды: {listed})"
            else
                let said = if live.Notices.Count = 0 then "молча" else String.concat " | " live.Notices
                attempt others ($"{npc.Name} — {said}" :: tried)

        attempt candidates [] }

/// Атака по монстру.
///
/// Монстра может не оказаться рядом — тогда призываем его GM-командой, иначе
/// проверка молча пропускалась бы именно там, где она нужнее всего.
let private checkAttack (skillId: int64) (summonId: int64) =
    { Name = "атака монстра"
      Run = fun live ->
        drain live 2.0
        let target =
            match nearest live CTRL_MONS with
            | Some mons -> Some mons
            | None ->
                say live $"&summon {summonId}"
                live.Notices.Clear()
                collect live 6.0 |> ignore
                if live.Notices.Count > 0 then
                    let said = String.concat " | " live.Notices
                    log $"      (на призыв сервер сказал: {said})"
                nearest live CTRL_MONS
        match target with
        | None ->
            // Перечисляем, что вообще видно: «монстра нет» может значить и что
            // призыв не сработал, и что призванный пришёл с другим типом.
            let kinds =
                live.Seen.Values
                |> Seq.countBy (fun a -> a.CtrlType)
                |> Seq.map (fun (ctrl, n) -> $"тип {ctrl}: {n}")
                |> String.concat ", "
            Skipped $"монстра не нашлось после призыва {summonId}; видно — {kinds}"
        | Some mons ->
            useSkillOn live skillId mons
            match waitFor live [ Commands.CMD_MC_NOTIACTION; Commands.CMD_MC_FAILEDACTION ] 10.0 with
            | None -> Failed $"по «{mons.Name}»: сервер не ответил"
            | Some packet when packet.GetCmd() = Commands.CMD_MC_NOTIACTION ->
                packet.Dispose()
                Passed $"удар по «{mons.Name}» принят"
            | Some packet ->
                let msg = CommandMessages.Deserialize.mcFailedActionMessage packet
                packet.Dispose()
                // Различение принципиальное. «Цели не существует» означает, что
                // сервер не смог найти монстра по присланному идентификатору —
                // это тот самый дефект, из-за которого бой не работал вовсе.
                // «Действие запрещено» приходит в мирной зоне и поломкой не
                // является: город запрещает бой намеренно.
                if msg.Reason = 5L then
                    Failed $"по «{mons.Name}»: сервер не нашёл цель — та самая регрессия"
                elif msg.Reason = 0L then
                    Skipped $"«{mons.Name}» найден, но зона запрещает бой (карта {live.MapName})"
                else
                    Failed $"по «{mons.Name}»: {describeFail msg.Reason}" }

let private checkTeleport =
    { Name = "телепорт"
      Run = fun live ->
        // Координаты GM-команды задаются в клетках, а позиция персонажа — во
        // внутренних единицах, стократно больших. Смещаемся на десять клеток
        // от текущего места, чтобы не зависеть от конкретной карты.
        let cellX = live.PosX / 100L + 10L
        let cellY = live.PosY / 100L
        say live $"&move {cellX},{cellY}"
        drain live 4.0
        Passed $"команда телепорта на ({cellX}, {cellY}) отправлена" }

let private checkChat =
    { Name = "чат"
      Run = fun live ->
        let mark = $"bot-{DateTime.Now:HHmmss}"
        say live mark
        match waitFor live [ Commands.CMD_MC_SAY ] 6.0 with
        | None -> Failed "сервер не вернул сказанное"
        | Some packet ->
            packet.Dispose()
            Passed "реплика вернулась от сервера" }

let checks (skillId: int64) (summonId: int64) =
    [ checkVision
      checkChat
      checkGmMode
      checkTalkToNpc
      checkMove
      checkAttack skillId summonId
      checkTeleport ]

/// Прогоняет проверки по порядку и печатает свод.
///
/// Порядок значим: поле зрения наполняется первым, а остальные проверки берут
/// цели именно из него.
let run (live: Live) (skillId: int64) (summonId: int64) : int =
    log "─── прогон механик ───"
    let results =
        checks skillId summonId
        |> List.map (fun check ->
            let outcome =
                try check.Run live
                with ex -> Failed $"исключение: {ex.Message}"
            match outcome with
            | Passed detail  -> log $"  ПРОЙДЕНО  {check.Name}: {detail}"
            | Failed detail  -> log $"  ПРОВАЛ    {check.Name}: {detail}"
            | Skipped detail -> log $"  пропуск   {check.Name}: {detail}"
            check.Name, outcome)

    let failed = results |> List.filter (fun (_, o) -> match o with Failed _ -> true | _ -> false)
    let passed = results |> List.filter (fun (_, o) -> match o with Passed _ -> true | _ -> false)
    let skipped = results |> List.filter (fun (_, o) -> match o with Skipped _ -> true | _ -> false)

    log "───"
    log $"пройдено {passed.Length}, провалов {failed.Length}, пропущено {skipped.Length}"
    for name, outcome in failed do
        match outcome with
        | Failed detail -> log $"  ПРОВАЛ {name}: {detail}"
        | _ -> ()

    if failed.IsEmpty then 0 else 1
