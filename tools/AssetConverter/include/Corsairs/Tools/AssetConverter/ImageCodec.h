#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

// Распакованное изображение: восемь бит на канал, порядок RGBA, строки сверху
// вниз — ровно так, как их ждёт PNG.
struct DecodedImage {
    std::uint32_t Width{0};
    std::uint32_t Height{0};
    std::vector<std::uint8_t> Pixels;   // Width * Height * 4
};

// Читает поддерживаемый stb_image файл и возвращает RGBA8 без переворота строк.
[[nodiscard]] std::optional<DecodedImage> DecodeImageFile(
    const std::filesystem::path& path, std::string& detail);

enum class DdsStatus : std::uint32_t {
    OK = 0,
    NOT_DDS,               // нет сигнатуры «DDS »
    HEADER_TRUNCATED,      // файл короче заголовка
    UNSUPPORTED_FORMAT,    // формат пикселей не реализован
    DATA_TRUNCATED,        // объявленных данных не хватает
};

[[nodiscard]] std::string_view ToString(DdsStatus status);

// Распаковывает DDS в RGBA8.
//
// Поддержаны сжатия BC1/BC2/BC3 (в файлах они помечены как DXT1/DXT3/DXT5) и
// несжатые 24- и 32-битные раскладки. В данных проекта встречаются только
// DXT1, DXT3 и обе несжатые; BC3 реализован на будущее.
//
// Мип-уровни отбрасываются: читается только нулевой. UE строит мип-цепочку
// сам, и переносить чужую незачем.
[[nodiscard]] std::optional<DecodedImage> DecodeDds(std::span<const std::uint8_t> bytes,
                                                    DdsStatus& status);

// Пишет RGBA8 в PNG.
//
// Совместимый адаптер поверх потокового zlib-кодировщика: проверяет весь
// DecodedImage и передаёт его строки без дополнительной копии изображения.
[[nodiscard]] bool WritePng(const std::filesystem::path& path, const DecodedImage& image);

} // namespace Corsairs::Tools::AssetConverter
