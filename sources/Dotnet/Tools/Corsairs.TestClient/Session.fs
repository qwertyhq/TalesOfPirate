/// Сетевая сессия против живой связки серверов: транспорт, вход и приём.
///
/// Вынесено из Program.fs, чтобы проверкой входа и прогоном механик занимался
/// один и тот же код. Расхождение между ними означало бы, что бот проверяет
/// не то, что делает клиент.
module Corsairs.TestClient.Session

open System
open System.Net.Sockets
open Corsairs.Platform.Network.Protocol

/// Версия клиента. Gate сверяет её с настройкой ClientVersion и отвергает
/// вход при несовпадении — значение обязано совпадать с appsettings.json.
let CLIENT_VERSION = 32125L

/// Заголовок пакета: размер (uint16), счётчик (uint32), команда (uint16).
let HEADER_SIZE = 8

/// Второй пароль учётной записи. GroupServer принимает только буквы и цифры
/// и не пускает в мир, пока он не задан.
let PASSWORD2 = "test1234"

let NEW_CHA_BIRTH = "Argent City"
let NEW_CHA_TYPE = 1L
let NEW_CHA_HAIR = 2000L
let NEW_CHA_FACE = 2554L

/// Метка времени в имени персонажа: имя обязано быть уникальным, а повторный
/// прогон против той же базы иначе упирался бы в отказ по занятому имени.
let NAME_STAMP_FORMAT = "HHmmss"

/// Номера характеристик из ChaAttrType.h: массив плотный, без дыр, поэтому
/// значения заданы числами — так же, как их принимает GM-команда `attr`.
let ATTR_LV = 0L
let ATTR_HP = 1L
let ATTR_GD = 8L
let ATTR_CEXP = 15L
let ATTR_MXHP = 31L
let ATTR_MNATK = 33L
let ATTR_MXATK = 34L

let private TIME_FORMAT = "HH:mm:ss.fff"

let log (text: string) =
    let stamp = DateTime.Now.ToString(TIME_FORMAT)
    Console.WriteLine($"[{stamp}] {text}")

/// Состояние подключённой сессии.
///
/// Счётчик пакетов изменяемый и общий: Gate при включённой защите WPE сверяет
/// его с собственным и разрывает соединение при расхождении, поэтому вести
/// два счётчика на одно соединение нельзя.
type Live =
    { Stream: NetworkStream
      mutable Counter: uint32
      mutable WorldId: int64
      mutable Handle: int64
      mutable MapName: string
      mutable PosX: int64
      mutable PosY: int64
      /// Идентификатор действия. Сервер отбрасывает повтор с тем же номером,
      /// поэтому счётчик обязан расти между действиями.
      mutable ActionId: int64
      /// Всё, что сервер показал в поле зрения: игроки, NPC и монстры приходят
      /// одним и тем же сообщением MC_CHABEGINSEE.
      Seen: Collections.Generic.Dictionary<int64, SeenActor>
      /// Системные сообщения сервера. Именно ими он объясняет отказы, и без
      /// них проверка видит лишь то, что «ничего не произошло».
      Notices: ResizeArray<string>
      /// Характеристики своего персонажа по номеру атрибута (ChaAttrType.h).
      /// Обновляются приходящими MC_SYNATTR — так видно урон, опыт и уровень.
      Attrs: Collections.Generic.Dictionary<int64, int64>
      /// Содержимое сумки: номер ячейки → номер предмета. Нужно, чтобы надеть
      /// вещь: действие «использовать» адресуется ячейками, а не предметами.
      Kitbag: Collections.Generic.Dictionary<int64, int64>
      /// Предметы на земле: идентификатор → handle. Приходят отдельным
      /// сообщением, а не как сущности поля зрения.
      Ground: Collections.Generic.Dictionary<int64, int64> }

and SeenActor =
    { WorldId: int64
      Handle: int64
      Name: string
      CtrlType: int64
      PosX: int64
      PosY: int64
      /// Здоровье на момент появления в поле зрения. Дальше меняется
      /// приходящими MC_SYNATTR — по нему и виден нанесённый урон.
      mutable Hp: int64 }

