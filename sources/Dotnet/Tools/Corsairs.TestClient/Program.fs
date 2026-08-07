/// Консольный клиент для сквозной проверки серверной связки.
///
///     dotnet run --project sources/Dotnet/Tools/Corsairs.TestClient -- \
///         [login|mechanics] [хост] [порт] [учётная-запись] [пароль]
///
/// По умолчанию: login 127.0.0.1 1973 admin admin.
///
/// Режим `login` проходит вход целиком: подключение к GateServer, рукопожатие,
/// CM_LOGIN, выбор персонажа и вход в мир. Ответ приходит от GameServer через
/// Gate, Group и Account, поэтому успех означает, что работает вся цепочка.
///
/// Режим `mechanics` после входа прогоняет игровые механики настоящими
/// клиентскими пакетами и печатает свод пройденного и проваленного.
///
/// Зачем отдельный клиент. Клиент на Unreal — это игровой слой, которого пока
/// нет, а старый клиент на DirectX 9 запускается только под Windows или через
/// CrossOver и ничего не сообщает о себе, кроме журналов. Без этого проверить
/// связку можно было бы лишь по логам серверов, то есть по тому, что серверы
/// сами о себе сообщают.
module Corsairs.TestClient.Program

open System.Net.Sockets
open Corsairs.TestClient.Session

/// Умение обычной атаки. Значение подтверждено журналом настоящего клиента:
/// он шлёт номер 28 при ударе по цели.
let private DEFAULT_SKILL = 28L

/// Монстр для призыва, когда рядом ни одного нет. «Melon» — первого уровня,
/// живёт в базе персонажей с ctrl_type = 5 и не убивает бота ответным ударом.
let private DEFAULT_SUMMON = 96L

[<EntryPoint>]
let main argv =
    // Режим необязателен: без него ведём себя как прежде, чтобы прежние
    // вызовы с одним лишь адресом продолжали работать.
    let known = [| "login"; "mechanics" |]
    let hasMode = argv.Length > 0 && Array.contains argv[0] known
    let mode = if hasMode then argv[0] else "login"
    let rest = if hasMode then argv[1..] else argv

    let host     = if rest.Length > 0 then rest[0] else "127.0.0.1"
    let port     = if rest.Length > 1 then int rest[1] else 1973
    let account  = if rest.Length > 2 then rest[2] else "admin"
    let password = if rest.Length > 3 then rest[3] else "admin"

    log $"Подключаюсь к {host}:{port} как {account} (режим {mode})"

    use client = new TcpClient()
    client.Connect(host, port)
    use stream = client.GetStream()
    stream.ReadTimeout <- 15000
    log "Соединение установлено"

    match logIn stream account password with
    | Error reason ->
        log $"ОШИБКА {reason}"
        1
    | Ok live ->
        if mode = "mechanics" then
            Mechanics.run live DEFAULT_SKILL DEFAULT_SUMMON
        else
            log "ВХОД В МИР ВЫПОЛНЕН"
            0
