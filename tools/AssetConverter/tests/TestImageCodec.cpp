#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/ImageCodec.h"

#include "TestHarness.h"

#include <cstring>
#include <filesystem>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path TempDir() {
    return std::filesystem::temp_directory_path() / "corsairs-image-tests";
}

// Собирает DDS с заданным заголовком пиксельного формата и телом данных.
std::vector<std::uint8_t> MakeDds(std::uint32_t width, std::uint32_t height,
                                  std::uint32_t pfFlags, const char* fourCc,
                                  std::uint32_t bitCount,
                                  std::uint32_t rMask, std::uint32_t gMask,
                                  std::uint32_t bMask, std::uint32_t aMask,
                                  const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> bytes(128, 0);
    std::memcpy(bytes.data(), "DDS ", 4);

    const auto put = [&bytes](std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };

    // Смещения — от начала файла, ровно как в спецификации DDS. Отсчёт от
    // начала DDS_HEADER сдвинул бы каждое поле на четыре байта; именно так
    // конструктор и был написан сначала, повторив ошибку декодера, отчего
    // тесты подтверждали неверную раскладку вместо того, чтобы её вскрыть.
    put(4, 124);                 // dwSize
    put(12, height);
    put(16, width);
    put(80, pfFlags);
    if (fourCc != nullptr) {
        std::memcpy(bytes.data() + 84, fourCc, 4);
    }
    put(88, bitCount);
    put(92, rMask);
    put(96, gMask);
    put(100, bMask);
    put(104, aMask);

    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

// Блок BC1 из одного цвета: оба опорных значения равны, индексы нулевые.
std::vector<std::uint8_t> SolidBc1Block(std::uint16_t color) {
    std::vector<std::uint8_t> block(8, 0);
    std::memcpy(block.data(), &color, 2);
    std::memcpy(block.data() + 2, &color, 2);
    return block;
}

CORSAIRS_TEST(Dds_RejectsFileWithoutSignature) {
    std::vector<std::uint8_t> bytes(200, 0);
    std::memcpy(bytes.data(), "NOPE", 4);

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(!image.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::DdsStatus::NOT_DDS));
}

CORSAIRS_TEST(Dds_RejectsTruncatedHeader) {
    std::vector<std::uint8_t> bytes(64, 0);
    std::memcpy(bytes.data(), "DDS ", 4);

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(!image.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::DdsStatus::HEADER_TRUNCATED));
}

CORSAIRS_TEST(Dds_RejectsUnknownFourCc) {
    const auto bytes = MakeDds(4, 4, 0x4, "DXT9", 0, 0, 0, 0, 0,
                               std::vector<std::uint8_t>(16, 0));

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(!image.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::DdsStatus::UNSUPPORTED_FORMAT));
}

CORSAIRS_TEST(Dds_RejectsTruncatedBody) {
    // Объявлено 4x4 в DXT1 — нужен один блок из восьми байт, дано четыре.
    const auto bytes = MakeDds(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0,
                               std::vector<std::uint8_t>(4, 0));

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(!image.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::DdsStatus::DATA_TRUNCATED));
}

CORSAIRS_TEST(Dds_DecodesSolidRedDxt1) {
    // 0xF800 в RGB565 — чистый красный. Разворот обязан дать 255, а не 248:
    // младшие биты достраиваются из старших.
    const auto bytes = MakeDds(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0,
                               SolidBc1Block(0xF800));

    AC::DdsStatus status = AC::DdsStatus::UNSUPPORTED_FORMAT;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(image.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::DdsStatus::OK));
    REQUIRE_EQ(image->Width, 4u);
    REQUIRE_EQ(image->Height, 4u);
    REQUIRE_EQ(image->Pixels.size(), 4u * 4u * 4u);

    for (std::size_t i = 0; i < 16; ++i) {
        REQUIRE_EQ(image->Pixels[i * 4 + 0], 255);
        REQUIRE_EQ(image->Pixels[i * 4 + 1], 0);
        REQUIRE_EQ(image->Pixels[i * 4 + 2], 0);
        REQUIRE_EQ(image->Pixels[i * 4 + 3], 255);
    }
}