let private readExact (stream: NetworkStream) (count: int) : byte[] option =
    let buffer = Array.zeroCreate<byte> count
    let mutable filled = 0
    let mutable broken = false
    while filled < count && not broken do
        let read = stream.Read(buffer, filled, count - filled)
        if read <= 0 then broken <- true else filled <- filled + read
    if broken then None else Some buffer

/// Читает один пакет: сперва два байта длины, затем остаток.
///
/// Длина в заголовке — это размер всего пакета вместе с заголовком, а не
/// длина полезной нагрузки.
let readPacket (stream: NetworkStream) : IRPacket option =
    match readExact stream 2 with
    | None -> None
    | Some sizeBytes ->
        let size = int (Buffers.Binary.BinaryPrimitives.ReadUInt16BigEndian(ReadOnlySpan<byte>(sizeBytes)))
        if size < HEADER_SIZE then
            log $"ОШИБКА размер пакета {size} меньше заголовка"
            None
        else
            match readExact stream (size - 2) with
            | None -> None
            | Some rest ->
                let full = Array.zeroCreate<byte> size
                Array.blit sizeBytes 0 full 0 2
                Array.blit rest 0 full 2 (size - 2)
                Some(new RPacket(Memory<byte>(full)) :> IRPacket)

let send (live: Live) (packet: WPacket byref) =
    packet.WriteSess(live.Counter)
    let memory = packet.GetPacketMemory()
    live.Stream.Write(memory.Span)
    live.Stream.Flush()
    live.Counter <- live.Counter + 1u

/// Запоминает сущность, о которой сервер сообщил, и забывает ушедшую.
///
/// Ведётся на каждом чтении пакета, а не по запросу: сообщения о появлении
/// приходят вперемешку с ответами на действия, и выборочное чтение просто
/// потеряло бы их.
let private absorb (live: Live) (packet: IRPacket) =
    let cmd = packet.GetCmd()
    if cmd = Commands.CMD_MC_CHABEGINSEE then
        try
            let msg = CommandMessages.Deserialize.mcChaBeginSeeMessage packet
            let hp =
                msg.Attr.Attrs
                |> Array.tryFind (fun a -> a.AttrId = ATTR_HP)
                |> Option.map (fun a -> a.AttrVal)
                |> Option.defaultValue 0L
            live.Seen[msg.Base.WorldId] <-
                { WorldId = msg.Base.WorldId
                  Handle = msg.Base.Handle
                  Name = msg.Base.Name
                  CtrlType = msg.Base.CtrlType
                  PosX = msg.Base.PosX
                  PosY = msg.Base.PosY
                  Hp = hp }
        with _ -> ()          // повреждённое сообщение не должно валить прогон
    elif cmd = Commands.CMD_MC_CHAENDSEE then
        try
            let worldId = packet.ReadInt64()
            live.Seen.Remove(worldId) |> ignore
        with _ -> ()
    elif cmd = Commands.CMD_MC_NOTIACTION then
        try
            let msg = CommandMessages.Deserialize.mcCharacterActionMessage packet
            match msg.Action with
            | CommandMessages.ActionSkillTar data ->
                // Итог удара приходит именно здесь: сервер рассылает результат
                // применения умения по цели, и новое здоровье лежит в перечне
                // изменённых характеристик, а не отдельным сообщением.
                match live.Seen.TryGetValue msg.WorldId with
                | true, actor ->
                    match data.Effects |> Array.tryFind (fun a -> a.AttrId = ATTR_HP) with
                    | Some hp -> actor.Hp <- hp.AttrVal
                    | None -> ()
                | _ -> ()
                // Характеристики бьющего идут тем же сообщением отдельным
                // набором — так виден расход маны и полученный опыт.
                if data.SrcId = live.WorldId then
                    for entry in data.SrcEffects do
                        live.Attrs[entry.AttrId] <- entry.AttrVal
            | _ -> ()
        with _ -> ()
    elif cmd = Commands.CMD_MC_SYNATTR then
        try
            let msg = CommandMessages.Deserialize.mcSynAttributeMessage packet
            if msg.WorldId = live.WorldId then
                for entry in msg.Attr.Attrs do
                    live.Attrs[entry.AttrId] <- entry.AttrVal
            else
                // Характеристики чужой сущности: интересует здоровье — по нему
                // видно, дошёл ли урон до цели.
                match live.Seen.TryGetValue msg.WorldId with
                | true, actor ->
                    match msg.Attr.Attrs |> Array.tryFind (fun a -> a.AttrId = ATTR_HP) with
                    | Some hp -> actor.Hp <- hp.AttrVal
                    | None -> ()
                | _ -> ()
        with _ -> ()
    elif cmd = Commands.CMD_MC_ITEMBEGINSEE then
        try
            // Нужны только идентификатор и handle: ими адресуется подбор.
            let worldId = packet.ReadInt64()
            let handle = packet.ReadInt64()
            live.Ground[worldId] <- handle
        with _ -> ()
    elif cmd = Commands.CMD_MC_ITEMENDSEE then
        try
            let worldId = packet.ReadInt64()
            live.Ground.Remove(worldId) |> ignore
        with _ -> ()
    elif cmd = Commands.CMD_MC_SYSINFO then
        try
            let msg = CommandMessages.Deserialize.mcSysInfoMessage packet
            live.Notices.Add(msg.Info)
        with _ -> ()

