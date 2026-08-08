#include "Corsairs/Tools/AssetConverter/ImageCodec.h"
#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/ProcessMetrics.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"
#include "Corsairs/Tools/AssetConverter/TerrainCatalog.h"
#include "Corsairs/Tools/AssetConverter/TerrainLayers.h"
#include "Corsairs/Tools/AssetConverter/TerrainPageBaker.h"

#include "TestHarness.h"

#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::int16_t ColorWord(std::uint16_t value) {
    return std::bit_cast<std::int16_t>(value);
}

AC::MapTile Tile(std::uint8_t baseTexture,
                 std::uint16_t color = 0xffffu,
                 std::uint32_t tileInfo = 0u) {
    AC::MapTile tile{};
    tile.BaseTex = baseTexture;
    tile.Color = ColorWord(color);
    tile.TileInfo = tileInfo;
    return tile;
}

std::uint32_t FirstUpper(std::uint8_t textureId, std::uint8_t alphaMask) {
    return (static_cast<std::uint32_t>(textureId) << 26u) |
           (static_cast<std::uint32_t>(alphaMask) << 22u);
}

AC::DecodedImage SolidImage(std::uint8_t red,
                            std::uint8_t green,
                            std::uint8_t blue,
                            std::uint8_t alpha = 255u,
                            std::uint32_t width = 4u,
                            std::uint32_t height = 4u) {
    AC::DecodedImage image;
    image.Width = width;
    image.Height = height;
    image.Pixels.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::size_t offset = 0; offset < image.Pixels.size(); offset += 4u) {
        image.Pixels[offset] = red;
        image.Pixels[offset + 1u] = green;
        image.Pixels[offset + 2u] = blue;
        image.Pixels[offset + 3u] = alpha;
    }
    return image;
}

AC::DecodedImage LiteralTerrainTexture() {
    AC::DecodedImage image = SolidImage(0u, 0u, 0u);
    const std::array<std::array<std::uint8_t, 4>, 4> colors{{
        {255u, 0u, 0u, 255u},
        {0u, 255u, 0u, 255u},
        {0u, 0u, 255u, 255u},
        {255u, 255u, 255u, 255u},
    }};
    for (std::uint32_t y = 0; y < 2u; ++y) {
        for (std::uint32_t x = 0; x < 2u; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * image.Width + x;
            std::copy(colors[y * 2u + x].begin(), colors[y * 2u + x].end(),
                      image.Pixels.begin() + static_cast<std::ptrdiff_t>(pixel * 4u));
        }
    }
    return image;
}

AC::DecodedImage AsymmetricTexture(bool upper) {
    AC::DecodedImage image = SolidImage(0u, 0u, 0u);
    for (std::uint32_t y = 0; y < 4u; ++y) {
        for (std::uint32_t x = 0; x < 4u; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * image.Width + x) * 4u;
            if (!upper) {
                image.Pixels[offset] = static_cast<std::uint8_t>(10u + 30u * x + 50u * y);
                image.Pixels[offset + 1u] = static_cast<std::uint8_t>(20u + 20u * x + 40u * y);
                image.Pixels[offset + 2u] = static_cast<std::uint8_t>(30u + 10u * x + 30u * y);
            }
            else {
                image.Pixels[offset] = static_cast<std::uint8_t>(200u - 20u * x - 30u * y);
                image.Pixels[offset + 1u] = static_cast<std::uint8_t>(5u + 35u * x + 15u * y);
                image.Pixels[offset + 2u] = static_cast<std::uint8_t>(40u + 25u * x + 20u * y);
            }
            image.Pixels[offset + 3u] = 255u;
        }
    }
    return image;
}

AC::DecodedImage AsymmetricAtlas() {
    AC::DecodedImage image = SolidImage(0u, 0u, 0u, 0u, 8u, 8u);
    for (std::uint32_t y = 0; y < image.Height; ++y) {
        for (std::uint32_t x = 0; x < image.Width; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * image.Width + x) * 4u;
            image.Pixels[offset + 3u] =
                static_cast<std::uint8_t>(5u + 10u * x + 15u * y);
        }
    }
    return image;
}

class TerrainFixture {
public:
    explicit TerrainFixture(std::string_view name)
        : _root(std::filesystem::temp_directory_path() /
                ("corsairs-terrain-baker-" + std::string{name})),
          _clientRoot(_root / "Client"),
          _databasePath(_root / "gamedata.sqlite"),
          _alphaPath(_clientRoot / "texture" / "terrain" / "alpha" / "total.png") {
        std::error_code error;
        std::filesystem::remove_all(_root, error);
        std::filesystem::create_directories(_alphaPath.parent_path(), error);
        _ready = !error && sqlite3_open(_databasePath.string().c_str(), &_database) == SQLITE_OK;
        if (_ready) {
            _ready = sqlite3_exec(_database,
                                  "CREATE TABLE terrains(id INTEGER, name TEXT)",
                                  nullptr, nullptr, nullptr) == SQLITE_OK;
        }
    }

    ~TerrainFixture() {
        CloseDatabase();
        std::error_code error;
        std::filesystem::remove_all(_root, error);
    }

    TerrainFixture(const TerrainFixture&) = delete;
    TerrainFixture& operator=(const TerrainFixture&) = delete;

    [[nodiscard]] bool Ready() const noexcept {
        return _ready;
    }

    [[nodiscard]] const std::filesystem::path& Root() const noexcept {
        return _root;
    }

    [[nodiscard]] const std::filesystem::path& AlphaPath() const noexcept {
        return _alphaPath;
    }

    bool WriteAlpha(const AC::DecodedImage& image) {
        return AC::WritePng(_alphaPath, image);
    }

