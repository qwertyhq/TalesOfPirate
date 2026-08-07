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

/// Площадь города, где стоят NPC. Персонаж бота остаётся между прогонами там,
/// куда ушёл, поэтому прогон начинается с возврата.
let private HOME_MAP = "garner"
let private HOME_X = 2246L
let private HOME_Y = 2704L

/// Место охоты — клетка, где по данным игры живут монстры первого уровня
/// (monster_list, «Mystic Shrub»). Это та же карта garner: в таблице maps она
/// зовётся «Ascaron», а «Argent City» — лишь город на ней. Внутри города бой
/// запрещён признаком зоны, за его пределами разрешён.
let private HUNT_X = 2092L
let private HUNT_Y = 2654L

/// Карта для проверки перехода. Обязана обслуживаться этим GameServer: список
/// его карт виден в журнале при старте, и на чужую он отвечает «карта
/// недоступна».
let private OTHER_MAP = "magicsea"
let private OTHER_X = 1350L
let private OTHER_Y = 550L

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

/// Состояние прогона. Сессия изменяемая, потому что смена карты рвёт
/// соединение: сервер отключает игрока, и на новой карте он входит заново.
type Ctx =
    { mutable Live: Live
      Host: string
      Port: int
      Account: string
      Password: string
      SkillId: int64
      SummonId: int64 }

type Check = { Name: string; Run: Ctx -> Outcome }

// ─────────────────────────────────────────────────────────────────────────
//  Вспомогательное
// ─────────────────────────────────────────────────────────────────────────

let private awaitAction (live: Live) (seconds: float) : Outcome =
    match waitFor live [ Commands.CMD_MC_NOTIACTION; Commands.CMD_MC_FAILEDACTION ] seconds with
    | None -> Failed "сервер не ответил"
    | Some packet when packet.GetCmd() = Commands.CMD_MC_NOTIACTION ->
        packet.Dispose()
        Passed "сервер принял действие"
    | Some packet ->
        let msg = CommandMessages.Deserialize.mcFailedActionMessage packet
        packet.Dispose()
        Failed $"отказано: {describeFail msg.Reason}"

let private actorsOf (live: Live) (ctrl: int64) =
    live.Seen.Values |> Seq.filter (fun a -> a.CtrlType = ctrl) |> Seq.toList

let private byDistance (live: Live) (ctrl: int64) =
    actorsOf live ctrl
    |> List.sortBy (fun a ->
        let dx = float (a.PosX - live.PosX)
        let dy = float (a.PosY - live.PosY)
        dx * dx + dy * dy)

let private nearest (live: Live) (ctrl: int64) = byDistance live ctrl |> List.tryHead

let private nearestMany (live: Live) (ctrl: int64) (count: int) =
    byDistance live ctrl |> List.truncate count

/// Умения обычной атаки, по убыванию неприхотливости.
///
/// Состояние умения зависит от надетого оружия: «Sword» (28) требует меч или
/// нож в руках (item_need_1 = «1,0;1,11»), а «Greatsword» (29) и «Dual
/// Implosion» (26) — «1,-1», то есть ничего. Сервер отказывает неактивному
/// умению тем же кодом «действие запрещено», что и запрету зоны, поэтому
/// умение подбирается перебором, а не задаётся одним числом.
let private ATTACK_SKILLS = [ 29L; 26L; 27L; 28L ]

/// Бьёт цель, перебирая умения, пока сервер не примет удар.
/// Возвращает номер сработавшего умения либо причину последнего отказа.
let private strikeWithAny (live: Live) (target: SeenActor) : Result<int64, int64> =
    // Признак движения только 2 — «подойти и ударить». Значение 0 сервер
    // молча игнорирует: ответа не приходит вовсе, и соединение выходит в
    // тайм-аут.
    let combos = [ for s in ATTACK_SKILLS -> s, 2L ]
    let rec attempt (rest: (int64 * int64) list) (lastReason: int64) =
        match rest with
        | [] -> Error lastReason
        | (skillId, chMove) :: others ->
            say live $"&skill {skillId},1"
            drain live 2.0
            useSkillOnWith live skillId target chMove
            match waitFor live [ Commands.CMD_MC_NOTIACTION; Commands.CMD_MC_FAILEDACTION ] 8.0 with
            | None -> attempt others lastReason
            | Some packet when packet.GetCmd() = Commands.CMD_MC_NOTIACTION ->
                packet.Dispose()
                Ok skillId
            | Some packet ->
                let msg = CommandMessages.Deserialize.mcFailedActionMessage packet
                packet.Dispose()
                // «Цели не существует» перебором не лечится: дело не в умении,
                // а в том, что сервер не нашёл сущность.
                if msg.Reason = 5L then Error 5L
                else attempt others msg.Reason
    attempt combos -1L

