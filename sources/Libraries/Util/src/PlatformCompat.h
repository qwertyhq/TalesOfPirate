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
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
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
using LONG32 = std::int32_t;
using ULONG32 = std::uint32_t;
using LONG64 = std::int64_t;

// Расширения MSVC для целых фиксированной ширины. В новом коде вместо них
// положено писать типы из <cstdint> (см. CLAUDE.md).
// __int64 объявлен через #define, а не using: в коде встречается форма
// `unsigned __int64`, а к псевдониму типа модификатор unsigned не применить.
#define __int64 long long
#define __int32 int
#define __int16 short
#define __int8 char
using ULONG64 = std::uint64_t;
using UINT32 = std::uint32_t;
using INT32 = std::int32_t;
using UINT64 = std::uint64_t;
using INT64 = std::int64_t;
using UINT16 = std::uint16_t;
using INT16 = std::int16_t;
using UINT8 = std::uint8_t;
using INT8 = std::int8_t;
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

// Дескрипторы Win32. В серверном коде они встречаются в полях и сигнатурах,
// но по существу используются как непрозрачные указатели.
using HANDLE = void*;
using HINSTANCE = void*;
using HWND = void*;
using HMODULE = void*;

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(-1))
#endif

// Константы длин путей MSVC. На POSIX им соответствуют PATH_MAX и NAME_MAX,
// но код опирается на конкретные числа при объявлении массивов, поэтому
// значения взяты из Windows.
#ifndef _MAX_PATH
#define _MAX_PATH 260
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef _MAX_DRIVE
#define _MAX_DRIVE 3
#endif
#ifndef _MAX_DIR
#define _MAX_DIR 256
#endif
#ifndef _MAX_FNAME
#define _MAX_FNAME 256
#endif
#ifndef _MAX_EXT
#define _MAX_EXT 256
#endif

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

// Направления для shutdown(). В POSIX это SHUT_RD/SHUT_WR/SHUT_RDWR.
#ifndef SD_RECEIVE
#define SD_RECEIVE SHUT_RD
#define SD_SEND    SHUT_WR
#define SD_BOTH    SHUT_RDWR
#endif

// ioctlsocket с FIONBIO переключает блокирующий режим. В POSIX это делается
// через fcntl: ioctl(FIONBIO) существует не везде и считается устаревшим.
#ifndef FIONBIO
#define FIONBIO 0x5421
#endif

inline int ioctlsocket(SOCKET sock, long command, unsigned long* argument) {
    if (command != FIONBIO || argument == nullptr) {
        return SOCKET_ERROR;
    }

    const int flags = ::fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return SOCKET_ERROR;
    }

    const int updated = (*argument != 0) ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(sock, F_SETFL, updated) < 0 ? SOCKET_ERROR : 0;
}

inline int closesocket(SOCKET sock) {
    return ::close(sock);
}

inline void WSASetLastError(int code) {
    errno = code;
}

inline void SetLastError(DWORD code) {
    errno = static_cast<int>(code);
}

inline DWORD GetLastError() {
    return static_cast<DWORD>(errno);
}

inline int WSAGetLastError() {
    return errno;
}

// В POSIX инициализация сетевой подсистемы не нужна; структура и макрос
// оставлены, чтобы вызывающий код не менялся.
struct WSADATA {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
};

#ifndef MAKEWORD
#define MAKEWORD(low, high) \
    (static_cast<WORD>((static_cast<BYTE>(low)) | \
                       (static_cast<WORD>(static_cast<BYTE>(high)) << 8)))
#endif

inline int WSAStartup(WORD /*version*/, WSADATA* data) {
    if (data != nullptr) {
        *data = WSADATA{};
    }
    return 0;
}

inline int WSACleanup() {
    return 0;
}

// --- Коды ошибок сокетов ---------------------------------------------------
// WinSock нумерует ошибки своими константами; в POSIX используются значения
// errno. Отображение один в один, поэтому код, разбирающий WSAGetLastError(),
// работает без изменений.
//
// ВНИМАНИЕ: совпадение имён не означает совпадения поведения. EAGAIN и
// EWOULDBLOCK на большинстве POSIX-систем равны, но EINTR приходит там, где
// WinSock его не возвращает: любой системный вызов может быть прерван
// сигналом. Циклы приёма и отправки обязаны это учитывать.