    bool AddTexture(std::uint8_t id,
                    std::string_view stem,
                    const AC::DecodedImage& image) {
        const std::filesystem::path pngRelative =
            std::filesystem::path{"texture"} / "terrain" /
            (std::string{stem} + ".png");
        const std::filesystem::path pngPath = _clientRoot / pngRelative;
        std::error_code error;
        std::filesystem::create_directories(pngPath.parent_path(), error);
        if (error || !AC::WritePng(pngPath, image)) {
            return false;
        }

        const std::string source =
            (std::filesystem::path{"texture"} / "terrain" /
             (std::string{stem} + ".bmp")).generic_string();
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(_database,
                              "INSERT INTO terrains(id,name) VALUES(?,?)",
                              -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_int(statement, 1, id);
        sqlite3_bind_text(statement, 2, source.c_str(),
                          static_cast<int>(source.size()), SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return ok;
    }

    std::optional<AC::TerrainCatalog> LoadCatalog(std::string& detail) {
        CloseDatabase();
        return AC::TerrainCatalog::Load(_databasePath, _clientRoot, detail);
    }

    bool WriteMap(std::string_view name,
                  std::int32_t width,
                  std::int32_t height,
                  const std::vector<std::optional<AC::MapTile>>& cells,
                  std::span<const std::uint8_t> trailingBytes = {}) {
        if (width <= 0 || height <= 0 ||
            cells.size() != static_cast<std::size_t>(width) * height) {
            return false;
        }
        const std::filesystem::path path = MapPath(name);
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        if (!output) {
            return false;
        }

        const AC::MapFileHeader header{
            AC::kMapFlagCurrent, width, height, 1, 1,
        };
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        std::vector<std::uint32_t> offsets(cells.size());
        std::uint32_t nextOffset = static_cast<std::uint32_t>(
            sizeof(header) + offsets.size() * sizeof(std::uint32_t));
        for (std::size_t index = 0; index < cells.size(); ++index) {
            if (cells[index].has_value()) {
                offsets[index] = nextOffset;
                nextOffset += sizeof(AC::MapTile);
            }
        }
        output.write(reinterpret_cast<const char*>(offsets.data()),
                     static_cast<std::streamsize>(offsets.size() * sizeof(std::uint32_t)));
        for (const auto& cell : cells) {
            if (cell.has_value()) {
                output.write(reinterpret_cast<const char*>(&*cell), sizeof(AC::MapTile));
            }
        }
        if (!trailingBytes.empty()) {
            output.write(reinterpret_cast<const char*>(trailingBytes.data()),
                         static_cast<std::streamsize>(trailingBytes.size()));
        }
        return output.good();
    }

    [[nodiscard]] std::filesystem::path MapPath(std::string_view name) const {
        return _root / (std::string{name} + ".map");
    }

    [[nodiscard]] std::filesystem::path Output(std::string_view name) const {
        return _root / ("output-" + std::string{name});
    }

private:
    void CloseDatabase() {
        if (_database != nullptr) {
            sqlite3_close(_database);
            _database = nullptr;
        }
    }

    std::filesystem::path _root;
    std::filesystem::path _clientRoot;
    std::filesystem::path _databasePath;
    std::filesystem::path _alphaPath;
    sqlite3* _database{nullptr};
    bool _ready{false};
};

std::optional<AC::MapSectionReader> OpenReader(
    const std::filesystem::path& path, std::string& detail) {
    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(path, diagnostics);
    detail = diagnostics.Detail;
    return reader;
}

AC::TerrainBakeOptions Options(std::uint32_t cellsPerPage,
                               std::uint32_t pixelsPerCell) {
    AC::TerrainBakeOptions options;
    options.CellsPerPage = cellsPerPage;
    options.PixelsPerCell = pixelsPerCell;
    options.MaxRssBytes = std::numeric_limits<std::size_t>::max();
    return options;
}

std::optional<AC::DecodedImage> DecodeResult(
    const AC::TerrainBakeResult& result, std::string& detail) {
    if (!result.Ok) {
        return std::nullopt;
    }
    return AC::DecodeImageFile(result.PngPath, detail);
}

bool IsLowerHexSha(std::string_view hash) {
    return hash.size() == 64u &&
           std::all_of(hash.begin(), hash.end(), [](unsigned char value) {
               return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f');
           });
}

bool IsWindowsSymlinkPermissionError(const std::error_code& error) {
#if defined(_WIN32)
    return error == std::errc::permission_denied ||
           error.value() == ERROR_ACCESS_DENIED ||
           error.value() == ERROR_PRIVILEGE_NOT_HELD;
#else
    static_cast<void>(error);
    return false;
#endif
}

void ReportWindowsSymlinkSkip(std::string_view testName,
                              const std::error_code& error) {
    std::cout << std::format(
        "        SKIP {}: Windows symlink privilege/access denied: {}\n",
        testName, error.message());
}

std::filesystem::file_status CleanupSymlinkStatus(
    const std::filesystem::path& path, std::error_code& error) {
    std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found &&
        error == std::errc::no_such_file_or_directory) {
        error.clear();
    }
    return status;
}

class ScopedOwnedTree {
public:
    explicit ScopedOwnedTree(std::filesystem::path path)
        : _path(std::move(path)) {
    }

    ~ScopedOwnedTree() {
        if (_owned) {
            std::error_code ignored;
            std::filesystem::remove_all(_path, ignored);
        }
    }

    ScopedOwnedTree(const ScopedOwnedTree&) = delete;
    ScopedOwnedTree& operator=(const ScopedOwnedTree&) = delete;

    void MarkOwned() noexcept {
        _owned = true;
    }

    [[nodiscard]] bool CleanupChecked(std::string& detail) {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
        if (error) {
            detail = error.message();
            return false;
        }
        error.clear();
        const std::filesystem::file_status cleanupStatus =
            CleanupSymlinkStatus(_path, error);
        if (error ||
            cleanupStatus.type() != std::filesystem::file_type::not_found) {
            detail = error ? error.message() : "test-owned tree retained";
            return false;
        }
        _owned = false;
        detail.clear();
        return true;
    }

private:
    std::filesystem::path _path;
    bool _owned{false};
};

std::optional<std::filesystem::path> CreateUniqueTestDirectory(
    std::string_view label, std::string& detail) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::uint32_t attempt = 0; attempt < 32u; ++attempt) {
        const std::filesystem::path candidate = std::filesystem::temp_directory_path() /
            std::format("corsairs-{}-{}-{}", label, stamp, attempt);
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            detail.clear();
            return candidate;
        }
        if (error != std::errc::file_exists) {
            detail = error.message();
            return std::nullopt;
        }
    }
    detail = "не удалось создать unique test directory";
    return std::nullopt;
}

std::string ShellQuote(const std::filesystem::path& path) {
    const std::string value = path.string();
#if defined(_WIN32)
    std::string quoted{"\""};
    for (const char character : value) {
        if (character == '"') {
            quoted += "\\\"";
        }
        else {
            quoted += character;
        }
    }
    quoted += '"';
    return quoted;
#else
    std::string quoted{"'"};
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        }
        else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
#endif
}

int RunBudgetProbe(const std::filesystem::path& repoRoot,
                   const std::filesystem::path& logPath) {
#if defined(_WIN32)
    const std::string command = ShellQuote(TERRAIN_PAGE_BUDGET_PROBE_PATH) +
        " --repo-root " + ShellQuote(repoRoot) + " > " + ShellQuote(logPath) +
        " 2>&1";
#else
    const std::string command = "nice -n 10 " +
        ShellQuote(TERRAIN_PAGE_BUDGET_PROBE_PATH) + " --repo-root " +
        ShellQuote(repoRoot) + " > " + ShellQuote(logPath) + " 2>&1";
#endif
    return std::system(command.c_str());
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

std::set<std::filesystem::path> ProbePrivateDirectories() {
    std::set<std::filesystem::path> paths;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{
             std::filesystem::temp_directory_path(), error};
         !error && iterator != std::filesystem::directory_iterator{};
         iterator.increment(error)) {
        const std::string leaf = iterator->path().filename().string();
        if (leaf.starts_with("corsairs-terrain-page-budget-probe-")) {
            paths.insert(iterator->path());
        }
    }
    return paths;
}

std::optional<std::uint32_t> ReadU32LeAt(
    const std::filesystem::path& path, std::uint64_t offset) {
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

std::vector<std::uint8_t> ExpectedUsedIds(const AC::MapPageTiles& page) {
    std::array<bool, 256> used{};
    for (std::uint32_t y = 0; y < page.Cells.Height; ++y) {
        for (std::uint32_t x = 0; x < page.Cells.Width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * page.StoredWidth + x;
            if (page.TilePresent[index] == 0u) {
                continue;
            }
            const AC::MapTile& tile = page.Tiles[index];
            if (tile.BaseTex == 0u) {
                continue;
            }
            used[tile.BaseTex] = true;
            constexpr std::array<std::uint32_t, 3> textureShifts{26u, 16u, 6u};
            constexpr std::array<std::uint32_t, 3> alphaShifts{22u, 12u, 2u};
            for (std::size_t layer = 0; layer < textureShifts.size(); ++layer) {
                const std::uint8_t textureId = static_cast<std::uint8_t>(
                    (tile.TileInfo >> textureShifts[layer]) & 0x3fu);
                if (textureId == 0u) {
                    break;
                }
                const std::uint8_t alphaMask = static_cast<std::uint8_t>(
                    (tile.TileInfo >> alphaShifts[layer]) & 0x0fu);
                if (alphaMask != 0u) {
                    used[textureId] = true;
                }
            }
        }
    }
    std::vector<std::uint8_t> result;
    for (std::size_t id = 0; id < used.size(); ++id) {
        if (used[id]) {
            result.push_back(static_cast<std::uint8_t>(id));
        }
    }
    return result;
}

void RequirePixel(bool& corsairsTestOk,
                  const AC::DecodedImage& image,
                  std::uint32_t x,
                  std::uint32_t y,
                  const std::array<std::uint8_t, 4>& expected) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * image.Width + x) * 4u;
    for (std::size_t channel = 0; channel < expected.size(); ++channel) {
        REQUIRE_EQ(image.Pixels[offset + channel], expected[channel]);
    }
}

