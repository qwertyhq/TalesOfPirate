#pragma once

#include "Corsairs/Tools/AssetConverter/MapParser.h"
#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"
#include "Corsairs/Tools/AssetConverter/SceneParity.h"
#include "Corsairs/Tools/AssetConverter/TerrainSurface.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace Corsairs::Tools::AssetConverter {

enum class MapWriteStatus : std::uint32_t {
    OK = 0,
    WRITE_FAILED,
};

enum class SceneManifestStatus : std::uint32_t {
    OK = 0,
    WRITE_FAILED,
    UNKNOWN_OBJECT_TYPE,
    INVALID_SOURCE_CONTEXT,
    TERRAIN_READ_FAILED,
    COUNT_MISMATCH,
    RECOVERY_REQUIRED,
};

struct SceneManifestStats {
    std::uint64_t SourceRecordCount{0u};
    std::uint64_t SceneModelCount{0u};
    std::uint64_t DeferredEffectCount{0u};
    std::uint64_t ReferenceObjectCount{0u};
    std::map<std::uint8_t, std::uint64_t> ReferenceIslandCounts;
};

struct SceneManifestSourceContext {
    SceneFileHeader ObjectHeader{};
    std::string SourceMapSha256;
    std::string SourceObjectSha256;
    std::uint64_t ExpectedSourceRecordCount{0u};
    std::uint64_t ExpectedSceneModelCount{0u};
    std::uint64_t ExpectedDeferredEffectCount{0u};
    std::uint64_t ExpectedReferenceObjectCount{0u};
    std::map<std::uint8_t, std::uint64_t> ExpectedReferenceIslandCounts;
};

enum class SceneManifestFaultAction : std::uint32_t {
    NONE = 0,
    FAIL,
};

using SceneManifestFaultInjector =
    std::function<SceneManifestFaultAction(std::string_view point)>;

// Пишет террейн в набор файлов рядом с basePath (без расширения):
//
//   <base>.height.r16   — карта высот, uint16 little-endian, построчно.
//                         Формат выбран потому, что Unreal Engine импортирует
//                         сырой r16 в Landscape напрямую, без кодеков; PNG
//                         потребовал бы zlib, а конвертер обходится без
//                         внешних зависимостей.
//   <base>.block.raw    — проходимость: по 4 байта на тайл (по одному на
//                         четверть), построчно. Это серверная истина о
//                         проходимости, навигация UE строится по ней.
//   <base>.region.raw   — идентификаторы регионов, uint16 на тайл.
//   <base>.terrain.json — метаданные: размеры, диапазон высот, статистика.
//
// Высота на диске — знаковый байт в единицах по 10 см. В .r16 она смещается в
// беззнаковый диапазон: value = (Height + 128) * 256, чтобы сохранить полный
// исходный диапазон и попасть в 16 бит без потерь.
[[nodiscard]] MapWriteStatus WriteTerrain(const MapTerrain& terrain,
                                          const std::filesystem::path& basePath,
                                          std::string& detail);

// Пишет слои текстур рельефа: `<base>.layers.raw`, по восемь байт на клетку —
// четыре пары «номер текстуры, прозрачность». Базовый слой лежит в отдельном
// поле и всегда непрозрачен, три верхних упакованы в TileInfo по 6 и 4 бита.
//
// Номер разворачивается в файл через таблицу `terrains` игровых данных.
// Смешивание слоёв — задача материала в UE; сюда доносятся исходные данные.
[[nodiscard]] MapWriteStatus WriteTerrainLayers(const MapTerrain& terrain,
                                                const std::filesystem::path& basePath,
                                                std::string& detail);

// Пишет манифест объектов сцены: <base>.objects.json. Позиции остаются в
// исходных целочисленных координатах карты — пересчёт в единицы UE делается
// при импорте, где известен масштаб мира.
[[nodiscard]] MapWriteStatus WriteSceneManifest(const SceneObjects& scene,
                                                const std::filesystem::path& basePath,
                                                std::string& detail);

// Формирует полную source truth сцены и публикует её только после проверки
// контекста, рельефа, статистики и durable read-back.
[[nodiscard]] SceneManifestStatus WriteSceneSourceManifest(
    const SceneSelection& selection,
    IMapTileSource& terrain,
    const SceneManifestSourceContext& context,
    const std::filesystem::path& basePath,
    SceneManifestStats& stats,
    std::string& detail);

// Узкий seam для fault-matrix: production вызывает ту же реализацию с пустым
// injector. Имена точек являются частью тестового контракта Task 4.
[[nodiscard]] SceneManifestStatus WriteSceneSourceManifestForTesting(
    const SceneSelection& selection,
    IMapTileSource& terrain,
    const SceneManifestSourceContext& context,
    const std::filesystem::path& basePath,
    SceneManifestStats& stats,
    std::string& detail,
    const SceneManifestFaultInjector& injectFault);

} // namespace Corsairs::Tools::AssetConverter
