#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/MapParser.h"
#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/MapWriter.h"
#include "Corsairs/Tools/AssetConverter/StreamingMapWriter.h"

#include "TestHarness.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

void AppendU8(std::vector<std::uint8_t>& bytes, std::uint8_t value) {
    bytes.push_back(value);
}

void AppendU16Le(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void AppendU32Le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24u));
}

void AppendI32Le(std::vector<std::uint8_t>& bytes, std::int32_t value) {
    AppendU32Le(bytes, static_cast<std::uint32_t>(value));
}

void AppendTile(std::vector<std::uint8_t>& bytes, const AC::MapTile& tile) {
    AppendU32Le(bytes, tile.TileInfo);
    AppendU8(bytes, tile.BaseTex);
    AppendU16Le(bytes, static_cast<std::uint16_t>(tile.Color));
    AppendU8(bytes, static_cast<std::uint8_t>(tile.Height));
    AppendU16Le(bytes, static_cast<std::uint16_t>(tile.Region));
    AppendU8(bytes, tile.Island);
    for (const std::uint8_t quarter : tile.Block) {
        AppendU8(bytes, quarter);
    }
}

std::array<AC::MapTile, 4> LiteralTiles() {
    return {{
        {0x01020304u, 5u, 0x1111, 6, 0x1234, 1u,
         {0x05u, 0x45u, 0x85u, 0x00u}},
        {0x11223344u, 6u, 0x2222, -4, -2, 2u,
         {0x05u, 0x45u, 0x05u, 0x00u}},
        {0xaabbccddu, 7u, 0x3333, 2, 0x0201, 3u,
         {0x00u, 0x05u, 0x45u, 0x00u}},
        {0x55667788u, 8u, 0x4444, -9, 404, 4u,
         {0x45u, 0x05u, 0x00u, 0x00u}},
    }};
}

std::vector<std::uint8_t> MakeSparseMap() {
    std::vector<std::uint8_t> bytes;
    AppendI32Le(bytes, AC::kMapFlagCurrent);
    AppendI32Le(bytes, 4);
    AppendI32Le(bytes, 2);
    AppendI32Le(bytes, 2);
    AppendI32Le(bytes, 2);
    AppendU32Le(bytes, 28u);
    AppendU32Le(bytes, 0u);

    for (const AC::MapTile& tile : LiteralTiles()) {
        AppendTile(bytes, tile);
    }
    return bytes;
}

std::vector<std::uint8_t> MakeTwoByTwoSectionMap(bool lateSectionPresent) {
    std::vector<std::uint8_t> bytes;
    AppendI32Le(bytes, AC::kMapFlagCurrent);
    AppendI32Le(bytes, 4);
    AppendI32Le(bytes, 4);
    AppendI32Le(bytes, 2);
    AppendI32Le(bytes, 2);
    AppendU32Le(bytes, 0u);
    AppendU32Le(bytes, 0u);
    AppendU32Le(bytes, 0u);
    AppendU32Le(bytes, lateSectionPresent ? 36u : 0u);

    if (lateSectionPresent) {
        for (const AC::MapTile& tile : LiteralTiles()) {
            AppendTile(bytes, tile);
        }
    }
    return bytes;
}

class ScopedDirectory {
public:
    explicit ScopedDirectory(std::string_view name)
        : _path(std::filesystem::temp_directory_path() /
                ("corsairs-streaming-map-writer-" + std::string{name})) {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
        error.clear();
        _created = std::filesystem::create_directories(_path, error) && !error;
    }

    ~ScopedDirectory() {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }

    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    [[nodiscard]] bool Created() const noexcept {
        return _created;
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return _path;
    }

private:
    std::filesystem::path _path;
    bool _created{false};
};