#include <cerrno>

#ifndef WSAEINTR
#define WSAEINTR         EINTR
#define WSAEACCES        EACCES
#define WSAEFAULT        EFAULT
#define WSAEINVAL        EINVAL
#define WSAEMFILE        EMFILE
#define WSAEWOULDBLOCK   EWOULDBLOCK
#define WSAEINPROGRESS   EINPROGRESS
#define WSAEALREADY      EALREADY
#define WSAENOTSOCK      ENOTSOCK
#define WSAEMSGSIZE      EMSGSIZE
#define WSAENOBUFS       ENOBUFS
#define WSAENOTCONN      ENOTCONN
#define WSAESHUTDOWN     ESHUTDOWN
#define WSAETIMEDOUT     ETIMEDOUT
#define WSAECONNREFUSED  ECONNREFUSED
#define WSAECONNRESET    ECONNRESET
#define WSAECONNABORTED  ECONNABORTED
#define WSAENETDOWN      ENETDOWN
#define WSAENETRESET     ENETRESET
#define WSAEHOSTDOWN     EHOSTDOWN
#define WSAEHOSTUNREACH  EHOSTUNREACH
// В POSIX нет кода «удалённая сторона начала закрытие» — это видно по recv(),
// вернувшему 0. Отобразить его на ESHUTDOWN нельзя: тогда WSAEDISCON и
// WSAESHUTDOWN совпадут и дадут два одинаковых case в switch. Берём значение
// заведомо вне диапазона errno.
#define WSAEDISCON       100001
// WSANOTINITIALISED тоже без аналога: в POSIX сетевую подсистему не
// инициализируют.
#define WSANOTINITIALISED 100002
#endif

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

// POINT и RECT — базовые геометрические структуры Win32.
struct POINT {
    LONG x;
    LONG y;
};

struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};

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

inline int strncat_s(char* dest, std::size_t destSize, const char* src, std::size_t count) {
    if (dest == nullptr || src == nullptr || destSize == 0) {
        return 22;
    }
    const std::size_t used = std::strlen(dest);
    if (used >= destSize - 1) {
        return 0;
    }
    const std::size_t room = destSize - used - 1;
    const std::size_t limit = (count == _TRUNCATE) ? room : count;
    const std::size_t copy = limit < room ? limit : room;
    std::memcpy(dest + used, src, copy);
    dest[used + copy] = '\0';
    return 0;
}

template <std::size_t N>
inline int strncat_s(char (&dest)[N], const char* src, std::size_t count) {
    return strncat_s(dest, N, src, count);
}

inline int strcpy_s(char* dest, std::size_t destSize, const char* src) {
    return strncpy_s(dest, destSize, src, _TRUNCATE);
}

template <std::size_t N>
inline int strcpy_s(char (&dest)[N], const char* src) {
    return strncpy_s(dest, N, src, _TRUNCATE);
}

// --- Прочее ----------------------------------------------------------------

// _access — MSVC-имя POSIX-функции access().
inline int _access(const char* path, int mode) {
    return ::access(path, mode);
}

inline int _unlink(const char* path) {
    return ::unlink(path);
}

inline int _stricmp(const char* a, const char* b) {
    return ::strcasecmp(a, b);
}

inline int _strnicmp(const char* a, const char* b, std::size_t n) {
    return ::strncasecmp(a, b, n);
}

// _countof — макрос MSVC для длины массива. std::size из <iterator> делает то
// же самое и является стандартным; макрос оставлен для существующих вызовов.
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

// itoa — нестандартная функция. Реализация через std::to_chars, чтобы не
// тянуть sprintf.
inline char* itoa(int value, char* buffer, int base) {
    if (buffer == nullptr) {
        return nullptr;
    }
    const auto result = std::to_chars(buffer, buffer + 32, value, base);
    *result.ptr = '\0';
    return buffer;
}

// --- Соглашения о вызовах --------------------------------------------------
// На Windows это атрибуты вызова; на POSIX они не значат ничего.

