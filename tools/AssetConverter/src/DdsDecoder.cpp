#include "Corsairs/Tools/AssetConverter/ImageCodec.h"

#include <algorithm>
#include <cstring>

namespace Corsairs::Tools::AssetConverter {

namespace {

// Смещения полей в DDS: сигнатура 4 байта, дальше DDS_HEADER на 124 байта.
constexpr std::size_t kMagicSize = 4;
constexpr std::size_t kHeaderSize = 124;
constexpr std::size_t kTotalHeader = kMagicSize + kHeaderSize;

// Смещения отсчитываются ОТ НАЧАЛА ФАЙЛА, вместе с сигнатурой, и читать по ним
// нужно из полного буфера. Отсчёт от начала DDS_HEADER сдвинул бы каждое поле
// на четыре байта: высота пришла бы из ширины, флаги формата — из FourCC.
constexpr std::size_t kOffHeight = 12;
constexpr std::size_t kOffWidth = 16;
constexpr std::size_t kOffPfFlags = 80;
constexpr std::size_t kOffFourCc = 84;
constexpr std::size_t kOffRgbBitCount = 88;
constexpr std::size_t kOffRMask = 92;
constexpr std::size_t kOffGMask = 96;
constexpr std::size_t kOffBMask = 100;
constexpr std::size_t kOffAMask = 104;

constexpr std::uint32_t kPfFourCc = 0x00000004;
constexpr std::uint32_t kPfRgb = 0x00000040;

std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Разворачивает RGB565 в три канала по восемь бит. Младшие биды дублируются
// из старших, иначе белый (0xFFFF) дал бы 248 вместо 255.
void Rgb565ToRgb8(std::uint16_t packed, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    const std::uint32_t r5 = (packed >> 11) & 0x1F;
    const std::uint32_t g6 = (packed >> 5) & 0x3F;
    const std::uint32_t b5 = packed & 0x1F;
    r = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
}

struct Rgba {
    std::uint8_t R, G, B, A;
};

// Цветовая часть блока BC1/BC2/BC3: два опорных цвета и по два бита индекса
// на пиксель. У BC1 порядок опорных цветов задаёт, есть ли прозрачность:
// если color0 <= color1, четвёртый цвет полностью прозрачен.
void DecodeColorBlock(const std::uint8_t* block, bool punchThroughAlpha, Rgba out[16]) {
    std::uint16_t c0 = 0;
    std::uint16_t c1 = 0;
    std::memcpy(&c0, block, sizeof(c0));
    std::memcpy(&c1, block + 2, sizeof(c1));

    Rgba palette[4]{};
    Rgb565ToRgb8(c0, palette[0].R, palette[0].G, palette[0].B);
    Rgb565ToRgb8(c1, palette[1].R, palette[1].G, palette[1].B);
    palette[0].A = 255;
    palette[1].A = 255;

    if (!punchThroughAlpha || c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            const std::uint8_t* a = &palette[0].R + i;
            const std::uint8_t* b = &palette[1].R + i;
            (&palette[2].R)[i] = static_cast<std::uint8_t>((2 * (*a) + (*b)) / 3);
            (&palette[3].R)[i] = static_cast<std::uint8_t>(((*a) + 2 * (*b)) / 3);
        }
        palette[2].A = 255;
        palette[3].A = 255;
    }
    else {
        for (int i = 0; i < 3; ++i) {
            const std::uint8_t* a = &palette[0].R + i;
            const std::uint8_t* b = &palette[1].R + i;
            (&palette[2].R)[i] = static_cast<std::uint8_t>(((*a) + (*b)) / 2);
            (&palette[3].R)[i] = 0;
        }
        palette[2].A = 255;
        palette[3].A = 0;
    }

    std::uint32_t indices = 0;
    std::memcpy(&indices, block + 4, sizeof(indices));
    for (int i = 0; i < 16; ++i) {
        out[i] = palette[(indices >> (i * 2)) & 0x3];
    }
}

// Альфа BC2: по четыре бита на пиксель, восемь байт на блок.
void DecodeAlphaBc2(const std::uint8_t* block, Rgba out[16]) {
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t byte = block[i / 2];
        const std::uint32_t nibble = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
        out[i].A = static_cast<std::uint8_t>(nibble * 17);   // 0..15 -> 0..255
    }
}

