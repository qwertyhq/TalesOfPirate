#pragma once

#include "Corsairs/Tools/AssetConverter/TerrainPage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Corsairs::Tools::AssetConverter {

struct TerrainLayer {
    std::uint8_t TextureId{0};
    std::uint8_t AlphaMask{0};
};

struct ResolvedTerrainLayers {
    std::array<TerrainLayer, 4> Values{};
    std::size_t Count{0};
};

[[nodiscard]] ResolvedTerrainLayers ResolveTerrainLayers(const MapTile& tile) noexcept;

struct AtlasRect {
    float U0{0};
    float V0{0};
    float U1{0};
    float V1{0};
};

[[nodiscard]] std::optional<AtlasRect> ResolveAlphaAtlasRect(
    std::uint8_t mask) noexcept;

} // namespace Corsairs::Tools::AssetConverter