/// Ждёт пакет с одной из ожидаемых команд, попутно впитывая всё остальное.
///
/// Ответ приходит не сразу и не один: Gate спрашивает Group, тот — Account
/// или GameServer, а попутно шлёт клиенту служебные команды.
let waitFor (live: Live) (expected: uint16 list) (seconds: float) : IRPacket option =
    let deadline = DateTime.UtcNow.AddSeconds(seconds)
    let mutable found = None
    let mutable searching = true
    while searching && DateTime.UtcNow < deadline do
        match readPacket live.Stream with
        | None ->
            log "ОШИБКА соединение закрыто сервером"
            searching <- false
        | Some packet ->
            if List.contains (packet.GetCmd()) expected then
                found <- Some packet
                searching <- false
            else
                absorb live packet
                packet.Dispose()
    found

/// Вычитывает всё, что успело прийти, за отведённое время.
///
/// Нужно там, где ответа как такового нет: сервер сообщает о результате
/// появлением сущности в поле зрения или изменением характеристик.
let drain (live: Live) (seconds: float) =
    let deadline = DateTime.UtcNow.AddSeconds(seconds)
    let saved = live.Stream.ReadTimeout
    live.Stream.ReadTimeout <- 300
    let mutable running = true
    while running && DateTime.UtcNow < deadline do
        try
            match readPacket live.Stream with
            | None -> running <- false
            | Some packet ->
                absorb live packet
                packet.Dispose()
        with :? IO.IOException -> ()      // тайм-аут чтения — просто тишина
    live.Stream.ReadTimeout <- saved

/// Собирает команды всех пакетов, пришедших за отведённое время.
///
/// Нужно там, где заранее неизвестно, чем сервер отвечает: список ожидаемых
/// команд приходится узнавать наблюдением, а не по заголовочным файлам —
/// ответ формирует Lua-скрипт NPC, и он волен слать что угодно.
let collect (live: Live) (seconds: float) : uint16 list =
    let deadline = DateTime.UtcNow.AddSeconds(seconds)
    let saved = live.Stream.ReadTimeout
    live.Stream.ReadTimeout <- 300
    let seen = ResizeArray<uint16>()
    let mutable running = true
    while running && DateTime.UtcNow < deadline do
        try
            match readPacket live.Stream with
            | None -> running <- false
            | Some packet ->
                seen.Add(packet.GetCmd())
                absorb live packet
                packet.Dispose()
        with :? IO.IOException -> ()
    live.Stream.ReadTimeout <- saved
    List.ofSeq seen

