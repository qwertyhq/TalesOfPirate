#pragma once

#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/TerrainCatalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

struct LegacyTerrainCornerSample {
    std::array<std::uint8_t, 4> Diffuse;
    double HeightCm;
};

[[nodiscard]] LegacyTerrainCornerSample ResolveLegacyTerrainCornerSample(
    const MapTile& tile, bool present) noexcept;

struct TerrainBakeOptions {
    std::uint32_t CellsPerPage{128};
    std::uint32_t PixelsPerCell{32};
    std::size_t MaxTextureCacheBytes{32u * 1024u * 1024u};
    std::size_t MaxRssBytes{128u * 1024u * 1024u};
    std::size_t MaxPngBytes{96u * 1024u * 1024u};
    std::size_t MaxRgbaRowBytes{16u * 1024u};
};

struct TerrainBakeResult {
    bool Ok{false};
    std::string AlgorithmVersion{"legacy-fixed-pipeline-v1"};
    MapCellRect SourceCellBounds{};
    std::filesystem::path PngPath;
    std::string PngSha256;
    std::vector<std::uint8_t> UsedTextureIds;
    std::uint32_t SectionOriginX{0};
    std::uint32_t SectionOriginY{0};
    std::uint32_t SectionGridWidth{0};
    std::uint32_t SectionGridHeight{0};
    std::vector<std::uint8_t> SectionPresenceMask;
    std::size_t AbsentSections{0};
    std::size_t UnresolvedLayers{0};
    std::size_t PeakRssBytes{0};
    std::size_t OutputBytes{0};
    std::size_t PeakTextureCacheBytes{0};
    std::size_t PeakRgbaRowBytes{0};
};

TerrainBakeResult BakeTerrainPage(
    MapSectionReader& reader,
    const TerrainCatalog& catalog,
    TerrainPageId page,
    const std::filesystem::path& alphaAtlas,
    const std::filesystem::path& outputDirectory,
    const TerrainBakeOptions& options,
    std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
