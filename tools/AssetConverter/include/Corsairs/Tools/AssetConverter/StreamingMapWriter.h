#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Corsairs::Tools::AssetConverter {

class MapSectionReader;
enum class MapWriteStatus : std::uint32_t;

struct MapRasterStats {
    std::size_t PresentSections{0};
    std::size_t AbsentSections{0};
    std::int8_t MinHeightRaw{0};
    std::int8_t MaxHeightRaw{0};
    std::size_t BlockedTiles{0};
};

[[nodiscard]] MapWriteStatus WriteTerrainRasters(
    MapSectionReader& reader,
    const std::filesystem::path& basePath,
    MapRasterStats& stats,
    std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