CORSAIRS_TEST(Dds_DecodesWhiteToFullRange) {
    const auto bytes = MakeDds(4, 4, 0x4, "DXT1", 0, 0, 0, 0, 0,
                               SolidBc1Block(0xFFFF));

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(image.has_value());
    REQUIRE_EQ(image->Pixels[0], 255);
    REQUIRE_EQ(image->Pixels[1], 255);
    REQUIRE_EQ(image->Pixels[2], 255);
}

CORSAIRS_TEST(Dds_Bc2AlphaNibblesExpandToFullRange) {
    // DXT3: восемь байт альфы, затем цветовой блок. 0x0F в младшей тетраде
    // означает полностью непрозрачный пиксель, 0x00 — полностью прозрачный.
    std::vector<std::uint8_t> body(8, 0);
    body[0] = 0xF0;             // пиксель 0 прозрачен, пиксель 1 непрозрачен
    const auto color = SolidBc1Block(0xFFFF);
    body.insert(body.end(), color.begin(), color.end());

    const auto bytes = MakeDds(4, 4, 0x4, "DXT3", 0, 0, 0, 0, 0, body);

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(image.has_value());
    REQUIRE_EQ(image->Pixels[3], 0);      // альфа пикселя 0
    REQUIRE_EQ(image->Pixels[7], 255);    // альфа пикселя 1
}

CORSAIRS_TEST(Dds_DecodesUncompressedBgra) {
    // Раскладка каналов в DDS задаётся масками, и BGRA встречается чаще RGBA.
    // Пиксель ниже записан как B=0x10, G=0x20, R=0x30, A=0x40.
    std::vector<std::uint8_t> body{0x10, 0x20, 0x30, 0x40};
    const auto bytes = MakeDds(1, 1, 0x40, nullptr, 32,
                               0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, body);

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(image.has_value());
    REQUIRE_EQ(image->Pixels[0], 0x30);   // R
    REQUIRE_EQ(image->Pixels[1], 0x20);   // G
    REQUIRE_EQ(image->Pixels[2], 0x10);   // B
    REQUIRE_EQ(image->Pixels[3], 0x40);   // A
}

CORSAIRS_TEST(Dds_UncompressedWithoutAlphaMaskIsOpaque) {
    // 24-битная раскладка не несёт альфы: пиксель обязан выйти непрозрачным,
    // иначе модель окажется невидимой.
    std::vector<std::uint8_t> body{0x10, 0x20, 0x30};
    const auto bytes = MakeDds(1, 1, 0x40, nullptr, 24,
                               0x00FF0000, 0x0000FF00, 0x000000FF, 0, body);

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(image.has_value());
    REQUIRE_EQ(image->Pixels[3], 255);
}

CORSAIRS_TEST(Dds_HandlesSizeNotMultipleOfFour) {
    // Блок 4x4 при размере 3x2: лишние пиксели обязаны быть отброшены, а не
    // выйти за границы буфера.
    const auto bytes = MakeDds(3, 2, 0x4, "DXT1", 0, 0, 0, 0, 0,
                               SolidBc1Block(0xF800));

    AC::DdsStatus status = AC::DdsStatus::OK;
    const auto image = AC::DecodeDds(bytes, status);

    REQUIRE(image.has_value());
    REQUIRE_EQ(image->Width, 3u);
    REQUIRE_EQ(image->Height, 2u);
    REQUIRE_EQ(image->Pixels.size(), 3u * 2u * 4u);
    REQUIRE_EQ(image->Pixels[0], 255);
}

