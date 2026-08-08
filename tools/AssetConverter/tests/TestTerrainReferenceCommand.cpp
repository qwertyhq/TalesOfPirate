#include "Corsairs/Tools/AssetConverter/TerrainReferenceCommand.h"

#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::string_view kHash =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kHashOfX =
    "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881";

bool SkipWindowsPrivilegeOrAccessDenied(
    const std::error_code& error,
    std::string_view operation) {
#if defined(_WIN32)
    if (error == std::errc::permission_denied) {
        std::cout << "SKIP: Windows " << operation
                  << " privilege/access unavailable: "
                  << error.message() << '\n';
        return true;
    }
#else
    (void)error;
    (void)operation;
#endif
    return false;
}

std::string ShellQuote(const std::filesystem::path& path) {
    const std::string value = path.string();
#if defined(_WIN32)
    std::string quoted{"\""};
    for (const char character : value) {
        if (character == '\"') {
            quoted += "\\\"";
        }
        else {
            quoted += character;
        }
    }
    quoted += '\"';
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

std::optional<std::string> ReadText(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

bool WriteText(const std::filesystem::path& path, std::string_view text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    return static_cast<bool>(output);
}

bool SetExactPrivateMode(const std::filesystem::path& path) {
#if defined(_WIN32)
    return SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0 &&
        GetFileAttributesW(path.c_str()) == FILE_ATTRIBUTE_NORMAL;
#else
    struct stat status {};
    return ::chmod(path.c_str(), 0600) == 0 &&
        ::lstat(path.c_str(), &status) == 0 &&
        static_cast<std::uint32_t>(status.st_mode & 07777u) == 0600u;
#endif
}

std::optional<std::pair<std::size_t, std::size_t>> JsonMemberValueRange(
    std::string_view json,
    std::string_view key) {
    const std::string needle = std::format("\"{}\":", key);
    const std::size_t member = json.find(needle);
    if (member == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t begin = member + needle.size();
    std::size_t end = begin;
    if (json[begin] == '"') {
        ++end;
        bool escaped = false;
        while (end < json.size()) {
            const char character = json[end++];
            if (!escaped && character == '"') break;
            if (!escaped && character == '\\') escaped = true;
            else escaped = false;
        }
    }
    else if (json[begin] == '{' || json[begin] == '[') {
        const char opening = json[begin];
        const char closing = opening == '{' ? '}' : ']';
        std::size_t depth = 0u;
        bool quoted = false;
        bool escaped = false;
        for (end = begin; end < json.size(); ++end) {
            const char character = json[end];
            if (quoted) {
                if (!escaped && character == '"') quoted = false;
                if (!escaped && character == '\\') escaped = true;
                else escaped = false;
                continue;
            }
            if (character == '"') quoted = true;
            else if (character == opening) ++depth;
            else if (character == closing && --depth == 0u) {
                ++end;
                break;
            }
        }
    }
    else {
        while (end < json.size() && json[end] != ',' && json[end] != '}' &&
               json[end] != ']') {
            ++end;
        }
    }
    if (end <= begin || end > json.size()) {
        return std::nullopt;
    }
    return std::pair{begin, end};
}

std::optional<std::string> RemoveFirstJsonMember(
    std::string json,
    std::string_view key) {
    const std::string needle = std::format("\"{}\":", key);
    const std::size_t member = json.find(needle);
    const auto value = JsonMemberValueRange(json, key);
    if (member == std::string::npos || !value.has_value()) {
        return std::nullopt;
    }
    std::size_t eraseBegin = member;
    std::size_t eraseEnd = value->second;
    if (eraseEnd < json.size() && json[eraseEnd] == ',') {
        ++eraseEnd;
    }
    else if (eraseBegin > 0u && json[eraseBegin - 1u] == ',') {
        --eraseBegin;
    }
    json.erase(eraseBegin, eraseEnd - eraseBegin);
    return json;
}

AC::TerrainReferenceManifestDto MinimalManifest();

class ReferenceFixture {
public:
    ReferenceFixture()
        : _relativeRoot{std::format(
              ".corsairs-task7-reference-{}",
              std::chrono::steady_clock::now().time_since_epoch().count())},
          _absoluteRoot{std::filesystem::current_path() / _relativeRoot} {
        std::error_code error;
        if (!std::filesystem::create_directory(_absoluteRoot, error) || error) {
            return;
        }
        _ready = WriteText(Absolute("Client/map/garner.map"), "x") &&
            WriteText(Absolute("databases/gamedata.sqlite"), "x") &&
            WriteText(Absolute("Client/texture/terrain/alpha/total.png"), "x") &&
            WriteText(Absolute("Client/texture/terrain/brick05.png"), "x");
        _ready = _ready && CreateRun("run-001");
    }

    ~ReferenceFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(_absoluteRoot, ignored);
    }

    ReferenceFixture(const ReferenceFixture&) = delete;
    ReferenceFixture& operator=(const ReferenceFixture&) = delete;

    [[nodiscard]] bool Ready() const noexcept { return _ready; }

    [[nodiscard]] AC::TerrainReferenceOptions Options() const {
        AC::TerrainReferenceOptions options;
        options.Map = Relative("Client/map/garner.map");
        options.Database = Relative("databases/gamedata.sqlite");
        options.ClientRoot = Relative("Client");
        options.AlphaAtlas = Relative("Client/texture/terrain/alpha/total.png");
        options.Output = Relative("maps");
        return options;
    }

    [[nodiscard]] AC::TerrainReferenceManifestDto Manifest(
        std::string_view runId = "run-001") const {
        AC::TerrainReferenceManifestDto manifest = MinimalManifest();
        manifest.Source.Map = {Relative("Client/map/garner.map"),
                               std::string{kHashOfX}};
        manifest.Source.Database = {Relative("databases/gamedata.sqlite"),
                                    std::string{kHashOfX}};
        manifest.Source.ClientRoot = Relative("Client");
        manifest.Source.AlphaAtlas = {
            Relative("Client/texture/terrain/alpha/total.png"),
            std::string{kHashOfX}};
        manifest.Source.UsedTextures = {{
            4u, Relative("Client/texture/terrain/brick05.png"),
            std::string{kHashOfX}}};
        const auto file = [runId](std::string_view leaf) {
            return AC::TerrainOutputFile{
                std::filesystem::path{"runs"} / runId / leaf,
                std::string{kHashOfX}, 1u};
        };
        manifest.Files.Height = file("garner.height.r16");
        manifest.Files.Block = file("garner.block.raw");
        manifest.Files.Region = file("garner.region.raw");
        manifest.Files.TerrainMetadata = file("garner.terrain.json");
        manifest.Files.Albedo = file("garner.albedo_17_21.png");
        manifest.Files.MeshGltf = file("garner.terrain_17_21.gltf");
        manifest.Files.MeshBin = file("garner.terrain_17_21.bin");
        return manifest;
    }

    bool CreateRun(std::string_view runId) const {
        bool result = true;
        for (const std::string_view leaf : Leaves()) {
            result = result && WriteText(
                Absolute(std::filesystem::path{"maps/runs"} / runId / leaf), "x");
        }
        return result;
    }

    [[nodiscard]] std::optional<AC::TerrainReferenceBuildProducts> BuildProducts(
        const std::filesystem::path& runDirectory,
        std::string& detail) const {
        detail.clear();
        for (const std::string_view leaf : Leaves()) {
            if (!WriteText(runDirectory / leaf, "x")) {
                detail = std::format("не удалось создать test product {}", leaf);
                return std::nullopt;
            }
        }
        AC::TerrainReferenceBuildProducts products;
        products.Source = Manifest().Source;
        products.Bake.Ok = true;
        products.Bake.AlgorithmVersion = "legacy-fixed-pipeline-v1";
        products.Bake.SourceCellBounds = {2176u, 2688u, 128u, 128u};
        products.Bake.PngPath = runDirectory / "garner.albedo_17_21.png";
        products.Bake.PngSha256 = std::string{kHash};
        products.Bake.UsedTextureIds = {4u};
        products.Bake.SectionOriginX = 272u;
        products.Bake.SectionOriginY = 336u;
        products.Bake.SectionGridWidth = 16u;
        products.Bake.SectionGridHeight = 16u;
        products.Bake.SectionPresenceMask.assign(256u, 1u);
        products.Bake.PeakRssBytes = 10u;
        products.Bake.OutputBytes = 1u;
        products.Bake.PeakTextureCacheBytes = 1u;
        products.Bake.PeakRgbaRowBytes = 1u;
        products.Mesh.Ok = true;
        products.Mesh.Step = 4u;
        products.Mesh.Error = {0.0, 0.0, 0.0, 1089u};
        products.Mesh.GltfPath = runDirectory / "garner.terrain_17_21.gltf";
        products.Mesh.BinPath = runDirectory / "garner.terrain_17_21.bin";
        products.Mesh.ActorWorldXcm = 217600.0;
        products.Mesh.ActorWorldYcm = -268800.0;
        const auto built = [&runDirectory](std::string_view leaf) {
            return AC::TerrainBuiltFile{runDirectory / leaf, std::string{kHash}};
        };
        products.Files.Height = built("garner.height.r16");
        products.Files.Block = built("garner.block.raw");
        products.Files.Region = built("garner.region.raw");
        products.Files.TerrainMetadata = built("garner.terrain.json");
        products.Files.Albedo = built("garner.albedo_17_21.png");
        products.Files.MeshGltf = built("garner.terrain_17_21.gltf");
        products.Files.MeshBin = built("garner.terrain_17_21.bin");
        return products;
    }

    [[nodiscard]] std::filesystem::path Absolute(
        const std::filesystem::path& suffix) const {
        return _absoluteRoot / suffix;
    }

private:
    [[nodiscard]] std::filesystem::path Relative(
        const std::filesystem::path& suffix) const {
        return _relativeRoot / suffix;
    }

    static constexpr std::array<std::string_view, 7> Leaves() {
        return {"garner.height.r16", "garner.block.raw", "garner.region.raw",
                "garner.terrain.json", "garner.albedo_17_21.png",
                "garner.terrain_17_21.gltf", "garner.terrain_17_21.bin"};
    }

    std::filesystem::path _relativeRoot;
    std::filesystem::path _absoluteRoot;
    bool _ready{false};
};

AC::TerrainReferenceManifestDto MinimalManifest() {
    AC::TerrainReferenceManifestDto manifest;
    manifest.SchemaVersion = 1u;
    manifest.AlgorithmVersion = "legacy-fixed-pipeline-v1";
    manifest.Source.Map = {"Client/map/garner.map", std::string{kHash}};
    manifest.Source.Database = {"databases/gamedata.sqlite", std::string{kHash}};
    manifest.Source.ClientRoot = "Client";
    manifest.Source.AlphaAtlas = {
        "Client/texture/terrain/alpha/total.png", std::string{kHash}};
    manifest.Source.UsedTextures = {{
        4u, "Client/texture/terrain/brick05.png", std::string{kHash}}};
    manifest.Page.Id = {17u, 21u};
    manifest.Page.SourceCellBounds = {2176u, 2688u, 128u, 128u};
    manifest.Page.PixelsPerCell = 32u;
    manifest.Page.PixelWidth = 4096u;
    manifest.Page.PixelHeight = 4096u;
    manifest.Page.Ambient = {1.0, 1.0, 1.0};
    manifest.Page.DwTColor = 0u;
    manifest.RequiredPresentRect = {2193u, 2756u, 80u, 47u};
    manifest.UsedTextureIds = {4u};
    manifest.SectionPresence = {272u, 336u, 16u, 16u,
                                std::vector<std::uint8_t>(256u, 1u)};

    const auto file = [](std::string_view leaf) {
        return AC::TerrainOutputFile{
            std::filesystem::path{"runs/run-001"} / leaf,
            std::string{kHash},
            1u,
        };
    };
    manifest.Files.Height = file("garner.height.r16");
    manifest.Files.Block = file("garner.block.raw");
    manifest.Files.Region = file("garner.region.raw");
    manifest.Files.TerrainMetadata = file("garner.terrain.json");
    manifest.Files.Albedo = file("garner.albedo_17_21.png");
    manifest.Files.MeshGltf = file("garner.terrain_17_21.gltf");
    manifest.Files.MeshBin = file("garner.terrain_17_21.bin");
    manifest.Metrics = {
        1u, 1u, 1u, 1u, 7u, 0.0, 0.0, 0.0, 0u, 0u,
    };
    return manifest;
}

CORSAIRS_TEST(TerrainReferenceManifest_SerializesAndParsesDeterministically) {
    const AC::TerrainReferenceManifestDto manifest = MinimalManifest();
    const std::string first = AC::SerializeTerrainReferenceManifest(manifest);
    const std::string second = AC::SerializeTerrainReferenceManifest(manifest);
    REQUIRE_EQ(first, second);

    std::vector<AC::TerrainManifestIssue> issues;
    const auto parsed = AC::ParseTerrainReferenceManifest(first, issues);
    REQUIRE(parsed.has_value());
    REQUIRE(issues.empty());
    REQUIRE_EQ(parsed->SchemaVersion, 1u);
    REQUIRE_EQ(parsed->Source.UsedTextures.size(), 1u);
    REQUIRE_EQ(parsed->Files.MeshBin.SizeBytes, 1u);
    REQUIRE_EQ(AC::SerializeTerrainReferenceManifest(*parsed), first);
}

CORSAIRS_TEST(TerrainReferenceManifest_RejectsMissingRequiredMember) {
    const std::string complete =
        AC::SerializeTerrainReferenceManifest(MinimalManifest());
    const std::string needle = "\"algorithmVersion\":\"legacy-fixed-pipeline-v1\",";
    const std::size_t offset = complete.find(needle);
    REQUIRE(offset != std::string::npos);
    std::string missing = complete;
    missing.erase(offset, needle.size());

    std::vector<AC::TerrainManifestIssue> issues;
    const auto parsed = AC::ParseTerrainReferenceManifest(missing, issues);
    REQUIRE(!parsed.has_value());
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_SCHEMA);
    REQUIRE_EQ(issues[0].Field, std::string{"/algorithmVersion"});
    REQUIRE_EQ(issues[0].Detail, std::string{"required field is missing"});
}

CORSAIRS_TEST(TerrainReferenceManifest_RejectsMalformedDuplicateUnknownAndTypes) {
    const std::string complete =
        AC::SerializeTerrainReferenceManifest(MinimalManifest());
    std::vector<std::string> invalid;
    invalid.push_back("{");
    invalid.push_back(complete + " trailing");

    std::string duplicate = complete;
    duplicate.replace(
        duplicate.find("\"schemaVersion\":1"),
        std::string_view{"\"schemaVersion\":1"}.size(),
        "\"schemaVersion\":1,\"schemaVersion\":1");
    invalid.push_back(std::move(duplicate));

    std::string unknown = complete;
    unknown.insert(1u, "\"unknown\":0,");
    invalid.push_back(std::move(unknown));

    std::string wrongType = complete;
    wrongType.replace(
        wrongType.find("\"schemaVersion\":1"),
        std::string_view{"\"schemaVersion\":1"}.size(),
        "\"schemaVersion\":\"1\"");
    invalid.push_back(std::move(wrongType));

    std::string overflow = complete;
    overflow.replace(
        overflow.find("\"schemaVersion\":1"),
        std::string_view{"\"schemaVersion\":1"}.size(),
        "\"schemaVersion\":4294967296");
    invalid.push_back(std::move(overflow));

    std::string nonFinite = complete;
    nonFinite.replace(
        nonFinite.find("\"ambient\":[1,1,1]"),
        std::string_view{"\"ambient\":[1,1,1]"}.size(),
        "\"ambient\":[1e999,1,1]");
    invalid.push_back(std::move(nonFinite));

    for (const std::string& json : invalid) {
        std::vector<AC::TerrainManifestIssue> issues;
        REQUIRE(!AC::ParseTerrainReferenceManifest(json, issues).has_value());
        REQUIRE_EQ(issues.size(), 1u);
        REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_SCHEMA);
        REQUIRE(!issues[0].Detail.empty());
    }
}

