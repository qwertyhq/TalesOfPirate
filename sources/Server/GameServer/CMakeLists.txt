cmake_minimum_required(VERSION 3.20)
project(CorsairsGameServer LANGUAGES CXX)

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
endif()

add_executable(GameServer ${GAMESERVER_SOURCES})

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
    "${SOURCES_ROOT}/Libraries/Util/src/Config"
    "${SOURCES_ROOT}/Libraries/Util/src/Encoding"
    "${SOURCES_ROOT}/Libraries/Util/src/Database"
    "${SOURCES_ROOT}/Libraries/CorsairsNet/include"
    "${SOURCES_ROOT}/Libraries/LuaJIT/src"
    "${SOURCES_ROOT}/Libraries/LuaBridge/include"
    "${SOURCES_ROOT}/Libraries/LuaBridge"
)

# stdafx.h подключается принудительно, как и в vcxproj: файлы не содержат
# #include "stdafx.h" (см. CLAUDE.md).
target_compile_options(GameServer PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/stdafx.h"
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

target_link_libraries(GameServer PRIVATE pthread)