CORSAIRS_TEST(Dds_DecodesRealFileFromDataset) {
    // Проверка на настоящем файле, чью раскладку никто из нас не сочинял.
    //
    // Синтетические тесты выше строят DDS тем же кодом, что читает декодер, и
    // подтверждают лишь внутреннюю связность: когда смещения полей были
    // ошибочны в обоих местах согласованно, все они проходили, а распаковка на
    // реальных данных не работала ни разу. Такое ловится только эталоном.
    //
    // 010013.dds — 256x256 DXT1 из текстур сцены; размеры прочитаны из
    // заголовка независимо от декодера.
    const auto path = std::filesystem::path{CORSAIRS_REPO_ROOT} /
                      "Client" / "texture" / "scene" / "010013.dds";
    const auto bytes = AC::ReadWholeFile(path);
    REQUIRE(bytes.has_value());

    AC::DdsStatus status = AC::DdsStatus::UNSUPPORTED_FORMAT;
    const auto image = AC::DecodeDds(*bytes, status);

    REQUIRE(image.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::DdsStatus::OK));
    REQUIRE_EQ(image->Width, 256u);
    REQUIRE_EQ(image->Height, 256u);
    REQUIRE_EQ(image->Pixels.size(), 256u * 256u * 4u);

    // Текстура здания не может быть одноцветной: если распаковка отдаёт
    // сплошную заливку, значит индексы блоков разобраны неверно.
    bool varies = false;
    for (std::size_t i = 4; i < image->Pixels.size(); i += 4) {
        if (image->Pixels[i] != image->Pixels[0]) {
            varies = true;
            break;
        }
    }
    REQUIRE(varies);
}

CORSAIRS_TEST(Png_WritesReadableFileWithCorrectSignature) {
    AC::DecodedImage image;
    image.Width = 2;
    image.Height = 2;
    image.Pixels = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 128,
    };

    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "two-by-two.png";
    REQUIRE(AC::WritePng(path, image));

    const auto bytes = AC::ReadWholeFile(path);
    REQUIRE(bytes.has_value());

    // Сигнатура PNG.
    const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    REQUIRE(bytes->size() > 8);
    for (int i = 0; i < 8; ++i) {
        REQUIRE_EQ((*bytes)[i], signature[i]);
    }

    // Первый блок обязан быть IHDR, последний — IEND.
    REQUIRE_EQ((*bytes)[12], 'I');
    REQUIRE_EQ((*bytes)[13], 'H');
    REQUIRE_EQ((*bytes)[14], 'D');
    REQUIRE_EQ((*bytes)[15], 'R');

    const std::size_t size = bytes->size();
    REQUIRE_EQ((*bytes)[size - 8], 'I');
    REQUIRE_EQ((*bytes)[size - 7], 'E');
    REQUIRE_EQ((*bytes)[size - 6], 'N');
    REQUIRE_EQ((*bytes)[size - 5], 'D');
}

CORSAIRS_TEST(Png_RejectsMismatchedPixelBuffer) {
    // Размер буфера не сходится с объявленными размерами — писать нельзя:
    // иначе получится обрезанный файл, который откроется, но покажет мусор.
    AC::DecodedImage image;
    image.Width = 4;
    image.Height = 4;
    image.Pixels.assign(8, 0);

    std::filesystem::create_directories(TempDir());
    REQUIRE(!AC::WritePng(TempDir() / "broken.png", image));
}

CORSAIRS_TEST(Png_CompatibilityAdapterCompressesAndRoundTripsLargeImage) {
    AC::DecodedImage image;
    image.Width = 200;
    image.Height = 200;
    image.Pixels.assign(static_cast<std::size_t>(200) * 200 * 4, 0x7F);

    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "large.png";
    REQUIRE(AC::WritePng(path, image));

    const auto bytes = AC::ReadWholeFile(path);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() < image.Pixels.size() + image.Height);

    std::string detail;
    const auto decoded = AC::DecodeImageFile(path, detail);
    REQUIRE(decoded.has_value());
    REQUIRE_EQ(decoded->Width, image.Width);
    REQUIRE_EQ(decoded->Height, image.Height);
    REQUIRE(decoded->Pixels == image.Pixels);
}

} // namespace
