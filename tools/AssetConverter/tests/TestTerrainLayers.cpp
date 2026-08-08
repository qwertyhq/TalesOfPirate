#include "Corsairs/Tools/AssetConverter/TerrainLayers.h"

#include "TestHarness.h"

#include <cstdint>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::uint32_t PackLayer(std::uint8_t texture,
                                  std::uint8_t alpha,
                                  int textureShift,
                                  int alphaShift) {
    return (static_cast<std::uint32_t>(texture) << textureShift) |
           (static_cast<std::uint32_t>(alpha) << alphaShift);
}

CORSAIRS_TEST(TerrainLayers_StopsAtFirstZeroTextureId) {
    const AC::MapTile tile{
        .TileInfo = 0x02cf2000u,
        .BaseTex = 4u,
    };

    const AC::ResolvedTerrainLayers layers = AC::ResolveTerrainLayers(tile);

    REQUIRE_EQ(layers.Count, 1u);
    REQUIRE_EQ(layers.Values[0].TextureId, 4u);
    REQUIRE_EQ(layers.Values[0].AlphaMask, 15u);
}

CORSAIRS_TEST(TerrainLayers_ZeroBaseTextureHasNoLayers) {
    const AC::MapTile tile{
        .TileInfo = PackLayer(7u, 15u, 26, 22),
        .BaseTex = 0u,
    };

    const AC::ResolvedTerrainLayers layers = AC::ResolveTerrainLayers(tile);

    REQUIRE_EQ(layers.Count, 0u);
}

CORSAIRS_TEST(TerrainLayers_SkipsZeroAlphaWithoutTerminating) {
    const AC::MapTile tile{
        .TileInfo = PackLayer(7u, 0u, 26, 22) |
                    PackLayer(8u, 15u, 16, 12),
        .BaseTex = 4u,
    };

    const AC::ResolvedTerrainLayers layers = AC::ResolveTerrainLayers(tile);

    REQUIRE_EQ(layers.Count, 2u);
    REQUIRE_EQ(layers.Values[0].TextureId, 4u);
    REQUIRE_EQ(layers.Values[0].AlphaMask, 15u);
    REQUIRE_EQ(layers.Values[1].TextureId, 8u);
    REQUIRE_EQ(layers.Values[1].AlphaMask, 15u);
}

CORSAIRS_TEST(TerrainLayers_AlphaZeroHasNoAtlasRect) {
    REQUIRE(!AC::ResolveAlphaAtlasRect(0u).has_value());
}

CORSAIRS_TEST(TerrainLayers_AlphaRectsAreRawAtlasQuarters) {
    const auto first = AC::ResolveAlphaAtlasRect(1u);
    const auto fourteenth = AC::ResolveAlphaAtlasRect(14u);
    const auto fifteenth = AC::ResolveAlphaAtlasRect(15u);

    REQUIRE(first.has_value());
    REQUIRE_EQ(first->U0, 0.0f);
    REQUIRE_EQ(first->V0, 0.0f);
    REQUIRE_EQ(first->U1, 0.25f);
    REQUIRE_EQ(first->V1, 0.25f);

    REQUIRE(fourteenth.has_value());
    REQUIRE_EQ(fourteenth->U0, 0.25f);
    REQUIRE_EQ(fourteenth->V0, 0.75f);
    REQUIRE_EQ(fourteenth->U1, 0.5f);
    REQUIRE_EQ(fourteenth->V1, 1.0f);

    REQUIRE(fifteenth.has_value());
    REQUIRE_EQ(fifteenth->U0, 0.5f);
    REQUIRE_EQ(fifteenth->V0, 0.75f);
    REQUIRE_EQ(fifteenth->U1, 0.75f);
    REQUIRE_EQ(fifteenth->V1, 1.0f);
}

} // namespace
