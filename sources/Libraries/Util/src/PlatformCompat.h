#pragma once

// PlatformCompat.h — слой совместимости для сборки под POSIX (Linux, macOS).
//
// На Windows файл не делает ничего: там подключаются настоящие <windows.h> и
// <winsock2.h>. На остальных платформах он даёт минимальный набор типов и
// функций, которыми пользуется серверный код, чтобы не переписывать 64 тысячи
// строк ради смены платформы.
//
// Принцип: здесь только то, что реально встречается в коде. Это не эмуляция
// Win32 — это словарь для конкретного проекта. Расширять по мере надобности,
// а лучше вытеснять: каждый вызов, заменённый на стандартный C++, уменьшает
// этот файл.

#ifdef _WIN32

#include <winsock2.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tchar.h>

#else // POSIX

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <string>

// --- Целочисленные типы Win32 ---------------------------------------------
// Ширина зафиксирована явно: на Windows DWORD это 32 бита независимо от
// разрядности, и код на это опирается.

using BYTE = std::uint8_t;
using UCHAR = std::uint8_t;
using CHAR = char;
using WORD = std::uint16_t;
using USHORT = std::uint16_t;
using SHORT = std::int16_t;
using DWORD = std::uint32_t;
using UINT = std::uint32_t;
using INT = std::int32_t;
using LONG = std::int32_t;
using ULONG = std::uint32_t;
using LONGLONG = std::int64_t;
using ULONGLONG = std::uint64_t;
using QWORD = std::uint64_t;
using BOOL = int;
using LPVOID = void*;
using LPCSTR = const char*;
using LPSTR = char*;
using LPCTSTR = const char*;
using LPTSTR = char*;
using LPBYTE = std::uint8_t*;
using LPWORD = std::uint16_t*;
using LPDWORD = std::uint32_t*;
using _TCHAR = char;

// Параметры оконных сообщений. В серверном коде встречаются в сигнатурах
// обработчиков, унаследованных от Windows-версии.
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using HRESULT = std::int32_t;

#ifndef S_OK
#define S_OK 0
#endif
#ifndef E_FAIL
#define E_FAIL (static_cast<HRESULT>(0x80004005))
#endif
#ifndef FAILED
#define FAILED(hr) ((hr) < 0)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) ((hr) >= 0)
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

// --- Строки TCHAR ----------------------------------------------------------
// Проект перешёл на UTF-8 (миграция закрыта 2026-04-19), поэтому TCHAR — это
// всегда char, а _T() — пустая обёртка. Макросы оставлены только чтобы не
// править места, где они ещё встречаются.

using TCHAR = char;
#ifndef _T
#define _T(x) x
#endif
#ifndef _TEXT
#define _TEXT(x) x
#endif

// --- Сокеты ----------------------------------------------------------------
// В WinSock SOCKET беззнаковый и признак ошибки — INVALID_SOCKET; в POSIX
// дескриптор знаковый и ошибка это -1. Разница существенная: сравнение
// `sock == INVALID_SOCKET` на POSIX без этих определений тихо сломается.

using SOCKET = int;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif

inline int closesocket(SOCKET sock) {
    return ::close(sock);
}

inline int WSAGetLastError() {
    return errno;
}

// В POSIX инициализация сетевой подсистемы не нужна.
inline int WSAStartup(std::uint16_t /*version*/, void* /*data*/) {
    return 0;
}

inline int WSACleanup() {
    return 0;
}

// --- Время -----------------------------------------------------------------
// GetTickCount возвращает миллисекунды с момента старта. Реализуем через
// steady_clock: он монотонный, в отличие от системных часов.
//
// ВАЖНО: это временная опора для порта. По правилам проекта (CLAUDE.md)
// тайминги должны считаться на std::chrono напрямую, а не через DWORD-
// таймстампы. Каждый вызов GetTickCount, заменённый на chrono, приближает
// удаление этой функции.

inline DWORD GetTickCount() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<DWORD>(
        duration_cast<milliseconds>(steady_clock::now() - start).count());
}

inline ULONGLONG GetTickCount64() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<ULONGLONG>(
        duration_cast<milliseconds>(steady_clock::now() - start).count());
}

