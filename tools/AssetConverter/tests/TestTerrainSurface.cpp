#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/TerrainSurface.h"

#include "TestHarness.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

AC::MapTile MakeTile(
    std::int8_t height,
    std::int16_t color = 0,
    std::uint8_t island = 0) {
    return AC::MapTile{0u, 0u, color, height, 0, island, {0u, 0u, 0u, 0u}};
}

std::vector<std::uint8_t> MakeTwoSectionMap(
    std::int32_t width,
    std::int32_t height,
    bool secondSectionPresent = true) {
    constexpr std::int32_t sectionWidth = 2;
    constexpr std::int32_t sectionHeight = 2;
    constexpr std::uint32_t prefixBytes =
        sizeof(AC::MapFileHeader) + 2u * sizeof(std::uint32_t);
    constexpr std::uint32_t sectionBytes =
        sectionWidth * sectionHeight * sizeof(AC::MapTile);

    std::vector<std::uint8_t> bytes;
    AppendI32Le(bytes, AC::kMapFlagCurrent);
    AppendI32Le(bytes, width);
    AppendI32Le(bytes, height);
    AppendI32Le(bytes, sectionWidth);
    AppendI32Le(bytes, sectionHeight);
    AppendU32Le(bytes, prefixBytes);
    AppendU32Le(bytes, secondSectionPresent ? prefixBytes + sectionBytes : 0u);

    for (const std::int8_t heightValue : {1, 2, 3, 4}) {
        AppendTile(bytes, MakeTile(heightValue, 0x1111, 1u));
    }
    if (secondSectionPresent) {
        for (const std::int8_t heightValue : {5, 6, 7, 8}) {
            AppendTile(bytes, MakeTile(heightValue, 0x2222, 2u));
        }
    }
    return bytes;
}

