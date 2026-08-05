#include "Corsairs/Tools/AssetConverter/ImageCodec.h"

#include <array>
#include <cstring>
#include <fstream>

namespace Corsairs::Tools::AssetConverter {

namespace {

// Таблица CRC-32 стандарта PNG (полином 0xEDB88320) считается один раз.
const std::array<std::uint32_t, 256>& CrcTable() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            result[n] = c;
        }
        return result;
    }();
    return table;
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    const auto& table = CrcTable();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// Контрольная сумма потока zlib.
std::uint32_t Adler32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void AppendBigEndian32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void AppendChunk(std::vector<std::uint8_t>& out, const char type[4],
                 const std::vector<std::uint8_t>& payload) {
    AppendBigEndian32(out, static_cast<std::uint32_t>(payload.size()));

    const std::size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());

    AppendBigEndian32(out, Crc32(out.data() + crcStart, out.size() - crcStart));
}

// Поток zlib из несжатых блоков deflate.
//
// Спецификация deflate разрешает блоки типа 00 — «сохранённые»: заголовок,
// длина, её дополнение и сырые данные. Сжатия при этом нет, зато не нужен ни
// zlib, ни собственная реализация Хаффмана. Для промежуточных текстур это
// приемлемо: при импорте в UE они всё равно перепаковываются в формат GPU.
std::vector<std::uint8_t> ZlibStored(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> out;
    out.reserve(raw.size() + raw.size() / 65535 * 5 + 16);

    // Заголовок zlib: метод deflate с окном 32 КБ, без словаря. 0x78 0x01
    // подобраны так, чтобы значение делилось на 31, как требует формат.
    out.push_back(0x78);
    out.push_back(0x01);

    constexpr std::size_t kMaxBlock = 65535;
    std::size_t offset = 0;
    do {
        const std::size_t chunk = std::min(kMaxBlock, raw.size() - offset);
        const bool last = (offset + chunk) >= raw.size();

        out.push_back(last ? 1 : 0);                      // BFINAL, BTYPE=00
        out.push_back(static_cast<std::uint8_t>(chunk));   // LEN, младший байт
        out.push_back(static_cast<std::uint8_t>(chunk >> 8));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~chunk);
        out.push_back(static_cast<std::uint8_t>(inverse));
        out.push_back(static_cast<std::uint8_t>(inverse >> 8));

        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                   raw.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        offset += chunk;
    } while (offset < raw.size());

    AppendBigEndian32(out, Adler32(raw.data(), raw.size()));
    return out;
}

} // namespace

bool WritePng(const std::filesystem::path& path, const DecodedImage& image) {
    if (image.Width == 0 || image.Height == 0) {
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(image.Width) * image.Height * 4;
    if (image.Pixels.size() != expected) {
        return false;
    }

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<std::uint8_t> ihdr;
    AppendBigEndian32(ihdr, image.Width);
    AppendBigEndian32(ihdr, image.Height);
    ihdr.push_back(8);      // бит на канал
    ihdr.push_back(6);      // тип цвета: RGBA
    ihdr.push_back(0);      // сжатие: deflate — единственное допустимое
    ihdr.push_back(0);      // фильтрация: базовый набор
    ihdr.push_back(0);      // без чересстрочности
    AppendChunk(png, "IHDR", ihdr);

    // Каждая строка предваряется байтом фильтра. Ноль означает «без фильтра»:
    // предсказание помогло бы сжатию, которого здесь всё равно нет.
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(image.Height) * (image.Width * 4 + 1));
    for (std::uint32_t y = 0; y < image.Height; ++y) {
        raw.push_back(0);
        const std::size_t rowStart = static_cast<std::size_t>(y) * image.Width * 4;
        raw.insert(raw.end(),
                   image.Pixels.begin() + static_cast<std::ptrdiff_t>(rowStart),
                   image.Pixels.begin() + static_cast<std::ptrdiff_t>(rowStart + image.Width * 4));
    }

    AppendChunk(png, "IDAT", ZlibStored(raw));
    AppendChunk(png, "IEND", {});

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(stream);
}

} // namespace Corsairs::Tools::AssetConverter
