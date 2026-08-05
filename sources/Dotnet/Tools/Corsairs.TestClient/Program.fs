/// Консольный клиент для сквозной проверки серверной связки.
///
///     dotnet run --project sources/Dotnet/Tools/Corsairs.TestClient -- \
///         [хост] [порт] [учётная-запись] [пароль]
///
/// По умолчанию: 127.0.0.1 1973 admin admin.
///
/// Проходит вход целиком: подключение к GateServer, рукопожатие, CM_LOGIN и
/// разбор списка персонажей. Ответ приходит от GameServer через Gate, Group и
/// Account, поэтому успешный вход означает, что работает вся цепочка, а не
/// только точка входа.
///
/// Зачем отдельный клиент. Клиент на Unreal — это игровой слой, которого пока
/// нет, а старый клиент на DirectX 9 вне Windows не запускается. Без этого
/// проверить связку можно было бы только по логам серверов, то есть по тому,
/// что серверы сами о себе сообщают.
module Corsairs.TestClient.Program

open System
open System.Net.Sockets
open System.Threading
open Corsairs.Platform.Network.Protocol

/// Версия клиента. Gate сверяет её с настройкой ClientVersion и отвергает
/// вход при несовпадении — значение обязано совпадать с appsettings.json.
let CLIENT_VERSION = 32125L

/// Заголовок пакета: размер (uint16), счётчик (uint32), команда (uint16).
let HEADER_SIZE = 8

/// Формат времени вынесен в константу: интерполяция F# не принимает ни
/// спецификаторы вида {x:HH:mm} — двоеточие там разбирается как часть
/// выражения, — ни строковые литералы внутри самой интерполяции.
let private TIME_FORMAT = "HH:mm:ss.fff"

let private log (text: string) =
    let stamp = DateTime.Now.ToString(TIME_FORMAT)
    Console.WriteLine($"[{stamp}] {text}")

/// Читает ровно `count` байт либо возвращает None, если соединение закрылось.
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
let private readPacket (stream: NetworkStream) : IRPacket option =
    match readExact stream 2 with
    | None -> None
    | Some sizeBytes ->
        let size = int (System.Buffers.Binary.BinaryPrimitives.ReadUInt16BigEndian(ReadOnlySpan<byte>(sizeBytes)))
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

/// Отправляет пакет, проставив счётчик.
///
/// Gate при включённой защите WPE сверяет поле счётчика с собственным и
/// разрывает соединение при расхождении. Счётчик начинается с нуля и растёт
/// на единицу за каждый принятый сервером пакет.
let private sendPacket (stream: NetworkStream) (packet: WPacket byref) (counter: uint32) =
    packet.WriteSess(counter)
    let memory = packet.GetPacketMemory()
    stream.Write(memory.Span)
    stream.Flush()

/// Параметры создаваемого персонажа.
///
/// GroupServer проверяет внешность по таблице: причёска зависит от типа тела,
/// лицо — общее. Значения ниже взяты из первой допустимой пары для типа 1.
/// Место рождения обязано совпадать со строкой из настройки Birthplaces
/// GroupServer'а; Argent City — это карта garner.
/// Метка времени в имени персонажа: имя обязано быть уникальным, а повторный
/// прогон против той же базы иначе упирался бы в отказ по занятому имени.
/// Допустимы только буквы, цифры и подчёркивание — отсюда формат без разделителей.
let NAME_STAMP_FORMAT = "HHmmss"

/// Второй пароль учётной записи. GroupServer принимает только буквы и цифры
/// и не пускает в мир, пока он не задан.
let PASSWORD2 = "test1234"

let NEW_CHA_BIRTH = "Argent City"
let NEW_CHA_TYPE = 1L
let NEW_CHA_HAIR = 2000L
let NEW_CHA_FACE = 2554L

let private describeLoginError (code: int16) =
    match code with
    | 1s -> "нет такой учётной записи"
    | 2s -> "неверный пароль"
    | 3s -> "учётная запись заблокирована"
    | 4s -> "уже в игре"
    | _  -> $"код {code}"

