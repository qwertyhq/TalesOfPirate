module Corsairs.Platform.Network.Tests.ChannelIOTests

open System
open System.Net
open System.Net.Sockets
open System.Threading
open System.Threading.Tasks
open Xunit
open Corsairs.Platform.Network.Network
open Microsoft.Extensions.Logging

/// Алиас: имя конфликтует с System.Net.Sockets.TcpListener.
type private NetListener = Corsairs.Platform.Network.Network.TcpListener

let private loggerFactory = LoggerFactory.Create(fun b -> b.SetMinimumLevel(LogLevel.Debug) |> ignore)

/// Найти свободный TCP-порт.
let private freePort () =
    let l = new System.Net.Sockets.TcpListener(IPAddress.Loopback, 0)
    l.Start()
    let port = (l.LocalEndpoint :?> IPEndPoint).Port
    l.Stop()
    port

let private loopback port = IPEndPoint(IPAddress.Loopback, port)

/// Новый TCP-сокет для клиентской стороны.
let private newSocket () =
    new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)

/// Пара соединённых сокетов: (принятый сервером, клиентский).
let private connectedPair () =
    let listener = newSocket ()
    listener.Bind(loopback 0)
    listener.Listen(1)
    let client = newSocket ()
    client.Connect(listener.LocalEndPoint)
    let accepted = listener.Accept()
    listener.Close()
    accepted, client

