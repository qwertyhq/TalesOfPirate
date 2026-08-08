#include "Corsairs/Tools/AssetConverter/TerrainLayers.h"

#include <array>
#include <cstdint>

namespace Corsairs::Tools::AssetConverter {

namespace {

struct LayerBits {
    int TextureShift;
    int AlphaShift;
};

constexpr std::array<LayerBits, 3> kUpperLayers{{
    {26, 22},
    {16, 12},
    {6, 2},
}};

constexpr std::uint32_t kTextureMask = 0x3Fu;
constexpr std::uint32_t kAlphaMask = 0x0Fu;

} // namespace

ResolvedTerrainLayers ResolveTerrainLayers(const MapTile& tile) noexcept {
    ResolvedTerrainLayers result;
    if (tile.BaseTex == 0u) {
        return result;
    }

    result.Values[result.Count++] = TerrainLayer{tile.BaseTex, 15u};
    for (const LayerBits bits : kUpperLayers) {
        const auto texture = static_cast<std::uint8_t>(
            (tile.TileInfo >> bits.TextureShift) & kTextureMask);
        if (texture == 0u) {
            break;
        }

        const auto alpha = static_cast<std::uint8_t>(
            (tile.TileInfo >> bits.AlphaShift) & kAlphaMask);
        if (alpha != 0u) {
            result.Values[result.Count++] = TerrainLayer{texture, alpha};
        }
    }
    return result;
}

std::optional<AtlasRect> ResolveAlphaAtlasRect(std::uint8_t mask) noexcept {
    if (mask == 0u || mask > 15u) {
        return std::nullopt;
    }

    constexpr float quarter = 0.25f;
    const std::uint8_t index = static_cast<std::uint8_t>(mask - 1u);
    const float column = static_cast<float>(index % 4u);
    const float row = static_cast<float>(index / 4u);
    return AtlasRect{
        column * quarter,
        row * quarter,
        (column + 1.0f) * quarter,
        (row + 1.0f) * quarter,
    };
}

} // namespace Corsairs::Tools::AssetConverter