let describeLoginError (code: int16) =
    match code with
    | 1s -> "нет такой учётной записи"
    | 2s -> "неверный пароль"
    | 3s -> "учётная запись заблокирована"
    | 4s -> "уже в игре"
    | _  -> $"код {code}"

/// Проходит вход целиком: рукопожатие, CM_LOGIN, второй пароль, выбор или
/// создание персонажа, CM_BGNPLAY и ожидание MC_ENTERMAP от GameServer.
///
/// Возвращает подключённую сессию либо причину отказа. Успех означает, что
/// работает вся цепочка Gate → Group → Account → GameServer, а не только
/// точка входа.
let logIn (stream: NetworkStream) (account: string) (password: string) : Result<Live, string> =
    let live =
        { Stream = stream; Counter = 0u; WorldId = 0L; Handle = 0L
          MapName = ""; PosX = 0L; PosY = 0L; ActionId = 0L
          Seen = Collections.Generic.Dictionary<int64, SeenActor>()
          Notices = ResizeArray<string>()
          Attrs = Collections.Generic.Dictionary<int64, int64>()
          Kitbag = Collections.Generic.Dictionary<int64, int64>()
          Ground = Collections.Generic.Dictionary<int64, int64>() }

    // Реализация хеша своя, поэтому сверяемся с известными значениями до
    // того, как отказ во входе спишут на неверный пароль.
    match Blake2s.SelfTest() with
    | Some problem -> Error $"самопроверка BLAKE2s не прошла: {problem}"
    | None ->

    // Сервер хранит и сравнивает хеш как строку, поэтому клиент шлёт именно
    // hex-представление, а не сам пароль.
    let passwordHash = Blake2s.HashPassword password

    // Первым говорит сервер: при выключенном RSA-AES это пустое рукопожатие,
    // при включённом — открытый ключ RSA.
    match readPacket stream with
    | None -> Error "сервер закрыл соединение до рукопожатия"
    | Some handshake ->

    let handshakeCmd = handshake.GetCmd()
    handshake.Dispose()
    if handshakeCmd <> Commands.CMD_MC_SEND_SERVER_PUBLIC_KEY then
        Error $"ожидалась команда рукопожатия {Commands.CMD_MC_SEND_SERVER_PUBLIC_KEY}, пришла {handshakeCmd}"
    else

    let mutable login = WPacket(256)
    login.WriteCmd(Commands.CMD_CM_LOGIN)
    login.WriteString(account)
    login.WriteString(passwordHash)
    login.WriteString("00-00-00-00-00-00")    // MAC — сервер только пишет его в журнал
    login.WriteInt64(0L)                      // CheatMarker
    login.WriteInt64(CLIENT_VERSION)
    send live &login

    match waitFor live [ Commands.CMD_MC_LOGIN ] 20.0 with
    | None -> Error "ответа на вход не дождались"
    | Some packet ->

    let response = CommandMessages.Deserialize.mcLoginResponse packet
    packet.Dispose()

    match response with
    | CommandMessages.McLoginError code -> Error $"вход отклонён: {describeLoginError code}"
    | CommandMessages.McLoginSuccess data ->

    log $"вход выполнен: слотов {data.MaxChaNum}, персонажей {data.Characters.Length}"

    // Второй пароль обязателен для входа в мир: GroupServer отвергает BGNPLAY,
    // пока он пуст, кодом ERR_PT_INVALID_PW2.
    if not data.HasPassword2 then
        let mutable pw2 = WPacket(64)
        pw2.WriteCmd(Commands.CMD_CM_CREATE_PASSWORD2)
        pw2.WriteString(PASSWORD2)
        send live &pw2
        match waitFor live [ Commands.CMD_MC_CREATE_PASSWORD2 ] 20.0 with
        | None -> log "ОШИБКА ответа на создание второго пароля не дождались"
        | Some reply ->
            let code = int16 (reply.ReadInt64())
            reply.Dispose()
            if code = 0s then log "второй пароль создан"
            else log $"создание второго пароля отклонено: код {code}"

    // Персонаж нужен, чтобы дойти до GameServer: вход в учётную запись его
    // не затрагивает вовсе.
    let slot =
        match data.Characters |> Array.tryFindIndex (fun c -> c.Valid) with
        | Some index -> Some index
        | None ->
            let name = $"Test{DateTime.Now.ToString(NAME_STAMP_FORMAT)}"
            let mutable create = WPacket(128)
            create.WriteCmd(Commands.CMD_CM_NEWCHA)
            create.WriteString(name)
            create.WriteString(NEW_CHA_BIRTH)
            create.WriteInt64(NEW_CHA_TYPE)
            create.WriteInt64(NEW_CHA_HAIR)
            create.WriteInt64(NEW_CHA_FACE)
            send live &create
            match waitFor live [ Commands.CMD_MC_NEWCHA ] 20.0 with
            | None -> None
            | Some reply ->
                let code = int16 (reply.ReadInt64())
                reply.Dispose()
                if code <> 0s then None
                else
                    log $"персонаж создан: {name}"
                    Some 0

    match slot with
    | None -> Error "персонажа нет и создать не удалось"
    | Some index ->

    let mutable play = WPacket(32)
    play.WriteCmd(Commands.CMD_CM_BGNPLAY)
    play.WriteInt64(int64 index)
    send live &play

    // Успех подтверждается не ответом на BGNPLAY, а входом в карту:
    // MC_ENTERMAP приходит уже от GameServer, и это единственная команда во
    // всей цепочке, доказывающая, что он участвует.
    let rec awaitEnter (attemptsLeft: int) : Result<IRPacket, string> =
        if attemptsLeft <= 0 then Error "MC_ENTERMAP не пришёл"
        else
            match waitFor live [ Commands.CMD_MC_ENTERMAP; Commands.CMD_MC_BGNPLAY ] 30.0 with
            | None -> Error "входа в мир не дождались"
            | Some reply when reply.GetCmd() = Commands.CMD_MC_ENTERMAP -> Ok reply
            | Some reply ->
                let code = int16 (reply.ReadInt64())
                reply.Dispose()
                if code = 0s then awaitEnter (attemptsLeft - 1)
                else Error $"вход в мир отклонён: код {code}"

    match awaitEnter 2 with
    | Error reason -> Error reason
    | Ok enter ->

    // Разбор MC_ENTERMAP до сведений о персонаже. Полного разбора в .NET нет,
    // а нужны только идентификатор с позицией — дальше содержимое пакета не
    // читается, и на этом чтение прекращается.
    let errCode = enter.ReadInt64()
    if errCode <> 0L then
        enter.Dispose()
        Error $"вход в карту отклонён, код {errCode}"
    else
        enter.ReadInt64() |> ignore          // autoLock
        enter.ReadInt64() |> ignore          // kitbagLock
        enter.ReadInt64() |> ignore          // enterType
        enter.ReadInt64() |> ignore          // isNewCha
        let mapName = enter.ReadString()
        enter.ReadInt64() |> ignore          // canTeam
        enter.ReadInt64() |> ignore          // imp
        let baseInfo = CommandMessages.Deserialize.deserializeChaBaseInfo enter

        // Читаем дальше до инвентаря и характеристик. Секции разделены
        // сторожевыми числами 0xBEEF000N — по ним видно, что разбор не съехал,
        // а без содержимого сумки нечем ни надеть оружие, ни судить о вещах.
        try
            let expectMark (mark: int64) (what: string) =
                let got = enter.ReadInt64()
                if got <> mark then
                    failwith $"сторож секции {what} не совпал: ждали {mark:X}, пришло {got:X}"

            expectMark 0xBEEF0001L "baseinfo"
            CommandMessages.Deserialize.deserializeChaSkillBagInfo enter |> ignore
            expectMark 0xBEEF0002L "skillbag"
            CommandMessages.Deserialize.deserializeChaSkillStateInfo enter |> ignore
            expectMark 0xBEEF0003L "skillstate"
            let attr = CommandMessages.Deserialize.deserializeChaAttrInfo enter
            expectMark 0xBEEF0004L "attr"
            let kitbag = CommandMessages.Deserialize.deserializeChaKitbagInfo enter

            for entry in attr.Attrs do
                live.Attrs[entry.AttrId] <- entry.AttrVal
            live.Kitbag.Clear()
            for item in kitbag.Items do
                if item.ItemId > 0L then
                    live.Kitbag[item.GridId] <- item.ItemId
        with ex ->
            log $"  (инвентарь из входа не прочитан: {ex.Message})"

        enter.Dispose()

        live.WorldId <- baseInfo.WorldId
        live.Handle <- baseInfo.Handle
        live.MapName <- mapName
        live.PosX <- baseInfo.PosX
        live.PosY <- baseInfo.PosY
        log $"в мире: {baseInfo.Name}, карта {mapName}, позиция ({baseInfo.PosX}, {baseInfo.PosY})"
        Ok live

