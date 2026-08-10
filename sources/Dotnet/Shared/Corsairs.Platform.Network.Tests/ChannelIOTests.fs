module Corsairs.Platform.Network.Tests.ChannelIOTests

open System
open System.Net
open System.Net.Sockets
open Xunit
open Corsairs.Platform.Network.Network

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