CORSAIRS_TEST(TerrainReferenceManifest_RequiredKeyAndTypeBehaviorMatrix) {
    const std::string complete =
        AC::SerializeTerrainReferenceManifest(MinimalManifest());
    constexpr std::array keys{
        "schemaVersion", "algorithmVersion", "source", "page",
        "requiredPresentRect", "usedTextureIds", "sectionPresence", "files",
        "metrics", "map", "database", "clientRoot", "alphaAtlas",
        "usedTextures", "path", "sha256", "textureId", "x", "y",
        "sourceCellBounds", "pixelsPerCell", "pixelWidth", "pixelHeight",
        "ambient", "dwTColor", "width", "height", "originX", "originY",
        "rowMajorMask", "block", "region", "terrainMetadata", "albedo",
        "meshGltf", "meshBin", "sizeBytes", "peakRssBytes",
        "peakTextureCacheBytes", "peakRgbaRowBytes", "pngBytes",
        "totalOutputBytes", "maxHeightErrorCm", "rmsHeightErrorCm",
        "sharedBoundaryMaxCm", "absentSectionCount", "unresolvedLayerCount",
    };
    for (const std::string_view key : keys) {
        const auto missing = RemoveFirstJsonMember(complete, key);
        REQUIRE(missing.has_value());
        std::vector<AC::TerrainManifestIssue> issues;
        REQUIRE(!AC::ParseTerrainReferenceManifest(
                     *missing, issues).has_value());
        REQUIRE(!issues.empty());
        REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_SCHEMA);

        std::string wrongType = complete;
        const auto value = JsonMemberValueRange(wrongType, key);
        REQUIRE(value.has_value());
        const char first = wrongType[value->first];
        const std::string replacement = first == '{'
            ? "[]"
            : first == '[' ? "{}" : first == '"' ? "0" : "\"invalid\"";
        wrongType.replace(
            value->first, value->second - value->first, replacement);
        issues.clear();
        REQUIRE(!AC::ParseTerrainReferenceManifest(
                     wrongType, issues).has_value());
        REQUIRE(!issues.empty());
        REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_SCHEMA);
    }
}

CORSAIRS_TEST(TerrainReferenceManifest_RejectsInvalidUtf8String) {
    std::string json = AC::SerializeTerrainReferenceManifest(MinimalManifest());
    const std::size_t offset = json.find("Client/map/garner.map");
    REQUIRE(offset != std::string::npos);
    json[offset] = static_cast<char>(0xff);
    std::vector<AC::TerrainManifestIssue> issues;
    REQUIRE(!AC::ParseTerrainReferenceManifest(json, issues).has_value());
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE_EQ(issues[0].Field, std::string{"/source/map/path"});
    REQUIRE_EQ(issues[0].Detail, std::string{"invalid UTF-8 string"});
}

CORSAIRS_TEST(TerrainReferenceManifest_RejectsExcessiveJsonNesting) {
    std::string json(512u, '[');
    json += "null";
    json.append(512u, ']');

    std::vector<AC::TerrainManifestIssue> issues;
    REQUIRE(!AC::ParseTerrainReferenceManifest(json, issues).has_value());
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_SCHEMA);
    REQUIRE(issues[0].Detail.contains("nesting exceeds 128"));
}

CORSAIRS_TEST(TerrainManifest_RejectsZeroPeakRss) {
    AC::TerrainReferenceManifestDto manifest = MinimalManifest();
    manifest.Metrics.PeakRssBytes = 0u;
    AC::TerrainReferenceOptions limits;
    limits.Bake.MaxRssBytes = 128u * 1024u * 1024u;
    const auto issues = AC::ValidateTerrainReferenceManifest(manifest, {}, limits);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_METRIC);
    REQUIRE_EQ(issues[0].Field, std::string{"/metrics/peakRssBytes"});
    REQUIRE_EQ(issues[0].Detail, std::string{"peak RSS must be nonzero"});
}

CORSAIRS_TEST(TerrainManifest_RejectsPeakRssOverBudget) {
    AC::TerrainReferenceManifestDto manifest = MinimalManifest();
    AC::TerrainReferenceOptions limits;
    limits.Bake.MaxRssBytes = 128u * 1024u * 1024u;
    manifest.Metrics.PeakRssBytes = limits.Bake.MaxRssBytes + 1u;
    const auto issues = AC::ValidateTerrainReferenceManifest(manifest, {}, limits);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::BUDGET_EXCEEDED);
    REQUIRE_EQ(issues[0].Field, std::string{"/metrics/peakRssBytes"});
    REQUIRE_EQ(issues[0].Detail, std::string{"peak RSS exceeds configured budget"});
}

CORSAIRS_TEST(TerrainManifest_RejectsSchemaAlgorithmBoundsAndTextureOrdering) {
    AC::TerrainReferenceOptions limits;
    limits.Page = {17u, 21u};
    limits.RequiredPresent = {2193u, 2756u, 80u, 47u};

    AC::TerrainReferenceManifestDto manifest = MinimalManifest();
    manifest.SchemaVersion = 2u;
    auto issues = AC::ValidateTerrainReferenceManifest(manifest, {}, limits);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_SCHEMA);
    REQUIRE_EQ(issues[0].Field, std::string{"/schemaVersion"});

    manifest = MinimalManifest();
    manifest.AlgorithmVersion = "other";
    issues = AC::ValidateTerrainReferenceManifest(manifest, {}, limits);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_ALGORITHM);
    REQUIRE_EQ(issues[0].Field, std::string{"/algorithmVersion"});

    manifest = MinimalManifest();
    manifest.Page.Id.X = 18u;
    issues = AC::ValidateTerrainReferenceManifest(manifest, {}, limits);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_BOUNDS);
    REQUIRE_EQ(issues[0].Field, std::string{"/page"});

    manifest = MinimalManifest();
    manifest.UsedTextureIds = {4u, 4u};
    issues = AC::ValidateTerrainReferenceManifest(manifest, {}, limits);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_TEXTURE_IDS);
    REQUIRE_EQ(issues[0].Field, std::string{"/usedTextureIds"});
}

CORSAIRS_TEST(TerrainManifest_ValidatesAllSevenFilesAndProvenance) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const auto issues = AC::ValidateTerrainReferenceManifest(
        fixture.Manifest(), options.Output, options);
    REQUIRE(issues.empty());
}

CORSAIRS_TEST(TerrainManifest_RejectsRunPathGrammarAndEighthProduct) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const auto requireInvalidFile = [&](AC::TerrainReferenceManifestDto manifest) {
        const auto issues = AC::ValidateTerrainReferenceManifest(
            manifest, options.Output, options);
        REQUIRE_EQ(issues.size(), 1u);
        REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_FILE);
    };

    AC::TerrainReferenceManifestDto manifest = fixture.Manifest();
    manifest.Files.Height.ManifestRelativePath = "garner.height.r16";
    requireInvalidFile(manifest);
    manifest = fixture.Manifest();
    manifest.Files.Height.ManifestRelativePath =
        "runs/run-001/nested/garner.height.r16";
    requireInvalidFile(manifest);
    manifest = fixture.Manifest();
    manifest.Files.Height.ManifestRelativePath =
        "runs/run-002/garner.height.r16";
    requireInvalidFile(manifest);
    manifest = fixture.Manifest();
    manifest.Files.Height.ManifestRelativePath =
        "runs/../run-001/garner.height.r16";
    requireInvalidFile(manifest);
    manifest = fixture.Manifest();
    manifest.Files.Height.ManifestRelativePath =
        fixture.Absolute("maps/runs/run-001/garner.height.r16");
    requireInvalidFile(manifest);
    manifest = fixture.Manifest();
    manifest.Files.Height.ManifestRelativePath =
        "runs/run-001/wrong.height.r16";
    requireInvalidFile(manifest);
    manifest = fixture.Manifest();
    manifest.Files.Height.ManifestRelativePath =
        manifest.Files.Block.ManifestRelativePath;
    requireInvalidFile(manifest);

    REQUIRE(WriteText(fixture.Absolute("maps/runs/run-001/eighth.file"), "x"));
    requireInvalidFile(fixture.Manifest());
}

CORSAIRS_TEST(TerrainManifest_RejectsTamperedOutputAndSource) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    AC::TerrainReferenceManifestDto manifest = fixture.Manifest();
    REQUIRE(WriteText(
        fixture.Absolute("maps/runs/run-001/garner.block.raw"), "y"));
    auto issues = AC::ValidateTerrainReferenceManifest(
        manifest, options.Output, options);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_HASH);
    REQUIRE_EQ(issues[0].Field, std::string{"/files/block/sha256"});

    REQUIRE(WriteText(fixture.Absolute("maps/runs/run-001/garner.block.raw"), "x"));
    REQUIRE(WriteText(fixture.Absolute("Client/map/garner.map"), "y"));
    issues = AC::ValidateTerrainReferenceManifest(manifest, options.Output, options);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_HASH);
    REQUIRE_EQ(issues[0].Field, std::string{"/source/map/sha256"});
}

CORSAIRS_TEST(TerrainManifest_RejectsProvenancePathMismatchAndTextureEscape) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    AC::TerrainReferenceManifestDto manifest = fixture.Manifest();
    manifest.Source.Map.Path = manifest.Source.Database.Path;
    auto issues = AC::ValidateTerrainReferenceManifest(
        manifest, options.Output, options);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_PROVENANCE);
    REQUIRE_EQ(issues[0].Field, std::string{"/source/map/path"});

    manifest = fixture.Manifest();
    manifest.Source.UsedTextures[0].Path =
        std::filesystem::path{"outside"} / "brick05.png";
    issues = AC::ValidateTerrainReferenceManifest(
        manifest, options.Output, options);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_PROVENANCE);
    REQUIRE_EQ(issues[0].Field, std::string{"/source/usedTextures/0/path"});
}

CORSAIRS_TEST(TerrainManifest_RejectsTextureLexicalAliasResolvingIntoClientRoot) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path alias = fixture.Absolute("AliasClient");
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        fixture.Absolute("Client"), alias, linkError);
#if defined(_WIN32)
    if (linkError == std::errc::permission_denied) {
        std::cout << "SKIP: Windows directory symlink privilege unavailable\n";
        return;
    }
#endif
    REQUIRE(!linkError);

    AC::TerrainReferenceManifestDto manifest = fixture.Manifest();
    manifest.Source.UsedTextures[0].Path =
        fixture.Absolute("AliasClient/texture/terrain/brick05.png")
            .lexically_relative(std::filesystem::current_path());
    const auto issues = AC::ValidateTerrainReferenceManifest(
        manifest, options.Output, options);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_PROVENANCE);
    REQUIRE_EQ(issues[0].Field,
               std::string{"/source/usedTextures/0/path"});
    REQUIRE_EQ(issues[0].Detail,
               std::string{"texture path is not lexically beneath client root"});
}

