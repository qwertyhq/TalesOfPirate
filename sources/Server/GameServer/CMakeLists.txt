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

target_link_libraries(GameServer PRIVATE pthread)