void RequireFailure(bool& corsairsTestOk,
                    const AC::TerrainBakeResult& result,
                    const std::string& detail,
                    const std::filesystem::path& expectedPng,
                    std::string_view expectedDetail) {
    REQUIRE(!result.Ok);
    REQUIRE(!expectedDetail.empty());
    REQUIRE_EQ(detail, std::string{expectedDetail});
    REQUIRE(result.PngPath.empty());
    REQUIRE(result.PngSha256.empty());
    std::error_code statusError;
    const std::filesystem::file_status outputStatus =
        CleanupSymlinkStatus(expectedPng, statusError);
    REQUIRE(!statusError);
    REQUIRE(outputStatus.type() == std::filesystem::file_type::not_found);
}

CORSAIRS_TEST(TerrainPageBaker_UsesLiteralLegacyBgra565AndDefaultWhite) {
    const std::array<std::array<std::uint8_t, 4>, 4> expected{{
        {0u, 0u, 248u, 255u},
        {0u, 252u, 0u, 255u},
        {248u, 0u, 0u, 255u},
        {248u, 252u, 248u, 255u},
    }};
    const std::array<std::uint16_t, 4> words{0xf800u, 0x07e0u, 0x001fu, 0xffffu};
    for (std::size_t index = 0; index < words.size(); ++index) {
        AC::MapTile tile = Tile(37u, words[index]);
        tile.Height = static_cast<std::int8_t>(-12);
        const AC::LegacyTerrainCornerSample sample =
            AC::ResolveLegacyTerrainCornerSample(tile, true);
        REQUIRE(sample.Diffuse == expected[index]);
        REQUIRE_EQ(sample.HeightCm, -120.0);
    }

    std::vector<AC::MapTile> absentPayloads(12u);
    absentPayloads[1].TileInfo = 0xffffffffu;
    absentPayloads[2].BaseTex = 255u;
    absentPayloads[3].Color = ColorWord(0xffffu);
    absentPayloads[4].Height = std::numeric_limits<std::int8_t>::max();
    absentPayloads[5].Region = std::numeric_limits<std::int16_t>::min();
    absentPayloads[6].Island = 255u;
    absentPayloads[7].Block[0] = 255u;
    absentPayloads[8].Block[1] = 255u;
    absentPayloads[9].Block[2] = 255u;
    absentPayloads[10].Block[3] = 255u;
    absentPayloads[11].TileInfo = 0xffffffffu;
    absentPayloads[11].BaseTex = 255u;
    absentPayloads[11].Color = ColorWord(0xffffu);
    absentPayloads[11].Height = std::numeric_limits<std::int8_t>::max();
    absentPayloads[11].Region = std::numeric_limits<std::int16_t>::min();
    absentPayloads[11].Island = 255u;
    std::fill(std::begin(absentPayloads[11].Block),
              std::end(absentPayloads[11].Block), 255u);
    const std::array<std::uint8_t, 4> white{255u, 255u, 255u, 255u};
    for (const AC::MapTile& payload : absentPayloads) {
        const AC::LegacyTerrainCornerSample absent =
            AC::ResolveLegacyTerrainCornerSample(payload, false);
        REQUIRE(absent.Diffuse == white);
        REQUIRE_EQ(absent.HeightCm, -200.0);
    }
}

CORSAIRS_TEST(TerrainPageBaker_BakesLiteralTexturePageAndRepeatsUvEveryFourCells) {
    TerrainFixture fixture{"literal-page"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "literal", LiteralTerrainTexture()));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));

    std::vector<std::optional<AC::MapTile>> cells(7u * 3u, Tile(1u));
    REQUIRE(fixture.WriteMap("page", 7, 3, cells));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());

    auto firstReader = OpenReader(fixture.MapPath("page"), detail);
    REQUIRE(firstReader.has_value());
    const AC::TerrainBakeResult first = AC::BakeTerrainPage(
        *firstReader, *catalog, AC::TerrainPageId{0u, 0u}, fixture.AlphaPath(),
        fixture.Output("first"), Options(2u, 1u), detail);
    REQUIRE(first.Ok);
    const auto firstImage = DecodeResult(first, detail);
    REQUIRE(firstImage.has_value());
    REQUIRE_EQ(firstImage->Width, 2u);
    REQUIRE_EQ(firstImage->Height, 2u);
    RequirePixel(corsairsTestOk, *firstImage, 0u, 0u, {248u, 0u, 0u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *firstImage, 1u, 0u, {0u, 252u, 0u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *firstImage, 0u, 1u, {0u, 0u, 248u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *firstImage, 1u, 1u, {248u, 252u, 248u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    REQUIRE_EQ(AC::Sha256Bytes(firstImage->Pixels),
               std::string{"8bafca593a1e31e011a6efc6afaeb9c010a115a1268abb22cdd4079f1c00244f"});

    auto repeatedReader = OpenReader(fixture.MapPath("page"), detail);
    REQUIRE(repeatedReader.has_value());
    const AC::TerrainBakeResult repeated = AC::BakeTerrainPage(
        *repeatedReader, *catalog, AC::TerrainPageId{2u, 0u}, fixture.AlphaPath(),
        fixture.Output("repeated"), Options(2u, 1u), detail);
    REQUIRE(repeated.Ok);
    const auto repeatedImage = DecodeResult(repeated, detail);
    REQUIRE(repeatedImage.has_value());
    RequirePixel(corsairsTestOk, *repeatedImage, 0u, 0u, {248u, 0u, 0u, 255u});
}

CORSAIRS_TEST(TerrainPageBaker_RoundsOverlayBeforeLegacyTint) {
    TerrainFixture fixture{"overlay-rounding"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "red", SolidImage(255u, 0u, 0u)));
    REQUIRE(fixture.AddTexture(2u, "blue", SolidImage(0u, 0u, 255u)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 85u)));
    std::vector<std::optional<AC::MapTile>> cells(
        4u, Tile(1u, 0xffffu, FirstUpper(2u, 1u)));
    REQUIRE(fixture.WriteMap("blend", 2, 2, cells));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());
    auto reader = OpenReader(fixture.MapPath("blend"), detail);
    REQUIRE(reader.has_value());
    const AC::TerrainBakeResult result = AC::BakeTerrainPage(
        *reader, *catalog, {0u, 0u}, fixture.AlphaPath(), fixture.Output("blend"),
        Options(1u, 1u), detail);
    REQUIRE(result.Ok);
    const auto image = DecodeResult(result, detail);
    REQUIRE(image.has_value());
    RequirePixel(corsairsTestOk, *image, 0u, 0u, {165u, 0u, 83u, 255u});
}

CORSAIRS_TEST(TerrainPageBaker_UsesSourceTrianglesNotBilinearCorners) {
    TerrainFixture fixture{"triangles"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "white", SolidImage(255u, 255u, 255u)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));
    const std::vector<std::optional<AC::MapTile>> cells{
        Tile(1u, 0xf800u), Tile(1u, 0x07e0u),
        Tile(1u, 0x001fu), Tile(1u, 0x0000u),
    };
    REQUIRE(fixture.WriteMap("triangles", 2, 2, cells));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());
    auto reader = OpenReader(fixture.MapPath("triangles"), detail);
    REQUIRE(reader.has_value());
    const auto result = AC::BakeTerrainPage(
        *reader, *catalog, {0u, 0u}, fixture.AlphaPath(), fixture.Output("triangles"),
        Options(1u, 2u), detail);
    REQUIRE(result.Ok);
    const auto image = DecodeResult(result, detail);
    REQUIRE(image.has_value());
    RequirePixel(corsairsTestOk, *image, 0u, 0u, {62u, 63u, 124u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *image, 1u, 1u, {62u, 63u, 0u, 255u});
}

