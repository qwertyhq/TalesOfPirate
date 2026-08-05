#pragma once

#include "Corsairs/Tools/AssetConverter/MapParser.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace Corsairs::Tools::AssetConverter {

struct TerrainMeshOptions {
    // Через сколько клеток карты брать вершину. Единица означает полную сетку.
    std::size_t Step{4};
    // Сколько четырёхугольников по стороне в одной плитке.
    std::size_t TileQuads{128};
};

struct TerrainMeshStats {
    bool Ok{false};
    std::size_t Tiles{0};
    std::size_t Vertices{0};
    std::size_t Triangles{0};
};

// Пишет рельеф набором мешей glTF: `<base>.terrain_XX_YY.gltf`.
//
// Зачем меш, а не Landscape. Ландшафт Unreal собирается инструментами
// редактора, которых нет в headless-режиме: импорт карты высот в Landscape
// требует запущенного редактора с графикой. Обычные статические меши
// импортируются тем же путём, что и все остальные модели, и потому доступны
// в автоматическом прогоне.
//
// Рельеф режется на плитки: цельная сетка 4096x4096 дала бы шестнадцать
// миллионов вершин, чего один статический меш не выдержит. Соседние плитки
// делят крайние ряды вершин, иначе на стыках появятся щели.
[[nodiscard]] TerrainMeshStats WriteTerrainMesh(const MapTerrain& terrain,
                                                const std::filesystem::path& basePath,
                                                const TerrainMeshOptions& options,
                                                std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
