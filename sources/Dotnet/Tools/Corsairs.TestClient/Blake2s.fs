/// BLAKE2s-256 — хеш паролей учётных записей.
///
/// В .NET такого алгоритма нет, а пакета Blake2Fast в доступной ленте NuGet не
/// оказалось. Реализация здесь по RFC 7693; в проекте уже есть её двойник на
/// C++ (`sources/Libraries/common/src/Crypto/Blake2s.cpp`), которым пользуется
/// клиент, и обе обязаны давать одно и то же.
///
/// Своя криптография — риск, поэтому `SelfTest` сверяет результат с известным
/// хешем: BLAKE2s("admin") лежит в `mssql/[4]Create Account.sql` как пароль
/// учётной записи admin. Расхождение означает ошибку в реализации, а не в
/// данных, и обнаружится сразу, а не в виде необъяснимого отказа во входе.
module Corsairs.TestClient.Blake2s

open System

let private IV = [|
    0x6A09E667u; 0xBB67AE85u; 0x3C6EF372u; 0xA54FF53Au
    0x510E527Fu; 0x9B05688Cu; 0x1F83D9ABu; 0x5BE0CD19u |]

let private SIGMA = [|
    [|  0;  1;  2;  3;  4;  5;  6;  7;  8;  9; 10; 11; 12; 13; 14; 15 |]
    [| 14; 10;  4;  8;  9; 15; 13;  6;  1; 12;  0;  2; 11;  7;  5;  3 |]
    [| 11;  8; 12;  0;  5;  2; 15; 13; 10; 14;  3;  6;  7;  1;  9;  4 |]
    [|  7;  9;  3;  1; 13; 12; 11; 14;  2;  6;  5; 10;  4;  0; 15;  8 |]
    [|  9;  0;  5;  7;  2;  4; 10; 15; 14;  1; 11; 12;  6;  8;  3; 13 |]
    [|  2; 12;  6; 10;  0; 11;  8;  3;  4; 13;  7;  5; 15; 14;  1;  9 |]
    [| 12;  5;  1; 15; 14; 13;  4; 10;  0;  7;  6;  3;  9;  2;  8; 11 |]
    [| 13; 11;  7; 14; 12;  1;  3;  9;  5;  0; 15;  4;  8;  6;  2; 10 |]
    [|  6; 15; 14;  9; 11;  3;  0;  8; 12;  2; 13;  7;  1;  4; 10;  5 |]
    [| 10;  2;  8;  4;  7;  6;  1;  5; 15; 11;  9; 14;  3; 12; 13;  0 |] |]

let private BLOCK_SIZE = 64
let private DIGEST_SIZE = 32

let inline private rotr (x: uint32) (n: int) = (x >>> n) ||| (x <<< (32 - n))

/// Перемешивание G из RFC 7693, повороты на 16, 12, 8 и 7 бит.
let inline private mix (v: uint32[]) a b c d (x: uint32) (y: uint32) =
    v[a] <- v[a] + v[b] + x
    v[d] <- rotr (v[d] ^^^ v[a]) 16
    v[c] <- v[c] + v[d]
    v[b] <- rotr (v[b] ^^^ v[c]) 12
    v[a] <- v[a] + v[b] + y
    v[d] <- rotr (v[d] ^^^ v[a]) 8
    v[c] <- v[c] + v[d]
    v[b] <- rotr (v[b] ^^^ v[c]) 7

/// Сжатие блока. `counter` — общее число обработанных байт, `isLast` —
/// признак последнего блока.
let private compress (h: uint32[]) (block: byte[]) (offset: int) (counter: uint64) (isLast: bool) =
    let m = Array.init 16 (fun i ->
        BitConverter.ToUInt32(block, offset + i * 4))

    let v = Array.zeroCreate<uint32> 16
    Array.blit h 0 v 0 8
    Array.blit IV 0 v 8 8

    v[12] <- v[12] ^^^ uint32 (counter &&& 0xFFFFFFFFUL)
    v[13] <- v[13] ^^^ uint32 (counter >>> 32)
    if isLast then
        v[14] <- v[14] ^^^ 0xFFFFFFFFu

    for round in 0 .. 9 do
        let s = SIGMA[round]
        mix v 0 4  8 12 m[s[0]]  m[s[1]]
        mix v 1 5  9 13 m[s[2]]  m[s[3]]
        mix v 2 6 10 14 m[s[4]]  m[s[5]]
        mix v 3 7 11 15 m[s[6]]  m[s[7]]
        mix v 0 5 10 15 m[s[8]]  m[s[9]]
        mix v 1 6 11 12 m[s[10]] m[s[11]]
        mix v 2 7  8 13 m[s[12]] m[s[13]]
        mix v 3 4  9 14 m[s[14]] m[s[15]]

    for i in 0 .. 7 do
        h[i] <- h[i] ^^^ v[i] ^^^ v[i + 8]

/// Хеш BLAKE2s-256 без ключа.
let ComputeHash (input: byte[]) : byte[] =
    let h = Array.copy IV
    // Блок параметров: длина дайджеста, длина ключа (0), fanout и depth по 1.
    h[0] <- h[0] ^^^ 0x01010000u ^^^ uint32 DIGEST_SIZE

    let mutable counter = 0UL
    let mutable offset = 0

    // Все блоки, кроме последнего. Последний обрабатывается отдельно: у него
    // взводится флаг завершения, а счётчик учитывает только реальные байты,
    // без дополнения нулями.
    while input.Length - offset > BLOCK_SIZE do
        counter <- counter + uint64 BLOCK_SIZE
        compress h input offset counter false
        offset <- offset + BLOCK_SIZE

    let remaining = input.Length - offset
    let lastBlock = Array.zeroCreate<byte> BLOCK_SIZE
    if remaining > 0 then
        Array.blit input offset lastBlock 0 remaining
    counter <- counter + uint64 remaining
    compress h lastBlock 0 counter true

    let digest = Array.zeroCreate<byte> DIGEST_SIZE
    for i in 0 .. 7 do
        BitConverter.GetBytes(h[i]).CopyTo(digest, i * 4)
    digest

/// Хеш строки в верхнем регистре hex — в этом виде он хранится в базе.
let HashPassword (password: string) : string =
    Convert.ToHexString(ComputeHash(Text.Encoding.UTF8.GetBytes password))

/// Сверяет реализацию с известными значениями.
///
/// Возвращает сообщение об ошибке либо None. Пустая строка проверяется отдельно
/// от непустой: у неё нет ни одного байта данных, и ошибка в обработке
/// последнего блока проявляется только на ней.
let SelfTest () : string option =
    let cases = [
        // Из mssql/[4]Create Account.sql — пароль учётной записи admin.
        "admin", "327E7E3821F5F6D33C090137F979BF48EE62E9051C1610E1D6468ECB3C67A124"
        // Контрольное значение BLAKE2s-256 для пустого входа.
        "", "69217A3079908094E11121D042354A7C1F55B6482CA1A51E1B250DFD1ED0EEF9"
    ]
    cases
    |> List.tryPick (fun (input, expected) ->
        let actual = HashPassword input
        if actual = expected then None
        else Some $"BLAKE2s(\"{input}\") дал {actual}, ожидалось {expected}")
