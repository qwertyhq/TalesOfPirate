#include "Corsairs/Tools/AssetConverter/TerrainSurface.h"

#include <algorithm>
#include <array>
#include <optional>

namespace Corsairs::Tools::AssetConverter {

namespace {

inline constexpr std::int32_t kTileSizeCm = 100;
inline constexpr float kRawHeightUnitCm = 10.0f;

struct TerrainQuadSample {
    std::array<TerrainTileRead, 4> Vertices;
    float FractionX{0};
    float FractionY{0};
};

std::optional<TerrainQuadSample> ReadTerrainQuad(
    IMapTileSource& source,
    std::int32_t sourceXcm,
    std::int32_t sourceYcm) {
    if (sourceXcm < 0 || sourceYcm < 0) {
        return std::nullopt;
    }

    const std::size_t gridWidth = source.GridWidth();
    const std::size_t gridHeight = source.GridHeight();
    if (gridWidth < 2 || gridHeight < 2) {
        return std::nullopt;
    }

    const std::int32_t tileX = sourceXcm / kTileSizeCm;
    const std::int32_t tileY = sourceYcm / kTileSizeCm;
    if (static_cast<std::size_t>(tileX) >= gridWidth - 1u ||
        static_cast<std::size_t>(tileY) >= gridHeight - 1u) {
        return std::nullopt;
    }

    TerrainQuadSample sample;
    sample.Vertices = {
        source.ReadTile(tileX, tileY),
        source.ReadTile(tileX + 1, tileY),
        source.ReadTile(tileX, tileY + 1),
        source.ReadTile(tileX + 1, tileY + 1),
    };
    sample.FractionX = static_cast<float>(sourceXcm % kTileSizeCm) /
                       static_cast<float>(kTileSizeCm);
    sample.FractionY = static_cast<float>(sourceYcm % kTileSizeCm) /
                       static_cast<float>(kTileSizeCm);
    return sample;
}

float VertexHeightCm(const TerrainTileRead& vertex) {
    if (!vertex.SectionPresent) {
        return 0.0f;
    }
    return static_cast<float>(vertex.Tile.Height) * kRawHeightUnitCm;
}

float InterpolateSurfaceHeight(const TerrainQuadSample& sample) {
    const float topLeft = VertexHeightCm(sample.Vertices[0]);
    const float topRight = VertexHeightCm(sample.Vertices[1]);
    const float bottomLeft = VertexHeightCm(sample.Vertices[2]);
    const float bottomRight = VertexHeightCm(sample.Vertices[3]);

    float height = 0.0f;
    if (sample.FractionX + sample.FractionY <= 1.0f) {
        height = topLeft +
                 sample.FractionX * (topRight - topLeft) +
                 sample.FractionY * (bottomLeft - topLeft);
    }
    else {
        height = bottomRight +
                 (1.0f - sample.FractionX) * (bottomLeft - bottomRight) +
                 (1.0f - sample.FractionY) * (topRight - bottomRight);
    }
    return std::max(height, 0.0f);
}

} // namespace

float TerrainSurfaceHeight(
    IMapTileSource& source,
    std::int32_t sourceXcm,
    std::int32_t sourceYcm) {
    const auto sample = ReadTerrainQuad(source, sourceXcm, sourceYcm);
    return sample.has_value() ? InterpolateSurfaceHeight(*sample) : 0.0f;
}

TerrainAnchorSample SampleTerrainAnchor(
    IMapTileSource& source,
    std::int32_t sourceXcm,
    std::int32_t sourceYcm) {
    const auto sample = ReadTerrainQuad(source, sourceXcm, sourceYcm);
    if (!sample.has_value()) {
        return {};
    }

    TerrainAnchorSample result;
    result.SurfaceHeightCm = InterpolateSurfaceHeight(*sample);
    const TerrainTileRead& floor = sample->Vertices[0];
    if (floor.SectionPresent) {
        result.TileColor565 = static_cast<std::uint16_t>(floor.Tile.Color);
        result.Island = floor.Tile.Island;
        result.SectionPresent = true;
    }
    return result;
}

} // namespace Corsairs::Tools::AssetConverter