CORSAIRS_TEST(TerrainManifest_RejectsHardLinkedRunProducts) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path height = fixture.Absolute(
        "maps/runs/run-001/garner.height.r16");
    const std::filesystem::path block = fixture.Absolute(
        "maps/runs/run-001/garner.block.raw");
    std::error_code error;
    REQUIRE(std::filesystem::remove(block, error));
    REQUIRE(!error);
    std::filesystem::create_hard_link(height, block, error);
    if (SkipWindowsPrivilegeOrAccessDenied(error, "hard-link")) {
        return;
    }
    REQUIRE(!error);

    const auto issues = AC::ValidateTerrainReferenceManifest(
        fixture.Manifest(), options.Output, options);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_FILE);
    REQUIRE_EQ(issues[0].Field, std::string{"/files/height/path"});
    REQUIRE_EQ(issues[0].Detail,
               std::string{"outputs must have seven distinct physical identities"});
}

CORSAIRS_TEST(TerrainDeterministicProjection_AllowsDifferentValidPeakRss) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.CreateRun("run-002"));
    const AC::TerrainReferenceOptions options = fixture.Options();
    AC::TerrainReferenceManifestDto first = fixture.Manifest("run-001");
    AC::TerrainReferenceManifestDto second = fixture.Manifest("run-002");
    first.Metrics.PeakRssBytes = 10u;
    second.Metrics.PeakRssBytes = 20u;
    const auto issues = AC::CompareTerrainDeterministicManifests(
        first, options.Output, second, options.Output, options);
    REQUIRE(issues.empty());
}

CORSAIRS_TEST(TerrainDeterministicProjection_RejectsAnyOtherFieldDifference) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    REQUIRE(fixture.CreateRun("run-002"));
    const AC::TerrainReferenceOptions options = fixture.Options();
    AC::TerrainReferenceManifestDto first = fixture.Manifest("run-001");
    AC::TerrainReferenceManifestDto second = fixture.Manifest("run-002");
    first.Metrics.PeakRssBytes = 10u;
    second.Metrics.PeakRssBytes = 20u;
    second.Metrics.PeakTextureCacheBytes = 2u;
    const auto issues = AC::CompareTerrainDeterministicManifests(
        first, options.Output, second, options.Output, options);
    REQUIRE_EQ(issues.size(), 1u);
    REQUIRE(issues[0].Code == AC::TerrainManifestIssueCode::INVALID_METRIC);
    REQUIRE_EQ(issues[0].Field, std::string{"/deterministicProjection"});
    REQUIRE_EQ(issues[0].Detail,
               std::string{"manifests differ outside run ID and peak RSS"});
}

CORSAIRS_TEST(TerrainReferenceCommand_RejectsInjectedInvalidBakeBeforePublish) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path top =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(top, "prior-top"));
    std::size_t buildCalls = 0u;
    std::size_t publishCalls = 0u;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path& runDirectory,
            std::string& detail) {
            ++buildCalls;
            auto products = fixture.BuildProducts(runDirectory, detail);
            if (products.has_value()) {
                products->Bake.Ok = false;
            }
            return products;
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path&, std::string_view) {
            ++publishCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(
                   options, dependencies, output, error), 1);
    REQUIRE_EQ(buildCalls, 1u);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE_EQ(ReadText(top).value_or(""), std::string{"prior-top"});
}

CORSAIRS_TEST(TerrainReferenceCommand_RejectsInjectedInvalidMeshBeforePublish) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path top =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(top, "prior-top"));
    std::size_t buildCalls = 0u;
    std::size_t publishCalls = 0u;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path& runDirectory,
            std::string& detail) {
            ++buildCalls;
            auto products = fixture.BuildProducts(runDirectory, detail);
            if (products.has_value()) {
                products->Mesh.Ok = false;
            }
            return products;
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path&, std::string_view) {
            ++publishCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(
                   options, dependencies, output, error), 1);
    REQUIRE_EQ(buildCalls, 1u);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE_EQ(ReadText(top).value_or(""), std::string{"prior-top"});
}

CORSAIRS_TEST(TerrainReferenceCommand_RejectsSerializedRoundTripBeforePublish) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path top =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(top, "prior-top"));
    std::size_t buildCalls = 0u;
    std::size_t publishCalls = 0u;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path& runDirectory,
            std::string& detail) {
            ++buildCalls;
            auto products = fixture.BuildProducts(runDirectory, detail);
            if (products.has_value()) {
                products->Source.Map.Sha256[0] = static_cast<char>(0xff);
            }
            return products;
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path&, std::string_view) {
            ++publishCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(
                   options, dependencies, output, error), 1);
    REQUIRE_EQ(buildCalls, 1u);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE_EQ(ReadText(top).value_or(""), std::string{"prior-top"});
}

CORSAIRS_TEST(TerrainReferenceCommand_PublishesValidatedDtoExactlyOnce) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    std::size_t buildCalls = 0u;
    std::size_t publishCalls = 0u;
    std::filesystem::path runDirectory;
    std::filesystem::path destination;
    std::string captured;
    bool receivedOptionsMatch = false;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions& received,
            const std::filesystem::path& run,
            std::string& detail) {
            ++buildCalls;
            receivedOptionsMatch = received.Output == options.Output;
            runDirectory = run;
            return fixture.BuildProducts(run, detail);
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path& path, std::string_view json) {
            ++publishCalls;
            destination = path;
            captured = json;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(
                   options, dependencies, output, error), 0);
    REQUIRE_EQ(buildCalls, 1u);
    REQUIRE(receivedOptionsMatch);
    REQUIRE_EQ(publishCalls, 1u);
    REQUIRE(destination == options.Output / "garner.reference-albedo.json");
    REQUIRE(runDirectory.parent_path() == options.Output / "runs");
    REQUIRE(runDirectory.filename().string().starts_with("run-"));
    REQUIRE(!std::filesystem::exists(runDirectory / "garner.reference-albedo.json"));
    std::vector<AC::TerrainManifestIssue> parseIssues;
    const auto manifest = AC::ParseTerrainReferenceManifest(captured, parseIssues);
    REQUIRE(manifest.has_value());
    REQUIRE(parseIssues.empty());
    REQUIRE(AC::ValidateTerrainReferenceManifest(
                *manifest, options.Output, options).empty());
    REQUIRE(error.str().empty());
    REQUIRE(output.str().contains("garner.reference-albedo.json"));
}

CORSAIRS_TEST(TerrainReferenceCommand_DurabilizesSevenProductsBeforePublish) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path top =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(top, "prior-top"));
    std::size_t durabilityCalls = 0u;
    std::size_t publishCalls = 0u;
    bool pathsSizeCorrect = false;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path& run,
            std::string& detail) {
            return fixture.BuildProducts(run, detail);
        };
    dependencies.DurabilizeRunProducts =
        [&](std::span<const std::filesystem::path> paths,
            std::string& detail) {
            ++durabilityCalls;
            pathsSizeCorrect = paths.size() == 7u;
            detail = "injected FlushFile failure";
            return false;
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path&, std::string_view) {
            ++publishCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(
                   options, dependencies, output, error), 1);
    REQUIRE_EQ(durabilityCalls, 1u);
    REQUIRE(pathsSizeCorrect);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE_EQ(ReadText(top).value_or(""), std::string{"prior-top"});
    REQUIRE(error.str().contains("injected FlushFile failure"));
}

CORSAIRS_TEST(TerrainReferenceCommand_RejectsMutationDuringRunDurability) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    std::size_t publishCalls = 0u;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path& run,
            std::string& detail) {
            return fixture.BuildProducts(run, detail);
        };
    dependencies.DurabilizeRunProducts =
        [](std::span<const std::filesystem::path> paths,
           std::string& detail) {
            detail.clear();
            return paths.size() == 7u && WriteText(paths[1], "mutated");
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path&, std::string_view) {
            ++publishCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(
                   options, dependencies, output, error), 1);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE(error.str().contains("changed during durability"));
}

CORSAIRS_TEST(TerrainReferenceCommand_RejectsHardLinkedProductsBeforeDurability) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    std::size_t durabilityCalls = 0u;
    std::size_t publishCalls = 0u;
    bool hardLinkPermissionUnavailable = false;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path& run,
            std::string& detail) {
            auto products = fixture.BuildProducts(run, detail);
            if (!products.has_value()) {
                return products;
            }
            std::error_code error;
            std::filesystem::remove(run / "garner.block.raw", error);
            if (!error) {
                std::filesystem::create_hard_link(
                    run / "garner.height.r16", run / "garner.block.raw", error);
            }
            if (SkipWindowsPrivilegeOrAccessDenied(error, "hard-link")) {
                hardLinkPermissionUnavailable = true;
            }
            if (error) {
                detail = error.message();
                return std::optional<AC::TerrainReferenceBuildProducts>{};
            }
            return products;
        };
    dependencies.DurabilizeRunProducts =
        [&](std::span<const std::filesystem::path>, std::string&) {
            ++durabilityCalls;
            return true;
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path&, std::string_view) {
            ++publishCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    const int exitCode = AC::RunTerrainReference(
        options, dependencies, output, error);
    if (hardLinkPermissionUnavailable) {
        return;
    }
    REQUIRE_EQ(exitCode, 1);
    REQUIRE_EQ(durabilityCalls, 0u);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE(error.str().contains("distinct physical identities"));
}

CORSAIRS_TEST(TerrainReferenceCommand_RejectsHardLinkSwapDuringDurability) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    std::size_t publishCalls = 0u;
    bool hardLinkPermissionUnavailable = false;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path& run,
            std::string& detail) {
            return fixture.BuildProducts(run, detail);
        };
    dependencies.DurabilizeRunProducts =
        [&](std::span<const std::filesystem::path> paths,
           std::string& detail) {
            std::error_code error;
            std::filesystem::remove(paths[1], error);
            if (!error) {
                std::filesystem::create_hard_link(paths[0], paths[1], error);
            }
            if (SkipWindowsPrivilegeOrAccessDenied(error, "hard-link")) {
                hardLinkPermissionUnavailable = true;
            }
            detail = error.message();
            return !error;
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path&, std::string_view) {
            ++publishCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    const int exitCode = AC::RunTerrainReference(
        options, dependencies, output, error);
    if (hardLinkPermissionUnavailable) {
        return;
    }
    REQUIRE_EQ(exitCode, 1);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE(error.str().contains("physical identity changed during durability"));
}

