#include "Corsairs/Tools/AssetConverter/ImageCodec.h"
#include "Corsairs/Tools/AssetConverter/TerrainCatalog.h"
#include "Corsairs/Tools/AssetConverter/TerrainTextureCache.h"

#include "TestHarness.h"

#include "sqlite3.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::size_t kDecoded256RgbaBytes = 256u * 256u * 4u;
constexpr std::size_t kHardCacheLimit = 32u * 1024u * 1024u;

std::filesystem::path RepoRoot() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT};
}

std::filesystem::path TestDirectory() {
    return std::filesystem::temp_directory_path() / "corsairs-terrain-catalog-tests";
}

std::string PathUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::string GenericUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

bool CreateTerrainDatabase(const std::filesystem::path& path, std::string_view source) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::remove(path, error);

    sqlite3* database = nullptr;
    const std::string sqlitePath = PathUtf8(path);
    if (sqlite3_open(sqlitePath.c_str(), &database) != SQLITE_OK) {
        if (database != nullptr) {
            sqlite3_close(database);
        }
        return false;
    }

    const char* schema =
        "CREATE TABLE terrains (id INTEGER PRIMARY KEY, name TEXT, type INTEGER, attr INTEGER);";
    if (sqlite3_exec(database, schema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }

    sqlite3_stmt* statement = nullptr;
    const char* insert = "INSERT INTO terrains(id, name, type, attr) VALUES(4, ?1, 1, 0);";
    if (sqlite3_prepare_v2(database, insert, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }

    const int bindResult = sqlite3_bind_text(statement, 1, source.data(),
                                             static_cast<int>(source.size()), SQLITE_TRANSIENT);
    const bool inserted = bindResult == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return inserted;
}

CORSAIRS_TEST(TerrainCatalog_ResolvesTrackedLegacyBmpNamesToPng) {
    std::string detail;
    const auto catalog = AC::TerrainCatalog::Load(
        RepoRoot() / "databases" / "gamedata.sqlite",
        RepoRoot() / "Client",
        detail);

    REQUIRE(catalog.has_value());
    const auto brick = catalog->Resolve(4u);
    const auto grass = catalog->Resolve(5u);
    REQUIRE(brick.has_value());
    REQUIRE(grass.has_value());
    REQUIRE_EQ(brick->generic_string(),
               (RepoRoot() / "Client" / "texture" / "terrain" / "brick05.png")
                   .generic_string());
    REQUIRE_EQ(grass->generic_string(),
               (RepoRoot() / "Client" / "texture" / "terrain" / "grass05.png")
                   .generic_string());
}

CORSAIRS_TEST(TerrainCatalog_RejectsUnsafeOrUnavailableSources) {
    const std::array<std::string, 4> rejected{{
        "texture/terrain/brick05.dds",
        "../Client/texture/terrain/brick05.bmp",
        "texture/terrain/does-not-exist.bmp",
        (TestDirectory() / "absolute.bmp").string(),
    }};

    for (std::size_t index = 0; index < rejected.size(); ++index) {
        const auto database = TestDirectory() /
                              ("rejected-" + std::to_string(index) + ".sqlite");
        REQUIRE(CreateTerrainDatabase(database, rejected[index]));

        std::string detail;
        const auto catalog = AC::TerrainCatalog::Load(database, RepoRoot() / "Client", detail);
        REQUIRE(!catalog.has_value());
        REQUIRE(!detail.empty());
    }
}

CORSAIRS_TEST(TerrainCatalog_RejectsDirectoryMasqueradingAsPng) {
    const auto clientRoot = TestDirectory() / "directory-client";
    const auto fakePng = clientRoot / "texture" / "terrain" / "directory.png";
    std::error_code error;
    std::filesystem::remove_all(clientRoot, error);
    std::filesystem::create_directories(fakePng, error);
    REQUIRE(!error);

    const auto database = TestDirectory() / "directory-source.sqlite";
    REQUIRE(CreateTerrainDatabase(database, "texture/terrain/directory.bmp"));

    std::string detail;
    const auto catalog = AC::TerrainCatalog::Load(database, clientRoot, detail);
    REQUIRE(!catalog.has_value());
    REQUIRE(!detail.empty());
}

CORSAIRS_TEST(TerrainCatalog_UsesUtf8ForDatabaseAndSourcePaths) {
    const auto caseRoot = TestDirectory() / std::filesystem::path{u8"юникод"};
    const auto clientRoot = caseRoot / std::filesystem::path{u8"клиент"};
    const auto relativeBmp = std::filesystem::path{u8"texture/terrain/берег.bmp"};
    auto relativePng = relativeBmp;
    relativePng.replace_extension(".png");
    const auto pngPath = clientRoot / relativePng;

    std::error_code error;
    std::filesystem::remove_all(caseRoot, error);
    std::filesystem::create_directories(pngPath.parent_path(), error);
    REQUIRE(!error);
    AC::DecodedImage pixel{1u, 1u, {10u, 20u, 30u, 255u}};
    REQUIRE(AC::WritePng(pngPath, pixel));

    const auto database = caseRoot / std::filesystem::path{u8"данные.sqlite"};
    REQUIRE(CreateTerrainDatabase(database, GenericUtf8(relativeBmp)));

    std::string detail;
    const auto catalog = AC::TerrainCatalog::Load(database, clientRoot, detail);
    REQUIRE(catalog.has_value());
    const auto resolved = catalog->Resolve(4u);
    REQUIRE(resolved.has_value());
    REQUIRE_EQ(PathUtf8(*resolved), PathUtf8(std::filesystem::canonical(pngPath)));

    AC::TerrainTextureCache cache{1024u};
    const auto decoded = cache.Load(*resolved, detail);
    REQUIRE(decoded != nullptr);
    REQUIRE_EQ(decoded->Width, 1u);
    REQUIRE_EQ(decoded->Height, 1u);
    REQUIRE_EQ(decoded->Pixels.size(), 4u);
    REQUIRE_EQ(decoded->Pixels[0], 10u);
    REQUIRE_EQ(decoded->Pixels[1], 20u);
    REQUIRE_EQ(decoded->Pixels[2], 30u);
    REQUIRE_EQ(decoded->Pixels[3], 255u);

    const auto cached = cache.Load(*resolved, detail);
    REQUIRE(cached.get() == decoded.get());
    REQUIRE_EQ(cache.EntryCount(), 1u);
    REQUIRE_EQ(cache.DecodedBytes(), 4u);
    REQUIRE_EQ(cache.PeakDecodedBytes(), 4u);

    // То же имя файла под другим non-ASCII client root обязано быть отдельным
    // cache key, а не столкнуться после потери каталога или кодировки.
    const auto secondClientRoot = caseRoot / std::filesystem::path{u8"другой-клиент"};
    const auto secondPngPath = secondClientRoot / relativePng;
    std::filesystem::create_directories(secondPngPath.parent_path(), error);
    REQUIRE(!error);
    AC::DecodedImage secondPixel{1u, 1u, {200u, 150u, 100u, 255u}};
    REQUIRE(AC::WritePng(secondPngPath, secondPixel));
    const auto secondDatabase = caseRoot / std::filesystem::path{u8"другие-данные.sqlite"};
    REQUIRE(CreateTerrainDatabase(secondDatabase, GenericUtf8(relativeBmp)));
    const auto secondCatalog =
        AC::TerrainCatalog::Load(secondDatabase, secondClientRoot, detail);
    REQUIRE(secondCatalog.has_value());
    const auto secondResolved = secondCatalog->Resolve(4u);
    REQUIRE(secondResolved.has_value());
    const auto secondDecoded = cache.Load(*secondResolved, detail);
    REQUIRE(secondDecoded != nullptr);
    REQUIRE(secondDecoded.get() != decoded.get());
    REQUIRE_EQ(secondDecoded->Pixels[0], 200u);
    REQUIRE_EQ(cache.EntryCount(), 2u);
    REQUIRE_EQ(cache.DecodedBytes(), 8u);
    REQUIRE_EQ(cache.PeakDecodedBytes(), 8u);

    const auto missing = caseRoot / std::filesystem::path{u8"нет/файл.png"};
    const auto missingImage = AC::DecodeImageFile(missing, detail);
    REQUIRE(!missingImage.has_value());
    REQUIRE(detail.find(PathUtf8(missing)) != std::string::npos);
}

CORSAIRS_TEST(TerrainDecoder_DecodesTrackedPngsAsTopDownRgba) {
    const std::array<std::filesystem::path, 2> paths{{
        RepoRoot() / "Client" / "texture" / "terrain" / "brick05.png",
        RepoRoot() / "Client" / "texture" / "terrain" / "alpha" / "total.png",
    }};

    for (const auto& path : paths) {
        std::string detail;
        const auto image = AC::DecodeImageFile(path, detail);
        REQUIRE(image.has_value());
        REQUIRE_EQ(image->Width, 256u);
        REQUIRE_EQ(image->Height, 256u);
        REQUIRE_EQ(image->Pixels.size(), kDecoded256RgbaBytes);
    }
}

CORSAIRS_TEST(TerrainDecoder_PreservesLiteralRgbaChannelsAndRowOrder) {
    AC::DecodedImage fixture;
    fixture.Width = 2u;
    fixture.Height = 2u;
    fixture.Pixels = {
        1u, 2u, 3u, 4u,       10u, 20u, 30u, 40u,
        50u, 60u, 70u, 80u,   90u, 100u, 110u, 120u,
    };
    const auto path = TestDirectory() / "literal-rgba.png";
    REQUIRE(AC::WritePng(path, fixture));

    std::string detail;
    const auto decoded = AC::DecodeImageFile(path, detail);

    REQUIRE(decoded.has_value());
    REQUIRE_EQ(decoded->Width, 2u);
    REQUIRE_EQ(decoded->Height, 2u);
    REQUIRE_EQ(decoded->Pixels.size(), 16u);
    for (std::size_t index = 0; index < fixture.Pixels.size(); ++index) {
        REQUIRE_EQ(decoded->Pixels[index], fixture.Pixels[index]);
    }
}

CORSAIRS_TEST(TerrainTextureCache_EvictsLookupButRetainedImageStaysValid) {
    AC::TerrainTextureCache cache{kDecoded256RgbaBytes};
    std::string detail;
    const auto brick = cache.Load(
        RepoRoot() / "Client" / "texture" / "terrain" / "brick05.png", detail);
    REQUIRE(brick != nullptr);
    const std::uint8_t retainedFirstRed = brick->Pixels[0];

    const auto grass = cache.Load(
        RepoRoot() / "Client" / "texture" / "terrain" / "grass05.png", detail);
    REQUIRE(grass != nullptr);
    REQUIRE_EQ(cache.EntryCount(), 1u);
    REQUIRE(cache.DecodedBytes() <= kHardCacheLimit);
    REQUIRE(cache.PeakDecodedBytes() <= kHardCacheLimit);
    REQUIRE_EQ(brick->Width, 256u);
    REQUIRE_EQ(brick->Pixels[0], retainedFirstRed);

    const auto brickReloaded = cache.Load(
        RepoRoot() / "Client" / "texture" / "terrain" / "brick05.png", detail);
    REQUIRE(brickReloaded != nullptr);
    REQUIRE(brickReloaded.get() != brick.get());
}

CORSAIRS_TEST(TerrainTextureCache_EvictsLeastRecentlyUsedEntry) {
    AC::TerrainTextureCache cache{kDecoded256RgbaBytes * 2u};
    std::string detail;
    const auto brickPath =
        RepoRoot() / "Client" / "texture" / "terrain" / "brick05.png";
    const auto grassPath =
        RepoRoot() / "Client" / "texture" / "terrain" / "grass05.png";
    const auto alphaPath =
        RepoRoot() / "Client" / "texture" / "terrain" / "alpha" / "total.png";

    const auto brick = cache.Load(brickPath, detail);
    const auto grass = cache.Load(grassPath, detail);
    REQUIRE(brick != nullptr);
    REQUIRE(grass != nullptr);
    REQUIRE(cache.Load(brickPath, detail).get() == brick.get());
    REQUIRE(cache.Load(alphaPath, detail) != nullptr);

    REQUIRE(cache.Load(brickPath, detail).get() == brick.get());
    REQUIRE(cache.Load(grassPath, detail).get() != grass.get());
    REQUIRE_EQ(cache.EntryCount(), 2u);
    REQUIRE(cache.DecodedBytes() <= kDecoded256RgbaBytes * 2u);
}

CORSAIRS_TEST(TerrainTextureCache_ClampsCapacityToThirtyTwoMiB) {
    AC::DecodedImage oversized;
    oversized.Width = 4097u;
    oversized.Height = 2048u;
    oversized.Pixels.assign(static_cast<std::size_t>(oversized.Width) * oversized.Height * 4u,
                            0x5Au);

    const auto path = TestDirectory() / "over-hard-limit.png";
    REQUIRE(AC::WritePng(path, oversized));

    AC::TerrainTextureCache cache{64u * 1024u * 1024u};
    std::string detail;
    const auto loaded = cache.Load(path, detail);

    REQUIRE(loaded != nullptr);
    REQUIRE_EQ(loaded->Pixels.size(), oversized.Pixels.size());
    REQUIRE_EQ(cache.EntryCount(), 0u);
    REQUIRE_EQ(cache.DecodedBytes(), 0u);
    REQUIRE_EQ(cache.PeakDecodedBytes(), 0u);
}

} // namespace
