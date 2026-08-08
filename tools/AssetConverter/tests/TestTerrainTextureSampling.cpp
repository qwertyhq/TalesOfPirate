#include "Corsairs/Tools/AssetConverter/TerrainTextureSampling.h"

#include "TestHarness.h"

#include <cstddef>
#include <cstdint>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

AC::DecodedImage RowMajorFixture() {
    AC::DecodedImage image;
    image.Width = 4u;
    image.Height = 4u;
    image.Pixels.resize(4u * 4u * 4u);
    for (std::size_t texel = 0; texel < 16u; ++texel) {
        image.Pixels[texel * 4u + 0u] = static_cast<std::uint8_t>(texel);
        image.Pixels[texel * 4u + 1u] = 0u;
        image.Pixels[texel * 4u + 2u] = 0u;
        image.Pixels[texel * 4u + 3u] = 255u;
    }
    return image;
}

CORSAIRS_TEST(TerrainSampling_WrapOriginBlendsFourAddressedTaps) {
    const auto sample = AC::SampleTerrainImageLinear(
        RowMajorFixture(), 0.0, 0.0,
        AC::TerrainAddressMode::WRAP,
        AC::TerrainAddressMode::WRAP);

    REQUIRE_EQ(sample[0], 8u);
}

CORSAIRS_TEST(TerrainSampling_MirrorOriginClampsToTopLeftTexel) {
    const auto sample = AC::SampleTerrainImageLinear(
        RowMajorFixture(), 0.0, 0.0,
        AC::TerrainAddressMode::MIRROR,
        AC::TerrainAddressMode::MIRROR);

    REQUIRE_EQ(sample[0], 0u);
}

CORSAIRS_TEST(TerrainSampling_FirstTexelCenterUsesTopDecodedRow) {
    const auto sample = AC::SampleTerrainImageLinear(
        RowMajorFixture(), 0.5 / 4.0, 0.5 / 4.0,
        AC::TerrainAddressMode::MIRROR,
        AC::TerrainAddressMode::MIRROR);

    REQUIRE_EQ(sample[0], 0u);
}

CORSAIRS_TEST(TerrainSampling_LastTopRowTexelCenterIsExact) {
    const auto sample = AC::SampleTerrainImageLinear(
        RowMajorFixture(), 3.5 / 4.0, 0.5 / 4.0,
        AC::TerrainAddressMode::WRAP,
        AC::TerrainAddressMode::WRAP);

    REQUIRE_EQ(sample[0], 3u);
}

CORSAIRS_TEST(TerrainSampling_InvalidImageReturnsTransparentBlack) {
    const AC::DecodedImage empty;
    const auto sample = AC::SampleTerrainImageLinear(
        empty, 0.5, 0.5,
        AC::TerrainAddressMode::WRAP,
        AC::TerrainAddressMode::MIRROR);

    REQUIRE_EQ(sample[0], 0u);
    REQUIRE_EQ(sample[1], 0u);
    REQUIRE_EQ(sample[2], 0u);
    REQUIRE_EQ(sample[3], 0u);
}

CORSAIRS_TEST(TerrainSampling_OutOfInt64TapRangeReturnsTransparentBlack) {
    const auto sample = AC::SampleTerrainImageLinear(
        RowMajorFixture(), 0x1p61, 0.5 / 4.0,
        AC::TerrainAddressMode::WRAP,
        AC::TerrainAddressMode::MIRROR);

    REQUIRE_EQ(sample[0], 0u);
    REQUIRE_EQ(sample[1], 0u);
    REQUIRE_EQ(sample[2], 0u);
    REQUIRE_EQ(sample[3], 0u);
}

} // namespace