CORSAIRS_TEST(TerrainReferenceCommand_EachProductFlushFailureBlocksPublication) {
    for (std::size_t failedIndex = 0u; failedIndex < 7u; ++failedIndex) {
        ReferenceFixture fixture;
        REQUIRE(fixture.Ready());
        const AC::TerrainReferenceOptions options = fixture.Options();
        std::size_t publishCalls = 0u;
        bool pathsSizeCorrect = false;
        AC::TerrainReferenceDependencies dependencies;
        dependencies.BuildProducts =
            [&](const AC::TerrainReferenceOptions&,
                const std::filesystem::path& run,
                std::string& detail) {
                return fixture.BuildProducts(run, detail);
            };
        dependencies.DurabilizeRunProducts =
            [failedIndex, &pathsSizeCorrect](
                std::span<const std::filesystem::path> paths,
                std::string& detail) {
                pathsSizeCorrect = paths.size() == 7u;
                detail = std::format("injected FlushFile index {}", failedIndex);
                return false;
            };
        dependencies.AtomicPublish =
            [&](const std::filesystem::path&, std::string_view) {
                ++publishCalls;
                return AC::TerrainPublicationResult{
                    AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
            };
        std::ostringstream output;
        std::ostringstream error;
        REQUIRE_EQ(AC::RunTerrainReference(
                       options, dependencies, output, error), 1);
        REQUIRE(pathsSizeCorrect);
        REQUIRE_EQ(publishCalls, 0u);
        REQUIRE(error.str().contains(
            std::format("injected FlushFile index {}", failedIndex)));
    }
}

CORSAIRS_TEST(TerrainRunDurability_SharedPlanCoversEveryFileAndBarrierFailure) {
    using Action = AC::TerrainRunDurabilityAction;
    const auto plan = AC::PlanTerrainRunDurability(7u);
    REQUIRE_EQ(plan.size(), 8u);
    for (std::size_t index = 0u; index < 7u; ++index) {
        REQUIRE(plan[index].Action == Action::FLUSH_FILE);
        REQUIRE_EQ(plan[index].ProductIndex, index);
    }
    REQUIRE(plan.back().Action == Action::SYNC_DIRECTORY_OR_EQUIVALENT);

    for (std::size_t failedStep = 0u; failedStep < plan.size(); ++failedStep) {
        std::size_t calls = 0u;
        std::string detail;
        REQUIRE(!AC::ExecuteTerrainRunDurabilitySteps(
            plan,
            [&](const AC::TerrainRunDurabilityStep& step,
                std::string& stepDetail) {
                if (calls++ == failedStep) {
                    stepDetail = std::format(
                        "injected run durability step {} product {}",
                        failedStep, step.ProductIndex);
                    return false;
                }
                return true;
            },
            detail));
        REQUIRE_EQ(calls, failedStep + 1u);
        REQUIRE_EQ(detail,
                   std::format(
                       "injected run durability step {} product {}",
                       failedStep, plan[failedStep].ProductIndex));
    }
}

CORSAIRS_TEST(TerrainReferencePublisher_ReplacesOnlyTopManifestAndCleansOwnedState) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(result.Status == AC::TerrainPublicationStatus::OK);
    REQUIRE_EQ(ReadText(destination).value_or(""), json);
    REQUIRE(!std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.json"));
    REQUIRE(!std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.json.retired"));
    REQUIRE(!std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.lock"));
    REQUIRE(!std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.lock.retired"));
    REQUIRE_EQ(ReadText(options.Output /
                        "runs/run-001/garner.height.r16").value_or(""),
               std::string{"x"});
}

CORSAIRS_TEST(TerrainReferencePublisher_RetainedExclusiveHandleProtectsForeignFile) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::filesystem::path foreign = options.Output / "foreign-sentinel";
    REQUIRE(WriteText(foreign, "must-survive"));
    bool swapped = false;
    std::filesystem::path displaced;
    std::string swapError;

    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination,
            AC::SerializeTerrainReferenceManifest(fixture.Manifest()),
            options,
            [&](std::string_view point)
                -> AC::TerrainReferenceFaultAction {
                if (!swapped &&
                    point == "MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE") {
                    for (const auto& entry :
                         std::filesystem::directory_iterator(options.Output)) {
                        const std::string leaf =
                            entry.path().filename().generic_string();
                        if (leaf.starts_with(
                                ".garner.reference-albedo.temp.")) {
                            displaced = entry.path();
                            displaced += ".opened";
                            std::error_code error;
                            std::filesystem::rename(entry.path(), displaced, error);
                            if (!error) {
                                std::filesystem::create_symlink(
                                    foreign, entry.path(), error);
                            }
                            if (error) {
                                swapError = error.message();
                            }
                            else {
                                swapped = true;
                            }
                            break;
                        }
                    }
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

#if defined(_WIN32)
    REQUIRE(!swapped);
    REQUIRE(!swapError.empty());
    REQUIRE(result.Status == AC::TerrainPublicationStatus::OK);
#else
    REQUIRE(swapped);
    REQUIRE(swapError.empty());
    REQUIRE(result.Status != AC::TerrainPublicationStatus::OK);
#endif
    REQUIRE_EQ(ReadText(foreign).value_or(""), std::string{"must-survive"});
#if defined(_WIN32)
    REQUIRE_EQ(ReadText(destination).value_or(""),
               AC::SerializeTerrainReferenceManifest(fixture.Manifest()));
#else
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{"prior-top"});
#endif
}

CORSAIRS_TEST(TerrainReferencePublisher_PreCommitFailureRestoresPriorBytes) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool injected = false;
    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!injected && point == "MANIFEST_AFTER_REPLACE") {
                    injected = true;
                    return AC::TerrainReferenceFaultAction::FAIL;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(injected);
    REQUIRE(result.Status == AC::TerrainPublicationStatus::WRITE_FAILED);
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{"prior-top"});
    REQUIRE(!std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.json"));
    REQUIRE(!std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.lock"));
}

CORSAIRS_TEST(TerrainReferencePublisher_PreCommitFaultMatrixRestoresPriorOrAbsence) {
    constexpr std::array<std::string_view, 8> points{
        "MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE",
        "MANIFEST_AFTER_TEMP_FLUSH",
        "MANIFEST_AFTER_BACKUP_FLUSH",
        "MANIFEST_AFTER_PREPARED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_REPLACE",
        "MANIFEST_AFTER_REPLACE_DURABILITY_BARRIER",
        "MANIFEST_AFTER_REPLACED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_READBACK_VERIFY",
    };
    for (const std::string_view point : points) {
        for (const bool priorExists : {false, true}) {
            ReferenceFixture fixture;
            REQUIRE(fixture.Ready());
            const AC::TerrainReferenceOptions options = fixture.Options();
            const std::filesystem::path destination =
                options.Output / "garner.reference-albedo.json";
            if (priorExists) {
                REQUIRE(WriteText(destination, "prior-top"));
            }
            const std::string json =
                AC::SerializeTerrainReferenceManifest(fixture.Manifest());
            bool injected = false;
            const AC::TerrainPublicationResult result =
                AC::PublishTerrainReferenceManifestForTesting(
                    destination, json, options,
                    [&](std::string_view candidate) {
                        if (!injected && candidate == point) {
                            injected = true;
                            return AC::TerrainReferenceFaultAction::FAIL;
                        }
                        return AC::TerrainReferenceFaultAction::NONE;
                    });
            REQUIRE(injected);
            REQUIRE(result.Status == AC::TerrainPublicationStatus::WRITE_FAILED);
            if (priorExists) {
                REQUIRE_EQ(ReadText(destination).value_or(""),
                           std::string{"prior-top"});
            }
            else {
                REQUIRE(!std::filesystem::exists(destination));
            }
            REQUIRE(!std::filesystem::exists(
                options.Output / ".garner.reference-albedo.publish.json"));
            REQUIRE(!std::filesystem::exists(
                options.Output / ".garner.reference-albedo.publish.lock"));
        }
    }
}

CORSAIRS_TEST(TerrainReferencePublisher_FreshStartupRecoversPreCommitCrash) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool injected = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!injected &&
                    point == "MANIFEST_AFTER_REPLACED_JOURNAL_DURABLE") {
                    injected = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(injected);
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::WRITE_FAILED);
    REQUIRE(std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.json"));
    REQUIRE(std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.lock"));

    const AC::TerrainPublicationResult recovered =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(recovered.Status == AC::TerrainPublicationStatus::OK);
    REQUIRE_EQ(ReadText(destination).value_or(""), json);
    REQUIRE(!std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.json"));
    REQUIRE(!std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.lock"));
}

CORSAIRS_TEST(TerrainReferencePublisher_StartupReportsOriginalCommandAndAllEvidence) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions original = fixture.Options();
    const std::filesystem::path destination =
        original.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool injected = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, original,
            [&](std::string_view point) {
                if (!injected &&
                    point == "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE") {
                    injected = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(injected);
    REQUIRE(crashed.Status ==
            AC::TerrainPublicationStatus::RECOVERY_REQUIRED);

    AC::TerrainReferenceOptions different = original;
    different.Map = original.Map.parent_path() / "restart-b.map";
    REQUIRE(WriteText(fixture.Absolute("Client/map/restart-b.map"), "x"));
    const AC::TerrainPublicationResult startup =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, different, {});

    REQUIRE(startup.Status ==
            AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(startup.RecoveryCommand.contains(ShellQuote(original.Map)));
    REQUIRE(!startup.RecoveryCommand.contains(ShellQuote(different.Map)));
    const std::filesystem::path journal =
        original.Output / ".garner.reference-albedo.publish.json";
    const std::filesystem::path lock =
        original.Output / ".garner.reference-albedo.publish.lock";
    REQUIRE(startup.RecoveryPaths.size() >= 2u);
    REQUIRE(std::ranges::find(startup.RecoveryPaths, journal) !=
            startup.RecoveryPaths.end());
    REQUIRE(std::ranges::find(startup.RecoveryPaths, lock) !=
            startup.RecoveryPaths.end());
}

CORSAIRS_TEST(TerrainReferencePublisher_CommittedCrashRetainsNewManifest) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool injected = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!injected &&
                    point == "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE") {
                    injected = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(injected);
    REQUIRE(crashed.Status ==
            AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE_EQ(ReadText(destination).value_or(""), json);

    const AC::TerrainPublicationResult recovered =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(recovered.Status == AC::TerrainPublicationStatus::OK);
    REQUIRE_EQ(ReadText(destination).value_or(""), json);
}

CORSAIRS_TEST(TerrainReferencePublisher_CommittedRecoveryRejectsModeMutation) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool injected = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!injected &&
                    point == "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE") {
                    injected = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(injected);
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    std::error_code modeError;
    std::filesystem::permissions(
        destination, std::filesystem::perms::owner_read,
        std::filesystem::perm_options::replace, modeError);
    REQUIRE(!modeError);

    const AC::TerrainPublicationResult recovered =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(recovered.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE_EQ(recovered.Detail,
               std::string{"committed publication target mode differs from journal"});
    REQUIRE(std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.json"));
    std::filesystem::permissions(
        destination, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, modeError);
}

CORSAIRS_TEST(TerrainReferencePublisher_FullLiteralFaultMatrixIsReachable) {
    constexpr std::array<std::string_view, 18> points{
        "MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE",
        "MANIFEST_AFTER_TEMP_FLUSH",
        "MANIFEST_AFTER_BACKUP_FLUSH",
        "MANIFEST_AFTER_PREPARED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_REPLACE",
        "MANIFEST_AFTER_REPLACE_DURABILITY_BARRIER",
        "MANIFEST_AFTER_REPLACED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_READBACK_VERIFY",
        "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE",
        "MANIFEST_AFTER_JOURNAL_RETIRE",
        "MANIFEST_ROLLBACK_REPLACE",
        "MANIFEST_ROLLBACK_AFTER_MODE_STAGE_FLUSH",
        "MANIFEST_ROLLBACK_DURABILITY_BARRIER",
        "MANIFEST_ROLLBACK_VERIFY",
        "MANIFEST_AFTER_LOCK_RESERVATION_DURABLE",
        "MANIFEST_AFTER_LOCK_MOVE_TO_RETIRED",
        "MANIFEST_AFTER_LOCK_DELETE_PENDING",
    };
    const std::set<std::string_view> rollbackPoints{
        "MANIFEST_ROLLBACK_REPLACE",
        "MANIFEST_ROLLBACK_AFTER_MODE_STAGE_FLUSH",
        "MANIFEST_ROLLBACK_DURABILITY_BARRIER",
        "MANIFEST_ROLLBACK_VERIFY",
    };
    const std::set<std::string_view> committedPoints{
        "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE",
        "MANIFEST_AFTER_JOURNAL_RETIRE",
        "MANIFEST_AFTER_LOCK_RESERVATION_DURABLE",
        "MANIFEST_AFTER_LOCK_MOVE_TO_RETIRED",
        "MANIFEST_AFTER_LOCK_DELETE_PENDING",
    };
    for (const std::string_view point : points) {
        ReferenceFixture fixture;
        REQUIRE(fixture.Ready());
        const AC::TerrainReferenceOptions options = fixture.Options();
        const std::filesystem::path destination =
            options.Output / "garner.reference-albedo.json";
        REQUIRE(WriteText(destination, "prior-top"));
        const std::string json =
            AC::SerializeTerrainReferenceManifest(fixture.Manifest());
        bool publicationTrigger = false;
        bool targetInjected = false;
        const AC::TerrainPublicationResult result =
            AC::PublishTerrainReferenceManifestForTesting(
                destination, json, options,
                [&](std::string_view candidate) {
                    if (rollbackPoints.contains(point) &&
                        !publicationTrigger &&
                        candidate == "MANIFEST_AFTER_REPLACE") {
                        publicationTrigger = true;
                        return AC::TerrainReferenceFaultAction::FAIL;
                    }
                    if (!targetInjected && candidate == point) {
                        targetInjected = true;
                        return committedPoints.contains(point)
                            ? AC::TerrainReferenceFaultAction::CRASH
                            : AC::TerrainReferenceFaultAction::FAIL;
                    }
                    return AC::TerrainReferenceFaultAction::NONE;
                });
        REQUIRE(targetInjected);
        REQUIRE(result.Status != AC::TerrainPublicationStatus::OK);
        if (committedPoints.contains(point)) {
            REQUIRE_EQ(ReadText(destination).value_or(""), json);
        }
        else if (!rollbackPoints.contains(point)) {
            REQUIRE_EQ(ReadText(destination).value_or(""),
                       std::string{"prior-top"});
        }
        else {
            REQUIRE(std::filesystem::exists(
                options.Output / ".garner.reference-albedo.publish.json"));
        }
        const AC::TerrainPublicationResult freshRecovery =
            AC::PublishTerrainReferenceManifestForTesting(
                destination, json, options, {});
        REQUIRE(freshRecovery.Status == AC::TerrainPublicationStatus::OK);
        REQUIRE_EQ(ReadText(destination).value_or(""), json);
    }
}

CORSAIRS_TEST(TerrainReferencePublisher_FullLiteralCrashMatrixRecoversFreshProcess) {
    constexpr std::array<std::string_view, 18> points{
        "MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE",
        "MANIFEST_AFTER_TEMP_FLUSH",
        "MANIFEST_AFTER_BACKUP_FLUSH",
        "MANIFEST_AFTER_PREPARED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_REPLACE",
        "MANIFEST_AFTER_REPLACE_DURABILITY_BARRIER",
        "MANIFEST_AFTER_REPLACED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_READBACK_VERIFY",
        "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE",
        "MANIFEST_AFTER_JOURNAL_RETIRE",
        "MANIFEST_ROLLBACK_REPLACE",
        "MANIFEST_ROLLBACK_AFTER_MODE_STAGE_FLUSH",
        "MANIFEST_ROLLBACK_DURABILITY_BARRIER",
        "MANIFEST_ROLLBACK_VERIFY",
        "MANIFEST_AFTER_LOCK_RESERVATION_DURABLE",
        "MANIFEST_AFTER_LOCK_MOVE_TO_RETIRED",
        "MANIFEST_AFTER_LOCK_DELETE_PENDING",
    };
    const std::set<std::string_view> rollbackPoints{
        "MANIFEST_ROLLBACK_REPLACE",
        "MANIFEST_ROLLBACK_AFTER_MODE_STAGE_FLUSH",
        "MANIFEST_ROLLBACK_DURABILITY_BARRIER",
        "MANIFEST_ROLLBACK_VERIFY",
    };
    for (const std::string_view point : points) {
        ReferenceFixture fixture;
        REQUIRE(fixture.Ready());
        const AC::TerrainReferenceOptions options = fixture.Options();
        const std::filesystem::path destination =
            options.Output / "garner.reference-albedo.json";
        REQUIRE(WriteText(destination, "prior-top"));
        const std::string json =
            AC::SerializeTerrainReferenceManifest(fixture.Manifest());
        bool publicationTrigger = false;
        bool targetInjected = false;
        const AC::TerrainPublicationResult crashed =
            AC::PublishTerrainReferenceManifestForTesting(
                destination, json, options,
                [&](std::string_view candidate) {
                    if (rollbackPoints.contains(point) &&
                        !publicationTrigger &&
                        candidate == "MANIFEST_AFTER_REPLACE") {
                        publicationTrigger = true;
                        return AC::TerrainReferenceFaultAction::FAIL;
                    }
                    if (!targetInjected && candidate == point) {
                        targetInjected = true;
                        return AC::TerrainReferenceFaultAction::CRASH;
                    }
                    return AC::TerrainReferenceFaultAction::NONE;
                });
        REQUIRE(targetInjected);
        REQUIRE(crashed.Status != AC::TerrainPublicationStatus::OK);
        const AC::TerrainPublicationResult recovered =
            AC::PublishTerrainReferenceManifestForTesting(
                destination, json, options, {});
        REQUIRE(recovered.Status == AC::TerrainPublicationStatus::OK);
        REQUIRE_EQ(ReadText(destination).value_or(""), json);
    }
}

CORSAIRS_TEST(TerrainReferencePublisher_PreCommitCrashMatrixCoversAbsentAndPrior) {
    constexpr std::array<std::string_view, 8> points{
        "MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE",
        "MANIFEST_AFTER_TEMP_FLUSH",
        "MANIFEST_AFTER_BACKUP_FLUSH",
        "MANIFEST_AFTER_PREPARED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_REPLACE",
        "MANIFEST_AFTER_REPLACE_DURABILITY_BARRIER",
        "MANIFEST_AFTER_REPLACED_JOURNAL_DURABLE",
        "MANIFEST_AFTER_READBACK_VERIFY"};
    for (const std::string_view point : points) {
        for (const bool priorExists : {false, true}) {
            ReferenceFixture fixture;
            REQUIRE(fixture.Ready());
            const AC::TerrainReferenceOptions options = fixture.Options();
            const std::filesystem::path destination =
                options.Output / "garner.reference-albedo.json";
            if (priorExists) {
                REQUIRE(WriteText(destination, "nondefault-prior"));
            }
            const std::string json =
                AC::SerializeTerrainReferenceManifest(fixture.Manifest());
            bool reached = false;
            const AC::TerrainPublicationResult crashed =
                AC::PublishTerrainReferenceManifestForTesting(
                    destination, json, options,
                    [&](std::string_view candidate) {
                        if (!reached && candidate == point) {
                            reached = true;
                            return AC::TerrainReferenceFaultAction::CRASH;
                        }
                        return AC::TerrainReferenceFaultAction::NONE;
                    });
            REQUIRE(reached);
            REQUIRE(crashed.Status != AC::TerrainPublicationStatus::OK);
            const AC::TerrainPublicationResult replayed =
                AC::PublishTerrainReferenceManifestForTesting(
                    destination, json, options, {});
            REQUIRE(replayed.Status == AC::TerrainPublicationStatus::OK);
            REQUIRE_EQ(ReadText(destination).value_or(""), json);
        }
    }
}

CORSAIRS_TEST(TerrainReferencePublisher_StartupRecoveryRetiresJournalBeforeRemoval) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool firstFault = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!firstFault &&
                    point == "MANIFEST_AFTER_REPLACED_JOURNAL_DURABLE") {
                    firstFault = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::WRITE_FAILED);
    const std::filesystem::path journal =
        options.Output / ".garner.reference-albedo.publish.json";
    const std::string originalJournal = ReadText(journal).value_or("");
    REQUIRE(!originalJournal.empty());

    bool recoveryFault = false;
    const AC::TerrainPublicationResult recoveryCrash =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!recoveryFault &&
                    point == "MANIFEST_AFTER_JOURNAL_RETIRE") {
                    recoveryFault = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(recoveryFault);
    REQUIRE(recoveryCrash.Status ==
            AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    const std::filesystem::path retired =
        options.Output / ".garner.reference-albedo.publish.json.retired";
    const std::string retiredBytes = ReadText(retired).value_or("");
    REQUIRE(!retiredBytes.empty());
    REQUIRE(retiredBytes != originalJournal);
    REQUIRE(!retiredBytes.contains("\"rollbackIdentity\":\"\""));
    REQUIRE(retiredBytes.contains("\"phase\":\"REPLACED\""));
}

CORSAIRS_TEST(TerrainReferencePublisher_JournalUpdateSwapIsRetainedAndBlocksStartup) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    std::filesystem::path update;
    std::filesystem::path displaced;
    bool swapped = false;
    const AC::TerrainPublicationResult failed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!swapped &&
                    point ==
                        "MANIFEST_JOURNAL_UPDATE_AFTER_EXCLUSIVE_CREATE") {
                    for (const auto& entry :
                         std::filesystem::directory_iterator{options.Output}) {
                        if (entry.path().filename().generic_string().starts_with(
                                ".garner.reference-albedo.publish.update.")) {
                            update = entry.path();
                            break;
                        }
                    }
                    if (!update.empty()) {
                        displaced = update;
                        displaced += ".owned";
                        std::error_code error;
                        std::filesystem::rename(update, displaced, error);
                        swapped = !error &&
                            WriteText(update, "foreign-update-sentinel");
                    }
                    return AC::TerrainReferenceFaultAction::FAIL;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

    REQUIRE(swapped);
    REQUIRE(failed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE_EQ(ReadText(update).value_or(""),
               std::string{"foreign-update-sentinel"});
    const AC::TerrainPublicationResult fresh =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(fresh.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE_EQ(ReadText(update).value_or(""),
               std::string{"foreign-update-sentinel"});
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{"prior-top"});
}

CORSAIRS_TEST(TerrainReferencePublisher_JournalUpdateRejectsExactCloneReplacement) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    std::filesystem::path update;
    std::filesystem::path displaced;
    std::string clonedBytes;
    bool swapped = false;
    const AC::TerrainPublicationResult failed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!swapped && point ==
                        "MANIFEST_JOURNAL_UPDATE_AFTER_EXCLUSIVE_CREATE") {
                    for (const auto& entry :
                         std::filesystem::directory_iterator{options.Output}) {
                        if (entry.path().filename().generic_string().starts_with(
                                ".garner.reference-albedo.publish.update.")) {
                            update = entry.path();
                            break;
                        }
                    }
                    const auto original = ReadText(update);
                    if (original.has_value()) {
                        clonedBytes = *original;
                        displaced = update;
                        displaced += ".owned";
                        std::error_code error;
                        std::filesystem::rename(update, displaced, error);
                        swapped = !error && WriteText(update, clonedBytes);
                        swapped = swapped && SetExactPrivateMode(update);
                    }
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

    REQUIRE(swapped);
    REQUIRE(failed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(failed.Detail.contains("identity"));
    REQUIRE_EQ(ReadText(update).value_or(""), clonedBytes);
}

CORSAIRS_TEST(TerrainReferencePublisher_JournalUpdateRejectsWriterCloseClone) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    std::filesystem::path update;
    std::string clonedBytes;
    bool swapped = false;
    const AC::TerrainPublicationResult failed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!swapped && point ==
                        "MANIFEST_JOURNAL_UPDATE_AFTER_WRITER_CLOSE") {
                    for (const auto& entry :
                         std::filesystem::directory_iterator{options.Output}) {
                        if (entry.path().filename().generic_string().starts_with(
                                ".garner.reference-albedo.publish.update.")) {
                            update = entry.path();
                            break;
                        }
                    }
                    const auto original = ReadText(update);
                    if (original.has_value()) {
                        clonedBytes = *original;
                        std::filesystem::path displaced = update;
                        displaced += ".owned";
                        std::error_code error;
                        std::filesystem::rename(update, displaced, error);
                        swapped = !error && WriteText(update, clonedBytes);
                        swapped = swapped && SetExactPrivateMode(update);
                    }
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

    REQUIRE(swapped);
    REQUIRE(failed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(failed.Detail.contains("identity"));
    REQUIRE_EQ(ReadText(update).value_or(""), clonedBytes);
}

CORSAIRS_TEST(TerrainReferencePublisher_JournalRenameBeforeBarrierIsUncertain) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    const std::filesystem::path journal =
        options.Output / ".garner.reference-albedo.publish.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool reached = false;
    const AC::TerrainPublicationResult uncertain =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!reached && point ==
                        "MANIFEST_JOURNAL_AFTER_RENAME_BEFORE_BARRIER") {
                    reached = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

    REQUIRE(reached);
    REQUIRE(uncertain.Status ==
            AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(std::filesystem::is_regular_file(journal));
    const AC::TerrainPublicationResult recovered =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(recovered.Status == AC::TerrainPublicationStatus::OK);
}

CORSAIRS_TEST(TerrainReferencePublisher_RetiredJournalRequiresIndependentHash) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    const std::filesystem::path retired =
        options.Output / ".garner.reference-albedo.publish.json.retired";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool retiredFault = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!retiredFault &&
                    point == "MANIFEST_AFTER_JOURNAL_RETIRE") {
                    retiredFault = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(retiredFault);
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    std::string changed = ReadText(retired).value_or("");
    REQUIRE(!changed.empty());
    const std::string original = "\"runId\":\"run-001\"";
    const std::size_t runId = changed.find(original);
    REQUIRE(runId != std::string::npos);
    changed.replace(runId, original.size(),
                    "\"runId\":\"foreign-run\"");
    REQUIRE(WriteText(retired, changed));

    const AC::TerrainPublicationResult recovered =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(recovered.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE_EQ(ReadText(retired).value_or(""), changed);
    REQUIRE_EQ(ReadText(destination).value_or(""), json);
}

CORSAIRS_TEST(TerrainReferencePublisher_JournalAuthRejectsExactCloneReplacement) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    const std::filesystem::path auth = options.Output /
        ".garner.reference-albedo.publish.json.retired-auth";
    std::filesystem::path displaced;
    std::string clonedBytes;
    bool swapped = false;
    const AC::TerrainPublicationResult failed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!swapped && point == "MANIFEST_AFTER_JOURNAL_RETIRE") {
                    const auto original = ReadText(auth);
                    if (original.has_value()) {
                        clonedBytes = *original;
                        displaced = auth;
                        displaced += ".owned";
                        std::error_code error;
                        std::filesystem::rename(auth, displaced, error);
                        swapped = !error && WriteText(auth, clonedBytes);
                        swapped = swapped && SetExactPrivateMode(auth);
                    }
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

    REQUIRE(swapped);
    REQUIRE(failed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(failed.Detail.contains("identity"));
    REQUIRE_EQ(ReadText(auth).value_or(""), clonedBytes);
}

CORSAIRS_TEST(TerrainReferencePublisher_JournalAuthRejectsWriterCloseClone) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    const std::filesystem::path auth = options.Output /
        ".garner.reference-albedo.publish.json.retired-auth";
    std::string clonedBytes;
    bool swapped = false;
    const AC::TerrainPublicationResult failed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!swapped && point ==
                        "MANIFEST_JOURNAL_AUTH_AFTER_WRITER_CLOSE") {
                    const auto original = ReadText(auth);
                    if (original.has_value()) {
                        clonedBytes = *original;
                        std::filesystem::path displaced = auth;
                        displaced += ".owned";
                        std::error_code error;
                        std::filesystem::rename(auth, displaced, error);
                        swapped = !error && WriteText(auth, clonedBytes);
                        swapped = swapped && SetExactPrivateMode(auth);
                    }
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

    REQUIRE(swapped);
    REQUIRE(failed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(failed.Detail.contains("identity"));
    REQUIRE_EQ(ReadText(auth).value_or(""), clonedBytes);
}

CORSAIRS_TEST(TerrainReferencePublisher_CleanupRejectsForeignBackupReplacement) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool injected = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!injected &&
                    point == "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE") {
                    injected = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(injected);
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);

    std::filesystem::path backup;
    for (const auto& entry : std::filesystem::directory_iterator(options.Output)) {
        if (entry.path().filename().generic_string().starts_with(
                ".garner.reference-albedo.backup.")) {
            backup = entry.path();
            break;
        }
    }
    REQUIRE(!backup.empty());
    std::error_code error;
    REQUIRE(std::filesystem::remove(backup, error));
    REQUIRE(!error);
    REQUIRE(WriteText(backup, "foreign-sentinel"));

    const AC::TerrainPublicationResult recovered =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(recovered.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(recovered.Detail.contains("owned artifact identity/hash/mode mismatch"));
    REQUIRE_EQ(ReadText(backup).value_or(""),
               std::string{"foreign-sentinel"});
    REQUIRE_EQ(ReadText(destination).value_or(""), json);
}

CORSAIRS_TEST(TerrainReferencePublisher_CleanupRechecksAfterReservationHook) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool committed = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!committed &&
                    point == "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE") {
                    committed = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(committed);
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    std::filesystem::path backup;
    for (const auto& entry :
         std::filesystem::directory_iterator{options.Output}) {
        if (entry.path().filename().generic_string().starts_with(
                ".garner.reference-albedo.backup.")) {
            backup = entry.path();
        }
    }
    REQUIRE(!backup.empty());
    bool swapped = false;
    bool swapSucceeded = false;
    const AC::TerrainPublicationResult recovery =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!swapped &&
                    point ==
                        "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE") {
                    std::error_code removeError;
                    const bool removed =
                        std::filesystem::remove(backup, removeError);
                    swapSucceeded = removed && !removeError &&
                        WriteText(backup, "foreign-sentinel");
                    swapped = true;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(swapped);
    REQUIRE(swapSucceeded);
    REQUIRE(recovery.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(recovery.Detail.contains("identity/hash/mode mismatch"));
    REQUIRE_EQ(ReadText(backup).value_or(""), std::string{"foreign-sentinel"});
}

CORSAIRS_TEST(TerrainReferencePublisher_CleanupRejectsSameBytesNewIdentityAtDelete) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool committed = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!committed &&
                    point == "MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE") {
                    committed = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(committed);
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);

    bool swapped = false;
    bool swapSucceeded = false;
    std::filesystem::path replacement;
    const AC::TerrainPublicationResult recovery =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point)
                -> AC::TerrainReferenceFaultAction {
                if (!swapped &&
                    point == "MANIFEST_BEFORE_TOMBSTONE_DELETE") {
                    for (const auto& entry :
                         std::filesystem::directory_iterator{options.Output}) {
                        if (entry.path().filename().generic_string().contains(
                                ".backup.")) {
                            replacement = entry.path();
                            break;
                        }
                    }
                    if (!replacement.empty()) {
                        std::error_code error;
                        const bool removed =
                            std::filesystem::remove(replacement, error);
                        swapSucceeded = removed && !error &&
                            WriteText(replacement, "prior-top");
                    }
                    swapped = true;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(swapped);
    REQUIRE(swapSucceeded);
    REQUIRE(recovery.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(recovery.Detail.contains("identity/hash/mode mismatch"));
    REQUIRE_EQ(ReadText(replacement).value_or(""), std::string{"prior-top"});
}

CORSAIRS_TEST(TerrainReferencePublisher_PersistentRollbackFailureRetainsEvidence) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool publicationFault = false;
    std::size_t rollbackFaults = 0u;
    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!publicationFault && point == "MANIFEST_AFTER_REPLACE") {
                    publicationFault = true;
                    return AC::TerrainReferenceFaultAction::FAIL;
                }
                if (point == "MANIFEST_ROLLBACK_REPLACE") {
                    ++rollbackFaults;
                    return AC::TerrainReferenceFaultAction::FAIL;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(publicationFault);
    REQUIRE_EQ(rollbackFaults, 1u);
    REQUIRE(result.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(!result.RecoveryBackup.empty());
    REQUIRE(!result.RecoveryCommand.empty());
    REQUIRE(result.RecoveryCommand.contains("terrain-reference"));
    REQUIRE(std::filesystem::exists(
        options.Output / ".garner.reference-albedo.publish.json"));
    REQUIRE(std::filesystem::exists(result.RecoveryBackup));
}

CORSAIRS_TEST(TerrainReferencePublisher_CorruptOwnedJournalFailsClosed) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::filesystem::path journal =
        options.Output / ".garner.reference-albedo.publish.json";
    REQUIRE(WriteText(journal, "{}"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(result.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{"prior-top"});
    REQUIRE_EQ(ReadText(journal).value_or(""), std::string{"{}"});
}

CORSAIRS_TEST(TerrainReferencePublisher_RejectsUnexpectedRetiredArtifactBeforeMutation) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::filesystem::path foreign =
        options.Output / ".garner.reference-albedo.publish.foreign.retired";
    REQUIRE(WriteText(foreign, "foreign"));

    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination,
            AC::SerializeTerrainReferenceManifest(fixture.Manifest()),
            options, {});

    REQUIRE(result.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE_EQ(result.Detail,
               std::string{"unexpected retired publication artifact: "} +
                   foreign.generic_string());
    REQUIRE_EQ(result.RecoveryBackup.generic_string(), foreign.generic_string());
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{"prior-top"});
    REQUIRE_EQ(ReadText(foreign).value_or(""), std::string{"foreign"});
}

CORSAIRS_TEST(DurableFsWindows_ProductionSourceBindsTypedMoveReservation) {
    const auto source = ReadText(
        std::filesystem::path{CORSAIRS_REPO_ROOT} /
        "tools/AssetConverter/src/TerrainReferenceCommand.cpp");
    REQUIRE(source.has_value());
    REQUIRE(source->contains("ReserveMoveTargetWindows"));
    REQUIRE(source->contains("PrivatePhysicalMode"));
    REQUIRE(source->contains("FILE_FLAG_OPEN_REPARSE_POINT"));
    REQUIRE(source->contains("GetFileInformationByHandleEx"));
    REQUIRE(source->contains("LockFileEx"));
    REQUIRE(source->contains("LOCKFILE_FAIL_IMMEDIATELY"));
    REQUIRE(source->contains("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH"));
    REQUIRE(!source->contains("WriteExclusiveText(journal.Temp, json, 0600u"));
}

CORSAIRS_TEST(DurableFsWindows_SharedPlannerOrdersRecoveryFlushAndMoves) {
    using State = AC::TerrainWindowsDurableEntryState;
    using Action = AC::TerrainWindowsDurableAction;
    std::string detail;
    const std::array states{
        State{true, false, false, false},
        State{true, true, true, false},
    };
    const auto plan =
        AC::PlanTerrainWindowsEntryFinalization(states, detail);
    REQUIRE(plan.has_value());
    REQUIRE(detail.empty());
    const std::vector<AC::TerrainWindowsDurableStep> expected{
        {1u, Action::REMOVE_RESERVATION},
        {0u, Action::FLUSH_SOURCE},
        {1u, Action::FLUSH_SOURCE},
        {0u, Action::RESERVE_TARGET},
        {0u, Action::MOVE_TO_RESERVATION},
        {0u, Action::MOVE_TO_SOURCE},
        {0u, Action::VERIFY_SOURCE},
        {1u, Action::RESERVE_TARGET},
        {1u, Action::MOVE_TO_RESERVATION},
        {1u, Action::MOVE_TO_SOURCE},
        {1u, Action::VERIFY_SOURCE},
    };
    REQUIRE_EQ(plan->size(), expected.size());
    for (std::size_t index = 0u; index < expected.size(); ++index) {
        REQUIRE_EQ((*plan)[index].EntryIndex, expected[index].EntryIndex);
        REQUIRE((*plan)[index].Action == expected[index].Action);
    }

    const std::array postMove{
        State{false, true, false, true},
    };
    const auto recoveryPlan =
        AC::PlanTerrainWindowsEntryFinalization(postMove, detail);
    REQUIRE(recoveryPlan.has_value());
    REQUIRE(recoveryPlan->front().Action == Action::RESTORE_SOURCE);
    REQUIRE((*recoveryPlan)[1].Action == Action::FLUSH_SOURCE);

    constexpr std::array invalidStates{
        State{false, false, false, false},
        State{false, true, true, false},
        State{true, true, false, true},
        State{true, false, false, false, false, true},
        State{false, true, false, true, true, false},
    };
    for (const State& state : invalidStates) {
        const std::array one{state};
        REQUIRE(!AC::PlanTerrainWindowsEntryFinalization(one, detail).has_value());
        REQUIRE(!detail.empty());
    }

    const State foreignReservationRecord{
        true, true, true, false, true, true, false};
    const std::array foreignReservationRecords{foreignReservationRecord};
    REQUIRE(!AC::PlanTerrainWindowsEntryFinalization(
        foreignReservationRecords, detail).has_value());
    REQUIRE_EQ(detail,
               std::string{"Windows finalization reservation record changed"});
}

CORSAIRS_TEST(DurableFsWindows_ReconcilesCreatedRollbackIdentityBeforeBarrier) {
    std::string detail;
    const auto adopted = AC::ReconcileTerrainWindowsFinalizationIdentity(
        "", "volume:created-file", detail);
    REQUIRE(adopted.has_value());
    REQUIRE_EQ(*adopted, std::string{"volume:created-file"});
    REQUIRE(detail.empty());

    const auto stable = AC::ReconcileTerrainWindowsFinalizationIdentity(
        "volume:created-file", "volume:created-file", detail);
    REQUIRE(stable.has_value());
    REQUIRE_EQ(*stable, std::string{"volume:created-file"});

    REQUIRE(!AC::ReconcileTerrainWindowsFinalizationIdentity(
        "volume:recorded", "volume:foreign", detail).has_value());
    REQUIRE_EQ(detail,
               std::string{"Windows finalization identity changed"});
}

CORSAIRS_TEST(DurableFsWindows_RemoveOwnedRecoversJournalTombstone) {
    using Action = AC::TerrainWindowsDurableAction;
    using State = AC::TerrainWindowsRemoveOwnedState;
    std::string detail;
    const auto postMove = AC::PlanTerrainWindowsOwnedRemoval(
        State{false, true, false, true, true}, detail);
    REQUIRE(postMove.has_value());
    bool payloadVerified = false;
    bool readonly = true;
    bool deleted = false;
    std::vector<Action> calls;
    REQUIRE(AC::ExecuteTerrainWindowsDurableSteps(
        *postMove,
        [&](const AC::TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
            calls.push_back(step.Action);
            switch (step.Action) {
            case Action::VERIFY_RESERVATION_PAYLOAD:
                payloadVerified = true;
                return true;
            case Action::CLEAR_READONLY:
                if (!payloadVerified) {
                    stepDetail = "attributes cleared before payload verify";
                    return false;
                }
                readonly = false;
                return true;
            case Action::DELETE_RESERVATION:
                if (readonly) {
                    stepDetail = "delete attempted while readonly";
                    return false;
                }
                deleted = true;
                return true;
            case Action::VERIFY_ABSENCE:
                return deleted;
            default:
                stepDetail = "unexpected mocked remove action";
                return false;
            }
        },
        detail));
    REQUIRE(deleted);
    const std::vector expected{
        Action::VERIFY_RESERVATION_PAYLOAD,
        Action::CLEAR_READONLY,
        Action::DELETE_RESERVATION,
        Action::VERIFY_ABSENCE};
    REQUIRE(calls == expected);

    const auto preMove = AC::PlanTerrainWindowsOwnedRemoval(
        State{true, true, true, true, true}, detail);
    REQUIRE(preMove.has_value());
    REQUIRE(preMove->front().Action == Action::REMOVE_RESERVATION);
    REQUIRE((*preMove)[1].Action == Action::VERIFY_SOURCE);
    REQUIRE(!AC::PlanTerrainWindowsOwnedRemoval(
        State{true, true, false, true, true}, detail).has_value());
    REQUIRE(!AC::PlanTerrainWindowsOwnedRemoval(
        State{false, true, false, true, false}, detail).has_value());
}

CORSAIRS_TEST(DurableFsWindows_StagesAttributesBeforeReplace) {
    using Action = AC::TerrainWindowsDurableAction;
    using State = AC::TerrainWindowsDurableEntryState;
    std::string detail;
    const std::array states{
        State{true, false, false, false, true, true},
        State{true, false, false, false, true, true},
        State{true, false, false, false, true, true},
        State{true, false, false, false, true, true},
        State{true, false, false, false, true, true},
        State{true, false, false, false, true, true},
        State{true, false, false, false, true, true},
    };
    const auto plan = AC::PlanTerrainWindowsEntryFinalization(states, detail);
    REQUIRE(plan.has_value());
    std::size_t flushed = 0u;
    std::size_t executed = 0u;
    REQUIRE(AC::ExecuteTerrainWindowsDurableSteps(
        *plan,
        [&](const AC::TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
            ++executed;
            if (step.Action == Action::FLUSH_SOURCE) {
                ++flushed;
                return true;
            }
            if (step.Action == Action::RESERVE_TARGET && flushed != 7u) {
                stepDetail = "reservation preceded all-file flush barrier";
                return false;
            }
            return true;
        },
        detail));
    REQUIRE_EQ(flushed, 7u);
    REQUIRE_EQ(executed, plan->size());

    for (std::size_t failedStep = 0u; failedStep < plan->size(); ++failedStep) {
        std::size_t attempts = 0u;
        REQUIRE(!AC::ExecuteTerrainWindowsDurableSteps(
            *plan,
            [&](const AC::TerrainWindowsDurableStep& step,
                std::string& stepDetail) {
                if (attempts++ == failedStep) {
                    stepDetail = std::format(
                        "DURABLE_FS_ERROR op=MockWin32Action path=\"entry-{}\" "
                        "native=win32:5:",
                        step.EntryIndex);
                    return false;
                }
                return true;
            },
            detail));
        REQUIRE_EQ(attempts, failedStep + 1u);
        REQUIRE_EQ(
            detail,
            std::format(
                "DURABLE_FS_ERROR op=MockWin32Action path=\"entry-{}\" "
                "native=win32:5:",
                (*plan)[failedStep].EntryIndex));
    }
}

CORSAIRS_TEST(DurableFsWindows_RecoversTypedMoveReservation) {
    using Action = AC::TerrainWindowsDurableAction;
    using State = AC::TerrainWindowsDurableEntryState;
    std::string detail;
    const std::array state{
        State{false, true, false, true, true, true}};
    const auto plan = AC::PlanTerrainWindowsEntryFinalization(state, detail);
    REQUIRE(plan.has_value());
    enum class Location { RESERVATION, SOURCE };
    Location payload = Location::RESERVATION;
    bool exactReservation = false;
    REQUIRE(AC::ExecuteTerrainWindowsDurableSteps(
        *plan,
        [&](const AC::TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
            switch (step.Action) {
            case Action::RESTORE_SOURCE:
                payload = Location::SOURCE;
                return true;
            case Action::FLUSH_SOURCE:
                return payload == Location::SOURCE;
            case Action::RESERVE_TARGET:
                exactReservation = true;
                return true;
            case Action::MOVE_TO_RESERVATION:
                if (!exactReservation || payload != Location::SOURCE) {
                    stepDetail = "typed reservation missing before move";
                    return false;
                }
                payload = Location::RESERVATION;
                exactReservation = false;
                return true;
            case Action::MOVE_TO_SOURCE:
                payload = Location::SOURCE;
                return true;
            case Action::VERIFY_SOURCE:
                return payload == Location::SOURCE;
            default:
                stepDetail = "unexpected typed recovery action";
                return false;
            }
        },
        detail));
    REQUIRE(payload == Location::SOURCE);

    const std::array sameBytesForeignIdentity{
        State{false, true, false, true, true, false}};
    REQUIRE(!AC::PlanTerrainWindowsEntryFinalization(
        sameBytesForeignIdentity, detail).has_value());

    enum class PayloadLocation { SOURCE, RESERVATION_RECORD, RESERVATION_PAYLOAD };
    const std::array cleanState{
        State{true, false, false, false, true, true, true}};
    const auto cleanPlan =
        AC::PlanTerrainWindowsEntryFinalization(cleanState, detail);
    REQUIRE(cleanPlan.has_value());
    for (std::size_t crashAfter = 0u; crashAfter < cleanPlan->size();
         ++crashAfter) {
        PayloadLocation location = PayloadLocation::SOURCE;
        for (std::size_t index = 0u; index <= crashAfter; ++index) {
            switch ((*cleanPlan)[index].Action) {
            case Action::FLUSH_SOURCE:
            case Action::VERIFY_SOURCE:
                REQUIRE(location == PayloadLocation::SOURCE);
                break;
            case Action::RESERVE_TARGET:
                REQUIRE(location == PayloadLocation::SOURCE);
                location = PayloadLocation::RESERVATION_RECORD;
                break;
            case Action::MOVE_TO_RESERVATION:
                REQUIRE(location == PayloadLocation::RESERVATION_RECORD);
                location = PayloadLocation::RESERVATION_PAYLOAD;
                break;
            case Action::MOVE_TO_SOURCE:
                REQUIRE(location == PayloadLocation::RESERVATION_PAYLOAD);
                location = PayloadLocation::SOURCE;
                break;
            default:
                REQUIRE(false);
            }
        }
        State restarted;
        restarted.SourcePresent = location == PayloadLocation::SOURCE ||
            location == PayloadLocation::RESERVATION_RECORD;
        restarted.ReservationPresent =
            location != PayloadLocation::SOURCE;
        restarted.ReservationIsExactRecord =
            location == PayloadLocation::RESERVATION_RECORD;
        restarted.ReservationIsRecordedPayload =
            location == PayloadLocation::RESERVATION_PAYLOAD;
        restarted.SourceMatchesRecordedArtifact = true;
        restarted.ReservationMatchesRecordedArtifact = true;
        restarted.ReservationRecordMatchesOwnedArtifact = true;
        const std::array restartedStates{restarted};
        const auto replay = AC::PlanTerrainWindowsEntryFinalization(
            restartedStates, detail);
        REQUIRE(replay.has_value());
        if (location == PayloadLocation::RESERVATION_RECORD) {
            REQUIRE(replay->front().Action == Action::REMOVE_RESERVATION);
        }
        else if (location == PayloadLocation::RESERVATION_PAYLOAD) {
            REQUIRE(replay->front().Action == Action::RESTORE_SOURCE);
        }
        else {
            REQUIRE(replay->front().Action == Action::FLUSH_SOURCE);
        }
    }
}

CORSAIRS_TEST(DurableFsWindows_RetireLockUsesDeleteOnClose) {
    using Action = AC::TerrainWindowsDurableAction;
    std::string detail;
    const auto plan = AC::PlanTerrainWindowsLockRetirement();
    bool identitiesVerified = false;
    bool reserved = false;
    bool moved = false;
    bool retirementPathPinned = false;
    bool deletePending = false;
    bool closed = false;
    std::size_t attempts = 0u;
    std::vector<Action> calls;
    REQUIRE(AC::ExecuteTerrainWindowsDurableSteps(
        plan,
        [&](const AC::TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
            calls.push_back(step.Action);
            switch (step.Action) {
            case Action::VERIFY_LOCK_IDENTITIES:
                identitiesVerified = true;
                return true;
            case Action::RESERVE_TARGET:
                reserved = identitiesVerified;
                return reserved;
            case Action::MOVE_TO_RESERVATION:
                moved = reserved;
                return moved;
            case Action::VERIFY_RESERVATION_PAYLOAD:
                return moved;
            case Action::PIN_RETIREMENT_PATH:
                retirementPathPinned = moved;
                return retirementPathPinned;
            case Action::ACCEPT_DELETE_PENDING:
                deletePending = moved && retirementPathPinned;
                return deletePending;
            case Action::CLOSE_LOCK_HANDLE:
                closed = deletePending;
                return closed;
            default:
                stepDetail = "unexpected mocked lock action";
                return false;
            }
        },
        detail));
    REQUIRE(closed);
    REQUIRE(calls.back() == Action::CLOSE_LOCK_HANDLE);

    attempts = 0u;
    REQUIRE(!AC::ExecuteTerrainWindowsDurableSteps(
        plan,
        [&](const AC::TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
            ++attempts;
            if (step.Action == Action::PIN_RETIREMENT_PATH) {
                stepDetail = "win32:1168 retired path file-ID changed";
                return false;
            }
            return true;
        },
        detail));
    REQUIRE_EQ(detail,
               std::string{"win32:1168 retired path file-ID changed"});

    attempts = 0u;
    REQUIRE(!AC::ExecuteTerrainWindowsDurableSteps(
        plan,
        [&](const AC::TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
            ++attempts;
            if (step.Action == Action::ACCEPT_DELETE_PENDING) {
                stepDetail = "win32:5 delete-pending rejected";
                return false;
            }
            return true;
        },
        detail));
    REQUIRE_EQ(attempts, plan.size() - 1u);
    REQUIRE_EQ(detail, std::string{"win32:5 delete-pending rejected"});
}

CORSAIRS_TEST(DurableFsLock_RejectsStaleHandleAfterRetirement) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::filesystem::path lock =
        options.Output / ".garner.reference-albedo.publish.lock";
    const std::filesystem::path retired =
        options.Output / ".garner.reference-albedo.publish.lock.retired";
    REQUIRE(WriteText(
        lock, std::format("corsairs-durable-lock-v1\npath={}\n",
                          lock.lexically_normal().generic_string())));
    std::error_code linkError;
    std::filesystem::create_hard_link(lock, retired, linkError);
    if (SkipWindowsPrivilegeOrAccessDenied(linkError, "hard-link")) {
        return;
    }
    REQUIRE(!linkError);

    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination,
            AC::SerializeTerrainReferenceManifest(fixture.Manifest()),
            options, {});

    REQUIRE(result.Status != AC::TerrainPublicationStatus::OK);
    REQUIRE(result.Detail.contains("LockExclusive") ||
            result.Detail.contains("retired durable lock"));
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{"prior-top"});
    REQUIRE(std::filesystem::exists(lock));
    REQUIRE(std::filesystem::exists(retired));
}

#if !defined(_WIN32)
CORSAIRS_TEST(DurableFsLock_RetirementPreservesExactBytesPathReplacement) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    const std::filesystem::path lock =
        options.Output / ".garner.reference-albedo.publish.lock";
    const std::filesystem::path displaced =
        options.Output / ".garner.reference-albedo.publish.lock.displaced";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string marker = std::format(
        "corsairs-durable-lock-v1\npath={}\n",
        lock.lexically_normal().generic_string());
    bool swapped = false;
    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination,
            AC::SerializeTerrainReferenceManifest(fixture.Manifest()),
            options,
            [&](std::string_view point) {
                if (!swapped &&
                    point == "MANIFEST_AFTER_LOCK_RESERVATION_DURABLE") {
                    std::error_code error;
                    std::filesystem::rename(lock, displaced, error);
                    swapped = !error && WriteText(lock, marker);
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

    REQUIRE(swapped);
    REQUIRE(result.Status == AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE_EQ(ReadText(lock).value_or("missing"), marker);
    REQUIRE_EQ(ReadText(displaced).value_or("missing"), marker);
    REQUIRE_EQ(ReadText(destination).value_or(""),
               AC::SerializeTerrainReferenceManifest(fixture.Manifest()));
}

CORSAIRS_TEST(DurableFsLock_StaleWaiterCannotBecomeNewCanonicalOwner) {
    struct ScopedDescriptor {
        int Value{-1};
        ~ScopedDescriptor() {
            if (Value >= 0) {
                ::close(Value);
            }
        }
    };

    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    const std::filesystem::path canonical =
        options.Output / ".garner.reference-albedo.publish.lock";
    const std::filesystem::path retired =
        options.Output / ".garner.reference-albedo.publish.lock.retired";
    const std::string marker = std::format(
        "corsairs-durable-lock-v1\npath={}\n",
        canonical.lexically_normal().generic_string());
    REQUIRE(WriteText(destination, "prior-top"));
    REQUIRE(WriteText(canonical, marker));

    ScopedDescriptor oldOwner{::open(
        canonical.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC)};
    ScopedDescriptor staleWaiter{::open(
        canonical.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC)};
    REQUIRE(oldOwner.Value >= 0);
    REQUIRE(staleWaiter.Value >= 0);
    REQUIRE(::flock(oldOwner.Value, LOCK_EX | LOCK_NB) == 0);
    errno = 0;
    REQUIRE(::flock(staleWaiter.Value, LOCK_EX | LOCK_NB) != 0);
    REQUIRE(errno == EWOULDBLOCK || errno == EAGAIN);

    struct stat oldIdentity {};
    REQUIRE(::fstat(staleWaiter.Value, &oldIdentity) == 0);
    REQUIRE(::rename(canonical.c_str(), retired.c_str()) == 0);
    REQUIRE(::unlink(retired.c_str()) == 0);
    REQUIRE(::close(oldOwner.Value) == 0);
    oldOwner.Value = -1;
    REQUIRE(::flock(staleWaiter.Value, LOCK_EX | LOCK_NB) == 0);

    bool observedDistinctOwner = false;
    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination,
            AC::SerializeTerrainReferenceManifest(fixture.Manifest()),
            options,
            [&](std::string_view point) {
                if (point == "MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE") {
                    struct stat currentIdentity {};
                    observedDistinctOwner =
                        ::lstat(canonical.c_str(), &currentIdentity) == 0 &&
                        (currentIdentity.st_dev != oldIdentity.st_dev ||
                         currentIdentity.st_ino != oldIdentity.st_ino);
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });

    REQUIRE(observedDistinctOwner);
    REQUIRE(result.Status == AC::TerrainPublicationStatus::WRITE_FAILED);
    struct stat staleIdentity {};
    REQUIRE(::fstat(staleWaiter.Value, &staleIdentity) == 0);
    REQUIRE_EQ(staleIdentity.st_dev, oldIdentity.st_dev);
    REQUIRE_EQ(staleIdentity.st_ino, oldIdentity.st_ino);
}
#endif

CORSAIRS_TEST(DurableFsLock_RejectsPreexistingEmptyForeignFile) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    const std::filesystem::path lock =
        options.Output / ".garner.reference-albedo.publish.lock";
    REQUIRE(WriteText(destination, "prior-top"));
    REQUIRE(WriteText(lock, ""));

    const AC::TerrainPublicationResult result =
        AC::PublishTerrainReferenceManifestForTesting(
            destination,
            AC::SerializeTerrainReferenceManifest(fixture.Manifest()),
            options, {});
    const AC::TerrainPublicationResult repeated =
        AC::PublishTerrainReferenceManifestForTesting(
            destination,
            AC::SerializeTerrainReferenceManifest(fixture.Manifest()),
            options, {});

    REQUIRE(result.Status == AC::TerrainPublicationStatus::WRITE_FAILED);
    REQUIRE(repeated.Status == result.Status);
    REQUIRE_EQ(repeated.Detail, result.Detail);
    REQUIRE_EQ(ReadText(lock).value_or("missing"), std::string{});
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{"prior-top"});
}

CORSAIRS_TEST(DurableFsLock_DurableMarkerBeforeRecovery) {
    {
        ReferenceFixture foreignFixture;
        REQUIRE(foreignFixture.Ready());
        const AC::TerrainReferenceOptions options = foreignFixture.Options();
        const std::filesystem::path destination =
            options.Output / "garner.reference-albedo.json";
        const std::filesystem::path lock =
            options.Output / ".garner.reference-albedo.publish.lock";
        const std::filesystem::path journal =
            options.Output / ".garner.reference-albedo.publish.json";
        REQUIRE(WriteText(destination, "prior-top"));
        REQUIRE(WriteText(lock, ""));
        bool transactionWorkReached = false;
        const AC::TerrainPublicationResult rejected =
            AC::PublishTerrainReferenceManifestForTesting(
                destination,
                AC::SerializeTerrainReferenceManifest(
                    foreignFixture.Manifest()),
                options,
                [&](std::string_view) {
                    transactionWorkReached = true;
                    return AC::TerrainReferenceFaultAction::NONE;
                });
        REQUIRE(rejected.Status == AC::TerrainPublicationStatus::WRITE_FAILED);
        REQUIRE(!transactionWorkReached);
        REQUIRE_EQ(ReadText(lock).value_or("missing"), std::string{});
        REQUIRE(!std::filesystem::exists(journal));
        REQUIRE_EQ(ReadText(destination).value_or(""),
                   std::string{"prior-top"});
    }

    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    const std::filesystem::path lock =
        options.Output / ".garner.reference-albedo.publish.lock";
    const std::filesystem::path journal =
        options.Output / ".garner.reference-albedo.publish.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    const std::string expectedMarker = std::format(
        "corsairs-durable-lock-v1\npath={}\n",
        lock.lexically_normal().generic_string());
    bool observedDurableMarker = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!observedDurableMarker &&
                    point == "MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE") {
                    observedDurableMarker =
                        ReadText(lock).value_or("") == expectedMarker &&
                        std::filesystem::file_size(lock) ==
                            expectedMarker.size();
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(observedDurableMarker);
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::WRITE_FAILED);
    REQUIRE_EQ(ReadText(lock).value_or(""), expectedMarker);
    REQUIRE(std::filesystem::is_regular_file(journal));

    const AC::TerrainPublicationResult recovered =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(recovered.Status == AC::TerrainPublicationStatus::OK);
    REQUIRE_EQ(ReadText(destination).value_or(""), json);
    REQUIRE(!std::filesystem::exists(lock));
    REQUIRE(!std::filesystem::exists(journal));
}

