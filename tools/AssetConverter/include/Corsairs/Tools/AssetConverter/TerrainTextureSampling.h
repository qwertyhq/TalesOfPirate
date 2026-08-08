#pragma once

#include "Corsairs/Tools/AssetConverter/ImageCodec.h"

#include <array>
#include <cstdint>

namespace Corsairs::Tools::AssetConverter {

enum class TerrainAddressMode : std::uint32_t {
    WRAP,
    MIRROR,
};

[[nodiscard]] std::array<std::uint8_t, 4> SampleTerrainImageLinear(
    const DecodedImage& image,
    double normalizedU,
    double normalizedV,
    TerrainAddressMode addressU,
    TerrainAddressMode addressV);

} // namespace Corsairs::Tools::AssetConverter
