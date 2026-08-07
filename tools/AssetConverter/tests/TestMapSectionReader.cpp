#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"

#include "TestHarness.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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
    for (const std::uint8_t block : tile.Block) {
        AppendU8(bytes, block);
    }
}

std::vector<std::uint8_t> MakeSparseMap(
    std::uint32_t firstOffset,
    std::uint32_t secondOffset,
    bool includePresentSection) {
    std::vector<std::uint8_t> bytes;
    AppendI32Le(bytes, AC::kMapFlagCurrent);
    AppendI32Le(bytes, 4);
    AppendI32Le(bytes, 2);
    AppendI32Le(bytes, 2);
    AppendI32Le(bytes, 2);
    AppendU32Le(bytes, firstOffset);
    AppendU32Le(bytes, secondOffset);

    if (includePresentSection) {
        const std::array<AC::MapTile, 4> tiles{{
            {0x01020304u, 5u, 0x1111, -4, 101, 1u, {1u, 2u, 3u, 4u}},
            {0x11223344u, 6u, 0x2222, 7, 202, 2u, {5u, 6u, 7u, 8u}},
            {0xaabbccddu, 7u, 0x3333, 12, 303, 3u, {9u, 10u, 11u, 12u}},
            {0x55667788u, 8u, 0x4444, -9, 404, 4u, {13u, 14u, 15u, 16u}},
        }};
        for (const AC::MapTile& tile : tiles) {
            AppendTile(bytes, tile);
        }
    }
    return bytes;
}

std::vector<std::uint8_t> MakeSingleSectionMap(
    std::int32_t sectionWidth,
    std::int32_t sectionHeight,
    std::uint32_t offset) {
    std::vector<std::uint8_t> bytes;
    AppendI32Le(bytes, AC::kMapFlagCurrent);
    AppendI32Le(bytes, sectionWidth);
    AppendI32Le(bytes, sectionHeight);
    AppendI32Le(bytes, sectionWidth);
    AppendI32Le(bytes, sectionHeight);
    AppendU32Le(bytes, offset);
    return bytes;
}

AC::MapTile MakeTaggedTile(std::uint8_t tag) {
    return AC::MapTile{
        0x70000000u | tag,
        tag,
        static_cast<std::int16_t>(0x1000u + tag),
        static_cast<std::int8_t>(tag),
        static_cast<std::int16_t>(100u + tag),
        tag,
        {tag, tag, tag, tag},
    };
}

std::vector<std::uint8_t> MakeCrossBoundaryMap() {
    std::vector<std::uint8_t> bytes;
    AppendI32Le(bytes, AC::kMapFlagCurrent);
    AppendI32Le(bytes, 4);
    AppendI32Le(bytes, 4);
    AppendI32Le(bytes, 2);
    AppendI32Le(bytes, 2);

    // Row-major присутствие секций: [есть, нет, есть, есть].
    AppendU32Le(bytes, 36u);
    AppendU32Le(bytes, 0u);
    AppendU32Le(bytes, 96u);
    AppendU32Le(bytes, 156u);

    for (const std::uint8_t tag : {11u, 12u, 13u, 14u}) {
        AppendTile(bytes, MakeTaggedTile(tag));
    }
    for (const std::uint8_t tag : {21u, 22u, 23u, 24u}) {
        AppendTile(bytes, MakeTaggedTile(tag));
    }
    for (const std::uint8_t tag : {31u, 32u, 33u, 34u}) {
        AppendTile(bytes, MakeTaggedTile(tag));
    }
    return bytes;
}

class ScopedMapFile {
public:
    ScopedMapFile(std::string_view name, std::span<const std::uint8_t> bytes)
        : _path(std::filesystem::temp_directory_path() /
                ("corsairs-map-section-" + std::string{name} + ".map")) {
        std::ofstream output{_path, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        _written = output.good();
    }

    ~ScopedMapFile() {
        std::error_code error;
        std::filesystem::remove(_path, error);
    }

    ScopedMapFile(const ScopedMapFile&) = delete;
    ScopedMapFile& operator=(const ScopedMapFile&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return _path;
    }

    [[nodiscard]] bool Written() const noexcept {
        return _written;
    }

private:
    std::filesystem::path _path;
    bool _written{false};
};

std::filesystem::path GarnerMapPath() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "map" /
           "garner.map";
}

