#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/ImageCodec.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"
#include "Corsairs/Tools/AssetConverter/StreamingPngWriter.h"

#include "TestHarness.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::array<std::uint8_t, 16> kTwoByTwoPixels{
    255, 0, 0, 255,   0, 255, 0, 255,
    0, 0, 255, 255,   255, 255, 255, 255,
};

std::filesystem::path TempDir() {
    return std::filesystem::temp_directory_path() / "corsairs-streaming-png-tests";
}

void RemoveFixture(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool WriteTwoByTwo(const std::filesystem::path& path, std::string& detail) {
    auto writer = AC::StreamingPngWriter::Open(path, 2, 2, detail);
    if (!writer) {
        return false;
    }
    if (!writer->WriteRgbaRow(std::span{kTwoByTwoPixels}.first<8>(), detail)) {
        return false;
    }
    if (!writer->WriteRgbaRow(std::span{kTwoByTwoPixels}.last<8>(), detail)) {
        return false;
    }
    return writer->Finish(detail);
}

std::uint32_t ReadBigEndian32(std::span<const std::uint8_t> bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

struct PngChunk {
    std::string Type;
    std::size_t PayloadLength{0};
};

std::vector<PngChunk> ParsePngChunks(std::span<const std::uint8_t> bytes) {
    std::vector<PngChunk> chunks;
    std::size_t offset = 8;
    while (offset <= bytes.size() && bytes.size() - offset >= 12) {
        const std::size_t payloadLength = ReadBigEndian32(bytes.subspan(offset, 4));
        if (payloadLength > bytes.size() - offset - 12) {
            return {};
        }
        chunks.push_back(PngChunk{
            std::string{reinterpret_cast<const char*>(bytes.data() + offset + 4), 4},
            payloadLength,
        });
        offset += 12 + payloadLength;
    }
    if (offset != bytes.size()) {
        return {};
    }
    return chunks;
}

CORSAIRS_TEST(StreamingPng_WritesRowsAndSha256MatchesLiteralPixels) {
    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "two-by-two.png";
    RemoveFixture(path);

    std::string detail;
    REQUIRE(WriteTwoByTwo(path, detail));

    const auto decoded = AC::DecodeImageFile(path, detail);
    REQUIRE(decoded.has_value());
    REQUIRE_EQ(decoded->Width, 2u);
    REQUIRE_EQ(decoded->Height, 2u);
    REQUIRE(decoded->Pixels == std::vector<std::uint8_t>(kTwoByTwoPixels.begin(),
                                                         kTwoByTwoPixels.end()));
    REQUIRE_EQ(AC::Sha256Bytes(kTwoByTwoPixels),
               std::string{"c21b35e3f28e676cedf24c13575a7346682e101a2d26aad9598d0cdbcee9ee3b"});
}

CORSAIRS_TEST(Sha256_FileMatchesBytesAndMissingFileExplainsFailure) {
    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "sha-fixture.png";
    RemoveFixture(path);

    std::string detail;
    REQUIRE(WriteTwoByTwo(path, detail));
    const auto bytes = AC::ReadWholeFile(path);
    REQUIRE(bytes.has_value());

    const auto fileHash = AC::Sha256File(path, detail);
    REQUIRE(fileHash.has_value());
    REQUIRE_EQ(*fileHash, AC::Sha256Bytes(*bytes));

    const auto missingPath = TempDir() / "missing.png";
    RemoveFixture(missingPath);
    detail.clear();
    REQUIRE(!AC::Sha256File(missingPath, detail).has_value());
    REQUIRE(!detail.empty());
}

CORSAIRS_TEST(StreamingPng_SameRowsProduceByteIdenticalFiles) {
    std::filesystem::create_directories(TempDir());
    const auto firstPath = TempDir() / "deterministic-a.png";
    const auto secondPath = TempDir() / "deterministic-b.png";
    RemoveFixture(firstPath);
    RemoveFixture(secondPath);

    std::string detail;
    REQUIRE(WriteTwoByTwo(firstPath, detail));
    REQUIRE(WriteTwoByTwo(secondPath, detail));

    const auto first = AC::ReadWholeFile(firstPath);
    const auto second = AC::ReadWholeFile(secondPath);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*first == *second);
}

CORSAIRS_TEST(StreamingPng_WrongRowLengthDoesNotConsumeRow) {
    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "wrong-row.png";
    RemoveFixture(path);

    std::string detail;
    auto writer = AC::StreamingPngWriter::Open(path, 2, 2, detail);
    REQUIRE(writer != nullptr);

    const std::array<std::uint8_t, 7> shortRow{};
    REQUIRE(!writer->WriteRgbaRow(shortRow, detail));
    REQUIRE(writer->WriteRgbaRow(std::span{kTwoByTwoPixels}.first<8>(), detail));
    REQUIRE(writer->WriteRgbaRow(std::span{kTwoByTwoPixels}.last<8>(), detail));
    REQUIRE(writer->Finish(detail));

    const auto decoded = AC::DecodeImageFile(path, detail);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->Pixels == std::vector<std::uint8_t>(kTwoByTwoPixels.begin(),
                                                         kTwoByTwoPixels.end()));
}