CORSAIRS_TEST(DurableFsLock_InternalMarkerFaultsAreReplayable) {
    constexpr std::array<std::string_view, 3> points{
        "MANIFEST_LOCK_MARKER_AFTER_WRITE_BEFORE_FLUSH",
        "MANIFEST_LOCK_MARKER_AFTER_FLUSH_BEFORE_READBACK",
        "MANIFEST_LOCK_MARKER_AFTER_READBACK"};
    for (const std::string_view point : points) {
        ReferenceFixture fixture;
        REQUIRE(fixture.Ready());
        const AC::TerrainReferenceOptions options = fixture.Options();
        const std::filesystem::path destination =
            options.Output / "garner.reference-albedo.json";
        const std::filesystem::path lock =
            options.Output / ".garner.reference-albedo.publish.lock";
        REQUIRE(WriteText(destination, "prior-top"));
        const std::string json =
            AC::SerializeTerrainReferenceManifest(fixture.Manifest());
        bool reached = false;
        const AC::TerrainPublicationResult interrupted =
            AC::PublishTerrainReferenceManifestForTesting(
                destination, json, options,
                [&](std::string_view candidate) {
                    if (!reached && candidate == point) {
                        reached = true;
                        return AC::TerrainReferenceFaultAction::CRASH;
                    }
                    return AC::TerrainReferenceFaultAction::NONE;
                });
        REQUIRE(reached);
        REQUIRE(interrupted.Status ==
                AC::TerrainPublicationStatus::WRITE_FAILED);
        REQUIRE_EQ(
            ReadText(lock).value_or(""),
            std::format("corsairs-durable-lock-v1\npath={}\n",
                        lock.lexically_normal().generic_string()));
        const AC::TerrainPublicationResult replayed =
            AC::PublishTerrainReferenceManifestForTesting(
                destination, json, options, {});
        REQUIRE(replayed.Status == AC::TerrainPublicationStatus::OK);
    }
}