// ─────────────────────────────────────────────────────────────────────────
//  Действия
// ─────────────────────────────────────────────────────────────────────────

/// Точки пути укладываются в двоичный блок ровно так, как их читает сервер:
/// пара 32-битных чисел на точку, без выравнивания. Число точек сервер
/// выводит из размера блока делением на размер точки.
let private pathBlob (points: (int64 * int64) list) : byte[] =
    let bytes = Array.zeroCreate<byte> (points.Length * 8)
    points
    |> List.iteri (fun i (x, y) ->
        BitConverter.TryWriteBytes(Span<byte>(bytes, i * 8, 4), int32 x) |> ignore
        BitConverter.TryWriteBytes(Span<byte>(bytes, i * 8 + 4, 4), int32 y) |> ignore)
    bytes

/// Отправляет строку в чат. GM-команды идут туда же с префиксом `&`.
let say (live: Live) (text: string) =
    let mutable packet = WPacket(64 + text.Length * 2)
    packet.WriteCmd(Commands.CMD_CM_SAY)
    packet.WriteString(text)
    send live &packet

let moveTo (live: Live) (x: int64) (y: int64) =
    let blob = pathBlob [ (live.PosX, live.PosY); (x, y) ]
    let mutable packet = WPacket(64 + blob.Length)
    packet.WriteCmd(Commands.CMD_CM_BEGINACTION)
    packet.WriteInt64(live.WorldId)
    live.ActionId <- live.ActionId + 1L
    packet.WriteInt64(live.ActionId)
    packet.WriteInt64(CommandMessages.ACT_MOVE)
    packet.WriteSequence(ReadOnlySpan<byte>(blob))
    send live &packet