CORSAIRS_TEST(StreamingPng_RejectsExtraRowAndRemovesUnfinishedFile) {
    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "extra-row.png";
    RemoveFixture(path);

    std::string detail;
    {
        auto writer = AC::StreamingPngWriter::Open(path, 1, 1, detail);
        REQUIRE(writer != nullptr);
        const std::array<std::uint8_t, 4> row{1, 2, 3, 4};
        REQUIRE(writer->WriteRgbaRow(row, detail));
        REQUIRE(!writer->WriteRgbaRow(row, detail));
    }
    REQUIRE(!std::filesystem::exists(path));
}

CORSAIRS_TEST(StreamingPng_EarlyFinishIsRejectedAndIncompleteFileIsRemoved) {
    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "early-finish.png";
    RemoveFixture(path);

    std::string detail;
    {
        auto writer = AC::StreamingPngWriter::Open(path, 2, 2, detail);
        REQUIRE(writer != nullptr);
        REQUIRE(writer->WriteRgbaRow(std::span{kTwoByTwoPixels}.first<8>(), detail));
        REQUIRE(!writer->Finish(detail));
    }
    REQUIRE(!std::filesystem::exists(path));
}

CORSAIRS_TEST(StreamingPng_DestructorRemovesFileWhenFinishWasNotCalled) {
    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "unfinished.png";
    RemoveFixture(path);

    std::string detail;
    {
        auto writer = AC::StreamingPngWriter::Open(path, 1, 1, detail);
        REQUIRE(writer != nullptr);
        const std::array<std::uint8_t, 4> row{1, 2, 3, 4};
        REQUIRE(writer->WriteRgbaRow(row, detail));
    }
    REQUIRE(!std::filesystem::exists(path));
}

CORSAIRS_TEST(StreamingPng_RejectsZeroDimensionsAndOverflow) {
    std::filesystem::create_directories(TempDir());
    std::string detail;
    REQUIRE(!AC::StreamingPngWriter::Open(TempDir() / "zero-width.png", 0, 1, detail));
    REQUIRE(!detail.empty());
    REQUIRE(!AC::StreamingPngWriter::Open(TempDir() / "zero-height.png", 1, 0, detail));
    REQUIRE(!detail.empty());
    REQUIRE(!AC::StreamingPngWriter::Open(
        TempDir() / "overflow.png",
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(), detail));
    REQUIRE(!detail.empty());
}

CORSAIRS_TEST(StreamingPng_PeakRowBytesIsBoundedAndLowEntropyCompresses) {
    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "low-entropy.png";
    RemoveFixture(path);

    constexpr std::uint32_t width = 4096;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4, 0x7F);
    std::string detail;
    auto writer = AC::StreamingPngWriter::Open(path, width, 1, detail);
    REQUIRE(writer != nullptr);
    REQUIRE(writer->WriteRgbaRow(row, detail));
    REQUIRE_EQ(writer->PeakRgbaRowBytes(), 16384u);
    REQUIRE(writer->Finish(detail));

    const auto bytes = AC::ReadWholeFile(path);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() < row.size() + 1);
}

CORSAIRS_TEST(StreamingPng_BoundsIdatChunksAndRoundTripsPseudoRandomRows) {
    std::filesystem::create_directories(TempDir());
    const auto path = TempDir() / "multi-idat.png";
    RemoveFixture(path);

    constexpr std::uint32_t width = 4096;
    constexpr std::uint32_t height = 8;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> pixels(rowBytes * height);
    std::uint32_t state = 0xC0FFEEu;
    for (std::uint8_t& value : pixels) {
        state = state * 1664525u + 1013904223u;
        value = static_cast<std::uint8_t>(state >> 24);
    }

    std::string detail;
    auto writer = AC::StreamingPngWriter::Open(path, width, height, detail);
    REQUIRE(writer != nullptr);
    for (std::uint32_t y = 0; y < height; ++y) {
        REQUIRE(writer->WriteRgbaRow(
            std::span<const std::uint8_t>{pixels}.subspan(
                static_cast<std::size_t>(y) * rowBytes, rowBytes), detail));
    }
    REQUIRE(writer->Finish(detail));

    const auto bytes = AC::ReadWholeFile(path);
    REQUIRE(bytes.has_value());
    const auto chunks = ParsePngChunks(*bytes);
    REQUIRE(!chunks.empty());

    std::size_t compressedPayloadBytes = 0;
    std::size_t idatCount = 0;
    bool consecutiveIdat = false;
    bool previousWasIdat = false;
    for (const PngChunk& chunk : chunks) {
        const bool isIdat = chunk.Type == "IDAT";
        if (isIdat) {
            REQUIRE(chunk.PayloadLength >= 1);
            REQUIRE(chunk.PayloadLength <= 65536);
            compressedPayloadBytes += chunk.PayloadLength;
            ++idatCount;
            consecutiveIdat = consecutiveIdat || previousWasIdat;
        }
        previousWasIdat = isIdat;
    }
    REQUIRE(compressedPayloadBytes > 65536);
    REQUIRE(idatCount >= 2);
    REQUIRE(consecutiveIdat);

    const auto decoded = AC::DecodeImageFile(path, detail);
    REQUIRE(decoded.has_value());
    REQUIRE_EQ(decoded->Width, width);
    REQUIRE_EQ(decoded->Height, height);
    REQUIRE(decoded->Pixels == pixels);
}

} // namespace
