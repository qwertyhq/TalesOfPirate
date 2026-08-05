#pragma once

#include "Corsairs/Tools/AssetConverter/MapParser.h"
#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace Corsairs::Tools::AssetConverter {

enum class MapWriteStatus : std::uint32_t {
    OK = 0,
    WRITE_FAILED,
};

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

// Пишет манифест объектов сцены: <base>.objects.json. Позиции остаются в
// исходных целочисленных координатах карты — пересчёт в единицы UE делается
// при импорте, где известен масштаб мира.
[[nodiscard]] MapWriteStatus WriteSceneManifest(const SceneObjects& scene,
                                                const std::filesystem::path& basePath,
                                                std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