/// Применяет умение к цели.
///
/// Признак движения: 2 — «подойти и ударить» с передачей пути, 0 — бить с
/// места. Путь передаётся только при 2, иначе сервер его не читает.
let useSkillOnWith (live: Live) (skillId: int64) (target: SeenActor) (chMove: int64) =
    let blob = pathBlob [ (live.PosX, live.PosY); (target.PosX, target.PosY) ]
    let mutable packet = WPacket(96 + blob.Length)
    packet.WriteCmd(Commands.CMD_CM_BEGINACTION)
    packet.WriteInt64(live.WorldId)
    live.ActionId <- live.ActionId + 1L
    packet.WriteInt64(live.ActionId)
    packet.WriteInt64(CommandMessages.ACT_SKILL)
    packet.WriteInt64(chMove)
    packet.WriteInt64(live.ActionId)         // fightId: номер боевого действия
    if chMove = 2L then
        packet.WriteSequence(ReadOnlySpan<byte>(blob))
    packet.WriteInt64(skillId)
    packet.WriteInt64(target.WorldId)        // tarInfo1 — идентификатор цели
    packet.WriteInt64(target.Handle)         // tarInfo2 — handle цели
    send live &packet

/// Код действия внутри разговора: открыть страницу диалога. Сервер читает его
/// вторым полем и по нему решает, какую ветку скрипта NPC вызвать
/// (см. BuildNpcActionTable в NpcScript.cpp).
let private NPC_ACTION_TALKPAGE = 302L