CORSAIRS_TEST(TerrainPageBaker_UsesLinearTopRowWrapMirrorInsetAndTexelCenters) {
    TerrainFixture fixture{"asymmetric"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "base-asymmetric", AsymmetricTexture(false)));
    REQUIRE(fixture.AddTexture(2u, "upper-asymmetric", AsymmetricTexture(true)));
    REQUIRE(fixture.WriteAlpha(AsymmetricAtlas()));
    std::vector<std::optional<AC::MapTile>> cells(
        4u, Tile(1u, 0xffffu, FirstUpper(2u, 1u)));
    REQUIRE(fixture.WriteMap("asymmetric", 2, 2, cells));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());
    auto reader = OpenReader(fixture.MapPath("asymmetric"), detail);
    REQUIRE(reader.has_value());
    const auto result = AC::BakeTerrainPage(
        *reader, *catalog, {0u, 0u}, fixture.AlphaPath(), fixture.Output("asymmetric"),
        Options(1u, 4u), detail);
    REQUIRE(result.Ok);
    const auto image = DecodeResult(result, detail);
    REQUIRE(image.has_value());
    RequirePixel(corsairsTestOk, *image, 0u, 0u, {98u, 86u, 73u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *image, 1u, 2u, {39u, 32u, 38u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *image, 3u, 3u, {57u, 40u, 46u, 255u});
}

CORSAIRS_TEST(TerrainPageBaker_RejectsCheckedPageMathAndRemovesNoOutput) {
    TerrainFixture fixture{"page-math"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "base", SolidImage(10u, 20u, 30u)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));
    std::vector<std::optional<AC::MapTile>> cells(9u, Tile(1u));
    REQUIRE(fixture.WriteMap("math", 3, 3, cells));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());

    std::vector<std::pair<AC::TerrainPageId, AC::TerrainBakeOptions>> cases;
    cases.emplace_back(AC::TerrainPageId{0u, 0u}, Options(0u, 1u));
    cases.emplace_back(AC::TerrainPageId{0u, 0u}, Options(1u, 0u));
    cases.emplace_back(AC::TerrainPageId{std::numeric_limits<std::uint32_t>::max(), 0u},
                       Options(2u, 1u));
    cases.emplace_back(AC::TerrainPageId{0u, 0u}, Options(65536u, 65536u));
    cases.emplace_back(AC::TerrainPageId{0u, 0u},
                       Options(std::numeric_limits<std::uint32_t>::max(), 1u));
    cases.emplace_back(AC::TerrainPageId{1u, 0u}, Options(2u, 1u));
    const std::array<std::string_view, 6> expectedDetails{
        "CellsPerPage и PixelsPerCell должны быть ненулевыми",
        "CellsPerPage и PixelsPerCell должны быть ненулевыми",
        "координаты страницы не помещаются в uint32_t",
        "размер изображения не помещается в uint32_t",
        "полный byte count изображения переполнен",
        "страница с right/bottom halo выходит за усечённую сетку карты",
    };

    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto reader = OpenReader(fixture.MapPath("math"), detail);
        REQUIRE(reader.has_value());
        const auto output = fixture.Output("math-" + std::to_string(index));
        const auto expected = output /
            ("garner.albedo_" + std::to_string(cases[index].first.X) + "_" +
             std::to_string(cases[index].first.Y) + ".png");
        const auto result = AC::BakeTerrainPage(
            *reader, *catalog, cases[index].first, fixture.AlphaPath(), output,
            cases[index].second, detail);
        RequireFailure(corsairsTestOk, result, detail, expected,
                       expectedDetails[index]);
        if (!corsairsTestOk) {
            return;
        }
    }
}

CORSAIRS_TEST(TerrainPageBaker_AllowsAbsentHaloAsWhiteButRejectsOwnedAbsence) {
    TerrainFixture fixture{"presence"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "base", SolidImage(255u, 255u, 255u)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());

    const std::array<std::uint8_t, sizeof(AC::MapTile)> staleA{0x11u, 0x22u, 0x33u};
    const std::array<std::uint8_t, sizeof(AC::MapTile)> staleB{0xfeu, 0xdcu, 0xbau};
    const std::vector<std::optional<AC::MapTile>> haloCells{
        Tile(1u, 0xffffu), std::nullopt,
        Tile(1u, 0xffffu), Tile(1u, 0xffffu),
    };
    REQUIRE(fixture.WriteMap("halo-a", 2, 2, haloCells, staleA));
    REQUIRE(fixture.WriteMap("halo-b", 2, 2, haloCells, staleB));

    auto firstReader = OpenReader(fixture.MapPath("halo-a"), detail);
    REQUIRE(firstReader.has_value());
    const auto first = AC::BakeTerrainPage(
        *firstReader, *catalog, {0u, 0u}, fixture.AlphaPath(), fixture.Output("halo-a"),
        Options(1u, 1u), detail);
    REQUIRE(first.Ok);
    REQUIRE_EQ(first.AbsentSections, 0u);
    REQUIRE(first.UsedTextureIds == std::vector<std::uint8_t>{1u});

    const std::vector<std::optional<AC::MapTile>> presentHaloOnly{
        Tile(1u), Tile(63u),
        Tile(63u), Tile(63u),
    };
    REQUIRE(fixture.WriteMap("present-halo-only", 2, 2, presentHaloOnly));
    auto presentHaloReader = OpenReader(fixture.MapPath("present-halo-only"), detail);
    REQUIRE(presentHaloReader.has_value());
    const auto presentHaloResult = AC::BakeTerrainPage(
        *presentHaloReader, *catalog, {0u, 0u}, fixture.AlphaPath(),
        fixture.Output("present-halo-only"), Options(1u, 1u), detail);
    REQUIRE(presentHaloResult.Ok);
    REQUIRE(presentHaloResult.UsedTextureIds == std::vector<std::uint8_t>{1u});
    REQUIRE_EQ(presentHaloResult.UnresolvedLayers, 0u);

    auto secondReader = OpenReader(fixture.MapPath("halo-b"), detail);
    REQUIRE(secondReader.has_value());
    const auto second = AC::BakeTerrainPage(
        *secondReader, *catalog, {0u, 0u}, fixture.AlphaPath(), fixture.Output("halo-b"),
        Options(1u, 1u), detail);
    REQUIRE(second.Ok);
    REQUIRE_EQ(first.PngSha256, second.PngSha256);
    REQUIRE(first.UsedTextureIds == second.UsedTextureIds);

    const std::vector<std::optional<AC::MapTile>> ownedMissing{
        std::nullopt, Tile(1u), Tile(1u), Tile(1u),
    };
    REQUIRE(fixture.WriteMap("owned-missing", 2, 2, ownedMissing));
    auto missingReader = OpenReader(fixture.MapPath("owned-missing"), detail);
    REQUIRE(missingReader.has_value());
    const auto output = fixture.Output("owned-missing");
    const auto failed = AC::BakeTerrainPage(
        *missingReader, *catalog, {0u, 0u}, fixture.AlphaPath(), output,
        Options(1u, 1u), detail);
    RequireFailure(corsairsTestOk, failed, detail,
                   output / "garner.albedo_0_0.png",
                   "owned page содержит отсутствующую секцию");
    if (!corsairsTestOk) {
        return;
    }

    std::vector<std::optional<AC::MapTile>> requiredMissing(9u, Tile(1u));
    requiredMissing[4u] = std::nullopt;
    REQUIRE(fixture.WriteMap("required-missing", 3, 3, requiredMissing));
    auto requiredReader = OpenReader(fixture.MapPath("required-missing"), detail);
    REQUIRE(requiredReader.has_value());
    const auto requiredOutput = fixture.Output("required-missing");
    const auto requiredFailed = AC::BakeTerrainPage(
        *requiredReader, *catalog, {0u, 0u}, fixture.AlphaPath(), requiredOutput,
        Options(2u, 1u), detail);
    RequireFailure(corsairsTestOk, requiredFailed, detail,
                   requiredOutput / "garner.albedo_0_0.png",
                   "owned page содержит отсутствующую секцию");
}

