#pragma once

#include "Corsairs/Tools/AssetConverter/TerrainPage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Corsairs::Tools::AssetConverter {

struct TerrainMeshError {
    double MaxAbsCm{0};
    double RmsCm{0};
    double SharedBoundaryMaxCm{0};
    std::size_t Samples{0};
};

[[nodiscard]] TerrainMeshError EvaluateTerrainPageStep(
    const MapPageTiles& page, std::uint32_t step);

struct TerrainPageMeshOptions {
    double MaxAbsCm{5.0};
    double MaxRmsCm{2.0};
    double MaxSharedBoundaryCm{0.0};
};

struct TerrainPageMeshResult {
    bool Ok{false};
    std::uint32_t Step{0};
    TerrainMeshError Error;
    std::filesystem::path GltfPath;
    std::filesystem::path BinPath;
    double ActorWorldXcm{0};
    double ActorWorldYcm{0};
};

[[nodiscard]] TerrainPageMeshResult WriteTerrainPageMesh(
    const MapPageTiles& page,
    TerrainPageId pageId,
    const std::filesystem::path& outputDirectory,
    const TerrainPageMeshOptions& options,
    std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
