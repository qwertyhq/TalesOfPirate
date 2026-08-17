cmake_minimum_required(VERSION 3.20)
# C нужен для sqlite3: он на чистом C, а не C++.
project(CorsairsGameServer LANGUAGES C CXX)

# Сборка GameServer под POSIX (Linux, macOS). Основной сборкой на Windows
# остаётся GameServer.vcxproj — этот файл не заменяет его, а добавляет вторую
# платформу.
#
# Порт держится на Libraries/Util/src/PlatformCompat.h: на Windows он пустой,
# на POSIX даёт словарь Win32-типов и функций, которыми пользуется код.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(SOURCES_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

file(GLOB_RECURSE GAMESERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# Диалог управления сервером — чистый Win32 (окна, сообщения, ресурсы).
# На POSIX сервер консольный, поэтому файл из сборки исключается.
if(NOT WIN32)
    list(FILTER GAMESERVER_SOURCES EXCLUDE REGEX "SystemDialog/SystemDialog\\.cpp$")
else()
    list(FILTER GAMESERVER_SOURCES EXCLUDE REGEX "SystemDialogPosix\\.cpp$")
endif()

# Общие библиотеки собираются вместе с сервером: отдельных .a под POSIX нет,
# а на Windows они приходят как проекты солюшена.
file(GLOB_RECURSE COMMON_SOURCES CONFIGURE_DEPENDS "${SOURCES_ROOT}/Libraries/common/src/*.cpp")
file(GLOB_RECURSE UTIL_SOURCES CONFIGURE_DEPENDS
    "${SOURCES_ROOT}/Libraries/Util/src/*.cpp"
    "${SOURCES_ROOT}/Libraries/Util/src/*.c")
file(GLOB_RECURSE NET_SOURCES CONFIGURE_DEPENDS "${SOURCES_ROOT}/Libraries/CorsairsNet/src/*.cpp")
file(GLOB MPACK_SOURCES CONFIGURE_DEPENDS "${SOURCES_ROOT}/Libraries/CorsairsNet/include/mpack-*.cpp")

if(NOT WIN32)
    # CrushSystem — обработчик аварий на SEH и dbghelp, механизмы Windows. На
    # POSIX его роль играют обработчики сигналов; это отдельная задача.
    list(FILTER UTIL_SOURCES EXCLUDE REGEX "Diagnostic/CrushSystem\\.cpp$")
else()
    list(FILTER UTIL_SOURCES EXCLUDE REGEX "Diagnostic/CrushSystemPosix\\.cpp$")
endif()

# mpack собирается отдельной библиотекой и БЕЗ принудительного stdafx.h.
#
# Каждый его .cpp сначала определяет MPACK_INTERNAL, и только потом подключает
# свои заголовки, где внутренние объявления спрятаны за #if MPACK_INTERNAL.
# Через PCH те же заголовки попадают в трансляцию раньше — защита от повторного
# включения не даёт обработать их снова, и внутренние функции остаются
# необъявленными.
# sqlite3 — амальгамация на C, собирается отдельной целью без PCH.
file(GLOB SQLITE_SOURCES CONFIGURE_DEPENDS "${SOURCES_ROOT}/Libraries/sqlite3/*.c")
add_library(sqlite3 STATIC ${SQLITE_SOURCES})
target_include_directories(sqlite3 PUBLIC "${SOURCES_ROOT}/Libraries/sqlite3")
target_compile_options(sqlite3 PRIVATE -w)

add_library(mpack STATIC ${MPACK_SOURCES})
target_include_directories(mpack PUBLIC "${SOURCES_ROOT}/Libraries/CorsairsNet/include")
target_compile_options(mpack PRIVATE -w)

add_executable(GameServer
    ${GAMESERVER_SOURCES}
    ${COMMON_SOURCES}
    ${UTIL_SOURCES}
    ${NET_SOURCES}
)

target_include_directories(GameServer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core"
    "${SOURCES_ROOT}/Libraries"
    "${SOURCES_ROOT}/Libraries/Util/src"
    "${SOURCES_ROOT}/Libraries/Util/src/Memory"
    "${SOURCES_ROOT}/Libraries/Util/src/Diagnostic"
    "${SOURCES_ROOT}/Libraries/Util/src/Text"
    "${SOURCES_ROOT}/Libraries/Util/src/Logging"
    "${SOURCES_ROOT}/Libraries/common/src"
    "${SOURCES_ROOT}/Libraries/common/src/Core"
    "${SOURCES_ROOT}/Libraries/Util/src/Math"
    "${SOURCES_ROOT}/Libraries/Util/src/Crypto"
    "${SOURCES_ROOT}/Libraries/common/src/Character"
    "${SOURCES_ROOT}/Libraries/Util/src/Config"
    "${SOURCES_ROOT}/Libraries/Util/src/Encoding"
    "${SOURCES_ROOT}/Libraries/Util/src/Database"
    "${SOURCES_ROOT}/Libraries/CorsairsNet/include"
    "${SOURCES_ROOT}/Libraries/LuaJIT/src"
    "${SOURCES_ROOT}/Libraries/sqlite3"
    "${SOURCES_ROOT}/Libraries/LuaBridge/include"
    "${SOURCES_ROOT}/Libraries/LuaBridge"
)

# stdafx.h подключается принудительно, как и в vcxproj: файлы не содержат
# #include "stdafx.h" (см. CLAUDE.md).
# stdafx.h подключается ТОЛЬКО к C++-файлам.
#
# Без ограничения по языку принудительный -include применялся и к C-файлам
# (md5c.c), а stdafx.h — заголовок C++ с <algorithm>, <format>, <thread>.
# Компилятор в режиме C уходил в разбор шаблонов и вставал намертво: три
# процесса крутили один файл по два часа CPU каждый.
target_compile_options(GameServer PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-include;${CMAKE_CURRENT_SOURCE_DIR}/src/Core/stdafx.h>"
    -Wall
    -Wno-unused-parameter
    -Wno-unused-variable
)

# ODBC. На Windows это системный odbc32; на POSIX — unixODBC, который на
# macOS ставится через Homebrew, а на Linux пакетом unixodbc-dev.
find_path(ODBC_INCLUDE_DIR sql.h
    HINTS /opt/homebrew/opt/unixodbc/include /usr/local/opt/unixodbc/include /usr/include)
find_library(ODBC_LIBRARY NAMES odbc odbc32
    HINTS /opt/homebrew/opt/unixodbc/lib /usr/local/opt/unixodbc/lib /usr/lib)

if(ODBC_INCLUDE_DIR AND ODBC_LIBRARY)
    target_include_directories(GameServer PRIVATE "${ODBC_INCLUDE_DIR}")
    target_link_libraries(GameServer PRIVATE "${ODBC_LIBRARY}")
    message(STATUS "ODBC: ${ODBC_LIBRARY}")
else()
    message(FATAL_ERROR "unixODBC не найден: brew install unixodbc (macOS) или apt install unixodbc-dev (Linux)")
endif()

# LuaJIT собирается своим Makefile:
#   MACOSX_DEPLOYMENT_TARGET=15.0 make -C Libraries/LuaJIT/src
# Указываем статическую библиотеку напрямую: find_library предпочитает .dylib,
# и бинарник получал зависимость от /usr/local/lib/libluajit-5.1.2.dylib,
# которой в системе нет.
# Продукты сборки LuaJIT в репозиторий не коммитятся (PIR-12), поэтому на
# чистом клоне собираем библиотеку один раз здесь, на этапе configure.
set(LUAJIT_LIBRARY "${SOURCES_ROOT}/Libraries/LuaJIT/src/libluajit.a")
if(NOT EXISTS "${LUAJIT_LIBRARY}")
    message(STATUS "libluajit.a не найден — собираю LuaJIT (make -C Libraries/LuaJIT/src)")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env MACOSX_DEPLOYMENT_TARGET=15.0
                make -C "${SOURCES_ROOT}/Libraries/LuaJIT/src" -j4
        RESULT_VARIABLE LUAJIT_MAKE_RESULT)
    if(NOT LUAJIT_MAKE_RESULT EQUAL 0 OR NOT EXISTS "${LUAJIT_LIBRARY}")
        message(FATAL_ERROR
            "Не удалось собрать LuaJIT. Соберите вручную:\n"
            "  MACOSX_DEPLOYMENT_TARGET=15.0 make -C Libraries/LuaJIT/src")
    endif()
endif()
target_link_libraries(GameServer PRIVATE "${LUAJIT_LIBRARY}")


target_link_libraries(GameServer PRIVATE mpack sqlite3 pthread)