CORSAIRS_TEST(TerrainPageBaker_RejectsCatalogDecodeAndAtlasFailures) {
    TerrainFixture fixture{"input-failures"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "base", SolidImage(20u, 30u, 40u)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));
    const std::vector<std::optional<AC::MapTile>> cells(4u, Tile(1u));
    REQUIRE(fixture.WriteMap("valid", 2, 2, cells));
    const std::vector<std::optional<AC::MapTile>> unresolvedCells(4u, Tile(2u));
    REQUIRE(fixture.WriteMap("unresolved", 2, 2, unresolvedCells));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());

    auto unresolvedReader = OpenReader(fixture.MapPath("unresolved"), detail);
    REQUIRE(unresolvedReader.has_value());
    const auto unresolvedOutput = fixture.Output("unresolved");
    const auto unresolved = AC::BakeTerrainPage(
        *unresolvedReader, *catalog, {0u, 0u}, fixture.AlphaPath(), unresolvedOutput,
        Options(1u, 1u), detail);
    RequireFailure(corsairsTestOk, unresolved, detail,
                   unresolvedOutput / "garner.albedo_0_0.png",
                   "не разрешён используемый terrain texture ID 2");
    if (!corsairsTestOk) {
        return;
    }
    REQUIRE(unresolved.UnresolvedLayers > 0u);

    const auto texture = catalog->Resolve(1u);
    REQUIRE(texture.has_value());
    std::filesystem::remove(*texture);
    auto unreadableReader = OpenReader(fixture.MapPath("valid"), detail);
    REQUIRE(unreadableReader.has_value());
    const auto unreadableOutput = fixture.Output("unreadable");
    const auto unreadable = AC::BakeTerrainPage(
        *unreadableReader, *catalog, {0u, 0u}, fixture.AlphaPath(), unreadableOutput,
        Options(1u, 1u), detail);
    const std::string unreadableDetail = detail;
    RequireFailure(corsairsTestOk, unreadable, detail,
                   unreadableOutput / "garner.albedo_0_0.png",
                   unreadableDetail);
    if (!corsairsTestOk) {
        return;
    }
    auto repeatedUnreadableReader = OpenReader(fixture.MapPath("valid"), detail);
    REQUIRE(repeatedUnreadableReader.has_value());
    const auto repeatedUnreadable = AC::BakeTerrainPage(
        *repeatedUnreadableReader, *catalog, {0u, 0u}, fixture.AlphaPath(),
        fixture.Output("unreadable-repeat"), Options(1u, 1u), detail);
    RequireFailure(corsairsTestOk, repeatedUnreadable, detail,
                   fixture.Output("unreadable-repeat") / "garner.albedo_0_0.png",
                   unreadableDetail);
    if (!corsairsTestOk) {
        return;
    }

    TerrainFixture atlasFixture{"atlas-failures"};
    REQUIRE(atlasFixture.Ready());
    REQUIRE(atlasFixture.AddTexture(1u, "base", SolidImage(20u, 30u, 40u)));
    REQUIRE(atlasFixture.WriteMap("valid", 2, 2, cells));
    auto atlasCatalog = atlasFixture.LoadCatalog(detail);
    REQUIRE(atlasCatalog.has_value());
    for (std::size_t malformed = 0; malformed < 2u; ++malformed) {
        if (malformed == 1u) {
            std::filesystem::create_directories(atlasFixture.AlphaPath().parent_path());
            std::ofstream output{atlasFixture.AlphaPath(), std::ios::binary};
            output << "not a png";
        }
        auto reader = OpenReader(atlasFixture.MapPath("valid"), detail);
        REQUIRE(reader.has_value());
        const auto output = atlasFixture.Output("atlas-" + std::to_string(malformed));
        const auto failed = AC::BakeTerrainPage(
            *reader, *atlasCatalog, {0u, 0u}, atlasFixture.AlphaPath(), output,
            Options(1u, 1u), detail);
        const std::string firstDetail = detail;
        RequireFailure(corsairsTestOk, failed, detail,
                       output / "garner.albedo_0_0.png", firstDetail);
        if (!corsairsTestOk) {
            return;
        }
        auto repeatReader = OpenReader(atlasFixture.MapPath("valid"), detail);
        REQUIRE(repeatReader.has_value());
        const auto repeatOutput =
            atlasFixture.Output("atlas-repeat-" + std::to_string(malformed));
        const auto repeated = AC::BakeTerrainPage(
            *repeatReader, *atlasCatalog, {0u, 0u}, atlasFixture.AlphaPath(),
            repeatOutput, Options(1u, 1u), detail);
        RequireFailure(corsairsTestOk, repeated, detail,
                       repeatOutput / "garner.albedo_0_0.png", firstDetail);
        if (!corsairsTestOk) {
            return;
        }
    }
}

CORSAIRS_TEST(TerrainPageBaker_EnforcesEveryProductionBudgetGate) {
    TerrainFixture fixture{"budget-gates"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "base", SolidImage(20u, 30u, 40u)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));
    const std::vector<std::optional<AC::MapTile>> cells(4u, Tile(1u));
    REQUIRE(fixture.WriteMap("valid", 2, 2, cells));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());
    constexpr std::array<std::string_view, 4> expectedDetails{
        "превышен budget lifetime peak RSS",
        "превышен budget размера PNG",
        "превышен budget декодированного texture cache",
        "превышен budget RGBA-строки",
    };

    for (std::size_t gate = 0; gate < 4u; ++gate) {
        AC::TerrainBakeOptions options = Options(1u, 1u);
        if (gate == 0u) {
            options.MaxRssBytes = 0u;
        }
        else if (gate == 1u) {
            options.MaxPngBytes = 0u;
        }
        else if (gate == 2u) {
            options.MaxTextureCacheBytes = 0u;
        }
        else {
            options.MaxRgbaRowBytes = 3u;
        }
        std::size_t cleanupCalls = 0u;
        std::filesystem::path cleanupPath;
        std::optional<std::string> cleanupHash;
        options.TestOnlyRemoveCompletedOutput =
            [&](const std::filesystem::path& path, std::string& cleanupDetail) {
                ++cleanupCalls;
                cleanupPath = path;
                cleanupHash = AC::Sha256File(path, cleanupDetail);
                if (!cleanupHash.has_value()) {
                    return false;
                }
                std::error_code removeError;
                const bool removed = std::filesystem::remove(path, removeError);
                if (removeError || !removed) {
                    cleanupDetail = "test cleanup не удалил completed PNG";
                    return false;
                }
                cleanupDetail.clear();
                return true;
            };
        auto reader = OpenReader(fixture.MapPath("valid"), detail);
        REQUIRE(reader.has_value());
        const auto output = fixture.Output("gate-" + std::to_string(gate));
        const auto expectedPng = output / "garner.albedo_0_0.png";
        const auto failed = AC::BakeTerrainPage(
            *reader, *catalog, {0u, 0u}, fixture.AlphaPath(), output, options, detail);
        RequireFailure(corsairsTestOk, failed, detail,
                       expectedPng, expectedDetails[gate]);
        if (!corsairsTestOk) {
            return;
        }
        REQUIRE_EQ(cleanupCalls, 1u);
        REQUIRE(cleanupPath == expectedPng);
        REQUIRE(cleanupHash.has_value());
        REQUIRE(IsLowerHexSha(*cleanupHash));
        REQUIRE(failed.OutputBytes > 0u);
        REQUIRE(failed.PeakRssBytes > 0u);
        REQUIRE_EQ(failed.PeakTextureCacheBytes, 64u);
        REQUIRE_EQ(failed.PeakRgbaRowBytes, 4u);
    }
}

