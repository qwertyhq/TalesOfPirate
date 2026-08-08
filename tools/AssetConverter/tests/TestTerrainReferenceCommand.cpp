#include "Corsairs/Tools/AssetConverter/TerrainReferenceCommand.h"

#include "TestHarness.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::string_view kHash =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kHashOfX =
    "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881";

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
                AC::TerrainPublicationStatus::OK, {}, {}, {}};
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
                AC::TerrainPublicationStatus::OK, {}, {}, {}};
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
                AC::TerrainPublicationStatus::OK, {}, {}, {}};
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
                AC::TerrainPublicationStatus::OK, {}, {}, {}};
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
                AC::TerrainPublicationStatus::OK, {}, {}, {}};
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
                AC::TerrainPublicationStatus::OK, {}, {}, {}};
        };
    std::ostringstream output;
    std::ostringstream error;
    REQUIRE_EQ(AC::RunTerrainReference(
                   options, dependencies, output, error), 1);
    REQUIRE_EQ(publishCalls, 0u);
    REQUIRE(error.str().contains("changed during durability"));
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
    constexpr std::array<std::string_view, 7> points{
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
    REQUIRE_EQ(ReadText(retired).value_or(""), originalJournal);
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

CORSAIRS_TEST(DurableFsLock_RejectsStaleHandleAfterRetirement) {
#if defined(_WIN32)
    // The Win32 stale-handle branch is covered by the static/mock contract;
    // creating a hard link requires platform privileges unrelated to locking.
    REQUIRE(true);
#else
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
#endif
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
                AC::TerrainPublicationStatus::OK, {}, {}, {}};
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
                AC::TerrainPublicationStatus::OK, {}, {}, {}};
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