bool WriteBytes(const std::filesystem::path& path,
                std::span<const std::uint8_t> bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

std::filesystem::path WithSuffix(const std::filesystem::path& base,
                                 std::string_view suffix) {
    std::filesystem::path path = base;
    path += suffix;
    return path;
}

template <typename Source>
concept AcceptsTerrainRasterSource = requires(
    Source& source,
    const std::filesystem::path& path,
    AC::MapRasterStats& stats,
    std::string& detail) {
    AC::WriteTerrainRasters(source, path, stats, detail);
};

static_assert(AcceptsTerrainRasterSource<AC::MapSectionReader>);
static_assert(!AcceptsTerrainRasterSource<AC::MapTerrain>);

void RequireBytesEqual(bool& corsairsTestOk,
                       std::span<const std::uint8_t> actual,
                       std::span<const std::uint8_t> expected) {
    REQUIRE_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE_EQ(actual[index], expected[index]);
    }
}

void RequireAbsentRangeIsZeroInEveryRow(
    bool& corsairsTestOk,
    std::span<const std::uint8_t> bytes,
    std::size_t width,
    std::size_t height,
    std::size_t bytesPerTile,
    std::size_t absentBeginX) {
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = absentBeginX; x < width; ++x) {
            for (std::size_t byte = 0; byte < bytesPerTile; ++byte) {
                const std::size_t index =
                    (y * width + x) * bytesPerTile + byte;
                REQUIRE_EQ(bytes[index], 0u);
            }
        }
    }
}

void RequireStatsEqual(bool& corsairsTestOk,
                       const AC::MapRasterStats& actual,
                       const AC::MapRasterStats& expected) {
    REQUIRE_EQ(actual.PresentSections, expected.PresentSections);
    REQUIRE_EQ(actual.AbsentSections, expected.AbsentSections);
    REQUIRE_EQ(actual.MinHeightRaw, expected.MinHeightRaw);
    REQUIRE_EQ(actual.MaxHeightRaw, expected.MaxHeightRaw);
    REQUIRE_EQ(actual.BlockedTiles, expected.BlockedTiles);
}