std::optional<std::uint32_t> ReadU32LeAt(
    const std::filesystem::path& path,
    std::uint64_t offset) {
    std::ifstream input{path, std::ios::binary};
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<std::uint8_t, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

void RequireTileEquals(
    bool& corsairsTestOk,
    const AC::MapTile& actual,
    const AC::MapTile& expected) {
    REQUIRE_EQ(actual.TileInfo, expected.TileInfo);
    REQUIRE_EQ(actual.BaseTex, expected.BaseTex);
    REQUIRE_EQ(actual.Color, expected.Color);
    REQUIRE_EQ(actual.Height, expected.Height);
    REQUIRE_EQ(actual.Region, expected.Region);
    REQUIRE_EQ(actual.Island, expected.Island);
    for (std::size_t index = 0; index < 4; ++index) {
        REQUIRE_EQ(actual.Block[index], expected.Block[index]);
    }
}

CORSAIRS_TEST(MapSectionReader_ReadsPresentAndAbsentSections) {
    const std::vector<std::uint8_t> bytes = MakeSparseMap(28u, 0u, true);
    const ScopedMapFile fixture{"sparse", bytes};
    REQUIRE(fixture.Written());

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(fixture.Path(), diagnostics);
    REQUIRE(reader.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
               static_cast<std::uint32_t>(AC::MapStatus::OK));

    const auto present = reader->ReadSection(0u, 0u, diagnostics);
    REQUIRE(present.has_value());
    REQUIRE(present->Present);
    REQUIRE_EQ(present->Tiles.size(), 4u);
    const std::array<AC::MapTile, 4> expected{{
        {0x01020304u, 5u, 0x1111, -4, 101, 1u, {1u, 2u, 3u, 4u}},
        {0x11223344u, 6u, 0x2222, 7, 202, 2u, {5u, 6u, 7u, 8u}},
        {0xaabbccddu, 7u, 0x3333, 12, 303, 3u, {9u, 10u, 11u, 12u}},
        {0x55667788u, 8u, 0x4444, -9, 404, 4u, {13u, 14u, 15u, 16u}},
    }};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        RequireTileEquals(corsairsTestOk, present->Tiles[index], expected[index]);
        if (!corsairsTestOk) {
            return;
        }
    }

    const auto absent = reader->ReadSection(1u, 0u, diagnostics);
    REQUIRE(absent.has_value());
    REQUIRE(!absent->Present);
    REQUIRE(absent->Tiles.empty());
    REQUIRE_EQ(reader->Stats().MetadataBytesRead, 28u);
    REQUIRE_EQ(reader->Stats().LargestMetadataRead, 20u);
    REQUIRE_EQ(reader->Stats().BodyBytesRead, 4u * sizeof(AC::MapTile));
    REQUIRE_EQ(reader->Stats().LargestBodyRead, 4u * sizeof(AC::MapTile));
}

CORSAIRS_TEST(MapSectionReader_RejectsAnyNonZeroOffsetOutsideFile) {
    const std::vector<std::uint8_t> bytes = MakeSparseMap(28u, 1024u, true);
    const ScopedMapFile fixture{"corrupt-offset", bytes};
    REQUIRE(fixture.Written());

    AC::MapDiagnostics diagnostics;
    const auto reader = AC::MapSectionReader::Open(fixture.Path(), diagnostics);
    REQUIRE(!reader.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
               static_cast<std::uint32_t>(AC::MapStatus::BODY_TRUNCATED));
}