CORSAIRS_TEST(TerrainReferencePublisher_RejectsJournalOwnedPathAlias) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    REQUIRE(WriteText(destination, "prior-top"));
    const std::string json =
        AC::SerializeTerrainReferenceManifest(fixture.Manifest());
    bool injected = false;
    const AC::TerrainPublicationResult crashed =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options,
            [&](std::string_view point) {
                if (!injected &&
                    point == "MANIFEST_AFTER_PREPARED_JOURNAL_DURABLE") {
                    injected = true;
                    return AC::TerrainReferenceFaultAction::CRASH;
                }
                return AC::TerrainReferenceFaultAction::NONE;
            });
    REQUIRE(injected);
    REQUIRE(crashed.Status == AC::TerrainPublicationStatus::WRITE_FAILED);

    const std::filesystem::path journal =
        options.Output / ".garner.reference-albedo.publish.json";
    std::string mutated = ReadText(journal).value_or("");
    const std::size_t valueBegin = mutated.find("\"temp\":\"");
    REQUIRE(valueBegin != std::string::npos);
    const std::size_t pathBegin = valueBegin + std::string_view{"\"temp\":\""}.size();
    const std::size_t pathEnd = mutated.find('"', pathBegin);
    REQUIRE(pathEnd != std::string::npos);
    const std::filesystem::path unrelated = options.Output / "unrelated.txt";
    REQUIRE(WriteText(unrelated, "must-survive"));
    mutated.replace(pathBegin, pathEnd - pathBegin,
                    unrelated.generic_string());
    REQUIRE(WriteText(journal, mutated));

    const AC::TerrainPublicationResult recovered =
        AC::PublishTerrainReferenceManifestForTesting(
            destination, json, options, {});
    REQUIRE(recovered.Status ==
            AC::TerrainPublicationStatus::RECOVERY_REQUIRED);
    REQUIRE(recovered.Detail.contains("journal path"));
    REQUIRE_EQ(ReadText(unrelated).value_or(""), std::string{"must-survive"});
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{"prior-top"});
}