CORSAIRS_TEST(TerrainPageBaker_RetainsRecoveryEvidenceWhenCleanupFails) {
    TerrainFixture fixture{"cleanup-recovery"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "base", SolidImage(20u, 30u, 40u)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));
    REQUIRE(fixture.WriteMap(
        "valid", 2, 2,
        std::vector<std::optional<AC::MapTile>>(4u, Tile(1u))));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());
    auto reader = OpenReader(fixture.MapPath("valid"), detail);
    REQUIRE(reader.has_value());

    AC::TerrainBakeOptions options = Options(1u, 1u);
    options.MaxPngBytes = 0u;
    std::size_t cleanupCalls = 0u;
    std::filesystem::path attemptedPath;
    options.TestOnlyRemoveCompletedOutput =
        [&](const std::filesystem::path& path, std::string& cleanupDetail) {
            ++cleanupCalls;
            attemptedPath = path;
            cleanupDetail = "injected remove denial";
            return false;
        };
    const auto output = fixture.Output("recovery");
    const auto expectedPng = output / "garner.albedo_0_0.png";
    const auto failed = AC::BakeTerrainPage(
        *reader, *catalog, {0u, 0u}, fixture.AlphaPath(), output, options, detail);

    REQUIRE(!failed.Ok);
    REQUIRE_EQ(cleanupCalls, 1u);
    REQUIRE(attemptedPath == expectedPng);
    REQUIRE(failed.PngPath == expectedPng);
    REQUIRE(IsLowerHexSha(failed.PngSha256));
    REQUIRE_EQ(failed.OutputBytes, std::filesystem::file_size(expectedPng));
    REQUIRE(failed.PeakRssBytes > 0u);
    REQUIRE_EQ(
        detail,
        std::format(
            "RECOVERY_REQUIRED: не удалось удалить PNG отклонённой попытки; "
            "cause=превышен budget размера PNG; retained={}; sha256={}; "
            "cleanup=injected remove denial",
            expectedPng.generic_string(), failed.PngSha256));
    const std::string firstDetail = detail;
    const std::string firstHash = failed.PngSha256;
    REQUIRE(std::filesystem::exists(expectedPng));
    std::error_code removeError;
    REQUIRE(std::filesystem::remove(expectedPng, removeError));
    REQUIRE(!removeError);
    REQUIRE(!std::filesystem::exists(expectedPng));

    auto repeatedReader = OpenReader(fixture.MapPath("valid"), detail);
    REQUIRE(repeatedReader.has_value());
    const auto repeated = AC::BakeTerrainPage(
        *repeatedReader, *catalog, {0u, 0u}, fixture.AlphaPath(), output,
        options, detail);
    REQUIRE(!repeated.Ok);
    REQUIRE_EQ(cleanupCalls, 2u);
    REQUIRE(repeated.PngPath == expectedPng);
    REQUIRE_EQ(repeated.PngSha256, firstHash);
    REQUIRE_EQ(detail, firstDetail);
    REQUIRE(std::filesystem::exists(expectedPng));
    removeError.clear();
    REQUIRE(std::filesystem::remove(expectedPng, removeError));
    REQUIRE(!removeError);
    REQUIRE(!std::filesystem::exists(expectedPng));
}

CORSAIRS_TEST(TerrainPageBaker_DanglingSymlinkIsRecoveryRequired) {
    TerrainFixture fixture{"cleanup-dangling-symlink"};
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.AddTexture(1u, "base", SolidImage(20u, 30u, 40u)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));
    REQUIRE(fixture.WriteMap(
        "valid", 2, 2,
        std::vector<std::optional<AC::MapTile>>(4u, Tile(1u))));
    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());
    auto reader = OpenReader(fixture.MapPath("valid"), detail);
    REQUIRE(reader.has_value());

    AC::TerrainBakeOptions options = Options(1u, 1u);
    options.MaxPngBytes = 0u;
    std::size_t cleanupCalls = 0u;
    std::optional<std::error_code> symlinkError;
    options.TestOnlyRemoveCompletedOutput =
        [&](const std::filesystem::path& path, std::string& cleanupDetail) {
            ++cleanupCalls;
            std::error_code error;
            const bool removed = std::filesystem::remove(path, error);
            if (error || !removed) {
                cleanupDetail = "injected remover не удалил original PNG";
                return false;
            }
            std::filesystem::create_symlink(
                "missing-recovery-target.png", path, error);
            if (error) {
                symlinkError = error;
                cleanupDetail = "injected remover не создал dangling symlink";
                return false;
            }
            cleanupDetail = "injected dangling symlink replacement";
            return true;
        };
    const auto output = fixture.Output("dangling-recovery");
    const auto expectedPng = output / "garner.albedo_0_0.png";
    const auto failed = AC::BakeTerrainPage(
        *reader, *catalog, {0u, 0u}, fixture.AlphaPath(), output, options, detail);

    REQUIRE_EQ(cleanupCalls, 1u);
    if (symlinkError.has_value() &&
        IsWindowsSymlinkPermissionError(*symlinkError)) {
        std::error_code skipCleanupError;
        std::filesystem::remove(expectedPng, skipCleanupError);
        REQUIRE(!skipCleanupError);
        const std::filesystem::file_status skipCleanupStatus =
            CleanupSymlinkStatus(expectedPng, skipCleanupError);
        REQUIRE(!skipCleanupError);
        REQUIRE(skipCleanupStatus.type() ==
                std::filesystem::file_type::not_found);
        ReportWindowsSymlinkSkip(
            "TerrainPageBaker_DanglingSymlinkIsRecoveryRequired", *symlinkError);
        return;
    }
    REQUIRE(!symlinkError.has_value());
    REQUIRE(!failed.Ok);
    REQUIRE(failed.PngPath == expectedPng);
    REQUIRE(IsLowerHexSha(failed.PngSha256));
    REQUIRE_EQ(
        detail,
        std::format(
            "RECOVERY_REQUIRED: не удалось удалить PNG отклонённой попытки; "
            "cause=превышен budget размера PNG; retained={}; sha256={}; "
            "cleanup=injected dangling symlink replacement",
            expectedPng.generic_string(), failed.PngSha256));
    std::error_code statusError;
    const std::filesystem::file_status retainedStatus =
        std::filesystem::symlink_status(expectedPng, statusError);
    REQUIRE(!statusError);
    REQUIRE(retainedStatus.type() == std::filesystem::file_type::symlink);

    std::error_code removeError;
    REQUIRE(std::filesystem::remove(expectedPng, removeError));
    REQUIRE(!removeError);
    statusError.clear();
    const std::filesystem::file_status removedStatus =
        CleanupSymlinkStatus(expectedPng, statusError);
    REQUIRE(!statusError);
    REQUIRE(removedStatus.type() == std::filesystem::file_type::not_found);
}

CORSAIRS_TEST(TerrainPageBaker_RejectsSingleDecodedTextureAboveHardLiveLimit) {
    TerrainFixture fixture{"oversized-decoded-texture"};
    REQUIRE(fixture.Ready());
    constexpr std::uint32_t width = 4097u;
    constexpr std::uint32_t height = 2048u;
    constexpr std::size_t decodedBytes =
        static_cast<std::size_t>(width) * height * 4u;
    REQUIRE_EQ(decodedBytes, 33562624u);
    REQUIRE(fixture.AddTexture(
        1u, "oversized", SolidImage(20u, 30u, 40u, 255u, width, height)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 0u)));
    REQUIRE(fixture.WriteMap(
        "oversized", 2, 2,
        std::vector<std::optional<AC::MapTile>>(4u, Tile(1u))));

    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());
    auto reader = OpenReader(fixture.MapPath("oversized"), detail);
    REQUIRE(reader.has_value());
    const auto output = fixture.Output("oversized");
    const auto failed = AC::BakeTerrainPage(
        *reader, *catalog, {0u, 0u}, fixture.AlphaPath(), output,
        Options(1u, 1u), detail);
    RequireFailure(
        corsairsTestOk, failed, detail, output / "garner.albedo_0_0.png",
        "terrain texture ID 1 имеет decoded размер 33562624 байт, hard limit 33554432");
    if (!corsairsTestOk) {
        return;
    }
    REQUIRE_EQ(failed.PeakTextureCacheBytes, decodedBytes);
}