let private awaitTimeout (tcs: TaskCompletionSource<'T>) (ms: int) =
    task {
        let! completed = Task.WhenAny(tcs.Task, Task.Delay(ms))
        if completed = (tcs.Task :> Task) then return tcs.Task.Result
        else return failwith $"Таймаут ожидания ({ms}мс)"
    }

/// Дождаться условия опросом.
let private waitUntil (predicate: unit -> bool) (ms: int) =
    task {
        let deadline = DateTime.UtcNow.AddMilliseconds(float ms)
        while not (predicate ()) && DateTime.UtcNow < deadline do
            do! Task.Delay(20)
        return predicate ()
    }

// ═══════════════════════════════════════════════════════════════
//  ChannelIO.RemoteEndPoint — адрес переживает смерть сокета
// ═══════════════════════════════════════════════════════════════

module RemoteEndPoint =

    [<Fact>]
    let ``Адрес известен сразу после подключения`` () =
        let accepted, client = connectedPair ()
        use _accepted = accepted
        use _client = client

        let channel = ChannelIO(accepted, null)

        Assert.Equal(client.LocalEndPoint :?> IPEndPoint, channel.RemoteEndPoint)

    /// Ключевой регрессионный тест: первое обращение к адресу происходит уже
    /// ПОСЛЕ закрытия сокета. Ленивое чтение здесь бросало
    /// ObjectDisposedException и убивало главный цикл сервера.
    [<Fact>]
    let ``Адрес доступен после закрытия канала, даже если до этого не запрашивался`` () =
        let accepted, client = connectedPair ()
        use _client = client
        let expected = client.LocalEndPoint :?> IPEndPoint

        let channel = ChannelIO(accepted, null)
        channel.Close()

        // Сам сокет уже мёртв — обращение к нему напрямую бросает.
        Assert.Throws<ObjectDisposedException>(fun () -> accepted.RemoteEndPoint |> ignore) |> ignore
        // Канал обязан отдать снятый заранее адрес и не бросить.
        Assert.Equal(expected, channel.RemoteEndPoint)

    [<Fact>]
    let ``Адрес неизвестен у ещё не подключённого сокета`` () =
        use socket = newSocket ()
        let channel = ChannelIO(socket, null)

        Assert.Equal(ChannelIO.UnknownEndPoint, channel.RemoteEndPoint)

    [<Fact>]
    let ``Адрес подхватывается после отложенного подключения`` () =
        let listener = newSocket ()
        listener.Bind(loopback 0)
        listener.Listen(1)
        let listenEndPoint = listener.LocalEndPoint :?> IPEndPoint

        // Исходящий канал создаётся до Connect — как в NetworkPlatform.ConnectAsync.
        use client = newSocket ()
        let channel = ChannelIO(client, null)
        Assert.Equal(ChannelIO.UnknownEndPoint, channel.RemoteEndPoint)

        client.Connect(listenEndPoint)
        use accepted = listener.Accept()
        listener.Close()

        Assert.Equal(listenEndPoint, channel.RemoteEndPoint)

    [<Fact>]
    let ``ToString не бросает на канале с умершим сокетом`` () =
        let accepted, client = connectedPair ()
        use _client = client

        let channel = ChannelIO(accepted, null)
        // Сокет умирает мимо ChannelIO.Close — канал ещё не помечен закрытым.
        accepted.Close()

        let text = channel.ToString()
        Assert.Contains("Channel#", text)

// ═══════════════════════════════════════════════════════════════
//  Живучесть: сбой одного соединения не останавливает сервер
// ═══════════════════════════════════════════════════════════════

module Resilience =

    let private channelFactory (socket, handler) = ChannelIO(socket, handler)

    /// Пакет протокола: [2b размер BE][4b SESS BE][2b CMD BE].
    let private commandBytes (cmd: uint16) =
        [| 0uy; 8uy; 0uy; 0uy; 0uy; 0uy; byte (cmd >>> 8); byte cmd |]

    /// Повторить сценарий на новом порту. freePort освобождает порт до того,
    /// как его займёт сервер, и в этот промежуток порт может достаться другому
    /// процессу — гонка заложена в самом способе выбора порта (тот же приём
    /// применён в SystemCommandTests).
    let private withRetries (scenario: unit -> Task<unit>) =
        task {
            let mutable attempt = 0
            let mutable succeeded = false
            let mutable lastError: exn = null

            while not succeeded && attempt < 5 do
                attempt <- attempt + 1

                try
                    do! scenario ()
                    succeeded <- true
                with ex ->
                    lastError <- ex

            if not succeeded then
                failwith $"Сценарий не прошёл за {attempt} попыток: {lastError.Message}"
        }

    [<Fact>]
    let ``Сбой обработчика accept не останавливает приём соединений`` () =
        withRetries (fun () ->
            task {
                let port = freePort ()
                use cts = new CancellationTokenSource(15000)
                let accepted = ref 0
                let secondAccepted = TaskCompletionSource<unit>()

                use listener = new NetListener(loggerFactory.CreateLogger<NetListener>())

                listener.OnAccept.Add(fun socket ->
                    socket.Close()
                    incr accepted

                    if accepted.Value = 1 then
                        failwith "падение обработчика на первом соединении"
                    else
                        secondAccepted.TrySetResult(()) |> ignore)

                listener.Start([| loopback port |], cts.Token)
                // Даём слушателю встать на порт: Start уходит в фоновую задачу.
                do! Task.Delay(200)

                use first = newSocket ()
                first.Connect(loopback port)
                let! _ = waitUntil (fun () -> accepted.Value >= 1) 5000

                use second = newSocket ()
                second.Connect(loopback port)

                let! _ = awaitTimeout secondAccepted 5000
                Assert.Equal(2, accepted.Value)
            })

    /// Живой сценарий из отчёта: клиент подключается и тут же рвёт соединение
    /// (RST через SO_LINGER 0). До починки первый же такой обрыв убивал цикл
    /// приёма — порт продолжал слушаться ядром, а сервер молчал.
    [<Fact>]
    let ``Резкий обрыв сразу после accept не останавливает сервер`` () =
        withRetries (fun () ->
            task {
                let port = freePort ()
                use cts = new CancellationTokenSource(20000)
                let survivorCmd = 321us
                let delivered = TaskCompletionSource<uint16>()

                use server =
                    new DirectSystemCommand<ChannelIO>(
                        { Endpoints = [| loopback port |] },
                        channelFactory,
                        loggerFactory)

                // Обращение к адресу — ровно то место, где падал главный цикл сервера.
                server.OnConnected.Add(fun ch -> ch.RemoteEndPoint |> ignore)

                server.OnCommand.Add(fun (_, packet) ->
                    delivered.TrySetResult(packet.GetCmd()) |> ignore)

                server.Start(cts.Token)

                for _ in 1..5 do
                    let socket = newSocket ()
                    socket.Connect(loopback port)
                    socket.LingerState <- LingerOption(true, 0)
                    socket.Close()

                // После череды обрывов сервер обязан принять соединение и команду.
                use survivor = newSocket ()
                survivor.Connect(loopback port)
                survivor.Send(commandBytes survivorCmd) |> ignore

                let! cmd = awaitTimeout delivered 5000
                Assert.Equal(survivorCmd, cmd)
            })

    /// Насос IOCP один на процесс: исключение из подписчика одного канала
    /// останавливало ввод-вывод всего сервера.
    [<Fact>]
    let ``Сбой обработчиков одного канала не останавливает ввод-вывод`` () =
        withRetries (fun () ->
            task {
                let port = freePort ()
                use cts = new CancellationTokenSource(20000)
                let poisonCmd = 100us
                let goodCmd = 123us
                let commandFailures = ref 0
                let disconnectFailures = ref 0
                let delivered = TaskCompletionSource<uint16>()

                use server =
                    new DirectSystemCommand<ChannelIO>(
                        { Endpoints = [| loopback port |] },
                        channelFactory,
                        loggerFactory)

                server.OnCommand.Add(fun (_, packet) ->
                    if packet.GetCmd() = poisonCmd then
                        incr commandFailures
                        failwith "падение обработчика команды"
                    else
                        delivered.TrySetResult(packet.GetCmd()) |> ignore)

                server.OnDisconnected.Add(fun _ ->
                    // Падаем на закрытии отравленного канала: до починки исключение
                    // из этого обработчика вылетало в общий насос и убивало его.
                    if commandFailures.Value > 0 && disconnectFailures.Value = 0 then
                        incr disconnectFailures
                        failwith "падение обработчика отключения")

                server.Start(cts.Token)
                // Даём слушателю встать на порт: Start уходит в фоновую задачу.
                do! Task.Delay(200)

                let poisoned = newSocket ()
                poisoned.Connect(loopback port)
                poisoned.Send(commandBytes poisonCmd) |> ignore
                let! poisonHandled = waitUntil (fun () -> disconnectFailures.Value = 1) 5000
                poisoned.Close()
                Assert.True(poisonHandled, "отравленный канал не дошёл до обработчика отключения")

                use survivor = newSocket ()
                survivor.Connect(loopback port)
                survivor.Send(commandBytes goodCmd) |> ignore

                let! cmd = awaitTimeout delivered 5000
                Assert.Equal(goodCmd, cmd)
            })