#ifndef WINAPI
#define WINAPI
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef __stdcall
#define __stdcall
#endif

// --- Консоль ---------------------------------------------------------------
// Цвета через ANSI-escape вместо SetConsoleTextAttribute. Константы совпадают
// по значению с Win32, поэтому вызывающий код не меняется.

#ifndef FOREGROUND_BLUE
#define FOREGROUND_BLUE      0x0001
#define FOREGROUND_GREEN     0x0002
#define FOREGROUND_RED       0x0004
#define FOREGROUND_INTENSITY 0x0008
#define BACKGROUND_BLUE      0x0010
#define BACKGROUND_GREEN     0x0020
#define BACKGROUND_RED       0x0040
#define BACKGROUND_INTENSITY 0x0080
#endif

#ifndef STD_OUTPUT_HANDLE
#define STD_OUTPUT_HANDLE (-11)
#define STD_ERROR_HANDLE  (-12)
#define STD_INPUT_HANDLE  (-10)
#endif

struct COORD {
    SHORT X;
    SHORT Y;
};

struct SMALL_RECT {
    SHORT Left;
    SHORT Top;
    SHORT Right;
    SHORT Bottom;
};

struct CONSOLE_SCREEN_BUFFER_INFO {
    COORD dwSize;
    COORD dwCursorPosition;
    WORD wAttributes;
    SMALL_RECT srWindow;
    COORD dwMaximumWindowSize;
};

// Прочитать текущие атрибуты консоли в POSIX нельзя: ANSI-терминал не
// сообщает свой цвет. Возвращаем неудачу — вызывающий код трактует это как
// «цвет неизвестен».
inline BOOL GetConsoleScreenBufferInfo(HANDLE /*handle*/,
                                       CONSOLE_SCREEN_BUFFER_INFO* /*info*/) {
    return FALSE;
}

inline HANDLE GetStdHandle(int /*which*/) {
    return nullptr;
}

// Переводит битовую маску цвета Win32 в ANSI-код и печатает escape-
// последовательность. Работает в любом современном терминале.
inline BOOL SetConsoleTextAttribute(HANDLE /*handle*/, WORD attributes) {
    const bool bright = (attributes & FOREGROUND_INTENSITY) != 0;
    int code = 30;
    if (attributes & FOREGROUND_RED) {
        code += 1;
    }
    if (attributes & FOREGROUND_GREEN) {
        code += 2;
    }
    if (attributes & FOREGROUND_BLUE) {
        code += 4;
    }
    std::printf("\033[%s;%dm", bright ? "1" : "0", code);
    return TRUE;
}

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

// На Windows переключает кодовую страницу консоли; на POSIX терминал и так
// работает в UTF-8.
inline BOOL SetConsoleOutputCP(UINT /*codepage*/) {
    return TRUE;
}

inline DWORD GetCurrentProcessId() {
    return static_cast<DWORD>(::getpid());
}

// lstrcmpi — Windows-вариант сравнения без учёта регистра.
inline int lstrcmpi(const char* a, const char* b) {
    return ::strcasecmp(a, b);
}

// strlwr переводит строку в нижний регистр на месте. Нестандартная функция,
// в новом коде положено использовать std::ranges::transform.
// _snprintf — старое имя MSVC для snprintf. Поведение при переполнении у них
// различается (MSVC не гарантирует завершающий ноль), но код и так проверяет
// результат, поэтому подмена безопасна.
#ifndef _snprintf
#define _snprintf snprintf
#endif
#ifndef _vsnprintf
#define _vsnprintf vsnprintf
#endif

// _snprintf_s — вариант snprintf от Microsoft с параметром размера буфера.
// По смыслу с _TRUNCATE совпадает с обычным snprintf.
template <typename... Args>
inline int _snprintf_s(char* buffer, std::size_t bufferSize, std::size_t /*count*/,
                       const char* format, Args... args) {
    return std::snprintf(buffer, bufferSize, format, args...);
}

template <std::size_t N, typename... Args>
inline int _snprintf_s(char (&buffer)[N], std::size_t count, const char* format,
                       Args... args) {
    return _snprintf_s(buffer, N, count, format, args...);
}