CORSAIRS_TEST(StreamingMapWriter_WritesExactSparseRastersAndMetadata) {
    const ScopedDirectory directory{"exact"};
    REQUIRE(directory.Created());
    const std::filesystem::path mapPath = directory.Path() / "sparse.map";
    const std::vector<std::uint8_t> fixture = MakeSparseMap();
    REQUIRE(WriteBytes(mapPath, fixture));

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, diagnostics);
    REQUIRE(reader.has_value());

    AC::MapRasterStats stats{99u, 98u, 97, 96, 95u};
    std::string detail{"stale detail"};
    const std::filesystem::path base = directory.Path() / "first";
    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::WriteTerrainRasters(*reader, base, stats, detail)),
        static_cast<std::uint32_t>(AC::MapWriteStatus::OK));
    REQUIRE(detail.empty());

    const auto heights = AC::ReadWholeFile(WithSuffix(base, ".height.r16"));
    const auto blocks = AC::ReadWholeFile(WithSuffix(base, ".block.raw"));
    const auto regions = AC::ReadWholeFile(WithSuffix(base, ".region.raw"));
    const auto metadata = AC::ReadWholeFile(WithSuffix(base, ".terrain.json"));
    REQUIRE(heights.has_value());
    REQUIRE(blocks.has_value());
    REQUIRE(regions.has_value());
    REQUIRE(metadata.has_value());

    REQUIRE_EQ(heights->size(), 4u * 2u * 2u);
    REQUIRE_EQ(blocks->size(), 4u * 2u * 4u);
    REQUIRE_EQ(regions->size(), 4u * 2u * 2u);

    const std::array<std::uint8_t, 4> firstHeightRow{0x00u, 0x86u,
                                                    0x00u, 0x7cu};
    RequireBytesEqual(corsairsTestOk,
                      std::span<const std::uint8_t>{*heights}.first(4u),
                      firstHeightRow);
    if (!corsairsTestOk) {
        return;
    }
    const std::array<std::uint8_t, 4> firstRegionRow{0x34u, 0x12u,
                                                    0xfeu, 0xffu};
    RequireBytesEqual(corsairsTestOk,
                      std::span<const std::uint8_t>{*regions}.first(4u),
                      firstRegionRow);
    if (!corsairsTestOk) {
        return;
    }
    const std::array<std::uint8_t, 8> firstBlockRow{
        0x05u, 0x45u, 0x85u, 0x00u,
        0x05u, 0x45u, 0x05u, 0x00u,
    };
    RequireBytesEqual(corsairsTestOk,
                      std::span<const std::uint8_t>{*blocks}.first(8u),
                      firstBlockRow);
    if (!corsairsTestOk) {
        return;
    }

    RequireAbsentRangeIsZeroInEveryRow(
        corsairsTestOk, *heights, 4u, 2u, 2u, 2u);
    if (!corsairsTestOk) {
        return;
    }
    RequireAbsentRangeIsZeroInEveryRow(
        corsairsTestOk, *blocks, 4u, 2u, 4u, 2u);
    if (!corsairsTestOk) {
        return;
    }
    RequireAbsentRangeIsZeroInEveryRow(
        corsairsTestOk, *regions, 4u, 2u, 2u, 2u);
    if (!corsairsTestOk) {
        return;
    }

    REQUIRE_EQ(stats.PresentSections, 1u);
    REQUIRE_EQ(stats.AbsentSections, 1u);
    REQUIRE_EQ(stats.MinHeightRaw, -128);
    REQUIRE_EQ(stats.MaxHeightRaw, 6);
    REQUIRE_EQ(stats.BlockedTiles, 1u);

    const std::string expectedMetadata =
        "{\"mapFlag\":780627,\"width\":4,\"height\":2,"
        "\"gridWidth\":4,\"gridHeight\":2,\"sectionWidth\":2,"
        "\"sectionHeight\":2,\"sectionsPresent\":1,"
        "\"sectionsTotal\":2,\"blockedTiles\":1,"
        "\"heightRangeRaw\":[-128,6],\"heightUnitMeters\":0.1,"
        "\"heightEncoding\":\"uint16 = (rawHeight + 128) * 256\"}";
    const std::string actualMetadata{metadata->begin(), metadata->end()};
    REQUIRE_EQ(actualMetadata, expectedMetadata);

    // Читатель видит только секцию 2x2. Потоковая запись дополнительно владеет
    // лишь тремя буферами строки на два тайла, но не строкой всей карты.
    REQUIRE(reader->Stats().PeakResidentTiles <= 4u + 2u);
}

CORSAIRS_TEST(StreamingMapWriter_AllAbsentMapIsFullyZeroWithRawMinus128Stats) {
    const ScopedDirectory directory{"all-absent"};
    REQUIRE(directory.Created());
    const std::filesystem::path mapPath = directory.Path() / "absent.map";
    const std::vector<std::uint8_t> fixture = MakeTwoByTwoSectionMap(false);
    REQUIRE(WriteBytes(mapPath, fixture));

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, diagnostics);
    REQUIRE(reader.has_value());

    AC::MapRasterStats stats{10u, 11u, 12, 13, 14u};
    std::string detail{"stale"};
    const std::filesystem::path base = directory.Path() / "absent";
    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::WriteTerrainRasters(*reader, base, stats, detail)),
        static_cast<std::uint32_t>(AC::MapWriteStatus::OK));
    REQUIRE(detail.empty());

    const auto heights = AC::ReadWholeFile(WithSuffix(base, ".height.r16"));
    const auto blocks = AC::ReadWholeFile(WithSuffix(base, ".block.raw"));
    const auto regions = AC::ReadWholeFile(WithSuffix(base, ".region.raw"));
    const auto metadata = AC::ReadWholeFile(WithSuffix(base, ".terrain.json"));
    REQUIRE(heights.has_value());
    REQUIRE(blocks.has_value());
    REQUIRE(regions.has_value());
    REQUIRE(metadata.has_value());

    const std::array<std::uint8_t, 32> expectedHeights{};
    const std::array<std::uint8_t, 64> expectedBlocks{};
    const std::array<std::uint8_t, 32> expectedRegions{};
    RequireBytesEqual(corsairsTestOk, *heights, expectedHeights);
    if (!corsairsTestOk) {
        return;
    }
    RequireBytesEqual(corsairsTestOk, *blocks, expectedBlocks);
    if (!corsairsTestOk) {
        return;
    }
    RequireBytesEqual(corsairsTestOk, *regions, expectedRegions);
    if (!corsairsTestOk) {
        return;
    }

    REQUIRE_EQ(stats.PresentSections, 0u);
    REQUIRE_EQ(stats.AbsentSections, 4u);
    REQUIRE_EQ(stats.MinHeightRaw, -128);
    REQUIRE_EQ(stats.MaxHeightRaw, -128);
    REQUIRE_EQ(stats.BlockedTiles, 0u);

    const std::string expectedMetadata =
        "{\"mapFlag\":780627,\"width\":4,\"height\":4,"
        "\"gridWidth\":4,\"gridHeight\":4,\"sectionWidth\":2,"
        "\"sectionHeight\":2,\"sectionsPresent\":0,"
        "\"sectionsTotal\":4,\"blockedTiles\":0,"
        "\"heightRangeRaw\":[-128,-128],\"heightUnitMeters\":0.1,"
        "\"heightEncoding\":\"uint16 = (rawHeight + 128) * 256\"}";
    const std::string actualMetadata{metadata->begin(), metadata->end()};
    REQUIRE_EQ(actualMetadata, expectedMetadata);
}

