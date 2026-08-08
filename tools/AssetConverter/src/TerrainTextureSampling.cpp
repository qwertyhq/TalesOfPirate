#include "Corsairs/Tools/AssetConverter/TerrainTextureSampling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Corsairs::Tools::AssetConverter {

namespace {

std::uint32_t AddressTap(std::int64_t tap,
                         std::uint32_t size,
                         TerrainAddressMode mode) {
    const std::int64_t extent = static_cast<std::int64_t>(size);
    const std::int64_t period = mode == TerrainAddressMode::WRAP
                                    ? extent
                                    : extent * 2;
    std::int64_t addressed = tap % period;
    if (addressed < 0) {
        addressed += period;
    }
    if (mode == TerrainAddressMode::MIRROR && addressed >= extent) {
        addressed = period - 1 - addressed;
    }
    return static_cast<std::uint32_t>(addressed);
}

const std::uint8_t* Texel(const DecodedImage& image,
                          std::uint32_t x,
                          std::uint32_t y) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * image.Width + x) * 4u;
    return image.Pixels.data() + offset;
}

} // namespace

std::array<std::uint8_t, 4> SampleTerrainImageLinear(
    const DecodedImage& image,
    double normalizedU,
    double normalizedV,
    TerrainAddressMode addressU,
    TerrainAddressMode addressV) {
    if (image.Width == 0u || image.Height == 0u ||
        !std::isfinite(normalizedU) || !std::isfinite(normalizedV)) {
        return {};
    }

    const std::size_t width = image.Width;
    const std::size_t height = image.Height;
    if (width > std::numeric_limits<std::size_t>::max() / height / 4u ||
        image.Pixels.size() != width * height * 4u) {
        return {};
    }

    const double sourceX = normalizedU * static_cast<double>(image.Width) - 0.5;
    const double sourceY = normalizedV * static_cast<double>(image.Height) - 0.5;
    const double floorX = std::floor(sourceX);
    const double floorY = std::floor(sourceY);
    constexpr double int64Lower = -0x1p63;
    constexpr double int64Upper = 0x1p63;
    if (floorX < int64Lower || floorX >= int64Upper ||
        floorY < int64Lower || floorY >= int64Upper) {
        return {};
    }

    const auto x0Tap = static_cast<std::int64_t>(floorX);
    const auto y0Tap = static_cast<std::int64_t>(floorY);
    const std::uint32_t x0 = AddressTap(x0Tap, image.Width, addressU);
    const std::uint32_t x1 = AddressTap(x0Tap + 1, image.Width, addressU);
    const std::uint32_t y0 = AddressTap(y0Tap, image.Height, addressV);
    const std::uint32_t y1 = AddressTap(y0Tap + 1, image.Height, addressV);
    const double fractionX = sourceX - floorX;
    const double fractionY = sourceY - floorY;

    const std::uint8_t* topLeft = Texel(image, x0, y0);
    const std::uint8_t* topRight = Texel(image, x1, y0);
    const std::uint8_t* bottomLeft = Texel(image, x0, y1);
    const std::uint8_t* bottomRight = Texel(image, x1, y1);

    std::array<std::uint8_t, 4> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        const double top = static_cast<double>(topLeft[channel]) +
                           (static_cast<double>(topRight[channel]) - topLeft[channel]) *
                               fractionX;
        const double bottom = static_cast<double>(bottomLeft[channel]) +
                              (static_cast<double>(bottomRight[channel]) - bottomLeft[channel]) *
                                  fractionX;
        const double value = top + (bottom - top) * fractionY;
        result[channel] = static_cast<std::uint8_t>(
            std::clamp(std::floor(value + 0.5), 0.0, 255.0));
    }
    return result;
}

} // namespace Corsairs::Tools::AssetConverter
