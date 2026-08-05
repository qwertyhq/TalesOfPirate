cmake_minimum_required(VERSION 3.20)
project(CorsairsAssetConverter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Корень репозитория — тесты читают реальные ассеты из Client/model.
get_filename_component(CORSAIRS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(AssetConverterLib STATIC
    src/LgoParser.cpp
    src/LabParser.cpp
    src/LmoParser.cpp
    src/MapParser.cpp
    src/SceneObjParser.cpp
    src/MapWriter.cpp
    src/JsonWriter.cpp
    src/GltfWriter.cpp
    src/GltfSkeletonWriter.cpp
    src/ConversionReport.cpp
)
target_include_directories(AssetConverterLib PUBLIC include)

if(MSVC)
    target_compile_options(AssetConverterLib PRIVATE /W4 /permissive-)
else()
    target_compile_options(AssetConverterLib PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(AssetConverter src/Main.cpp)
target_link_libraries(AssetConverter PRIVATE AssetConverterLib)

enable_testing()

add_executable(AssetConverterTests
    tests/TestMain.cpp
    tests/TestBinaryReader.cpp
    tests/TestLgoTypes.cpp
    tests/TestLgoParser.cpp
    tests/TestJsonWriter.cpp
    tests/TestGltfWriter.cpp
    tests/TestConversionReport.cpp
    tests/TestLabParser.cpp
    tests/TestGltfSkeletonWriter.cpp
    tests/TestLmoParser.cpp
    tests/TestMapParser.cpp
)
target_link_libraries(AssetConverterTests PRIVATE AssetConverterLib)
target_include_directories(AssetConverterTests PRIVATE tests)
target_compile_definitions(AssetConverterTests
    PRIVATE CORSAIRS_REPO_ROOT="${CORSAIRS_REPO_ROOT}")

add_test(NAME AssetConverterTests COMMAND AssetConverterTests)
