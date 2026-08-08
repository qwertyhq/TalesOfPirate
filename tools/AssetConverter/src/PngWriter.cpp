#include "Corsairs/Tools/AssetConverter/ImageCodec.h"
#include "Corsairs/Tools/AssetConverter/StreamingPngWriter.h"

#include <limits>

namespace Corsairs::Tools::AssetConverter {

namespace {

bool MultiplyOverflows(std::size_t left, std::size_t right) {
    return left != 0 && right > std::numeric_limits<std::size_t>::max() / left;
}

} // namespace

bool WritePng(const std::filesystem::path& path, const DecodedImage& image) {
    if (image.Width == 0 || image.Height == 0) {
        return false;
    }
    const std::size_t width = image.Width;
    if (MultiplyOverflows(width, 4u)) {
        return false;
    }

    const std::size_t rowBytes = width * 4u;
    if (MultiplyOverflows(rowBytes, image.Height)) {
        return false;
    }
    if (image.Pixels.size() != rowBytes * image.Height) {
        return false;
    }

    std::string detail;
    auto writer = StreamingPngWriter::Open(path, image.Width, image.Height, detail);
    if (!writer) {
        return false;
    }

    const std::span<const std::uint8_t> pixels{image.Pixels};
    for (std::uint32_t y = 0; y < image.Height; ++y) {
        if (!writer->WriteRgbaRow(
                pixels.subspan(static_cast<std::size_t>(y) * rowBytes, rowBytes), detail)) {
            return false;
        }
    }
    return writer->Finish(detail);
}

} // namespace Corsairs::Tools::AssetConverter