// Альфа BC3: два опорных значения и по три бита индекса на пиксель.
void DecodeAlphaBc3(const std::uint8_t* block, Rgba out[16]) {
    const std::uint8_t a0 = block[0];
    const std::uint8_t a1 = block[1];

    std::uint8_t palette[8]{a0, a1};
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i) {
            palette[i + 1] = static_cast<std::uint8_t>(((7 - i) * a0 + i * a1) / 7);
        }
    }
    else {
        for (int i = 1; i <= 4; ++i) {
            palette[i + 1] = static_cast<std::uint8_t>(((5 - i) * a0 + i * a1) / 5);
        }
        palette[6] = 0;
        palette[7] = 255;
    }

    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<std::uint64_t>(block[2 + i]) << (i * 8);
    }
    for (int i = 0; i < 16; ++i) {
        out[i].A = palette[(bits >> (i * 3)) & 0x7];
    }
}

// Кладёт разобранный блок 4x4 в изображение, обрезая по краям: размеры не
// обязаны делиться на четыре.
void BlitBlock(const Rgba block[16], std::uint32_t blockX, std::uint32_t blockY,
               DecodedImage& image) {
    for (std::uint32_t row = 0; row < 4; ++row) {
        const std::uint32_t y = blockY + row;
        if (y >= image.Height) {
            break;
        }
        for (std::uint32_t col = 0; col < 4; ++col) {
            const std::uint32_t x = blockX + col;
            if (x >= image.Width) {
                break;
            }
            const Rgba& pixel = block[row * 4 + col];
            const std::size_t at = (static_cast<std::size_t>(y) * image.Width + x) * 4;
            image.Pixels[at + 0] = pixel.R;
            image.Pixels[at + 1] = pixel.G;
            image.Pixels[at + 2] = pixel.B;
            image.Pixels[at + 3] = pixel.A;
        }
    }
}

// Сколько бит занимает маска и на сколько сдвинута. Нужно, чтобы разобрать
// несжатые раскладки, не полагаясь на конкретный порядок каналов: в DDS он
// задаётся масками, и BGRA встречается чаще RGBA.
void MaskInfo(std::uint32_t mask, std::uint32_t& shift, std::uint32_t& bits) {
    shift = 0;
    bits = 0;
    if (mask == 0) {
        return;
    }
    while ((mask & 1u) == 0) {
        mask >>= 1;
        ++shift;
    }
    while ((mask & 1u) != 0) {
        mask >>= 1;
        ++bits;
    }
}

std::uint8_t ExtractChannel(std::uint32_t value, std::uint32_t shift, std::uint32_t bits) {
    if (bits == 0) {
        return 255;
    }
    const std::uint32_t raw = (value >> shift) & ((1u << bits) - 1u);
    const std::uint32_t maxValue = (1u << bits) - 1u;
    return static_cast<std::uint8_t>(raw * 255 / maxValue);
}

} // namespace

std::string_view ToString(DdsStatus status) {
    switch (status) {
    case DdsStatus::OK:                 return "OK";
    case DdsStatus::NOT_DDS:            return "NOT_DDS";
    case DdsStatus::HEADER_TRUNCATED:   return "HEADER_TRUNCATED";
    case DdsStatus::UNSUPPORTED_FORMAT: return "UNSUPPORTED_FORMAT";
    case DdsStatus::DATA_TRUNCATED:     return "DATA_TRUNCATED";
    }
    return "UNKNOWN";
}