/// Оружие для боя: у стартового персонажа это «Newbie Knife».
let private WEAPON_ITEMS = [ 8L ]

/// Надевает оружие, если оно лежит в сумке.
///
/// Без оружия в руках сервер принимает намерение ударить, но самого удара не
/// разыгрывает: подтверждение приходит, а результата по цели нет. Отличить это
/// от поломки боя по одному лишь молчанию невозможно.
let private equipWeapon (live: Live) : string option =
    let found =
        live.Kitbag
        |> Seq.tryPick (fun kv ->
            if List.contains kv.Value WEAPON_ITEMS then Some(kv.Key, kv.Value) else None)
    match found with
    | None -> None
    | Some(grid, itemId) ->
        equipItem live grid EQUIP_RHAND
        drain live 3.0
        Some $"предмет {itemId} из ячейки {grid} надет в правую руку"


/// Телепорт внутри текущей карты. Безопасен, в отличие от перехода между
/// картами: соединение остаётся тем же.
let private warpWithin (live: Live) (cellX: int64) (cellY: int64) =
    say live $"&move {cellX},{cellY}"
    drain live 3.0
    live.PosX <- cellX * 100L
    live.PosY <- cellY * 100L

/// Переход на другую карту с переподключением.
///
/// Карта живёт на своём сервере, поэтому GameServer отключает игрока, а Gate
/// направляет его заново. Для клиента это выглядит как разрыв и повторный
/// вход — ровно то, что здесь и делается.
let private switchMap (ctx: Ctx) (mapName: string) (cellX: int64) (cellY: int64) : Result<unit, string> =
    if ctx.Live.MapName = mapName then Ok() else

    say ctx.Live $"&move {cellX},{cellY},{mapName}"
    if not (awaitDisconnect ctx.Live 15.0) then
        Error "сервер не отключил игрока — переход не начался"
    else
        // Пауза перед повторным входом: сервер ещё держит игрока в списке и
        // отвечает «уже в игре», пока не завершит его выгрузку.
        Threading.Thread.Sleep(3000)
        let rec attempt (left: int) : Result<unit, string> =
            match connect ctx.Host ctx.Port ctx.Account ctx.Password with
            | Ok(_, live) ->
                ctx.Live <- live
                Ok()
            | Error reason when left > 0 ->
                Threading.Thread.Sleep(4000)
                attempt (left - 1)
            | Error reason -> Error $"повторный вход не удался: {reason}"
        attempt 4

// ─────────────────────────────────────────────────────────────────────────
//  Проверки
// ─────────────────────────────────────────────────────────────────────────

let private checkVision =
    { Name = "поле зрения"
      Run = fun ctx ->
        let live = ctx.Live
        if live.MapName = HOME_MAP then
            warpWithin live HOME_X HOME_Y
        // Сущности приходят не мгновенно: сервер рассылает их по мере обхода
        // поля зрения.
        drain live 6.0
        let players = actorsOf live CTRL_PLAYER |> List.length
        let npcs = (actorsOf live CTRL_NPC |> List.length) + (actorsOf live CTRL_NPC_EVENT |> List.length)
        let mons = actorsOf live CTRL_MONS |> List.length
        if live.Seen.Count = 0 then Failed "сервер не показал ни одной сущности"
        else Passed $"видно {live.Seen.Count}: игроков {players}, NPC {npcs}, монстров {mons}" }

let private checkChat =
    { Name = "чат"
      Run = fun ctx ->
        let mark = $"bot-{DateTime.Now:HHmmss}"
        say ctx.Live mark
        match waitFor ctx.Live [ Commands.CMD_MC_SAY ] 6.0 with
        | None -> Failed "сервер не вернул сказанное"
        | Some packet ->
            packet.Dispose()
            Passed "реплика вернулась от сервера" }

