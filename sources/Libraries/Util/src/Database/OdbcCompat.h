#pragma once

// OdbcCompat.h — подключение заголовков ODBC на POSIX без конфликта типов.
//
// Проблема. unixODBC в sqltypes.h объявляет собственный набор Win32-типов, и
// его `ULONG` — это `unsigned long`, то есть 64 бита на 64-битном POSIX. У нас
// же `ULONG` из PlatformCompat.h ровно 32-битный, как на Windows. Два разных
// определения одного имени — ошибка компиляции, а «починить» её, сделав наш
// ULONG 64-битным, нельзя: тип встречается в структурах с фиксированной
// раскладкой, и ширина там принципиальна.
//
// Решение. unixODBC пропускает свои определения, если задан
// ALREADY_HAVE_WINDOWS_TYPE. Задаём его и доопределяем то, чего в нашем
// словаре нет, но что ODBC ожидает увидеть.

#include "PlatformCompat.h"

#ifdef _WIN32

#include <sql.h>
#include <sqlext.h>

#else // POSIX

#include <cstdint>

// Говорим unixODBC, что Win32-типы уже есть.
#define ALREADY_HAVE_WINDOWS_TYPE 1

// Типы, которые unixODBC определил бы сам. Ширина взята из Windows, где
// long — 32 бита, чтобы совпадала с нашим словарём.
using SWORD = std::int16_t;
using UWORD = std::uint16_t;
using SSHORT = std::int16_t;
using SLONG = std::int32_t;
using SDOUBLE = double;
using LDOUBLE = double;
using SFLOAT = float;
using PTR = void*;
using WCHAR = wchar_t;
using RETCODE = std::int16_t;
using SQLHWND = HWND;
using SQLSCHAR = signed char;

#ifndef FAR
#define FAR
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef SQL_API
#define SQL_API
#endif

#ifndef GUID_DEFINED
#define GUID_DEFINED
struct GUID {
    std::uint32_t Data1;
    std::uint16_t Data2;
    std::uint16_t Data3;
    std::uint8_t Data4[8];
};
#endif

#include <sql.h>
#include <sqlext.h>

#endif // _WIN32