CORSAIRS_TEST(StreamingMapWriter_WritesLateSectionAtExactRowOffsets) {
    const ScopedDirectory directory{"late-section"};
    REQUIRE(directory.Created());
    const std::filesystem::path mapPath = directory.Path() / "late.map";
    const std::vector<std::uint8_t> fixture = MakeTwoByTwoSectionMap(true);
    REQUIRE(WriteBytes(mapPath, fixture));

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, diagnostics);
    REQUIRE(reader.has_value());

    AC::MapRasterStats stats;
    std::string detail;
    const std::filesystem::path base = directory.Path() / "late";
    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::WriteTerrainRasters(*reader, base, stats, detail)),
        static_cast<std::uint32_t>(AC::MapWriteStatus::OK));
    REQUIRE(detail.empty());

    const auto heights = AC::ReadWholeFile(WithSuffix(base, ".height.r16"));
    const auto blocks = AC::ReadWholeFile(WithSuffix(base, ".block.raw"));
    const auto regions = AC::ReadWholeFile(WithSuffix(base, ".region.raw"));
    REQUIRE(heights.has_value());
    REQUIRE(blocks.has_value());
    REQUIRE(regions.has_value());

    const std::array<std::uint8_t, 32> expectedHeights{
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0x00, 0x86, 0x00, 0x7c,
        0, 0, 0, 0, 0x00, 0x82, 0x00, 0x77,
    };
    const std::array<std::uint8_t, 64> expectedBlocks{
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0x05, 0x45, 0x85, 0x00, 0x05, 0x45, 0x05, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0,
        0x00, 0x05, 0x45, 0x00, 0x45, 0x05, 0x00, 0x00,
    };
    const std::array<std::uint8_t, 32> expectedRegions{
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0x34, 0x12, 0xfe, 0xff,
        0, 0, 0, 0, 0x01, 0x02, 0x94, 0x01,
    };
    RequireBytesEqual(corsairsTestOk, *heights, expectedHeights);
    if (!corsairsTestOk) {
        return;
    }
    RequireBytesEqual(corsairsTestOk, *blocks, expectedBlocks);
    if (!corsairsTestOk) {
        return;
    }
    RequireBytesEqual(corsairsTestOk, *regions, expectedRegions);
    if (!corsairsTestOk) {
        return;
    }

    REQUIRE_EQ(stats.PresentSections, 1u);
    REQUIRE_EQ(stats.AbsentSections, 3u);
    REQUIRE_EQ(stats.MinHeightRaw, -128);
    REQUIRE_EQ(stats.MaxHeightRaw, 6);
    REQUIRE_EQ(stats.BlockedTiles, 1u);
}