[<EntryPoint>]
let main argv =
    let host    = if argv.Length > 0 then argv[0] else "127.0.0.1"
    let port    = if argv.Length > 1 then Int32.Parse argv[1] else 1973
    let account = if argv.Length > 2 then argv[2] else "admin"
    let password = if argv.Length > 3 then argv[3] else "admin"

    // Реализация хеша своя, поэтому сверяемся с известными значениями до
    // того, как отказ во входе спишут на неверный пароль.
    match Blake2s.SelfTest() with
    | Some problem ->
        log $"ОШИБКА самопроверка BLAKE2s не прошла: {problem}"
        exit 1
    | None -> ()

    // Сервер хранит и сравнивает хеш как строку, поэтому клиент шлёт именно
    // hex-представление, а не сам пароль.
    let passwordHash = Blake2s.HashPassword password

    log $"Подключаюсь к {host}:{port} как {account}"

    use client = new TcpClient()
    client.Connect(host, port)
    use stream = client.GetStream()
    stream.ReadTimeout <- 15000
    log "Соединение установлено"

    // Первым говорит сервер: при выключенном RSA-AES это пустое рукопожатие,
    // при включённом — открытый ключ RSA.
    match readPacket stream with
    | None ->
        log "ОШИБКА сервер закрыл соединение до рукопожатия"
        1
    | Some handshake ->
        let cmd = handshake.GetCmd()
        log $"Получено рукопожатие: команда {cmd}, полезных байт {handshake.PayloadLength}"
        handshake.Dispose()

        if cmd <> Commands.CMD_MC_SEND_SERVER_PUBLIC_KEY then
            log $"ОШИБКА ожидалась команда {Commands.CMD_MC_SEND_SERVER_PUBLIC_KEY}"
            1
        else

        let mutable counter = 0u

        let mutable login = WPacket(256)
        login.WriteCmd(Commands.CMD_CM_LOGIN)
        login.WriteString(account)
        login.WriteString(passwordHash)
        login.WriteString("00-00-00-00-00-00")    // MAC — сервер только пишет его в журнал
        login.WriteInt64(0L)                      // CheatMarker
        login.WriteInt64(CLIENT_VERSION)
        sendPacket stream &login counter
        counter <- counter + 1u
        log $"Отправлен CM_LOGIN (версия клиента {CLIENT_VERSION})"

        /// Ждёт пакет с одной из ожидаемых команд, пропуская остальные.
        ///
        /// Ответ приходит не сразу и не один: Gate спрашивает Group, тот —
        /// Account или GameServer, а попутно шлёт клиенту служебные команды.
        let waitFor (expected: uint16 list) (seconds: float) : IRPacket option =
            let deadline = DateTime.UtcNow.AddSeconds(seconds)
            let mutable found = None
            let mutable searching = true
            while searching && DateTime.UtcNow < deadline do
                match readPacket stream with
                | None ->
                    log "ОШИБКА соединение закрыто сервером"
                    searching <- false
                | Some packet ->
                    if List.contains (packet.GetCmd()) expected then
                        found <- Some packet
                        searching <- false
                    else
                        log $"  (попутно команда {packet.GetCmd()}, {packet.PayloadLength} байт)"
                        packet.Dispose()
            found

        let mutable result = 1

        match waitFor [ Commands.CMD_MC_LOGIN ] 20.0 with
        | None ->
            log "ОШИБКА ответа на вход не дождались"
        | Some packet ->
            let response = CommandMessages.Deserialize.mcLoginResponse packet
            packet.Dispose()

            match response with
            | CommandMessages.McLoginError code ->
                log $"ВХОД ОТКЛОНЁН: {describeLoginError code}"
            | CommandMessages.McLoginSuccess data ->
                log $"ВХОД ВЫПОЛНЕН: слотов {data.MaxChaNum}, персонажей {data.Characters.Length}"
                data.Characters
                |> Array.iteri (fun i cha ->
                    if cha.Valid then
                        log $"  слот {i}: {cha.ChaName}, профессия {cha.Job}, уровень {cha.Degree}"
                    else
                        log $"  слот {i}: пусто")

                // Второй пароль обязателен для входа в мир: GroupServer
                // отвергает BGNPLAY, пока он пуст, кодом ERR_PT_INVALID_PW2.
                // У новой учётной записи его нет, поэтому создаём.
                if not data.HasPassword2 then
                    let mutable pw2 = WPacket(64)
                    pw2.WriteCmd(Commands.CMD_CM_CREATE_PASSWORD2)
                    pw2.WriteString(PASSWORD2)
                    sendPacket stream &pw2 counter
                    counter <- counter + 1u
                    log "Отправлен CM_CREATE_PASSWORD2"

                    match waitFor [ Commands.CMD_MC_CREATE_PASSWORD2 ] 20.0 with
                    | None -> log "ОШИБКА ответа на создание второго пароля не дождались"
                    | Some reply ->
                        let code = int16 (reply.ReadInt64())
                        reply.Dispose()
                        if code = 0s then
                            log "ВТОРОЙ ПАРОЛЬ СОЗДАН"
                        else
                            log $"СОЗДАНИЕ ВТОРОГО ПАРОЛЯ ОТКЛОНЕНО: код {code}"

                // Персонаж нужен, чтобы дойти до GameServer: вход в учётную
                // запись его не затрагивает вовсе.
                let existing = data.Characters |> Array.tryFindIndex (fun c -> c.Valid)

                let slot =
                    match existing with
                    | Some index ->
                        log $"Использую персонажа из слота {index}"
                        Some index
                    | None ->
                        let name = $"Test{DateTime.Now.ToString(NAME_STAMP_FORMAT)}"
                        let mutable create = WPacket(128)
                        create.WriteCmd(Commands.CMD_CM_NEWCHA)
                        create.WriteString(name)
                        create.WriteString(NEW_CHA_BIRTH)
                        create.WriteInt64(NEW_CHA_TYPE)
                        create.WriteInt64(NEW_CHA_HAIR)
                        create.WriteInt64(NEW_CHA_FACE)
                        sendPacket stream &create counter
                        counter <- counter + 1u
                        log $"Отправлен CM_NEWCHA: {name}, место рождения {NEW_CHA_BIRTH}"

                        match waitFor [ Commands.CMD_MC_NEWCHA ] 20.0 with
                        | None ->
                            log "ОШИБКА ответа на создание персонажа не дождались"
                            None
                        | Some reply ->
                            let code = int16 (reply.ReadInt64())
                            reply.Dispose()
                            if code <> 0s then
                                log $"СОЗДАНИЕ ОТКЛОНЕНО: код {code}"
                                None
                            else
                                log $"ПЕРСОНАЖ СОЗДАН: {name}"
                                Some 0

                match slot with
                | None -> ()
                | Some index ->
                    let mutable play = WPacket(32)
                    play.WriteCmd(Commands.CMD_CM_BGNPLAY)
                    play.WriteInt64(int64 index)
                    sendPacket stream &play counter
                    counter <- counter + 1u
                    log $"Отправлен CM_BGNPLAY: слот {index}"

                    // Успех подтверждается не ответом на BGNPLAY, а входом в
                    // карту: MC_ENTERMAP приходит уже от GameServer, и это
                    // единственная команда во всей цепочке, доказывающая, что
                    // он участвует.
                    match waitFor [ Commands.CMD_MC_ENTERMAP; Commands.CMD_MC_BGNPLAY ] 30.0 with
                    | None ->
                        log "ОШИБКА входа в мир не дождались"
                    | Some reply ->
                        let cmd = reply.GetCmd()
                        if cmd = Commands.CMD_MC_ENTERMAP then
                            log "ВХОД В МИР ВЫПОЛНЕН: получен MC_ENTERMAP от GameServer"
                            result <- 0
                        else
                            let code = int16 (reply.ReadInt64())
                            if code = 0s then
                                log "BGNPLAY принят, жду MC_ENTERMAP"
                                match waitFor [ Commands.CMD_MC_ENTERMAP ] 30.0 with
                                | None -> log "ОШИБКА MC_ENTERMAP не пришёл"
                                | Some enter ->
                                    log "ВХОД В МИР ВЫПОЛНЕН: получен MC_ENTERMAP от GameServer"
                                    enter.Dispose()
                                    result <- 0
                            else
                                log $"ВХОД В МИР ОТКЛОНЁН: код {code}"
                        reply.Dispose()

        Thread.Sleep(200)
        result