CORSAIRS_TEST(TerrainPageBaker_ReleasesSampleBeforeEvictingNextTexture) {
    TerrainFixture fixture{"decoded-texture-lifetime"};
    REQUIRE(fixture.Ready());
    constexpr std::uint32_t dimension = 2300u;
    constexpr std::size_t decodedBytes =
        static_cast<std::size_t>(dimension) * dimension * 4u;
    REQUIRE_EQ(decodedBytes, 21160000u);
    REQUIRE(fixture.AddTexture(
        1u, "large-base", SolidImage(20u, 30u, 40u, 255u, dimension, dimension)));
    REQUIRE(fixture.AddTexture(
        2u, "large-upper", SolidImage(50u, 60u, 70u, 255u, dimension, dimension)));
    REQUIRE(fixture.WriteAlpha(SolidImage(0u, 0u, 0u, 128u)));
    REQUIRE(fixture.WriteMap(
        "two-large", 2, 2,
        std::vector<std::optional<AC::MapTile>>(
            4u, Tile(1u, 0xffffu, FirstUpper(2u, 1u)))));

    std::string detail;
    auto catalog = fixture.LoadCatalog(detail);
    REQUIRE(catalog.has_value());
    auto reader = OpenReader(fixture.MapPath("two-large"), detail);
    REQUIRE(reader.has_value());
    AC::TerrainBakeOptions options = Options(1u, 1u);
    options.MaxTextureCacheBytes = 22u * 1024u * 1024u;
    const auto result = AC::BakeTerrainPage(
        *reader, *catalog, {0u, 0u}, fixture.AlphaPath(), fixture.Output("two-large"),
        options, detail);
    REQUIRE(result.Ok);
    REQUIRE_EQ(result.PeakTextureCacheBytes, decodedBytes);
    REQUIRE(result.PeakTextureCacheBytes <= options.MaxTextureCacheBytes);
}