inline void Sleep(DWORD milliseconds) {
    ::usleep(static_cast<useconds_t>(milliseconds) * 1000);
}

// --- Календарное время -----------------------------------------------------
// SYSTEMTIME используется логгером для имён файлов и меток времени. Раскладка
// полей повторяет Win32, чтобы код, читающий wYear/wMonth/..., не менялся.

struct SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
};

inline void GetLocalTime(SYSTEMTIME* out) {
    if (out == nullptr) {
        return;
    }

    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto seconds = time_point_cast<std::chrono::seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - seconds).count();

    const std::time_t raw = system_clock::to_time_t(now);
    std::tm local{};
    ::localtime_r(&raw, &local);

    out->wYear = static_cast<WORD>(local.tm_year + 1900);
    out->wMonth = static_cast<WORD>(local.tm_mon + 1);
    out->wDayOfWeek = static_cast<WORD>(local.tm_wday);
    out->wDay = static_cast<WORD>(local.tm_mday);
    out->wHour = static_cast<WORD>(local.tm_hour);
    out->wMinute = static_cast<WORD>(local.tm_min);
    out->wSecond = static_cast<WORD>(local.tm_sec);
    out->wMilliseconds = static_cast<WORD>(millis);
}

inline void GetSystemTime(SYSTEMTIME* out) {
    if (out == nullptr) {
        return;
    }

    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto seconds = time_point_cast<std::chrono::seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - seconds).count();

    const std::time_t raw = system_clock::to_time_t(now);
    std::tm utc{};
    ::gmtime_r(&raw, &utc);

    out->wYear = static_cast<WORD>(utc.tm_year + 1900);
    out->wMonth = static_cast<WORD>(utc.tm_mon + 1);
    out->wDayOfWeek = static_cast<WORD>(utc.tm_wday);
    out->wDay = static_cast<WORD>(utc.tm_mday);
    out->wHour = static_cast<WORD>(utc.tm_hour);
    out->wMinute = static_cast<WORD>(utc.tm_min);
    out->wSecond = static_cast<WORD>(utc.tm_sec);
    out->wMilliseconds = static_cast<WORD>(millis);
}

// --- Безопасные строки MSVC ------------------------------------------------
// strncpy_s/strcpy_s — расширения Microsoft. Здесь дан минимальный эквивалент
// с той же семантикой усечения по _TRUNCATE: копировать сколько влезет и
// всегда завершать нулём.
//
// По правилам проекта (CLAUDE.md) такие вызовы подлежат замене на std::string.
// Эти обёртки — опора на время порта, а не приглашение писать так дальше.

#ifndef _TRUNCATE
#define _TRUNCATE (static_cast<std::size_t>(-1))
#endif

inline int strncpy_s(char* dest, std::size_t destSize, const char* src, std::size_t count) {
    if (dest == nullptr || destSize == 0) {
        return 22; // EINVAL
    }
    if (src == nullptr) {
        dest[0] = '\0';
        return 22;
    }

    const std::size_t limit = (count == _TRUNCATE) ? destSize - 1 : count;
    const std::size_t available = destSize - 1;
    const std::size_t copy = limit < available ? limit : available;

    std::memcpy(dest, src, copy);
    dest[copy] = '\0';
    return 0;
}

// Перегрузка для массивов известного размера: `strncpy_s(dest, src, _TRUNCATE)`.
template <std::size_t N>
inline int strncpy_s(char (&dest)[N], const char* src, std::size_t count) {
    return strncpy_s(dest, N, src, count);
}

inline int strcpy_s(char* dest, std::size_t destSize, const char* src) {
    return strncpy_s(dest, destSize, src, _TRUNCATE);
}

template <std::size_t N>
inline int strcpy_s(char (&dest)[N], const char* src) {
    return strncpy_s(dest, N, src, _TRUNCATE);
}

// --- Прочее ----------------------------------------------------------------

inline int _stricmp(const char* a, const char* b) {
    return ::strcasecmp(a, b);
}

inline int _strnicmp(const char* a, const char* b, std::size_t n) {
    return ::strncasecmp(a, b, n);
}

#endif // _WIN32
