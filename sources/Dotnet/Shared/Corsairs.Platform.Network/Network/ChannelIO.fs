namespace rec Corsairs.Platform.Network.Network

open System
open System.Net
open System.Net.Sockets
open System.Runtime.CompilerServices
open System.Threading
open Corsairs.Platform.Network
open Corsairs.Platform.Network.Crypto
open Corsairs.Platform.Network.Protocol

/// Обёртка TCP-сокета: уникальный ID, отправка команд, статистика.
/// Наследники добавляют бизнес-логику (данные сессии и т.д.).
[<AllowNullLiteral>]
type ChannelIO(socket: Socket, handler: IoHandler) =
    static let mutable _generator = 0u

    /// Адрес, который отдаётся, когда удалённая сторона неизвестна:
    /// сокет уже закрыт либо ещё не подключён.
    static let unknownEndPoint = IPEndPoint(IPAddress.None, 0)

    /// Снять удалённый адрес с сокета; null — адрес пока недоступен.
    /// Socket.RemoteEndPoint бросает ObjectDisposedException на закрытом сокете и
    /// SocketException, пока соединение не установлено (исходящий канал создаётся
    /// до ConnectAsync). Оба случая означают «адреса нет», а не сбой канала,
    /// поэтому переводятся в null, а не в исключение у вызывающего.
    static let tryReadRemoteEndPoint (s: Socket) : IPEndPoint =
        try
            match s.RemoteEndPoint with
            | :? IPEndPoint as ep -> ep
            | _ -> null
        with
        | :? ObjectDisposedException -> null
        | :? SocketException -> null

    let _id = ChannelId_ (Interlocked.Increment(&_generator))
    let mutable _resetOperation = false
    let mutable _disposed = false
    let _stats = ChannelStats()

    // Адрес снимается сразу при создании канала, пока сокет заведомо жив.
    // Ленивое чтение приводило к ObjectDisposedException, когда до первого
    // обращения соединение успевало оборваться (и Lazy кэшировал исключение).
    let mutable _remoteEndPoint = tryReadRemoteEndPoint socket

    /// Маркер «удалённый адрес неизвестен».
    static member UnknownEndPoint = unknownEndPoint

    /// Уникальный ID канала.
    member _.Id =  _id

    /// Сокет.
    member _.Socket = socket

    /// Удалённый адрес. Никогда не бросает: для закрытого или ещё не подключённого
    /// сокета возвращает UnknownEndPoint.
    member _.RemoteEndPoint : IPEndPoint =
        match _remoteEndPoint with
        | null ->
            // Исходящий канал создаётся до ConnectAsync — адреса при создании ещё нет,
            // пробуем снять его повторно и запомнить.
            match tryReadRemoteEndPoint socket with
            | null -> unknownEndPoint
            | ep ->
                _remoteEndPoint <- ep
                ep
        | ep -> ep

    /// Статистика I/O.
    member _.Stats = _stats

    /// Флаг остановки операций (после ResetOperation новые send не принимаются).
    member _.IsResetOperation = Volatile.Read(&_resetOperation)

    /// Пометить канал как остановленный.
    member _.ResetOperation() = Volatile.Write(&_resetOperation, true)

    /// Шифрование исходящего пакета перед отправкой. По умолчанию — passthrough.
    /// Наследники перегружают для AES-шифрования (PlayerChannelIO).
    /// При шифровании: создаёт новый WPacket, оригинал Dispose'ится внутри.
    abstract EncryptOutgoing: WPacket -> WPacket
    default _.EncryptOutgoing(packet) = packet

    /// Расшифровка входящего пакета после приёма. По умолчанию — passthrough.
    /// Наследники перегружают для AES-дешифровки (PlayerChannelIO).
    /// При дешифровке: возвращает новый IRPacket, вызывающий должен Dispose'ить.
    abstract DecryptIncoming: IRPacket -> IRPacket
    default _.DecryptIncoming(packet) = packet

    /// Отправить WPacket. Перед отправкой вызывается EncryptOutgoing.
    /// Владение буфером передаётся — после вызова пакет нельзя использовать.
    member this.SendPacket(packet: WPacket) =
        if not this.IsResetOperation then
            let encrypted = this.EncryptOutgoing(packet)
            handler.DoSend(_id, encrypted)
        else
            packet.Dispose()

    /// Переслать пакет (создаёт WPacket-копию из IRPacket).
    member this.ForwardPacket(packet: IRPacket) =
        if not this.IsResetOperation then
            this.SendPacket(WPacket(packet))

    member this.SendPing() =
        if not this.IsResetOperation then
            handler.SendPing(_id)

    /// Закрыть сокет. Потокобезопасно.
    [<MethodImpl(MethodImplOptions.Synchronized)>]
    member _.Close() =
        if not _disposed then
            Volatile.Write(&_resetOperation, true)

            try
                socket.Close()
            with _ ->
                ()

            _disposed <- true

    /// Закрыт ли канал.
    member _.IsDisposed = Volatile.Read(&_disposed)

    [<MethodImpl(MethodImplOptions.Synchronized)>]
    override this.ToString() =
        if _disposed then $"Channel#{_id} [disposed]" else $"Channel#{_id} {this.RemoteEndPoint}"

/// Интерфейс обработчика I/O-операций.
/// Реализация — IoHandlerImpl.
[<AllowNullLiteral>]
type IoHandler =
    abstract DoSend: channelId: ChannelId * packet: WPacket -> unit
    abstract SendPing: channelId: ChannelId -> unit
    inherit IDisposable