CORSAIRS_TEST(TerrainPageBaker_BakesCanonicalGarnerDeterministically) {
    const std::filesystem::path repoRoot{CORSAIRS_REPO_ROOT};
    const std::filesystem::path mapPath = repoRoot / "Client" / "map" / "garner.map";
    const std::filesystem::path databasePath = repoRoot / "databases" / "gamedata.sqlite";
    const std::filesystem::path clientRoot = repoRoot / "Client";
    const std::filesystem::path alphaAtlas =
        clientRoot / "texture" / "terrain" / "alpha" / "total.png";
    REQUIRE(mapPath == repoRoot / "Client" / "map" / "garner.map");
    REQUIRE(databasePath == repoRoot / "databases" / "gamedata.sqlite");
    REQUIRE(clientRoot == repoRoot / "Client");
    REQUIRE(alphaAtlas == clientRoot / "texture" / "terrain" / "alpha" / "total.png");

    std::string detail;
    const auto catalog = AC::TerrainCatalog::Load(databasePath, clientRoot, detail);
    REQUIRE(catalog.has_value());
    const auto brick = catalog->Resolve(4u);
    REQUIRE(brick.has_value());
    REQUIRE_EQ(brick->generic_string(),
               (clientRoot / "texture" / "terrain" / "brick05.png").generic_string());

    auto inspectionReader = OpenReader(mapPath, detail);
    REQUIRE(inspectionReader.has_value());
    AC::MapDiagnostics diagnostics;
    const auto goldenCell = inspectionReader->ReadWindow({2233u, 2784u, 1u, 1u},
                                                          0u, 0u, diagnostics);
    REQUIRE(goldenCell.has_value());
    REQUIRE_EQ(goldenCell->Tiles[0].BaseTex, 4u);
    REQUIRE_EQ(goldenCell->Tiles[0].TileInfo, 0x02cf2000u);
    REQUIRE_EQ((goldenCell->Tiles[0].TileInfo >> 26u) & 0x3fu, 0u);
    const auto goldenPath = catalog->Resolve(goldenCell->Tiles[0].BaseTex);
    REQUIRE(goldenPath.has_value());
    REQUIRE_EQ(goldenPath->generic_string(),
               (clientRoot / "texture" / "terrain" / "brick05.png").generic_string());

    const auto frustum = inspectionReader->ReadWindow(
        {2193u, 2756u, 80u, 47u}, 0u, 0u, diagnostics);
    REQUIRE(frustum.has_value());
    REQUIRE(std::all_of(frustum->SectionPresent.begin(), frustum->SectionPresent.end(),
                        [](std::uint8_t value) { return value == 1u; }));

    const auto page = inspectionReader->ReadWindow(
        {2176u, 2688u, 128u, 128u}, 1u, 1u, diagnostics);
    REQUIRE(page.has_value());
    const auto sparseOffset = ReadU32LeAt(mapPath, 717972u);
    REQUIRE(sparseOffset.has_value());
    REQUIRE_EQ(*sparseOffset, 0u);
    REQUIRE_EQ(page->StoredWidth, 129u);
    REQUIRE_EQ(page->StoredHeight, 129u);
    REQUIRE_EQ(page->SectionPresent.size(), 17u * 17u);
    REQUIRE_EQ(std::count(page->SectionPresent.begin(), page->SectionPresent.end(), 0u), 1);
    REQUIRE_EQ(page->SectionPresent[254u], 0u);
    REQUIRE_EQ(std::count(page->TilePresent.begin(), page->TilePresent.end(), 0u), 8);
    for (std::uint32_t y = 112u; y <= 119u; ++y) {
        const std::size_t haloIndex = static_cast<std::size_t>(y) * 129u + 128u;
        REQUIRE_EQ(page->TilePresent[haloIndex], 0u);
        const AC::MapTile zero{};
        REQUIRE(std::memcmp(&page->Tiles[haloIndex], &zero, sizeof(zero)) == 0);
    }
    for (std::uint32_t y = 0; y < 128u; ++y) {
        for (std::uint32_t x = 0; x < 128u; ++x) {
            REQUIRE_EQ(page->TilePresent[static_cast<std::size_t>(y) * 129u + x], 1u);
        }
    }

    for (std::uint32_t y = 112u; y <= 119u; ++y) {
        const std::array<std::size_t, 4> indices{
            static_cast<std::size_t>(y) * 129u + 127u,
            static_cast<std::size_t>(y) * 129u + 128u,
            static_cast<std::size_t>(y + 1u) * 129u + 127u,
            static_cast<std::size_t>(y + 1u) * 129u + 128u,
        };
        const std::array<std::uint8_t, 4> expectedPresence =
            y < 119u ? std::array<std::uint8_t, 4>{1u, 0u, 1u, 0u}
                     : std::array<std::uint8_t, 4>{1u, 0u, 1u, 1u};
        for (std::size_t corner = 0; corner < indices.size(); ++corner) {
            REQUIRE_EQ(page->TilePresent[indices[corner]], expectedPresence[corner]);
            if (expectedPresence[corner] == 1u) {
                REQUIRE_EQ(static_cast<std::uint16_t>(page->Tiles[indices[corner]].Color),
                           0xffffu);
                const AC::LegacyTerrainCornerSample sample =
                    AC::ResolveLegacyTerrainCornerSample(
                        page->Tiles[indices[corner]], true);
                REQUIRE(sample.Diffuse ==
                        (std::array<std::uint8_t, 4>{248u, 252u, 248u, 255u}));
            }
            else {
                AC::MapTile ignored = Tile(255u, 0x1234u, 0xffffffffu);
                ignored.Height = 127;
                const AC::LegacyTerrainCornerSample sample =
                    AC::ResolveLegacyTerrainCornerSample(ignored, false);
                REQUIRE(sample.Diffuse ==
                        (std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u}));
                REQUIRE_EQ(sample.HeightCm, -200.0);
            }
        }
    }

    const std::vector<std::uint8_t> expectedIds = ExpectedUsedIds(*page);
    REQUIRE(expectedIds == (std::vector<std::uint8_t>{
        2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 12u, 16u, 18u, 19u, 20u,
        22u, 25u, 33u, 35u,
    }));
    AC::TerrainBakeOptions options;
    options.MaxRssBytes = std::numeric_limits<std::size_t>::max();
    const auto outputRoot = CreateUniqueTestDirectory("garner-page-tests", detail);
    REQUIRE(outputRoot.has_value());
    ScopedOwnedTree outputCleanup{*outputRoot};
    outputCleanup.MarkOwned();

    auto firstReader = OpenReader(mapPath, detail);
    REQUIRE(firstReader.has_value());
    const auto firstDirectory = *outputRoot / "run-a";
    const auto first = AC::BakeTerrainPage(
        *firstReader, *catalog, {17u, 21u}, alphaAtlas, firstDirectory, options, detail);
    REQUIRE(first.Ok);
    REQUIRE_EQ(first.SourceCellBounds.X, 2176u);
    REQUIRE_EQ(first.SourceCellBounds.Y, 2688u);
    REQUIRE_EQ(first.SourceCellBounds.Width, 128u);
    REQUIRE_EQ(first.SourceCellBounds.Height, 128u);
    REQUIRE_EQ(first.SectionOriginX, 272u);
    REQUIRE_EQ(first.SectionOriginY, 336u);
    REQUIRE_EQ(first.SectionGridWidth, 16u);
    REQUIRE_EQ(first.SectionGridHeight, 16u);
    REQUIRE_EQ(first.SectionPresenceMask.size(), 256u);
    REQUIRE(std::all_of(first.SectionPresenceMask.begin(), first.SectionPresenceMask.end(),
                        [](std::uint8_t value) { return value == 1u; }));
    REQUIRE_EQ(first.AbsentSections, 0u);
    REQUIRE_EQ(first.UnresolvedLayers, 0u);
    REQUIRE(first.UsedTextureIds == expectedIds);
    REQUIRE(first.PngPath == firstDirectory / "garner.albedo_17_21.png");
    REQUIRE(IsLowerHexSha(first.PngSha256));
    const auto firstHash = AC::Sha256File(first.PngPath, detail);
    REQUIRE(firstHash.has_value());
    REQUIRE_EQ(first.PngSha256, *firstHash);
    REQUIRE_EQ(first.OutputBytes, std::filesystem::file_size(first.PngPath));
    REQUIRE(first.OutputBytes <= options.MaxPngBytes);
    REQUIRE(first.PeakTextureCacheBytes <= options.MaxTextureCacheBytes);
    REQUIRE(first.PeakRgbaRowBytes <= options.MaxRgbaRowBytes);
    REQUIRE(first.PeakRssBytes > 0u);
    const auto oracleImage = AC::DecodeImageFile(first.PngPath, detail);
    REQUIRE(oracleImage.has_value());
    REQUIRE_EQ(oracleImage->Width, 4096u);
    REQUIRE_EQ(oracleImage->Height, 4096u);
    RequirePixel(corsairsTestOk, *oracleImage, 4095u, 3584u, {0u, 0u, 0u, 0u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *oracleImage, 4095u, 3600u, {0u, 0u, 0u, 0u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *oracleImage, 4095u, 3615u, {0u, 0u, 0u, 0u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *oracleImage, 4095u, 3808u,
                 {148u, 130u, 93u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *oracleImage, 4095u, 3824u,
                 {141u, 128u, 96u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    RequirePixel(corsairsTestOk, *oracleImage, 4095u, 3839u,
                 {143u, 127u, 92u, 255u});
    if (!corsairsTestOk) {
        return;
    }
    REQUIRE_EQ(first.OutputBytes, 43610431u);
    REQUIRE_EQ(first.PngSha256,
               std::string{"3308d429e43c42b67a69a75cbe5eadd34a70f5289d2d024330cdeb0509b86c93"});

    auto secondReader = OpenReader(mapPath, detail);
    REQUIRE(secondReader.has_value());
    const auto secondDirectory = *outputRoot / "run-b";
    const auto second = AC::BakeTerrainPage(
        *secondReader, *catalog, {17u, 21u}, alphaAtlas, secondDirectory, options, detail);
    REQUIRE(second.Ok);
    REQUIRE(second.PngPath == secondDirectory / "garner.albedo_17_21.png");
    REQUIRE_EQ(first.PngSha256, second.PngSha256);
    REQUIRE(first.UsedTextureIds == second.UsedTextureIds);
    REQUIRE(outputCleanup.CleanupChecked(detail));
    REQUIRE(detail.empty());
}

CORSAIRS_TEST(TerrainPageBudgetProbe_LeavesForeignCollisionAndCleansOwnedOutput) {
    const std::filesystem::path foreignDirectory =
        std::filesystem::temp_directory_path() /
        "corsairs-terrain-page-budget-probe";
    std::error_code error;
    REQUIRE(!std::filesystem::exists(foreignDirectory, error));
    REQUIRE(!error);
    ScopedOwnedTree foreignCleanup{foreignDirectory};
    REQUIRE(std::filesystem::create_directory(foreignDirectory, error));
    REQUIRE(!error);
    foreignCleanup.MarkOwned();
    const std::filesystem::path sentinel = foreignDirectory / "foreign-sentinel.txt";
    {
        std::ofstream output{sentinel};
        REQUIRE(static_cast<bool>(output));
        output << "do not remove\n";
        REQUIRE(output.good());
    }

    const auto before = ProbePrivateDirectories();
    std::string detail;
    const auto logDirectory = CreateUniqueTestDirectory("probe-collision-log", detail);
    REQUIRE(logDirectory.has_value());
    ScopedOwnedTree logCleanup{*logDirectory};
    logCleanup.MarkOwned();
    const std::filesystem::path logPath = *logDirectory / "probe.log";
    REQUIRE_EQ(RunBudgetProbe(std::filesystem::path{CORSAIRS_REPO_ROOT}, logPath), 0);
    REQUIRE(std::filesystem::is_regular_file(sentinel));
    REQUIRE(ProbePrivateDirectories() == before);
    const auto log = ReadTextFile(logPath);
    REQUIRE(log.has_value());
    REQUIRE(log->contains(
        "sha256=3308d429e43c42b67a69a75cbe5eadd34a70f5289d2d024330cdeb0509b86c93"));
    REQUIRE(foreignCleanup.CleanupChecked(detail));
    REQUIRE(detail.empty());
    REQUIRE(logCleanup.CleanupChecked(detail));
    REQUIRE(detail.empty());
}

CORSAIRS_TEST(TerrainPageBudgetProbe_RejectsCanonicalRepoSymlinkEscape) {
    std::string detail;
    const auto fakeRoot = CreateUniqueTestDirectory("probe-symlink-root", detail);
    REQUIRE(fakeRoot.has_value());
    ScopedOwnedTree fakeCleanup{*fakeRoot};
    fakeCleanup.MarkOwned();
    const std::filesystem::path realRoot{CORSAIRS_REPO_ROOT};
    std::error_code error;
    std::filesystem::create_directory_symlink(
        realRoot / "Client", *fakeRoot / "Client", error);
    if (error && IsWindowsSymlinkPermissionError(error)) {
        ReportWindowsSymlinkSkip(
            "TerrainPageBudgetProbe_RejectsCanonicalRepoSymlinkEscape", error);
        REQUIRE(fakeCleanup.CleanupChecked(detail));
        return;
    }
    REQUIRE(!error);
    error.clear();
    std::filesystem::create_directory_symlink(
        realRoot / "databases", *fakeRoot / "databases", error);
    if (error && IsWindowsSymlinkPermissionError(error)) {
        ReportWindowsSymlinkSkip(
            "TerrainPageBudgetProbe_RejectsCanonicalRepoSymlinkEscape", error);
        REQUIRE(fakeCleanup.CleanupChecked(detail));
        return;
    }
    REQUIRE(!error);

    const std::filesystem::path logPath = *fakeRoot / "probe.log";
    REQUIRE(RunBudgetProbe(*fakeRoot, logPath) != 0);
    const auto log = ReadTextFile(logPath);
    REQUIRE(log.has_value());
    REQUIRE_EQ(*log,
               std::string{
                   "TerrainPageBudget: вход Client/map/garner.map выходит за "
                   "canonical repo root\n"});
    REQUIRE(fakeCleanup.CleanupChecked(detail));
    REQUIRE(detail.empty());
}

CORSAIRS_TEST(ProcessMetrics_ReportsNonzeroLifetimePeakRss) {
    std::string detail = "stale";
    const auto rss = AC::QueryPeakProcessRssBytes(detail);
    REQUIRE(rss.has_value());
    REQUIRE(*rss > 0u);
    REQUIRE(detail.empty());
}

} // namespace