inline char* strlwr(char* text) {
    if (text == nullptr) {
        return nullptr;
    }
    for (char* p = text; *p != '\0'; ++p) {
        *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    return text;
}

inline BOOL SetConsoleTitleA(const char* title);

inline BOOL SetConsoleTitle(const char* title) {
    if (title != nullptr) {
        // OSC 0 — установка заголовка окна терминала.
        std::printf("\033]0;%s\007", title);
    }
    return TRUE;
}

// --- Заглушки процессов и модулей ------------------------------------------

// Окна консоли в POSIX нет: терминалом управляет эмулятор, а не процесс.
inline HWND GetConsoleWindow() {
    return nullptr;
}

#ifndef ATTACH_PARENT_PROCESS
#define ATTACH_PARENT_PROCESS (static_cast<DWORD>(-1))
#endif

inline BOOL AttachConsole(DWORD /*processId*/) {
    return FALSE;
}

inline BOOL AllocConsole() {
    return FALSE;
}

inline BOOL FreeConsole() {
    return FALSE;
}

inline BOOL SetConsoleTitleA(const char* title) {
    return SetConsoleTitle(title);
}

// freopen_s — вариант freopen от Microsoft с выходным параметром.
inline int freopen_s(std::FILE** stream, const char* path, const char* mode,
                     std::FILE* old) {
    if (stream == nullptr) {
        return 22;
    }
    *stream = std::freopen(path, mode, old);
    return (*stream != nullptr) ? 0 : errno;
}

inline BOOL CloseHandle(HANDLE /*handle*/) {
    return TRUE;
}

inline HMODULE GetModuleHandle(const char* /*name*/) {
    return nullptr;
}

// На Windows ограничивает число открытых потоков stdio. В POSIX лимит задаётся
// через setrlimit и по умолчанию достаточен.
inline int _setmaxstdio(int count) {
    return count;
}

// --- Диалоги --------------------------------------------------------------
// MessageBox на сервере используется для фатальных ошибок запуска. На POSIX
// выводим в stderr: у консольного демона окон нет.

#ifndef MB_OK
#define MB_OK          0x0000
#define MB_ICONERROR   0x0010
#define MB_ICONWARNING 0x0030
#define IDOK 1
#endif

// OutputDebugString пишет в отладчик Windows. На POSIX эквивалент — stderr:
// его видно и в терминале, и в логах systemd/docker.
inline void OutputDebugStringA(const char* text) {
    if (text != nullptr) {
        std::fputs(text, stderr);
    }
}

inline void OutputDebugString(const char* text) {
    OutputDebugStringA(text);
}

inline int MessageBox(HWND /*owner*/, const char* text, const char* caption,
                      UINT /*type*/) {
    std::fprintf(stderr, "[%s] %s\n", caption ? caption : "GameServer",
                 text ? text : "");
    return IDOK;
}

// --- Структурная обработка исключений --------------------------------------
// SEH — механизм Windows. На POSIX аварии ловятся сигналами, поэтому здесь
// только тип-заглушка, чтобы сигнатуры обработчиков компилировались.

struct EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    void* ExceptionAddress;
};

struct EXCEPTION_POINTERS {
    EXCEPTION_RECORD* ExceptionRecord;
    void* ContextRecord;
};

// --- Цикл оконных сообщений ------------------------------------------------
// У консольного сервера на POSIX его нет: PeekMessage всегда сообщает, что
// сообщений не поступало, и главный цикл просто крутит игровую логику.

struct MSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
};

#ifndef PM_REMOVE
#define PM_REMOVE 0x0001
#define PM_NOREMOVE 0x0000
#endif
#ifndef WM_QUIT
#define WM_QUIT 0x0012
#endif

inline BOOL PeekMessage(MSG* /*msg*/, HWND /*hwnd*/, UINT /*min*/, UINT /*max*/,
                        UINT /*remove*/) {
    return FALSE;
}

inline BOOL TranslateMessage(const MSG* /*msg*/) {
    return FALSE;
}

inline LRESULT DispatchMessage(const MSG* /*msg*/) {
    return 0;
}

#endif // _WIN32