class ScopedMapFile {
public:
    ScopedMapFile(std::string_view name, std::span<const std::uint8_t> bytes)
        : _path(std::filesystem::temp_directory_path() /
                ("corsairs-scene-surface-" +
                 std::to_string(std::hash<std::string>{}(CORSAIRS_REPO_ROOT)) +
                 "-" + std::string{name} + ".map")) {
        std::ofstream output{_path, std::ios::binary | std::ios::trunc};
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
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

class MemoryTileSource final : public AC::IMapTileSource {
public:
    MemoryTileSource(std::size_t width, std::size_t height)
        : _width(width),
          _height(height),
          _tiles(width * height) {
    }

    [[nodiscard]] std::size_t GridWidth() const override {
        return _width;
    }

    [[nodiscard]] std::size_t GridHeight() const override {
        return _height;
    }

    AC::TerrainTileRead ReadTile(
        std::int32_t tileX,
        std::int32_t tileY) override {
        ++_readCount;
        if (_failure == std::pair{tileX, tileY}) {
            if (_error.empty()) {
                _error = "внедрённая ошибка чтения";
            }
            return {};
        }
        if (tileX < 0 || tileY < 0 ||
            static_cast<std::size_t>(tileX) >= _width ||
            static_cast<std::size_t>(tileY) >= _height) {
            return {};
        }
        return _tiles[
            static_cast<std::size_t>(tileY) * _width +
            static_cast<std::size_t>(tileX)];
    }

    [[nodiscard]] const std::string& LastError() const override {
        return _error;
    }

    void Set(
        std::size_t x,
        std::size_t y,
        const AC::MapTile& tile,
        bool sectionPresent = true) {
        _tiles[y * _width + x] = AC::TerrainTileRead{tile, sectionPresent};
    }

    void InjectFailure(std::int32_t x, std::int32_t y) {
        _failure = std::pair{x, y};
    }

    [[nodiscard]] std::size_t ReadCount() const noexcept {
        return _readCount;
    }

private:
    std::size_t _width{0};
    std::size_t _height{0};
    std::vector<AC::TerrainTileRead> _tiles;
    std::optional<std::pair<std::int32_t, std::int32_t>> _failure;
    std::string _error;
    std::size_t _readCount{0};
};

void RequireNear(
    bool& corsairsTestOk,
    float actual,
    float expected,
    float tolerance = 0.001f) {
    REQUIRE(std::abs(actual - expected) <= tolerance);
}

MemoryTileSource MakePresentSurface(
    std::int8_t topLeft,
    std::int8_t topRight,
    std::int8_t bottomLeft,
    std::int8_t bottomRight) {
    MemoryTileSource source{2u, 2u};
    source.Set(0u, 0u, MakeTile(topLeft));
    source.Set(1u, 0u, MakeTile(topRight));
    source.Set(0u, 1u, MakeTile(bottomLeft));
    source.Set(1u, 1u, MakeTile(bottomRight));
    return source;
}

CORSAIRS_TEST(TerrainSurfaceHeight_UsesLegacyTriangles) {
    MemoryTileSource source = MakePresentSurface(0, 10, 20, 0);

    const float firstTriangle = AC::TerrainSurfaceHeight(source, 25, 25);
    const float secondTriangle = AC::TerrainSurfaceHeight(source, 75, 75);
    RequireNear(corsairsTestOk, firstTriangle, 75.0f);
    if (!corsairsTestOk) {
        return;
    }
    RequireNear(corsairsTestOk, secondTriangle, 75.0f);
    if (!corsairsTestOk) {
        return;
    }
    REQUIRE(std::abs(firstTriangle - 56.25f) > 0.001f);
    REQUIRE(std::abs(secondTriangle - 56.25f) > 0.001f);
    REQUIRE(source.LastError().empty());
}

CORSAIRS_TEST(TerrainSurfaceHeight_ClampsOnlyTheFinalHeightAtSea) {
    MemoryTileSource belowSea = MakePresentSurface(-10, -10, -10, -10);
    RequireNear(
        corsairsTestOk,
        AC::TerrainSurfaceHeight(belowSea, 25, 25),
        0.0f);
    if (!corsairsTestOk) {
        return;
    }

    // Если зажать отрицательную вершину до интерполяции, получится 100 см.
    MemoryTileSource mixed = MakePresentSurface(-10, 20, 20, 20);
    RequireNear(
        corsairsTestOk,
        AC::TerrainSurfaceHeight(mixed, 25, 25),
        50.0f);
}

CORSAIRS_TEST(TerrainSurfaceHeight_RejectsCoordinatesWithoutFourVertices) {
    MemoryTileSource source = MakePresentSurface(1, 2, 3, 4);

    RequireNear(corsairsTestOk, AC::TerrainSurfaceHeight(source, -1, 0), 0.0f);
    if (!corsairsTestOk) {
        return;
    }
    RequireNear(corsairsTestOk, AC::TerrainSurfaceHeight(source, 0, -1), 0.0f);
    if (!corsairsTestOk) {
        return;
    }
    RequireNear(corsairsTestOk, AC::TerrainSurfaceHeight(source, 100, 0), 0.0f);
    if (!corsairsTestOk) {
        return;
    }
    RequireNear(corsairsTestOk, AC::TerrainSurfaceHeight(source, 0, 100), 0.0f);
    if (!corsairsTestOk) {
        return;
    }
    REQUIRE_EQ(source.ReadCount(), 0u);
    REQUIRE(source.LastError().empty());
}

CORSAIRS_TEST(SampleTerrainAnchor_PreservesFloorMetadataAndSceneZ) {
    MemoryTileSource source = MakePresentSurface(6, 6, 6, 6);
    source.Set(0u, 0u, MakeTile(6, 0x7bef, 2u));

    const AC::TerrainAnchorSample sample =
        AC::SampleTerrainAnchor(source, 25, 25);
    RequireNear(corsairsTestOk, sample.SurfaceHeightCm, 60.0f);
    if (!corsairsTestOk) {
        return;
    }
    REQUIRE_EQ(sample.TileColor565, 0x7befu);
    REQUIRE_EQ(sample.Island, 2u);
    REQUIRE(sample.SectionPresent);

    constexpr float heightOffCm = 0.0f;
    RequireNear(
        corsairsTestOk,
        sample.SurfaceHeightCm + heightOffCm,
        60.0f);
}

CORSAIRS_TEST(SampleTerrainAnchor_UsesZeroDefaultsForAbsentSceneTile) {
    MemoryTileSource source{2u, 2u};
    const AC::MapTile poisonedAbsent = MakeTile(10, -1, 9u);
    source.Set(0u, 0u, poisonedAbsent, false);
    source.Set(1u, 0u, poisonedAbsent, false);
    source.Set(0u, 1u, poisonedAbsent, false);
    source.Set(1u, 1u, poisonedAbsent, false);

    const AC::TerrainAnchorSample sample =
        AC::SampleTerrainAnchor(source, 25, 25);
    RequireNear(corsairsTestOk, sample.SurfaceHeightCm, 0.0f);
    if (!corsairsTestOk) {
        return;
    }
    REQUIRE_EQ(sample.TileColor565, 0u);
    REQUIRE_EQ(sample.Island, 0u);
    REQUIRE(!sample.SectionPresent);
    REQUIRE(source.LastError().empty());
}

CORSAIRS_TEST(TerrainSurfaceHeight_LeavesGenericStickyErrorVisible) {
    MemoryTileSource concrete = MakePresentSurface(1, 1, 1, 1);
    concrete.InjectFailure(1, 0);
    AC::IMapTileSource& source = concrete;

    static_cast<void>(AC::TerrainSurfaceHeight(source, 25, 25));
    REQUIRE_EQ(source.LastError(), std::string{"внедрённая ошибка чтения"});
    static_cast<void>(AC::SampleTerrainAnchor(source, 25, 25));
    REQUIRE_EQ(source.LastError(), std::string{"внедрённая ошибка чтения"});
}

CORSAIRS_TEST(MapSectionTileSource_UsesTruncatedGridAndBoundsDefaults) {
    const std::vector<std::uint8_t> bytes = MakeTwoSectionMap(5, 3, false);
    const ScopedMapFile fixture{"non-divisible", bytes};
    REQUIRE(fixture.Written());

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(fixture.Path(), diagnostics);
    REQUIRE(reader.has_value());
    AC::MapSectionTileSource concrete{*reader};
    AC::IMapTileSource& source = concrete;

    REQUIRE_EQ(source.GridWidth(), 4u);
    REQUIRE_EQ(source.GridHeight(), 2u);
    REQUIRE(!source.ReadTile(-1, 0).SectionPresent);
    REQUIRE(!source.ReadTile(0, -1).SectionPresent);
    REQUIRE(!source.ReadTile(4, 0).SectionPresent);
    REQUIRE(!source.ReadTile(0, 2).SectionPresent);
    REQUIRE_EQ(reader->Stats().BodyBytesRead, 0u);
    REQUIRE(source.LastError().empty());

    const AC::TerrainTileRead absent = source.ReadTile(2, 0);
    REQUIRE(!absent.SectionPresent);
    REQUIRE_EQ(absent.Tile.Color, 0);
    REQUIRE_EQ(absent.Tile.Height, 0);
    REQUIRE_EQ(absent.Tile.Island, 0u);
    REQUIRE(source.LastError().empty());

    RequireNear(corsairsTestOk, AC::TerrainSurfaceHeight(source, 300, 0), 0.0f);
}

CORSAIRS_TEST(MapSectionTileSource_KeepsAtMostOneCachedSection) {
    const std::vector<std::uint8_t> bytes = MakeTwoSectionMap(4, 2);
    const ScopedMapFile fixture{"one-section-cache", bytes};
    REQUIRE(fixture.Written());

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(fixture.Path(), diagnostics);
    REQUIRE(reader.has_value());
    AC::MapSectionTileSource source{*reader};

    REQUIRE(source.ReadTile(0, 0).SectionPresent);
    REQUIRE_EQ(reader->Stats().BodyBytesRead, 4u * sizeof(AC::MapTile));
    const AC::TerrainTileRead firstSectionCorner = source.ReadTile(1, 1);
    REQUIRE(firstSectionCorner.SectionPresent);
    REQUIRE_EQ(firstSectionCorner.Tile.Height, 4);
    REQUIRE_EQ(reader->Stats().BodyBytesRead, 4u * sizeof(AC::MapTile));

    const AC::TerrainTileRead second = source.ReadTile(2, 0);
    REQUIRE(second.SectionPresent);
    REQUIRE_EQ(second.Tile.Height, 5);
    REQUIRE_EQ(reader->Stats().BodyBytesRead, 8u * sizeof(AC::MapTile));

    const AC::TerrainTileRead reloadedFirst = source.ReadTile(0, 1);
    REQUIRE(reloadedFirst.SectionPresent);
    REQUIRE_EQ(reloadedFirst.Tile.Height, 3);
    REQUIRE_EQ(reader->Stats().BodyBytesRead, 12u * sizeof(AC::MapTile));
    REQUIRE(source.LastError().empty());
}

CORSAIRS_TEST(MapSectionTileSource_RecordsFirstStickyIoErrorGenerically) {
    const std::vector<std::uint8_t> bytes = MakeTwoSectionMap(4, 2);
    const ScopedMapFile fixture{"sticky-io-error", bytes};
    REQUIRE(fixture.Written());

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(fixture.Path(), diagnostics);
    REQUIRE(reader.has_value());
    AC::MapSectionTileSource concrete{*reader};
    AC::IMapTileSource& source = concrete;

    REQUIRE(source.ReadTile(0, 0).SectionPresent);
    std::error_code resizeError;
    std::filesystem::resize_file(
        fixture.Path(),
        sizeof(AC::MapFileHeader) + 2u * sizeof(std::uint32_t) +
            4u * sizeof(AC::MapTile),
        resizeError);
    REQUIRE(!resizeError);

    const AC::TerrainTileRead failed = source.ReadTile(2, 0);
    REQUIRE(!failed.SectionPresent);
    REQUIRE(!source.LastError().empty());
    REQUIRE(source.LastError().starts_with("BODY_TRUNCATED: "));
    const std::string firstError = source.LastError();

    // Успешный cached read и безопасный bounds default не очищают ошибку.
    const AC::TerrainTileRead cachedSuccess = source.ReadTile(0, 1);
    REQUIRE(cachedSuccess.SectionPresent);
    REQUIRE_EQ(cachedSuccess.Tile.Height, 3);
    REQUIRE(!source.ReadTile(4, 0).SectionPresent);
    REQUIRE_EQ(source.LastError(), firstError);
}

} // namespace
