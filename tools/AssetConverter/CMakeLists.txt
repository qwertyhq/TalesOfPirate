cmake_minimum_required(VERSION 3.20)
project(CorsairsAssetConverter LANGUAGES C CXX)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Корень репозитория — тесты читают реальные ассеты из Client/model.
# TO_CMAKE_PATH гарантирует прямые слэши: путь уходит в строковый литерал через
# target_compile_definitions, и обратные слэши Windows стали бы escape-
# последовательностями (C:\Users -> \U — недопустимый escape).
get_filename_component(CORSAIRS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(TO_CMAKE_PATH "${CORSAIRS_REPO_ROOT}" CORSAIRS_REPO_ROOT)

# Общие флаги для всех целей проекта.
#
# /utf-8 обязателен: исходники в UTF-8 и содержат русские строковые литералы, а
# MSVC без этого флага трактует файл без BOM как текст в системной ANSI-кодировке
# (CP1251 на русской Windows) и молча портит литералы. Проявляется не при
# сборке, а в рантайме — мусором в выводе отчёта.
function(corsairs_set_target_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(AssetConverterLib STATIC
    src/LgoParser.cpp
    src/LabParser.cpp
    src/LmoParser.cpp
    src/MapParser.cpp
    src/MapSectionReader.cpp
    src/StreamingMapWriter.cpp
    src/SceneObjParser.cpp
    src/SceneParity.cpp
    src/MapWriter.cpp
    src/TextureResolver.cpp
    src/JsonWriter.cpp
    src/GltfWriter.cpp
    src/GltfSkeletonWriter.cpp
    src/ConversionReport.cpp
    src/DdsDecoder.cpp
    src/PngWriter.cpp
    src/StreamingPngWriter.cpp
    src/Sha256.cpp
    src/TerrainMeshWriter.cpp
    src/TerrainLayers.cpp
    src/TerrainCatalog.cpp
    src/TerrainTextureCache.cpp
    src/TerrainTextureSampling.cpp
    src/TerrainPageBaker.cpp
    src/TerrainPageMeshWriter.cpp
    src/TerrainReferenceCommand.cpp
    src/ProcessMetrics.cpp
    src/StbImageDecoder.cpp
    ${CORSAIRS_REPO_ROOT}/sources/Libraries/sqlite3/sqlite3.c
)
target_include_directories(AssetConverterLib PUBLIC include)
target_include_directories(AssetConverterLib PRIVATE
    ${CORSAIRS_REPO_ROOT}/sources/Libraries/sqlite3
    ${CORSAIRS_REPO_ROOT}/sources/Libraries/stb_image
)
target_link_libraries(AssetConverterLib PRIVATE
    Threads::Threads
    ZLIB::ZLIB
    ${CMAKE_DL_LIBS}
)
if(WIN32)
    target_link_libraries(AssetConverterLib PRIVATE Psapi)
endif()
corsairs_set_target_options(AssetConverterLib)

add_executable(AssetConverter src/Main.cpp)
target_link_libraries(AssetConverter PRIVATE AssetConverterLib)
corsairs_set_target_options(AssetConverter)

enable_testing()

add_executable(AssetConverterTests
    tests/TestMain.cpp
    tests/TestBinaryReader.cpp
    tests/TestLgoTypes.cpp
    tests/TestLgoParser.cpp
    tests/TestJsonWriter.cpp
    tests/TestGltfWriter.cpp
    tests/TestConversionReport.cpp
    tests/TestImageCodec.cpp
    tests/TestStreamingPngWriter.cpp
    tests/TestLabParser.cpp
    tests/TestGltfSkeletonWriter.cpp
    tests/TestLmoParser.cpp
    tests/TestMapParser.cpp
    tests/TestSceneParity.cpp
    tests/TestMapSectionReader.cpp
    tests/TestStreamingMapWriter.cpp
    tests/TestTextureResolver.cpp
    tests/TestTerrainLayers.cpp
    tests/TestTerrainCatalog.cpp
    tests/TestTerrainTextureSampling.cpp
    tests/TestTerrainPageBaker.cpp
    tests/TestTerrainPageMeshWriter.cpp
    tests/TestTerrainReferenceCommand.cpp
)
target_link_libraries(AssetConverterTests PRIVATE AssetConverterLib)
target_include_directories(AssetConverterTests PRIVATE tests)
target_include_directories(AssetConverterTests PRIVATE
    ${CORSAIRS_REPO_ROOT}/sources/Libraries/sqlite3
)
corsairs_set_target_options(AssetConverterTests)
target_compile_definitions(AssetConverterTests
    PRIVATE CORSAIRS_REPO_ROOT="${CORSAIRS_REPO_ROOT}")

add_test(NAME AssetConverterTests COMMAND AssetConverterTests)

add_executable(TerrainReferenceCommandTests
    tests/TestMain.cpp
    tests/TestTerrainReferenceCommand.cpp
)
target_link_libraries(TerrainReferenceCommandTests PRIVATE AssetConverterLib)
target_include_directories(TerrainReferenceCommandTests PRIVATE tests)
corsairs_set_target_options(TerrainReferenceCommandTests)
target_compile_definitions(TerrainReferenceCommandTests
    PRIVATE
        ASSET_CONVERTER_PATH="$<TARGET_FILE:AssetConverter>"
        CORSAIRS_REPO_ROOT="${CORSAIRS_REPO_ROOT}")
add_dependencies(TerrainReferenceCommandTests AssetConverter)

add_executable(
    TerrainPageBudgetProbe
    tests/TerrainPageBudgetProbe.cpp)
target_link_libraries(
    TerrainPageBudgetProbe
    PRIVATE AssetConverterLib)
corsairs_set_target_options(TerrainPageBudgetProbe)
add_dependencies(AssetConverterTests AssetConverter TerrainPageBudgetProbe)
target_compile_definitions(
    AssetConverterTests
    PRIVATE
        ASSET_CONVERTER_PATH="$<TARGET_FILE:AssetConverter>"
        TERRAIN_PAGE_BUDGET_PROBE_PATH="$<TARGET_FILE:TerrainPageBudgetProbe>")
add_test(
    NAME TerrainPageBudget
    COMMAND TerrainPageBudgetProbe
        --repo-root
        ${CORSAIRS_REPO_ROOT})
set_tests_properties(
    TerrainPageBudget
    PROPERTIES
    WORKING_DIRECTORY
        ${CORSAIRS_REPO_ROOT})