CORSAIRS_TEST(MapSectionReader_RejectsOffsetWhenSectionEndOverflows) {
    // tilesPerSection=1,229,782,938,000,000,000, поэтому sectionBytes равен
    // UINT64_MAX-3,709,551,615. В непроверенной uint64-арифметике прибавление
    // literal offset оборачивается в ноль и делало 24-байтный файл валидным.
    const std::vector<std::uint8_t> bytes = MakeSingleSectionMap(
        1'000'000'000,
        1'229'782'938,
        3'709'551'616u);
    const ScopedMapFile fixture{"overflow-offset", bytes};
    REQUIRE(fixture.Written());

    AC::MapDiagnostics diagnostics;
    const auto reader = AC::MapSectionReader::Open(fixture.Path(), diagnostics);
    REQUIRE(!reader.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
               static_cast<std::uint32_t>(AC::MapStatus::BODY_TRUNCATED));
}

CORSAIRS_TEST(MapSectionReader_ReadsSparseWindowAcrossSectionBoundaries) {
    const std::vector<std::uint8_t> bytes = MakeCrossBoundaryMap();
    const ScopedMapFile fixture{"cross-boundary", bytes};
    REQUIRE(fixture.Written());

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(fixture.Path(), diagnostics);
    REQUIRE(reader.has_value());

    const auto page = reader->ReadWindow(
        AC::MapCellRect{1u, 1u, 2u, 2u},
        1u,
        1u,
        diagnostics);
    REQUIRE(page.has_value());
    REQUIRE_EQ(page->StoredWidth, 3u);
    REQUIRE_EQ(page->StoredHeight, 3u);
    REQUIRE_EQ(page->Tiles.size(), 9u);

    const std::array<std::uint8_t, 4> expectedSections{1u, 0u, 1u, 1u};
    REQUIRE_EQ(page->SectionPresent.size(), expectedSections.size());
    for (std::size_t index = 0; index < expectedSections.size(); ++index) {
        REQUIRE_EQ(page->SectionPresent[index], expectedSections[index]);
    }

    const std::array<std::uint8_t, 9> expectedTiles{
        1u, 0u, 0u,
        1u, 1u, 1u,
        1u, 1u, 1u,
    };
    REQUIRE_EQ(page->TilePresent.size(), expectedTiles.size());
    for (std::size_t index = 0; index < expectedTiles.size(); ++index) {
        REQUIRE_EQ(page->TilePresent[index], expectedTiles[index]);
    }

    const AC::MapTile zero{};
    RequireTileEquals(corsairsTestOk, page->Tiles[1u], zero);
    if (!corsairsTestOk) {
        return;
    }
    RequireTileEquals(corsairsTestOk, page->Tiles[2u], zero);
    if (!corsairsTestOk) {
        return;
    }

    // Значения основного окна, затем literal-значения правого и нижнего halo.
    REQUIRE_EQ(page->Tiles[0u].BaseTex, 14u);
    REQUIRE_EQ(page->Tiles[3u].BaseTex, 22u);
    REQUIRE_EQ(page->Tiles[4u].BaseTex, 31u);
    REQUIRE_EQ(page->Tiles[2u].BaseTex, 0u);
    REQUIRE_EQ(page->Tiles[5u].BaseTex, 32u);
    REQUIRE_EQ(page->Tiles[8u].BaseTex, 34u);
    REQUIRE_EQ(page->Tiles[6u].BaseTex, 24u);
    REQUIRE_EQ(page->Tiles[7u].BaseTex, 33u);

    REQUIRE_EQ(reader->Stats().BodyBytesRead, 180u);
    REQUIRE_EQ(reader->Stats().LargestBodyRead, 60u);
    REQUIRE_EQ(reader->Stats().PeakResidentTiles, 9u + 4u);
}

CORSAIRS_TEST(MapSectionReader_ReadsGarnerGoldenCell) {
    const std::filesystem::path path = GarnerMapPath();
    constexpr std::uint32_t sectionX = 279u;
    constexpr std::uint32_t sectionY = 348u;
    constexpr std::uint64_t offsetTableByte = 713840u;
    constexpr std::uint32_t sectionOffset = 35564436u;
    constexpr std::uint64_t tileByteOffset = 35564451u;

    const auto rawOffset = ReadU32LeAt(path, offsetTableByte);
    REQUIRE(rawOffset.has_value());
    REQUIRE_EQ(*rawOffset, sectionOffset);
    REQUIRE_EQ(static_cast<std::uint64_t>(sectionOffset) + sizeof(AC::MapTile),
               tileByteOffset);

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(path, diagnostics);
    REQUIRE(reader.has_value());
    const auto section = reader->ReadSection(sectionX, sectionY, diagnostics);
    REQUIRE(section.has_value());
    REQUIRE(section->Present);
    REQUIRE_EQ(section->Tiles.size(), 64u);

    const AC::MapTile& tile = section->Tiles[1u];
    REQUIRE_EQ(tile.BaseTex, 4u);
    REQUIRE_EQ(tile.TileInfo, 0x02cf2000u);
    REQUIRE_EQ(static_cast<std::uint16_t>(tile.Color), 0xffffu);
    REQUIRE_EQ(tile.Height, 6);
}

CORSAIRS_TEST(MapSectionReader_ReadsBoundedPageAndHalo) {
    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(GarnerMapPath(), diagnostics);
    REQUIRE(reader.has_value());

    const AC::MapCellRect cells{2176u, 2688u, 128u, 128u};
    const auto page = reader->ReadWindow(cells, 1u, 1u, diagnostics);
    REQUIRE(page.has_value());
    REQUIRE_EQ(page->Cells.X, 2176u);
    REQUIRE_EQ(page->Cells.Y, 2688u);
    REQUIRE_EQ(page->Cells.Width, 128u);
    REQUIRE_EQ(page->Cells.Height, 128u);
    REQUIRE_EQ(page->StoredWidth, 129u);
    REQUIRE_EQ(page->StoredHeight, 129u);
    REQUIRE_EQ(page->Tiles.size(), 16641u);
    REQUIRE_EQ(page->TilePresent.size(), 16641u);
    REQUIRE_EQ(page->SectionPresent.size(), 17u * 17u);

    const std::size_t goldenIndex = 96u * 129u + 57u;
    REQUIRE_EQ(page->Tiles[goldenIndex].BaseTex, 4u);
    REQUIRE_EQ(page->Tiles[goldenIndex].TileInfo, 0x02cf2000u);
    REQUIRE_EQ(page->TilePresent[goldenIndex], 1u);
    REQUIRE_EQ(reader->Stats().MetadataBytesRead, 1048596u);
    REQUIRE_EQ(reader->Stats().LargestMetadataRead, 1048576u);
    REQUIRE_EQ(reader->Stats().LargestBodyRead, 960u);
    REQUIRE_EQ(reader->Stats().PeakResidentTiles, 16641u + 64u);
}

} // namespace
