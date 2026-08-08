#include "Corsairs/Tools/AssetConverter/ImageCodec.h"

#include <fstream>
#include <limits>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_PSD
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Corsairs::Tools::AssetConverter {

std::optional<DecodedImage> DecodeImageFile(
    const std::filesystem::path& path, std::string& detail) {
    detail.clear();
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        detail = "не удалось открыть изображение: " + path.string();
        return std::nullopt;
    }

    const std::streampos end = stream.tellg();
    if (end <= 0 || end > static_cast<std::streamoff>(std::numeric_limits<int>::max())) {
        detail = "некорректный размер изображения: " + path.string();
        return std::nullopt;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        detail = "не удалось прочитать изображение: " + path.string();
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        bytes.data(), static_cast<int>(bytes.size()),
        &width, &height, &sourceChannels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        detail = "stb_image не декодировал " + path.string();
        if (const char* reason = stbi_failure_reason(); reason != nullptr) {
            detail += ": ";
            detail += reason;
        }
        stbi_image_free(pixels);
        return std::nullopt;
    }

    const std::size_t pixelCount = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height);
    if (pixelCount > std::numeric_limits<std::size_t>::max() / 4u) {
        stbi_image_free(pixels);
        detail = "размер RGBA изображения переполнен: " + path.string();
        return std::nullopt;
    }

    DecodedImage image;
    image.Width = static_cast<std::uint32_t>(width);
    image.Height = static_cast<std::uint32_t>(height);
    image.Pixels.assign(pixels, pixels + pixelCount * 4u);
    stbi_image_free(pixels);
    return image;
}

} // namespace Corsairs::Tools::AssetConverter
