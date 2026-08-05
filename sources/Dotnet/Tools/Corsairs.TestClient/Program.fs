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

        // Ответ идёт не сразу: Gate спрашивает Group, тот — Account.
        let deadline = DateTime.UtcNow.AddSeconds(20.0)
        let mutable result = 1
        let mutable waiting = true

        while waiting && DateTime.UtcNow < deadline do
            match readPacket stream with
            | None ->
                log "ОШИБКА соединение закрыто сервером"
                waiting <- false
            | Some packet ->
                let cmd = packet.GetCmd()
                if cmd = Commands.CMD_MC_LOGIN then
                    match CommandMessages.Deserialize.mcLoginResponse packet with
                    | CommandMessages.McLoginError code ->
                        log $"ВХОД ОТКЛОНЁН: {describeLoginError code}"
                        result <- 1
                    | CommandMessages.McLoginSuccess data ->
                        log $"ВХОД ВЫПОЛНЕН: слотов {data.MaxChaNum}, персонажей {data.Characters.Length}"
                        data.Characters
                        |> Array.iteri (fun i cha ->
                            if cha.Valid then
                                log $"  слот {i}: {cha.ChaName}, профессия {cha.Job}, уровень {cha.Degree}"
                            else
                                log $"  слот {i}: пусто")
                        result <- 0
                    waiting <- false
                else
                    log $"Получена команда {cmd} ({packet.PayloadLength} байт) — жду ответа на вход"
                packet.Dispose()

        if waiting then
            log "ОШИБКА ответа на вход не дождались"

        Thread.Sleep(200)
        result