CORSAIRS_TEST(StreamingMapWriter_RepeatedWriteResetsStatsAndIsDeterministic) {
    const ScopedDirectory directory{"repeat"};
    REQUIRE(directory.Created());
    const std::filesystem::path mapPath = directory.Path() / "sparse.map";
    const std::vector<std::uint8_t> fixture = MakeSparseMap();
    REQUIRE(WriteBytes(mapPath, fixture));

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, diagnostics);
    REQUIRE(reader.has_value());

    AC::MapRasterStats firstStats{91u, 92u, 93, 94, 95u};
    std::string firstDetail{"old"};
    const std::filesystem::path firstBase = directory.Path() / "first";
    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::WriteTerrainRasters(
                *reader, firstBase, firstStats, firstDetail)),
        static_cast<std::uint32_t>(AC::MapWriteStatus::OK));
    REQUIRE(firstDetail.empty());

    AC::MapRasterStats secondStats{81u, 82u, 83, 84, 85u};
    std::string secondDetail{"old"};
    const std::filesystem::path secondBase = directory.Path() / "second";
    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::WriteTerrainRasters(
                *reader, secondBase, secondStats, secondDetail)),
        static_cast<std::uint32_t>(AC::MapWriteStatus::OK));
    REQUIRE(secondDetail.empty());
    RequireStatsEqual(corsairsTestOk, secondStats, firstStats);
    if (!corsairsTestOk) {
        return;
    }

    for (const std::string_view suffix : {
             ".height.r16", ".block.raw", ".region.raw", ".terrain.json"}) {
        const auto first = AC::ReadWholeFile(WithSuffix(firstBase, suffix));
        const auto second = AC::ReadWholeFile(WithSuffix(secondBase, suffix));
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        RequireBytesEqual(corsairsTestOk, *second, *first);
        if (!corsairsTestOk) {
            return;
        }
    }
}

CORSAIRS_TEST(StreamingMapWriter_ReportsOutputOpenFailure) {
    const ScopedDirectory directory{"open-failure"};
    REQUIRE(directory.Created());
    const std::filesystem::path mapPath = directory.Path() / "sparse.map";
    const std::vector<std::uint8_t> fixture = MakeSparseMap();
    REQUIRE(WriteBytes(mapPath, fixture));

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, diagnostics);
    REQUIRE(reader.has_value());

    AC::MapRasterStats stats{1u, 2u, 3, 4, 5u};
    std::string detail;
    const std::filesystem::path base =
        directory.Path() / "missing-parent" / "terrain";
    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::WriteTerrainRasters(*reader, base, stats, detail)),
        static_cast<std::uint32_t>(AC::MapWriteStatus::WRITE_FAILED));
    REQUIRE(!detail.empty());
    const AC::MapRasterStats reset{};
    RequireStatsEqual(corsairsTestOk, stats, reset);
}

CORSAIRS_TEST(StreamingMapWriter_ReportsSectionReadFailure) {
    const ScopedDirectory directory{"read-failure"};
    REQUIRE(directory.Created());
    const std::filesystem::path mapPath = directory.Path() / "sparse.map";
    const std::vector<std::uint8_t> fixture = MakeSparseMap();
    REQUIRE(WriteBytes(mapPath, fixture));

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, diagnostics);
    REQUIRE(reader.has_value());
    std::error_code error;
    std::filesystem::resize_file(mapPath, 28u, error);
    REQUIRE(!error);

    AC::MapRasterStats stats;
    std::string detail;
    const std::filesystem::path base = directory.Path() / "failed";
    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::WriteTerrainRasters(*reader, base, stats, detail)),
        static_cast<std::uint32_t>(AC::MapWriteStatus::WRITE_FAILED));
    REQUIRE(!detail.empty());
}

} // namespace