/// Запрашивает разговор с NPC.
///
/// Handle не передаётся — в отличие от выбора цели умения, NPC сервер ищет по
/// одному идентификатору на своей подкарте. Зато нужен код действия и номер
/// страницы: без них скрипт NPC разберёт мусор и промолчит.
let talkTo (live: Live) (target: SeenActor) =
    let mutable packet = WPacket(64)
    packet.WriteCmd(Commands.CMD_CM_REQUESTNPC)
    packet.WriteInt64(target.WorldId)
    packet.WriteInt64(NPC_ACTION_TALKPAGE)
    packet.WriteInt64(0L)                     // номер страницы: начальная
    send live &packet

/// Подключается и проходит вход, возвращая соединение вместе с сессией.
///
/// Соединение отдаётся наружу, потому что владеть им должен вызывающий: при
/// смене карты старое закрывается и открывается новое.
let connect (host: string) (port: int) (account: string) (password: string)
            : Result<NetworkStream * Live, string> =
    let client = new TcpClient()
    client.Connect(host, port)
    let stream = client.GetStream()
    stream.ReadTimeout <- 15000
    match logIn stream account password with
    | Ok live -> Ok(stream, live)
    | Error reason ->
        client.Dispose()
        Error reason

/// Ждёт, пока сервер закроет соединение.
///
/// Нужно там, где сервер сам обрывает связь: при отказе входа или кике. Для
/// перехода между картами это НЕ требуется — Gate сохраняет канал.
let awaitDisconnect (live: Live) (seconds: float) : bool =
    let deadline = DateTime.UtcNow.AddSeconds(seconds)
    let mutable closed = false
    live.Stream.ReadTimeout <- 500
    while not closed && DateTime.UtcNow < deadline do
        try
            match readPacket live.Stream with
            | None -> closed <- true
            | Some packet ->
                absorb live packet
                packet.Dispose()
        with
        | :? IO.IOException -> ()
        | :? ObjectDisposedException -> closed <- true
    closed

/// Корректно выходит из игры.
///
/// Без этого сервер считает игрока в сети до собственного тайм-аута, и
/// следующий прогон упирается в отказ входа. Разрыв TCP сам по себе выходом
/// не считается.
let logOut (live: Live) =
    try
        let mutable packet = WPacket(32)
        packet.WriteCmd(Commands.CMD_CM_LOGOUT)
        send live &packet
        drain live 2.0
    with _ -> ()      // соединение могло уже закрыться — выход всё равно состоялся


/// Удар с подходом к цели — обычный случай.
let useSkillOn (live: Live) (skillId: int64) (target: SeenActor) =
    useSkillOnWith live skillId target 2L


/// Слоты экипировки (enumEQUIP_* из CompCommand.h). Оружие держат руки.
let EQUIP_RHAND = 9L
let EQUIP_HAND1 = 7L

/// Надевает предмет: перекладывает его из ячейки сумки в слот экипировки.
///
/// Действие адресуется ячейками, а не номерами предметов, поэтому содержимое
/// сумки приходится знать заранее — оно приходит внутри MC_ENTERMAP.
let equipItem (live: Live) (fromGrid: int64) (toSlot: int64) =
    let mutable packet = WPacket(64)
    packet.WriteCmd(Commands.CMD_CM_BEGINACTION)
    packet.WriteInt64(live.WorldId)
    live.ActionId <- live.ActionId + 1L
    packet.WriteInt64(live.ActionId)
    packet.WriteInt64(11L)                   // ActionType::ITEM_USE
    packet.WriteInt64(fromGrid)
    packet.WriteInt64(toSlot)
    send live &packet

/// Действия с NPC внутри разговора (см. BuildNpcActionTable в NpcScript.cpp).
let NPC_ACTION_FUNCITEM = 303L
let NPC_ACTION_TRADEITEM = 309L

/// Открывает у NPC страницу с перечнем товаров.
let openNpcPage (live: Live) (target: SeenActor) (page: int64) (item: int64) =
    let mutable packet = WPacket(64)
    packet.WriteCmd(Commands.CMD_CM_REQUESTNPC)
    packet.WriteInt64(target.WorldId)
    packet.WriteInt64(NPC_ACTION_FUNCITEM)
    packet.WriteInt64(page)
    packet.WriteInt64(item)
    send live &packet

