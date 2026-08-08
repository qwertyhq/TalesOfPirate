#pragma once

#include "Corsairs/Tools/AssetConverter/TerrainPage.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Corsairs::Tools::AssetConverter {

struct TerrainTileRead {
    MapTile Tile{};
    bool SectionPresent{false};
};

class IMapTileSource {
public:
    virtual ~IMapTileSource() = default;

    [[nodiscard]] virtual std::size_t GridWidth() const = 0;
    [[nodiscard]] virtual std::size_t GridHeight() const = 0;
    virtual TerrainTileRead ReadTile(
        std::int32_t tileX,
        std::int32_t tileY) = 0;
    [[nodiscard]] virtual const std::string& LastError() const = 0;
};

struct TerrainAnchorSample {
    float SurfaceHeightCm{0};
    std::uint16_t TileColor565{0};
    std::uint8_t Island{0};
    bool SectionPresent{false};
};

[[nodiscard]] float TerrainSurfaceHeight(
    IMapTileSource& source,
    std::int32_t sourceXcm,
    std::int32_t sourceYcm);

[[nodiscard]] TerrainAnchorSample SampleTerrainAnchor(
    IMapTileSource& source,
    std::int32_t sourceXcm,
    std::int32_t sourceYcm);

} // namespace Corsairs::Tools::AssetConverter