let private checkGmMode =
    { Name = "режим разработчика"
      Run = fun ctx ->
        say ctx.Live "&dev on"
        drain ctx.Live 2.0
        say ctx.Live "&dev off"
        drain ctx.Live 1.0
        Passed "GM-команды приняты" }

let private checkTalkToNpc =
    { Name = "разговор с NPC"
      Run = fun ctx ->
        let live = ctx.Live
        drain live 2.0
        // Перебираем нескольких: у части NPC скрипт пустой, и молчание одного
        // ничего не говорит о механике разговора в целом.
        let candidates = nearestMany live CTRL_NPC 3
        if candidates.IsEmpty then Skipped "рядом нет NPC" else
        let rec attempt (rest: SeenActor list) (tried: string list) =
          match rest with
          | [] ->
            let listedTried = String.concat "; " (List.rev tried)
            Failed $"ни один NPC не открыл страницу разговора (пробовали: {listedTried})"
          | npc :: others ->
            // Сервер ищет NPC вокруг позиции персонажа и на дальности поля
            // зрения уже не находит: говорить можно только вплотную.
            warpWithin live (npc.PosX / 100L) (npc.PosY / 100L)
            talkTo live npc
            live.Notices.Clear()
            let cmds = collect live 5.0
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

let private checkMove =
    { Name = "движение"
      Run = fun ctx ->
        let live = ctx.Live
        let targetX = live.PosX + 600L
        let targetY = live.PosY
        moveTo live targetX targetY
        match awaitAction live 8.0 with
        | Passed _ ->
            drain live 2.0
            Passed $"путь принят: ({live.PosX}, {live.PosY}) → ({targetX}, {targetY})"
        | other -> other }

let private checkSwitchMap =
    { Name = "переход на другую карту"
      Run = fun ctx ->
        match switchMap ctx OTHER_MAP OTHER_X OTHER_Y with
        | Error reason -> Failed reason
        | Ok() ->
            drain ctx.Live 3.0
            Passed $"на карте {ctx.Live.MapName}, позиция ({ctx.Live.PosX}, {ctx.Live.PosY})" }

/// Ищет монстра поблизости, при необходимости призывая его.
let private findMonster (ctx: Ctx) : SeenActor option =
    let live = ctx.Live
    // Уходим из города к месту, где по данным игры водятся монстры: внутри
    // города сервер запретил бы бой признаком зоны.
    if live.MapName = HOME_MAP then
        warpWithin live HUNT_X HUNT_Y
    drain live 4.0
    match nearest live CTRL_MONS with
    | Some mons -> Some mons
    | None ->
        say live $"&summon {ctx.SummonId}"
        live.Notices.Clear()
        drain live 6.0
        if live.Notices.Count > 0 then
            let said = String.concat " | " live.Notices
            log $"      (на призыв сервер сказал: {said})"
        nearest live CTRL_MONS

let private checkAttack =
    { Name = "атака монстра"
      Run = fun ctx ->
        let live = ctx.Live
        match findMonster ctx with
        | None ->
            let kinds =
                live.Seen.Values
                |> Seq.countBy (fun a -> a.CtrlType)
                |> Seq.map (fun (ctrl, n) -> $"тип {ctrl}: {n}")
                |> String.concat ", "
            Skipped $"монстра не нашлось; видно — {kinds}"
        | Some mons ->
            equipWeapon live |> ignore
            // Телепорт вплотную не годится: путь в пакете стал бы нулевым —
            // из точки цели в неё же, — и сервер такой удар не разыгрывает.
            // Подводить персонажа должен он сам, как и для настоящего клиента.
            live.Notices.Clear()
            match strikeWithAny live mons with
            | Ok skillId -> Passed $"удар по «{mons.Name}» принят (умение {skillId})"
            | Error reason ->
                let msg = {| Reason = reason |}
                // Различение принципиальное. «Цели не существует» означает, что
                // сервер не нашёл монстра по присланному идентификатору — это
                // тот самый дефект, из-за которого бой не работал вовсе.
                // «Действие запрещено» приходит в мирной зоне и поломкой не
                // является: город запрещает бой намеренно.
                if msg.Reason = 5L then
                    Failed $"по «{mons.Name}»: сервер не нашёл цель — регрессия поиска сущности"
                elif msg.Reason = 0L then
                    // Причина «действие запрещено» общая для полудюжины
                    // условий, но каждое сопровождается пояснением сервера.
                    drain live 1.0
                    let said =
                        if live.Notices.Count = 0 then "без пояснения"
                        else String.concat " | " live.Notices
                    Failed $"«{mons.Name}» найден, но удар запрещён: {said}"
                else
                    Failed $"по «{mons.Name}»: {describeFail msg.Reason}" }


/// Урон: бьём монстра и смотрим, убыло ли у него здоровье.
///
/// Проверяется не число, а сам факт: величина зависит от характеристик и
/// формулы умения, а вот «удар прошёл, а здоровье не изменилось» — поломка,
/// которую сервер никак не сообщает.
let private checkDamage =
    { Name = "урон по монстру"
      Run = fun ctx ->
        let live = ctx.Live
        match findMonster ctx with
        | None -> Skipped "монстра не нашлось"
        | Some mons ->
            // Режим разработчика поднимает урон до потолка: персонаж первого
            // уровня бьёт слабо, и «здоровье не изменилось» означало бы просто
            // промах, а не поломку.
            say live "&dev on"
            drain live 3.0
            let hpBefore = mons.Hp
            if hpBefore <= 0L then Skipped $"у «{mons.Name}» неизвестно здоровье" else

            let struck = strikeWithAny live mons
            // Урон приходит не с подтверждением действия, а отдельными
            // обновлениями характеристик: сервер сперва принимает удар, потом
            // разыгрывает его по времени применения умения.
            // Ждём дольше подтверждения: сервер сперва ведёт персонажа к цели,
            // затем разыгрывает удар по скорости умения (fire_speed), и лишь
            // потом рассылает результат.
            let after = collect live 15.0
            let listed = after |> List.distinct |> List.map string |> String.concat ", "
            log $"      (после удара пришли команды: {listed})"
            match struck with
            | Error reason ->
                say live "&dev off"
                Failed $"по «{mons.Name}» ударить не вышло: {describeFail reason}"
            | Ok _ ->
            let hpAfter =
                match live.Seen.TryGetValue mons.WorldId with
                | true, actor -> actor.Hp
                | _ -> 0L                 // исчез из поля зрения — значит убит

            say live "&dev off"
            drain live 1.0

            if hpAfter < hpBefore then
                let dealt = hpBefore - hpAfter
                Passed $"«{mons.Name}»: здоровье {hpBefore} → {hpAfter}, урон {dealt}"
            elif hpAfter = 0L then
                Passed $"«{mons.Name}» убит с {hpBefore} здоровья"
            else
                Failed $"«{mons.Name}»: удар прошёл, но здоровье осталось {hpBefore}" }

/// Опыт: убиваем монстра в режиме разработчика и смотрим прирост опыта.
///
/// Режим нужен, чтобы убить с одного удара: иначе проверка растянулась бы на
/// десятки ударов и зависела от того, успеет ли монстр убить бота.
let private checkExperience =
    { Name = "опыт за убийство"
      Run = fun ctx ->
        let live = ctx.Live
        say live "&dev on"
        drain live 3.0
        try
            match findMonster ctx with
            | None -> Skipped "монстра не нашлось"
            | Some mons ->
                drain live 3.0
                let expBefore =
                    match live.Attrs.TryGetValue ATTR_CEXP with
                    | true, v -> v
                    | _ -> -1L

                strikeWithAny live mons |> ignore
                drain live 10.0

                let expAfter =
                    match live.Attrs.TryGetValue ATTR_CEXP with
                    | true, v -> v
                    | _ -> -1L
                let dead = not (live.Seen.ContainsKey mons.WorldId)

                if expBefore < 0L || expAfter < 0L then
                    Skipped "сервер не прислал опыт персонажа"
                elif expAfter > expBefore then
                    let fate = if dead then "убит" else "жив"
                    let gained = expAfter - expBefore
                    Passed $"опыт {expBefore} → {expAfter} (+{gained}), монстр {fate}"
                elif dead then
                    Failed $"«{mons.Name}» убит, но опыт не изменился ({expBefore})"
                else
                    Skipped $"«{mons.Name}» выжил — опыт начислять не за что"
        finally
            say live "&dev off"
            drain live 2.0 }

/// Инвентарь: у нового персонажа он не пуст — сервер выдаёт стартовый набор.
let private checkKitbag =
    { Name = "инвентарь"
      Run = fun ctx ->
        let live = ctx.Live
        say live "&make 289,1"
        live.Notices.Clear()
        let cmds = collect live 5.0
        // Выдача предмета — это действие, и подтверждается она тем же
        // MC_NOTIACTION, что удар или шаг: отдельного «предмет добавлен»
        // сервер не шлёт, содержимое сумки приезжает внутри действия.
        let got =
            cmds |> List.exists (fun c ->
                c = Commands.CMD_MC_NOTIACTION || c = Commands.CMD_MC_ADD_ITEM_CHA
                || c = Commands.CMD_MC_KITBAGTEMP_SYNC || c = Commands.CMD_MC_ITEM_USE_SUC)
        if got then Passed "предмет выдан и пришёл в инвентарь"
        else
            let said = if live.Notices.Count = 0 then "без пояснения" else String.concat " | " live.Notices
            let listed = cmds |> List.distinct |> List.map string |> String.concat ", "
            Failed $"предмет не пришёл; команды: {listed}; сервер сказал: {said}" }


let private checkEquip =
    { Name = "экипировка"
      Run = fun ctx ->
        let live = ctx.Live
        if live.Kitbag.Count = 0 then Skipped "содержимое сумки неизвестно" else
        match equipWeapon live with
        | Some detail -> Passed detail
        | None ->
            let ids = live.Kitbag.Values |> Seq.distinct |> Seq.truncate 8 |> Seq.map string |> String.concat ", "
            Skipped $"оружия в сумке нет; предметы: {ids}" }

let private checkTeleport =
    { Name = "телепорт внутри карты"
      Run = fun ctx ->
        let live = ctx.Live
        let cellX = live.PosX / 100L + 10L
        let cellY = live.PosY / 100L
        let before = live.PosX
        warpWithin live cellX cellY
        Passed $"перемещение с {before} на {live.PosX}" }

/// Порядок значим: поле зрения наполняется первым, разговор идёт до движения
/// (сервер ищет NPC вокруг персонажа), а бой — только после перехода на карту,
/// где он разрешён.
let private ALL =
    [ checkVision
      checkChat
      checkGmMode
      checkTalkToNpc
      checkMove
      checkTeleport
      checkEquip
      checkAttack
      checkDamage
      checkExperience
      // Инвентарь последним: выдача предмета — тоже действие, и следующий за
      // ней удар сервер отклоняет, пока прежнее действие не завершилось.
      checkKitbag ]

// checkSwitchMap намеренно не входит в прогон. Переход между картами — это не
// одна команда, а переключение сервера: GameServer отключает игрока и через
// Gate передаёт его на сервер целевой карты. Бот получает лишь разрыв, не зная
// адреса назначения, и повторный вход попадает в подвешенное состояние, из
// которого GameServer выходит только перезапуском. Механика останется
// непокрытой, пока бот не научится читать указание Gate, — и это же придётся
// реализовать клиенту на Unreal.

let run (host: string) (port: int) (account: string) (password: string)
        (live: Live) (skillId: int64) (summonId: int64) : int =
    let ctx =
        { Live = live; Host = host; Port = port; Account = account; Password = password
          SkillId = skillId; SummonId = summonId }

    log "─── прогон механик ───"
    let results =
        ALL
        |> List.map (fun check ->
            let outcome =
                try check.Run ctx
                with ex -> Failed $"исключение: {ex.Message}"
            match outcome with
            | Passed detail  -> log $"  ПРОЙДЕНО  {check.Name}: {detail}"
            | Failed detail  -> log $"  ПРОВАЛ    {check.Name}: {detail}"
            | Skipped detail -> log $"  пропуск   {check.Name}: {detail}"
            check.Name, outcome)

    // Прощаемся явно: без этого сервер держит игрока в сети до собственного
    // тайм-аута, и следующий прогон упирается в отказ входа.
    logOut ctx.Live

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