/// Продаёт предмет из ячейки сумки торговцу.
///
/// Тип сделки 0 — продажа: за ним идут номер ячейки и количество. Покупка
/// (тип 1) требует ещё и указания страницы витрины, которую сперва надо
/// получить, поэтому проверяется именно продажа.
let sellItem (live: Live) (target: SeenActor) (grid: int64) (count: int64) =
    let mutable packet = WPacket(96)
    packet.WriteCmd(Commands.CMD_CM_REQUESTNPC)
    packet.WriteInt64(target.WorldId)
    packet.WriteInt64(NPC_ACTION_TRADEITEM)
    packet.WriteInt64(0L)                     // ROLE_TRADE_SALE
    packet.WriteInt64(grid)
    packet.WriteInt64(count)
    send live &packet

/// Поднимает лежащий на земле предмет.
let pickItem (live: Live) (worldId: int64) (handle: int64) =
    let mutable packet = WPacket(64)
    packet.WriteCmd(Commands.CMD_CM_BEGINACTION)
    packet.WriteInt64(live.WorldId)
    live.ActionId <- live.ActionId + 1L
    packet.WriteInt64(live.ActionId)
    packet.WriteInt64(8L)                     // ActionType::ITEM_PICK
    packet.WriteInt64(worldId)
    packet.WriteInt64(handle)
    send live &packet


/// Ждёт нового входа в карту после перехода.
///
/// Соединение при переходе не рвётся: GameServer отключает игрока у себя, но
/// Gate удерживает канал клиента и переводит его на сервер целевой карты,
/// откуда приходит новый MC_ENTERMAP. Клиенту остаётся дождаться его и
/// перечитать своё положение — переподключение было бы ошибкой.
let awaitMapEntry (live: Live) (seconds: float) : Result<string, string> =
    // Читаем короткими попытками и глотаем тайм-ауты: переход идёт через два
    // сервера и занимает секунды, а долгое чтение одним куском упирается в
    // тайм-аут сокета и вылетает исключением.
    let deadline = DateTime.UtcNow.AddSeconds(seconds)
    let saved = live.Stream.ReadTimeout
    live.Stream.ReadTimeout <- 500
    let mutable found = None
    let mutable running = true
    while running && DateTime.UtcNow < deadline do
        try
            match readPacket live.Stream with
            | None -> running <- false
            | Some packet ->
                if packet.GetCmd() = Commands.CMD_MC_ENTERMAP then
                    found <- Some packet
                    running <- false
                else
                    absorb live packet
                    packet.Dispose()
        with :? IO.IOException -> ()
    live.Stream.ReadTimeout <- saved

    match found with
    | None ->
        // Отказ приходит системным сообщением, а не отдельной командой:
        // «Cannot find the destination map» шлёт сам Gate.
        let said = if live.Notices.Count = 0 then "молча" else String.concat " | " live.Notices
        Error $"нового входа в карту не пришло ({said})"
    | Some enter ->
        let errCode = enter.ReadInt64()
        if errCode <> 0L then
            enter.Dispose()
            Error $"вход в карту отклонён, код {errCode}"
        else
            enter.ReadInt64() |> ignore      // autoLock
            enter.ReadInt64() |> ignore      // kitbagLock
            enter.ReadInt64() |> ignore      // enterType
            enter.ReadInt64() |> ignore      // isNewCha
            let mapName = enter.ReadString()
            enter.ReadInt64() |> ignore      // canTeam
            enter.ReadInt64() |> ignore      // imp
            let baseInfo = CommandMessages.Deserialize.deserializeChaBaseInfo enter
            enter.Dispose()

            live.WorldId <- baseInfo.WorldId
            live.Handle <- baseInfo.Handle
            live.MapName <- mapName
            live.PosX <- baseInfo.PosX
            live.PosY <- baseInfo.PosY
            // Прежнее окружение осталось на старой карте.
            live.Seen.Clear()
            Ok mapName