std::optional<DecodedImage> DecodeDds(std::span<const std::uint8_t> bytes, DdsStatus& status) {
    if (bytes.size() < kTotalHeader) {
        status = bytes.size() < kMagicSize ? DdsStatus::NOT_DDS : DdsStatus::HEADER_TRUNCATED;
        return std::nullopt;
    }
    if (std::memcmp(bytes.data(), "DDS ", kMagicSize) != 0) {
        status = DdsStatus::NOT_DDS;
        return std::nullopt;
    }

    DecodedImage image;
    image.Height = ReadU32(bytes, kOffHeight);
    image.Width = ReadU32(bytes, kOffWidth);
    if (image.Width == 0 || image.Height == 0) {
        status = DdsStatus::HEADER_TRUNCATED;
        return std::nullopt;
    }
    image.Pixels.assign(static_cast<std::size_t>(image.Width) * image.Height * 4, 0);

    const std::uint32_t pfFlags = ReadU32(bytes, kOffPfFlags);
    const std::span<const std::uint8_t> data = bytes.subspan(kTotalHeader);

    if ((pfFlags & kPfFourCc) != 0) {
        char fourCc[5]{};
        std::memcpy(fourCc, bytes.data() + kOffFourCc, 4);

        std::size_t blockBytes = 0;
        bool hasBc2Alpha = false;
        bool hasBc3Alpha = false;
        bool punchThrough = false;

        if (std::memcmp(fourCc, "DXT1", 4) == 0) {
            blockBytes = 8;
            punchThrough = true;
        }
        else if (std::memcmp(fourCc, "DXT3", 4) == 0) {
            blockBytes = 16;
            hasBc2Alpha = true;
        }
        else if (std::memcmp(fourCc, "DXT5", 4) == 0) {
            blockBytes = 16;
            hasBc3Alpha = true;
        }
        else {
            status = DdsStatus::UNSUPPORTED_FORMAT;
            return std::nullopt;
        }

        const std::uint32_t blocksX = (image.Width + 3) / 4;
        const std::uint32_t blocksY = (image.Height + 3) / 4;
        const std::size_t needed = static_cast<std::size_t>(blocksX) * blocksY * blockBytes;
        if (data.size() < needed) {
            status = DdsStatus::DATA_TRUNCATED;
            return std::nullopt;
        }

        for (std::uint32_t by = 0; by < blocksY; ++by) {
            for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
                const std::uint8_t* block =
                    data.data() + (static_cast<std::size_t>(by) * blocksX + bx) * blockBytes;

                Rgba pixels[16]{};
                const std::uint8_t* colorBlock = (blockBytes == 16) ? block + 8 : block;
                DecodeColorBlock(colorBlock, punchThrough, pixels);

                if (hasBc2Alpha) {
                    DecodeAlphaBc2(block, pixels);
                }
                else if (hasBc3Alpha) {
                    DecodeAlphaBc3(block, pixels);
                }

                BlitBlock(pixels, bx * 4, by * 4, image);
            }
        }

        status = DdsStatus::OK;
        return image;
    }

    if ((pfFlags & kPfRgb) != 0) {
        const std::uint32_t bitCount = ReadU32(bytes, kOffRgbBitCount);
        if (bitCount != 24 && bitCount != 32) {
            status = DdsStatus::UNSUPPORTED_FORMAT;
            return std::nullopt;
        }

        std::uint32_t rShift = 0, rBits = 0, gShift = 0, gBits = 0;
        std::uint32_t bShift = 0, bBits = 0, aShift = 0, aBits = 0;
        MaskInfo(ReadU32(bytes, kOffRMask), rShift, rBits);
        MaskInfo(ReadU32(bytes, kOffGMask), gShift, gBits);
        MaskInfo(ReadU32(bytes, kOffBMask), bShift, bBits);
        MaskInfo(ReadU32(bytes, kOffAMask), aShift, aBits);

        const std::size_t bytesPerPixel = bitCount / 8;
        const std::size_t needed =
            static_cast<std::size_t>(image.Width) * image.Height * bytesPerPixel;
        if (data.size() < needed) {
            status = DdsStatus::DATA_TRUNCATED;
            return std::nullopt;
        }

        for (std::uint32_t y = 0; y < image.Height; ++y) {
            for (std::uint32_t x = 0; x < image.Width; ++x) {
                const std::size_t at =
                    (static_cast<std::size_t>(y) * image.Width + x) * bytesPerPixel;

                std::uint32_t packed = 0;
                std::memcpy(&packed, data.data() + at, bytesPerPixel);

                const std::size_t out = (static_cast<std::size_t>(y) * image.Width + x) * 4;
                image.Pixels[out + 0] = ExtractChannel(packed, rShift, rBits);
                image.Pixels[out + 1] = ExtractChannel(packed, gShift, gBits);
                image.Pixels[out + 2] = ExtractChannel(packed, bShift, bBits);
                image.Pixels[out + 3] = ExtractChannel(packed, aShift, aBits);
            }
        }

        status = DdsStatus::OK;
        return image;
    }

    status = DdsStatus::UNSUPPORTED_FORMAT;
    return std::nullopt;
}

} // namespace Corsairs::Tools::AssetConverter