CORSAIRS_TEST(TerrainReferenceProductionDependencies_AreFullyBound) {
    const AC::TerrainReferenceDependencies dependencies =
        AC::MakeProductionTerrainReferenceDependencies();
    REQUIRE(static_cast<bool>(dependencies.RecoverBeforeBuild));
    REQUIRE(static_cast<bool>(dependencies.BuildProducts));
    REQUIRE(static_cast<bool>(dependencies.DurabilizeRunProducts));
    REQUIRE(static_cast<bool>(dependencies.AtomicPublish));
}

CORSAIRS_TEST(TerrainReferenceProductionOverload_RejectsInvalidMapBeforeOutputs) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    const AC::TerrainReferenceOptions options = fixture.Options();
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(options, output, error), 1);
    REQUIRE(output.str().empty());
    REQUIRE(error.str().contains("map"));
    REQUIRE(!std::filesystem::exists(
        options.Output / "garner.reference-albedo.json"));
}

CORSAIRS_TEST(TerrainReferenceCommand_RejectsNonCanonicalOptionsBeforeDependencies) {
    ReferenceFixture fixture;
    REQUIRE(fixture.Ready());
    AC::TerrainReferenceOptions options = fixture.Options();
    options.Map = "../escape.map";
    std::size_t recoveryCalls = 0u;
    std::size_t buildCalls = 0u;
    std::size_t publishCalls = 0u;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.RecoverBeforeBuild =
        [&](const AC::TerrainReferenceOptions&) {
            ++recoveryCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path&,
            std::string&) -> std::optional<AC::TerrainReferenceBuildProducts> {
            ++buildCalls;
            return std::nullopt;
        };
    dependencies.AtomicPublish =
        [&](const std::filesystem::path&, std::string_view) {
            ++publishCalls;
            return AC::TerrainPublicationResult{
                AC::TerrainPublicationStatus::OK, {}, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(
                   options, dependencies, output, error), 1);
    REQUIRE_EQ(recoveryCalls, 0u);
    REQUIRE_EQ(buildCalls, 0u);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE(error.str().contains("normalized relative"));
}

CORSAIRS_TEST(TerrainReferenceCli_ParsesLiteralWidthAndHeightArguments) {
    const std::array<std::string_view, 28> arguments{
        "--map", "Client/map/garner.map",
        "--database", "databases/gamedata.sqlite",
        "--client-root", "Client",
        "--alpha", "Client/texture/terrain/alpha/total.png",
        "--output", "artifacts/maps",
        "--page", "17", "21",
        "--require-present-rect", "2193", "2756", "80", "47",
        "--max-rss-mib", "128",
        "--max-cache-mib", "32",
        "--max-png-mib", "96",
        "--max-height-error-cm", "5",
        "--max-rms-error-cm", "2",
    };
    std::string detail;
    const auto options = AC::ParseTerrainReferenceArguments(arguments, detail);
    REQUIRE(options.has_value());
    REQUIRE(detail.empty());
    REQUIRE_EQ(options->Page.X, 17u);
    REQUIRE_EQ(options->Page.Y, 21u);
    REQUIRE_EQ(options->RequiredPresent.X, 2193u);
    REQUIRE_EQ(options->RequiredPresent.Y, 2756u);
    REQUIRE_EQ(options->RequiredPresent.Width, 80u);
    REQUIRE_EQ(options->RequiredPresent.Height, 47u);
    REQUIRE_EQ(options->Bake.MaxRssBytes, 128u * 1024u * 1024u);
    REQUIRE_EQ(options->Bake.MaxTextureCacheBytes, 32u * 1024u * 1024u);
    REQUIRE_EQ(options->Bake.MaxPngBytes, 96u * 1024u * 1024u);
    REQUIRE_EQ(options->Mesh.MaxAbsCm, 5.0);
    REQUIRE_EQ(options->Mesh.MaxRmsCm, 2.0);
}

CORSAIRS_TEST(TerrainReferenceCli_RejectsMalformedArgumentVectors) {
    const std::vector<std::vector<std::string_view>> invalid{
        {},
        {"--unknown"},
        {"--map", "one", "--map", "two"},
        {"--map"},
        {"--page", "17", "bad"},
        {"--page", "4294967296", "21"},
        {"--require-present-rect", "1", "2", "0", "4"},
        {"--max-rss-mib", "18446744073709551615"},
        {"extra"},
    };
    for (const auto& arguments : invalid) {
        std::string detail;
        REQUIRE(!AC::ParseTerrainReferenceArguments(arguments, detail).has_value());
        REQUIRE(!detail.empty());
    }
}

CORSAIRS_TEST(TerrainReferenceCli_DispatchesBeforeLegacyArgumentHandling) {
    bool buildCalled = false;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.BuildProducts =
        [&](const AC::TerrainReferenceOptions&,
            const std::filesystem::path&,
            std::string&) -> std::optional<AC::TerrainReferenceBuildProducts> {
            buildCalled = true;
            return std::nullopt;
        };
    dependencies.AtomicPublish =
        [](const std::filesystem::path&, std::string_view) {
            return AC::TerrainPublicationResult{};
        };
    const std::array<std::string_view, 3> arguments{
        "AssetConverter", "terrain-reference", "--unknown"};
    std::ostringstream output;
    std::ostringstream error;
    const auto exitCode = AC::TryRunTerrainReferenceSubcommand(
        arguments, dependencies, output, error);
    REQUIRE(exitCode.has_value());
    REQUIRE_EQ(*exitCode, 2);
    REQUIRE(!buildCalled);
    REQUIRE(output.str().empty());
    REQUIRE(error.str().starts_with("Использование: AssetConverter terrain-reference"));

    const std::array<std::string_view, 3> legacy{
        "AssetConverter", "input", "output"};
    REQUIRE(!AC::TryRunTerrainReferenceSubcommand(
        legacy, dependencies, output, error).has_value());
}

CORSAIRS_TEST(TerrainReferenceCli_ExecutableDispatchesBeforeLegacyRoots) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::format(
            "corsairs-terrain-reference-cli-{}",
            std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code error;
    REQUIRE(std::filesystem::create_directory(directory, error));
    REQUIRE(!error);
    const std::filesystem::path log = directory / "output.log";
#if defined(_WIN32)
    const std::string command = ShellQuote(ASSET_CONVERTER_PATH) +
        " terrain-reference --unknown > " + ShellQuote(log) + " 2>&1";
#else
    const std::string command = "nice -n 10 " + ShellQuote(ASSET_CONVERTER_PATH) +
        " terrain-reference --unknown > " + ShellQuote(log) + " 2>&1";
#endif
    REQUIRE(std::system(command.c_str()) != 0);
    const auto text = ReadText(log);
    REQUIRE(text.has_value());
    REQUIRE(text->starts_with("Использование: AssetConverter terrain-reference"));
    std::filesystem::remove_all(directory, error);
    REQUIRE(!error);
}

} // namespace
