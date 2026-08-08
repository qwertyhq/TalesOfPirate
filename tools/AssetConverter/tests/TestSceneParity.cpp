#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/MapWriter.h"
#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"
#include "Corsairs/Tools/AssetConverter/SceneParity.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"
#include "Corsairs/Tools/AssetConverter/TerrainSurface.h"

#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::size_t kFixtureBlockOffset = 100u;

template<typename T>
void WriteAt(std::vector<std::uint8_t>& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

AC::SceneObjInfo MakeObject(std::uint16_t type,
                            std::int32_t x,
                            std::int32_t y,
                            std::uint16_t modelId = 1u) {
    AC::SceneObjInfo info{};
    const std::uint16_t encoded =
        static_cast<std::uint16_t>((type << 14u) | (modelId & 0x3FFFu));
    info.TypeId = static_cast<std::int16_t>(encoded);
    info.X = x;
    info.Y = y;
    info.HeightOff = 17;
    info.YawAngle = -35;
    info.Scale = 913;
    return info;
}

std::vector<std::uint8_t> MakeFixture(const std::vector<std::uint16_t>& types,
                                      std::int32_t sectionWidth = 7,
                                      std::int32_t sectionHeight = 9) {
    const std::size_t fileSize =
        kFixtureBlockOffset + types.size() * sizeof(AC::SceneObjInfo);
    std::vector<std::uint8_t> bytes(fileSize, 0u);

    AC::SceneFileHeader header{};
    const std::array<char, 16> title{
        'H', 'F', ' ', 'O', 'b', 'j', 'e', 'c',
        't', ' ', 'F', 'i', 'l', 'e', '!', 'Z',
    };
    std::copy(title.begin(), title.end(), header.Title);
    header.Version = AC::kObjVersionCurrent;
    header.FileSize = static_cast<std::int32_t>(fileSize);
    header.SectionCntX = 2;
    header.SectionCntY = 1;
    header.SectionWidth = sectionWidth;
    header.SectionHeight = sectionHeight;
    header.SectionObjNum = static_cast<std::int32_t>(types.size());
    WriteAt(bytes, 0u, header);

    const AC::SectionIndex empty{};
    const AC::SectionIndex populated{
        static_cast<std::int32_t>(kFixtureBlockOffset),
        static_cast<std::int32_t>(types.size()),
    };
    WriteAt(bytes, sizeof(header), empty);
    WriteAt(bytes, sizeof(header) + sizeof(empty), populated);

    for (std::size_t slot = 0; slot < types.size(); ++slot) {
        const std::int64_t relativeX =
            223325 - static_cast<std::int64_t>(sectionWidth) *
                         AC::kWorldUnitsPerTile;
        const auto info = MakeObject(
            types[slot],
            relativeX >= std::numeric_limits<std::int32_t>::min() &&
                    relativeX <= std::numeric_limits<std::int32_t>::max()
                ? static_cast<std::int32_t>(relativeX)
                : 0,
            278475);
        WriteAt(bytes, kFixtureBlockOffset + slot * sizeof(info), info);
    }
    return bytes;
}

AC::SceneFileHeader ReadFixtureHeader(const std::vector<std::uint8_t>& bytes) {
    AC::SceneFileHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    return header;
}

void WriteFixtureHeader(std::vector<std::uint8_t>& bytes,
                        const AC::SceneFileHeader& header) {
    WriteAt(bytes, 0u, header);
}

void WriteFixtureIndex(std::vector<std::uint8_t>& bytes,
                       std::size_t section,
                       const AC::SectionIndex& index) {
    WriteAt(bytes,
            sizeof(AC::SceneFileHeader) + section * sizeof(AC::SectionIndex),
            index);
}

bool SameHeader(const AC::SceneFileHeader& left,
                const AC::SceneFileHeader& right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

bool SameInfo(const AC::SceneObjInfo& left, const AC::SceneObjInfo& right) {
    return left.TypeId == right.TypeId &&
           left.X == right.X &&
           left.Y == right.Y &&
           left.HeightOff == right.HeightOff &&
           left.YawAngle == right.YawAngle &&
           left.Scale == right.Scale;
}

bool SamePlacedObject(const AC::PlacedObject& left,
                      const AC::PlacedObject& right) {
    return SameInfo(left.Info, right.Info) &&
           left.SectionX == right.SectionX &&
           left.SectionY == right.SectionY &&
           left.SectionWidth == right.SectionWidth &&
           left.SectionHeight == right.SectionHeight &&
           left.Source == right.Source;
}

bool SamePlacedObjects(const std::vector<AC::PlacedObject>& left,
                       const std::vector<AC::PlacedObject>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!SamePlacedObject(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

bool SameSelection(const AC::SceneSelection& left,
                   const AC::SceneSelection& right) {
    return SameHeader(left.SourceHeader, right.SourceHeader) &&
           SamePlacedObjects(left.Models, right.Models) &&
           SamePlacedObjects(left.DeferredEffects, right.DeferredEffects) &&
           left.ReferenceKeys == right.ReferenceKeys;
}

AC::SceneSelection MakeSentinelSelection() {
    AC::SceneSelection selection{};
    std::fill(std::begin(selection.SourceHeader.Title),
              std::end(selection.SourceHeader.Title), 'S');
    selection.SourceHeader.Version = 123;
    selection.SourceHeader.FileSize = 456;
    selection.SourceHeader.SectionCntX = 7;
    selection.SourceHeader.SectionCntY = 8;
    selection.SourceHeader.SectionWidth = 9;
    selection.SourceHeader.SectionHeight = 10;
    selection.SourceHeader.SectionObjNum = 11;

    AC::PlacedObject model{};
    model.Info = MakeObject(0u, -100, 200, 77u);
    model.SectionX = 3u;
    model.SectionY = 4u;
    model.SectionWidth = 5u;
    model.SectionHeight = 6u;
    model.Source = AC::SceneSourceKey{12u, 13u, 14u};
    selection.Models.push_back(model);

    AC::PlacedObject effect = model;
    effect.Info = MakeObject(1u, 300, -400, 88u);
    effect.Source = AC::SceneSourceKey{15u, 16u, 17u};
    selection.DeferredEffects.push_back(effect);
    selection.ReferenceKeys.push_back(AC::SceneSourceKey{18u, 19u, 20u});
    return selection;
}

bool SelectionRejectsWithoutPublication(
    const AC::SceneObjects& scene,
    AC::SceneSelectionStatus expectedStatus,
    const AC::ReferenceZone& zone = {}) {
    AC::SceneSelection output = MakeSentinelSelection();
    const AC::SceneSelection before = output;
    std::string detail = "sentinel detail";
    const auto status = AC::BuildSceneSelection(scene, zone, output, detail);
    return status == expectedStatus && !detail.empty() &&
           SameSelection(output, before);
}

std::filesystem::path GarnerObjectPath() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
           "Client" / "map" / "garner.obj";
}

constexpr std::string_view kManifestMapHash =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kManifestObjectHash =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

std::optional<std::string> ReadText(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

bool WriteText(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return static_cast<bool>(output);
}

class WritablePathGuard {
public:
    explicit WritablePathGuard(std::filesystem::path path)
        : _path(std::move(path)) {
    }

    ~WritablePathGuard() {
        std::error_code ignored;
        std::filesystem::permissions(
            _path, std::filesystem::perms::owner_write,
            std::filesystem::perm_options::add, ignored);
    }

    WritablePathGuard(const WritablePathGuard&) = delete;
    WritablePathGuard& operator=(const WritablePathGuard&) = delete;

private:
    std::filesystem::path _path;
};

class ManifestTileSource final : public AC::IMapTileSource {
public:
    ManifestTileSource(std::size_t width, std::size_t height)
        : _width(width), _height(height), _tiles(width * height) {
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
        if (_failure == std::pair{tileX, tileY}) {
            if (_error.empty()) {
                _error = "injected manifest terrain read failure";
            }
            return {};
        }
        if (tileX < 0 || tileY < 0 ||
            static_cast<std::size_t>(tileX) >= _width ||
            static_cast<std::size_t>(tileY) >= _height) {
            return {};
        }
        return _tiles[static_cast<std::size_t>(tileY) * _width +
                      static_cast<std::size_t>(tileX)];
    }

    [[nodiscard]] const std::string& LastError() const override {
        return _error;
    }

    void Set(std::size_t x,
             std::size_t y,
             std::int8_t height,
             std::uint16_t color,
             std::uint8_t island,
             bool present = true) {
        AC::MapTile tile{};
        tile.Height = height;
        tile.Color = static_cast<std::int16_t>(color);
        tile.Island = island;
        _tiles[y * _width + x] = AC::TerrainTileRead{tile, present};
    }

    void Fill(std::int8_t height,
              std::uint16_t color,
              std::uint8_t island,
              bool present = true) {
        for (std::size_t y = 0; y < _height; ++y) {
            for (std::size_t x = 0; x < _width; ++x) {
                Set(x, y, height, color, island, present);
            }
        }
    }

    void InjectFailure(std::int32_t tileX, std::int32_t tileY) {
        _failure = std::pair{tileX, tileY};
    }

private:
    std::size_t _width{0u};
    std::size_t _height{0u};
    std::vector<AC::TerrainTileRead> _tiles;
    std::optional<std::pair<std::int32_t, std::int32_t>> _failure;
    std::string _error;
};

bool SameManifestStats(const AC::SceneManifestStats& left,
                       const AC::SceneManifestStats& right) {
    return left.SourceRecordCount == right.SourceRecordCount &&
           left.SceneModelCount == right.SceneModelCount &&
           left.DeferredEffectCount == right.DeferredEffectCount &&
           left.ReferenceObjectCount == right.ReferenceObjectCount &&
           left.ReferenceIslandCounts == right.ReferenceIslandCounts;
}

AC::SceneManifestStats SentinelManifestStats() {
    AC::SceneManifestStats stats;
    stats.SourceRecordCount = 91u;
    stats.SceneModelCount = 92u;
    stats.DeferredEffectCount = 93u;
    stats.ReferenceObjectCount = 94u;
    stats.ReferenceIslandCounts = {{7u, 95u}};
    return stats;
}

std::filesystem::path ManifestPath(const std::filesystem::path& base) {
    std::filesystem::path path = base;
    path += ".objects.json";
    return path;
}

std::vector<std::filesystem::path> PublicationArtifacts(
    const std::filesystem::path& directory,
    const std::filesystem::path& destination) {
    std::vector<std::filesystem::path> artifacts;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{directory, error};
         !error && iterator != std::filesystem::directory_iterator{};
         iterator.increment(error)) {
        if (iterator->path() != destination &&
            iterator->path().filename().string().contains(".objects.json.")) {
            artifacts.push_back(iterator->path());
        }
    }
    return artifacts;
}

std::optional<std::filesystem::path> FindArtifact(
    const std::filesystem::path& directory,
    std::string_view marker) {
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{directory, error};
         !error && iterator != std::filesystem::directory_iterator{};
         iterator.increment(error)) {
        if (iterator->path().filename().string().contains(marker)) {
            return iterator->path();
        }
    }
    return std::nullopt;
}

class SceneManifestFixture {
public:
    SceneManifestFixture()
        : _root{std::filesystem::current_path() /
                std::format(".corsairs-scene-manifest-{}",
                    std::chrono::steady_clock::now()
                        .time_since_epoch().count())},
          _source{4u, 2u} {
        std::error_code error;
        _ready = std::filesystem::create_directory(_root, error) && !error;
        if (!_ready) {
            return;
        }

        auto bytes = MakeFixture({0u, 1u}, 2, 2);
        AC::SceneObjDiagnostics diagnostics;
        auto scene = AC::ParseSceneObj(bytes, diagnostics);
        if (!scene.has_value()) {
            _ready = false;
            return;
        }

        // Section X=1 starts at 200 cm. These source-relative coordinates
        // therefore place the two ordered records at (50,50) and (150,50).
        scene->Objects[0].Info.X = -150;
        scene->Objects[0].Info.Y = 50;
        scene->Objects[0].Info.HeightOff = 0;
        scene->Objects[0].Info.YawAngle = 90;
        scene->Objects[0].Info.Scale = 37;
        scene->Objects[1].Info.X = -50;
        scene->Objects[1].Info.Y = 50;
        scene->Objects[1].Info.HeightOff = 5;
        scene->Objects[1].Info.YawAngle = -10;
        scene->Objects[1].Info.Scale = 12;

        AC::ReferenceZone zone;
        zone.CenterX = 50;
        zone.CenterY = 50;
        zone.RadiusCm = 0;
        std::string detail;
        _ready = AC::BuildSceneSelection(
                     *scene, zone, _selection, detail) ==
                 AC::SceneSelectionStatus::OK;
        if (!_ready) {
            return;
        }

        _source.Fill(6, 0x7BEFu, 2u);
        _context.ObjectHeader = scene->Header;
        _context.SourceMapSha256 = kManifestMapHash;
        _context.SourceObjectSha256 = kManifestObjectHash;
        _context.ExpectedSourceRecordCount = 2u;
        _context.ExpectedSceneModelCount = 1u;
        _context.ExpectedDeferredEffectCount = 1u;
        _context.ExpectedReferenceObjectCount = 1u;
        _context.ExpectedReferenceIslandCounts = {{2u, 1u}};
    }

    ~SceneManifestFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(_root, ignored);
    }

    SceneManifestFixture(const SceneManifestFixture&) = delete;
    SceneManifestFixture& operator=(const SceneManifestFixture&) = delete;

    [[nodiscard]] bool Ready() const noexcept {
        return _ready;
    }

    [[nodiscard]] const std::filesystem::path& Root() const noexcept {
        return _root;
    }

    [[nodiscard]] std::filesystem::path Base(
        std::string_view stem = "garner") const {
        return _root / stem;
    }

    [[nodiscard]] AC::SceneSelection& Selection() noexcept {
        return _selection;
    }

    [[nodiscard]] ManifestTileSource& Source() noexcept {
        return _source;
    }

    [[nodiscard]] AC::SceneManifestSourceContext& Context() noexcept {
        return _context;
    }

private:
    std::filesystem::path _root;
    ManifestTileSource _source;
    AC::SceneSelection _selection;
    AC::SceneManifestSourceContext _context;
    bool _ready{false};
};

void MutateHeaderField(AC::SceneFileHeader& header, std::size_t field) {
    switch (field) {
    case 0u: header.Title[15] = header.Title[15] == 'X' ? 'Y' : 'X'; break;
    case 1u: ++header.Version; break;
    case 2u: ++header.FileSize; break;
    case 3u: ++header.SectionCntX; break;
    case 4u: ++header.SectionCntY; break;
    case 5u: ++header.SectionWidth; break;
    case 6u: ++header.SectionHeight; break;
    case 7u: ++header.SectionObjNum; break;
    default: break;
    }
}

CORSAIRS_TEST(SceneParity_SyntheticRecordsKeepKeysHeaderAndTypePartition) {
    const auto bytes = MakeFixture({0u, 1u});
    const AC::SceneFileHeader expectedHeader = ReadFixtureHeader(bytes);

    AC::SceneObjDiagnostics diagnostics;
    const auto scene = AC::ParseSceneObj(bytes, diagnostics);
    REQUIRE(scene.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
               static_cast<std::uint32_t>(AC::SceneObjStatus::OK));
    REQUIRE_EQ(scene->Objects.size(), 2u);
    REQUIRE((scene->Objects[0].Source == AC::SceneSourceKey{1u, 0u, 100u}));
    REQUIRE((scene->Objects[1].Source == AC::SceneSourceKey{1u, 1u, 120u}));

    for (const auto& object : scene->Objects) {
        REQUIRE_EQ(object.SectionX, 1u);
        REQUIRE_EQ(object.SectionY, 0u);
        REQUIRE_EQ(object.SectionWidth, 7u);
        REQUIRE_EQ(object.SectionHeight, 9u);
        REQUIRE_EQ(object.WorldX(), 223325);
        REQUIRE_EQ(object.WorldY(), 278475);
    }

    AC::PlacedObject nonZeroSectionY = scene->Objects.front();
    nonZeroSectionY.Info.X = -55;
    nonZeroSectionY.Info.Y = 75;
    nonZeroSectionY.SectionX = 0u;
    nonZeroSectionY.SectionY = 2u;
    REQUIRE_EQ(nonZeroSectionY.WorldX(), -55);
    REQUIRE_EQ(nonZeroSectionY.WorldY(), 1875);

    AC::SceneSelection selection;
    std::string detail = "not cleared";
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BuildSceneSelection(*scene, {}, selection, detail)),
               static_cast<std::uint32_t>(AC::SceneSelectionStatus::OK));
    REQUIRE(detail.empty());
    REQUIRE(SameHeader(selection.SourceHeader, expectedHeader));
    REQUIRE_EQ(selection.Models.size(), 1u);
    REQUIRE_EQ(selection.DeferredEffects.size(), 1u);
    REQUIRE_EQ(selection.ReferenceKeys.size(), 1u);
    REQUIRE((selection.Models[0].Source == AC::SceneSourceKey{1u, 0u, 100u}));
    REQUIRE((selection.DeferredEffects[0].Source ==
             AC::SceneSourceKey{1u, 1u, 120u}));
    REQUIRE((selection.ReferenceKeys[0] == AC::SceneSourceKey{1u, 0u, 100u}));
    REQUIRE_EQ(selection.Models[0].Info.Id(), 1);
    REQUIRE_EQ(selection.DeferredEffects[0].Info.Id(), 1);
}

CORSAIRS_TEST(SceneObjParser_RejectsCheckedArithmeticAndBodyBoundaries) {
    {
        auto bytes = MakeFixture({0u, 1u});
        WriteFixtureIndex(bytes, 1u, AC::SectionIndex{-1, 2});
        AC::SceneObjDiagnostics diagnostics;
        const auto scene = AC::ParseSceneObj(bytes, diagnostics);
        REQUIRE(!scene.has_value());
        REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
                   static_cast<std::uint32_t>(AC::SceneObjStatus::BODY_TRUNCATED));
    }
    {
        auto bytes = MakeFixture({0u, 1u});
        constexpr std::int32_t prefix =
            static_cast<std::int32_t>(sizeof(AC::SceneFileHeader) +
                                      2u * sizeof(AC::SectionIndex));
        WriteFixtureIndex(bytes, 1u, AC::SectionIndex{prefix - 1, 2});
        AC::SceneObjDiagnostics diagnostics;
        const auto scene = AC::ParseSceneObj(bytes, diagnostics);
        REQUIRE(!scene.has_value());
        REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
                   static_cast<std::uint32_t>(AC::SceneObjStatus::BODY_TRUNCATED));
    }
    {
        auto bytes = MakeFixture({0u, 1u});
        WriteFixtureIndex(bytes, 1u, AC::SectionIndex{101, 2});
        AC::SceneObjDiagnostics diagnostics;
        const auto scene = AC::ParseSceneObj(bytes, diagnostics);
        REQUIRE(!scene.has_value());
        REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
                   static_cast<std::uint32_t>(AC::SceneObjStatus::BODY_TRUNCATED));
    }
    {
        std::vector<std::uint8_t> bytes(sizeof(AC::SceneFileHeader), 0u);
        AC::SceneFileHeader header = ReadFixtureHeader(MakeFixture({0u}));
        header.FileSize = static_cast<std::int32_t>(bytes.size());
        header.SectionCntX = std::numeric_limits<std::int32_t>::max();
        header.SectionCntY = std::numeric_limits<std::int32_t>::max();
        header.SectionObjNum = 1;
        WriteFixtureHeader(bytes, header);

        AC::SceneObjDiagnostics diagnostics;
        const auto scene = AC::ParseSceneObj(bytes, diagnostics);
        REQUIRE(!scene.has_value());
        REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
                   static_cast<std::uint32_t>(AC::SceneObjStatus::INTEGER_OVERFLOW));
        REQUIRE_EQ(diagnostics.Detail,
                   std::string{"scene integer overflow: section-table-bytes"});
        REQUIRE_EQ(AC::ToString(AC::SceneObjStatus::INTEGER_OVERFLOW),
                   std::string_view{"INTEGER_OVERFLOW"});
    }
}

CORSAIRS_TEST(SceneObjParser_RejectsHeaderTableAndObjectCountViolations) {
    {
        auto bytes = MakeFixture({0u, 1u});
        auto header = ReadFixtureHeader(bytes);
        header.FileSize -= 1;
        WriteFixtureHeader(bytes, header);
        AC::SceneObjDiagnostics diagnostics;
        REQUIRE(!AC::ParseSceneObj(bytes, diagnostics).has_value());
        REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
                   static_cast<std::uint32_t>(AC::SceneObjStatus::HEADER_TRUNCATED));
    }
    {
        std::vector<std::uint8_t> bytes(59u, 0u);
        auto header = ReadFixtureHeader(MakeFixture({0u, 1u}));
        header.FileSize = static_cast<std::int32_t>(bytes.size());
        WriteFixtureHeader(bytes, header);
        AC::SceneObjDiagnostics diagnostics;
        REQUIRE(!AC::ParseSceneObj(bytes, diagnostics).has_value());
        REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
                   static_cast<std::uint32_t>(AC::SceneObjStatus::SECTION_TABLE_TRUNCATED));
    }
    {
        auto bytes = MakeFixture({0u, 1u});
        WriteFixtureIndex(bytes, 1u, AC::SectionIndex{100, -1});
        AC::SceneObjDiagnostics diagnostics;
        REQUIRE(!AC::ParseSceneObj(bytes, diagnostics).has_value());
        REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
                   static_cast<std::uint32_t>(AC::SceneObjStatus::BODY_TRUNCATED));
    }
    {
        auto bytes = MakeFixture({0u, 1u});
        WriteFixtureIndex(bytes, 1u, AC::SectionIndex{100, 3});
        AC::SceneObjDiagnostics diagnostics;
        REQUIRE(!AC::ParseSceneObj(bytes, diagnostics).has_value());
        REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
                   static_cast<std::uint32_t>(AC::SceneObjStatus::BODY_TRUNCATED));
    }
}

CORSAIRS_TEST(SceneObjParser_RejectsUnrepresentableWorldCoordinateBeforePublication) {
    const auto bytes = MakeFixture(
        {0u}, std::numeric_limits<std::int32_t>::max(), 9);
    AC::SceneObjDiagnostics diagnostics;
    const auto scene = AC::ParseSceneObj(bytes, diagnostics);
    REQUIRE(!scene.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diagnostics.Status),
               static_cast<std::uint32_t>(AC::SceneObjStatus::BODY_TRUNCATED));
}

CORSAIRS_TEST(SceneParity_LateUnknownTypeIsAtomicAndDiagnosticIsStable) {
    const auto bytes = MakeFixture({0u, 1u, 2u});
    AC::SceneObjDiagnostics diagnostics;
    const auto scene = AC::ParseSceneObj(bytes, diagnostics);
    REQUIRE(scene.has_value());

    AC::SceneSelection output = MakeSentinelSelection();
    const AC::SceneSelection before = output;
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BuildSceneSelection(*scene, {}, output, detail)),
               static_cast<std::uint32_t>(
                   AC::SceneSelectionStatus::UNKNOWN_OBJECT_TYPE));
    REQUIRE_EQ(detail,
               std::string{"unknown scene object type 2 at source key (1,2,140)"});
    REQUIRE(SameSelection(output, before));
}

CORSAIRS_TEST(SceneParity_InvalidKeysAndCoordinatesNeverPublish) {
    const auto bytes = MakeFixture({0u, 1u});
    AC::SceneObjDiagnostics diagnostics;
    const auto parsed = AC::ParseSceneObj(bytes, diagnostics);
    REQUIRE(parsed.has_value());

    {
        auto scene = *parsed;
        scene.Objects[1].Source = scene.Objects[0].Source;
        REQUIRE(SelectionRejectsWithoutPublication(
            scene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
    {
        auto scene = *parsed;
        std::swap(scene.Objects[0].Source, scene.Objects[1].Source);
        REQUIRE(SelectionRejectsWithoutPublication(
            scene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
    {
        auto scene = *parsed;
        scene.Objects[0].Source.SectionIndex = 2u;
        REQUIRE(SelectionRejectsWithoutPublication(
            scene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
    {
        auto scene = *parsed;
        scene.Objects[0].Source.SlotIndex = 2u;
        REQUIRE(SelectionRejectsWithoutPublication(
            scene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
    {
        auto scene = *parsed;
        scene.Objects[0].SectionX = 0u;
        REQUIRE(SelectionRejectsWithoutPublication(
            scene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
    {
        auto scene = *parsed;
        scene.Objects[0].Source.ByteOffset = 59u;
        REQUIRE(SelectionRejectsWithoutPublication(
            scene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
    {
        auto scene = *parsed;
        scene.Objects[0].Source.ByteOffset = 121u;
        REQUIRE(SelectionRejectsWithoutPublication(
            scene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
    {
        auto scene = *parsed;
        scene.Objects.resize(1u);
        scene.Objects[0].Source.ByteOffset =
            std::numeric_limits<std::uint64_t>::max() - 7u;
        REQUIRE(SelectionRejectsWithoutPublication(
            scene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
    {
        AC::ReferenceZone invalidZone{};
        invalidZone.RadiusCm = -1;
        REQUIRE(SelectionRejectsWithoutPublication(
            *parsed, AC::SceneSelectionStatus::INVALID_SOURCE_KEY, invalidZone));
    }
    {
        auto overflowingScene = *parsed;
        overflowingScene.Objects[0].Info.X =
            std::numeric_limits<std::int32_t>::max();
        REQUIRE(SelectionRejectsWithoutPublication(
            overflowingScene, AC::SceneSelectionStatus::INVALID_SOURCE_KEY));
    }
}

CORSAIRS_TEST(SceneParity_ReferenceRadiusIsInclusiveAndModelsOnly) {
    auto bytes = MakeFixture({0u, 0u, 0u}, 1, 1);
    const std::array<std::int32_t, 3> distances{7999, 8000, 8001};
    for (std::size_t slot = 0; slot < distances.size(); ++slot) {
        const auto info = MakeObject(
            0u,
            223325 + distances[slot] - AC::kWorldUnitsPerTile,
            278475);
        WriteAt(bytes, kFixtureBlockOffset + slot * sizeof(info), info);
    }

    AC::SceneObjDiagnostics diagnostics;
    const auto scene = AC::ParseSceneObj(bytes, diagnostics);
    REQUIRE(scene.has_value());

    AC::SceneSelection selection;
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BuildSceneSelection(*scene, {}, selection, detail)),
               static_cast<std::uint32_t>(AC::SceneSelectionStatus::OK));
    REQUIRE_EQ(selection.Models.size(), 3u);
    REQUIRE_EQ(selection.ReferenceKeys.size(), 2u);
    REQUIRE((selection.ReferenceKeys[0] == AC::SceneSourceKey{1u, 0u, 100u}));
    REQUIRE((selection.ReferenceKeys[1] == AC::SceneSourceKey{1u, 1u, 120u}));
}

CORSAIRS_TEST(SceneParity_GarnerLiteralCountsKeysAndRepeatability) {
    const auto firstBytes = AC::ReadWholeFile(GarnerObjectPath());
    REQUIRE(firstBytes.has_value());
    AC::SceneObjDiagnostics firstDiagnostics;
    const auto firstScene = AC::ParseSceneObj(*firstBytes, firstDiagnostics);
    REQUIRE(firstScene.has_value());
    REQUIRE_EQ(firstScene->Objects.size(), 50017u);

    AC::SceneSelection firstSelection;
    std::string firstDetail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BuildSceneSelection(
                       *firstScene, {}, firstSelection, firstDetail)),
               static_cast<std::uint32_t>(AC::SceneSelectionStatus::OK));
    REQUIRE_EQ(firstSelection.Models.size(), 46991u);
    REQUIRE_EQ(firstSelection.DeferredEffects.size(), 3026u);
    REQUIRE_EQ(firstSelection.ReferenceKeys.size(), 1634u);
    REQUIRE((firstSelection.ReferenceKeys.front() ==
             AC::SceneSourceKey{173333u, 0u, 9554196u}));
    REQUIRE((firstSelection.ReferenceKeys.back() ==
             AC::SceneSourceKey{183575u, 4u, 10915776u}));
    for (std::size_t index = 1; index < firstSelection.ReferenceKeys.size(); ++index) {
        REQUIRE(firstSelection.ReferenceKeys[index - 1u] <
                firstSelection.ReferenceKeys[index]);
    }

    const auto secondBytes = AC::ReadWholeFile(GarnerObjectPath());
    REQUIRE(secondBytes.has_value());
    AC::SceneObjDiagnostics secondDiagnostics;
    const auto secondScene = AC::ParseSceneObj(*secondBytes, secondDiagnostics);
    REQUIRE(secondScene.has_value());
    REQUIRE_EQ(secondScene->Objects.size(), 50017u);

    AC::SceneSelection secondSelection;
    std::string secondDetail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BuildSceneSelection(
                       *secondScene, {}, secondSelection, secondDetail)),
               static_cast<std::uint32_t>(AC::SceneSelectionStatus::OK));
    REQUIRE(SameHeader(firstScene->Header, secondScene->Header));
    REQUIRE(SameHeader(firstSelection.SourceHeader, secondSelection.SourceHeader));
    REQUIRE(SamePlacedObjects(firstSelection.Models, secondSelection.Models));
    REQUIRE(SamePlacedObjects(firstSelection.DeferredEffects,
                              secondSelection.DeferredEffects));
    REQUIRE(firstSelection.ReferenceKeys == secondSelection.ReferenceKeys);
}

CORSAIRS_TEST(SceneManifest_WritesCompleteSchemaV2InStableSourceOrder) {
    SceneManifestFixture fixture;
    REQUIRE(fixture.Ready());

    AC::SceneManifestStats stats = SentinelManifestStats();
    std::string detail = "sentinel";
    const AC::SceneManifestStatus status = AC::WriteSceneSourceManifest(
        fixture.Selection(), fixture.Source(), fixture.Context(),
        fixture.Base(), stats, detail);

    REQUIRE(status == AC::SceneManifestStatus::OK);
    REQUIRE(detail.empty());
    REQUIRE_EQ(stats.SourceRecordCount, 2u);
    REQUIRE_EQ(stats.SceneModelCount, 1u);
    REQUIRE_EQ(stats.DeferredEffectCount, 1u);
    REQUIRE_EQ(stats.ReferenceObjectCount, 1u);
    REQUIRE((stats.ReferenceIslandCounts ==
             std::map<std::uint8_t, std::uint64_t>{{2u, 1u}}));

    const auto bytes = ReadText(ManifestPath(fixture.Base()));
    REQUIRE(bytes.has_value());
    const std::string expected =
        "{\"schemaVersion\":2,"
        "\"sourceMapSha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"sourceObjectSha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"sectionCntX\":2,\"sectionCntY\":1,"
        "\"sectionWidth\":2,\"sectionHeight\":2,"
        "\"stats\":{\"sourceRecordCount\":2,\"sceneModelCount\":1,"
        "\"deferredEffectCount\":1,\"referenceObjectCount\":1,"
        "\"referenceIslandCounts\":{\"2\":1}},"
        "\"records\":["
        "{\"sourceKey\":{\"sectionIndex\":1,\"slotIndex\":0,\"byteOffset\":100},"
        "\"modelId\":1,\"type\":0,\"disposition\":\"scene-model\","
        "\"x\":50,\"y\":50,\"heightOff\":0,\"sourceYawDegrees\":90,"
        "\"sourceScaleDiagnostic\":37,\"surfaceHeightCm\":60,\"zCm\":60,"
        "\"terrainSectionPresent\":true,\"island\":2,\"tileColor565\":31727,"
        "\"inReferenceSet\":true},"
        "{\"sourceKey\":{\"sectionIndex\":1,\"slotIndex\":1,\"byteOffset\":120},"
        "\"modelId\":1,\"type\":1,\"disposition\":\"deferred-effect\","
        "\"x\":150,\"y\":50,\"heightOff\":5,\"sourceYawDegrees\":-10,"
        "\"sourceScaleDiagnostic\":12,\"surfaceHeightCm\":60,\"zCm\":65,"
        "\"terrainSectionPresent\":true,\"island\":2,\"tileColor565\":31727,"
        "\"inReferenceSet\":false}]}";
    REQUIRE_EQ(*bytes, expected);
}

CORSAIRS_TEST(SceneManifest_TwoIndependentWritesAreByteAndShaIdentical) {
    SceneManifestFixture fixture;
    REQUIRE(fixture.Ready());

    AC::SceneManifestStats firstStats;
    AC::SceneManifestStats secondStats;
    std::string detail;
    REQUIRE(AC::WriteSceneSourceManifest(
                fixture.Selection(), fixture.Source(), fixture.Context(),
                fixture.Base("first"), firstStats, detail) ==
            AC::SceneManifestStatus::OK);
    REQUIRE(AC::WriteSceneSourceManifest(
                fixture.Selection(), fixture.Source(), fixture.Context(),
                fixture.Base("second"), secondStats, detail) ==
            AC::SceneManifestStatus::OK);
    const auto first = ReadText(ManifestPath(fixture.Base("first")));
    const auto second = ReadText(ManifestPath(fixture.Base("second")));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE_EQ(*first, *second);
    REQUIRE(SameManifestStats(firstStats, secondStats));

    std::string hashDetail;
    const auto firstHash = AC::Sha256File(
        ManifestPath(fixture.Base("first")), hashDetail);
    const auto secondHash = AC::Sha256File(
        ManifestPath(fixture.Base("second")), hashDetail);
    REQUIRE(firstHash.has_value());
    REQUIRE(secondHash.has_value());
    REQUIRE_EQ(*firstHash, *secondHash);
}

CORSAIRS_TEST(SceneManifest_RequiresEveryIndependentHeaderFieldOnBothSides) {
    for (const bool mutateSelection : {false, true}) {
        for (std::size_t field = 0u; field < 8u; ++field) {
            SceneManifestFixture fixture;
            REQUIRE(fixture.Ready());
            if (mutateSelection) {
                MutateHeaderField(fixture.Selection().SourceHeader, field);
            }
            else {
                MutateHeaderField(fixture.Context().ObjectHeader, field);
            }
            const AC::SceneManifestStats sentinel = SentinelManifestStats();
            AC::SceneManifestStats stats = sentinel;
            std::string detail;
            REQUIRE(AC::WriteSceneSourceManifest(
                        fixture.Selection(), fixture.Source(), fixture.Context(),
                        fixture.Base(), stats, detail) ==
                    AC::SceneManifestStatus::INVALID_SOURCE_CONTEXT);
            REQUIRE(!detail.empty());
            REQUIRE(SameManifestStats(stats, sentinel));
            REQUIRE(!std::filesystem::exists(
                ManifestPath(fixture.Base())));
        }
    }
}

CORSAIRS_TEST(SceneManifest_RejectsHashesGridAndEveryExpectedCountAtomically) {
    const auto requireInvalidContext = [&](std::string_view hash,
                                           bool mapHash) {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        if (mapHash) {
            fixture.Context().SourceMapSha256 = hash;
        }
        else {
            fixture.Context().SourceObjectSha256 = hash;
        }
        const AC::SceneManifestStats sentinel = SentinelManifestStats();
        AC::SceneManifestStats stats = sentinel;
        std::string detail;
        REQUIRE(AC::WriteSceneSourceManifest(
                    fixture.Selection(), fixture.Source(), fixture.Context(),
                    fixture.Base(), stats, detail) ==
                AC::SceneManifestStatus::INVALID_SOURCE_CONTEXT);
        REQUIRE(SameManifestStats(stats, sentinel));
        REQUIRE(!std::filesystem::exists(ManifestPath(fixture.Base())));
    };
    for (const std::string_view invalid : {
             std::string_view{"abc"},
             std::string_view{
                 "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
             std::string_view{
                 "gaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}) {
        requireInvalidContext(invalid, true);
        requireInvalidContext(invalid, false);
    }

    {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        ManifestTileSource tooNarrow{3u, 2u};
        tooNarrow.Fill(6, 0x7BEFu, 2u);
        AC::SceneManifestStats stats = SentinelManifestStats();
        const AC::SceneManifestStats before = stats;
        std::string detail;
        REQUIRE(AC::WriteSceneSourceManifest(
                    fixture.Selection(), tooNarrow, fixture.Context(),
                    fixture.Base(), stats, detail) ==
                AC::SceneManifestStatus::INVALID_SOURCE_CONTEXT);
        REQUIRE(SameManifestStats(stats, before));
        REQUIRE(!std::filesystem::exists(ManifestPath(fixture.Base())));
    }

    for (std::size_t mismatch = 0u; mismatch < 5u; ++mismatch) {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        switch (mismatch) {
        case 0u: ++fixture.Context().ExpectedSourceRecordCount; break;
        case 1u: ++fixture.Context().ExpectedSceneModelCount; break;
        case 2u: ++fixture.Context().ExpectedDeferredEffectCount; break;
        case 3u: ++fixture.Context().ExpectedReferenceObjectCount; break;
        case 4u: ++fixture.Context().ExpectedReferenceIslandCounts[2u]; break;
        default: break;
        }
        const AC::SceneManifestStats sentinel = SentinelManifestStats();
        AC::SceneManifestStats stats = sentinel;
        std::string detail;
        REQUIRE(AC::WriteSceneSourceManifest(
                    fixture.Selection(), fixture.Source(), fixture.Context(),
                    fixture.Base(), stats, detail) ==
                AC::SceneManifestStatus::COUNT_MISMATCH);
        REQUIRE(SameManifestStats(stats, sentinel));
        REQUIRE(!std::filesystem::exists(ManifestPath(fixture.Base())));
    }

    {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        fixture.Context().ExpectedReferenceIslandCounts = {{2u, 1u}, {3u, 0u}};
        AC::SceneManifestStats stats;
        std::string detail;
        REQUIRE(AC::WriteSceneSourceManifest(
                    fixture.Selection(), fixture.Source(), fixture.Context(),
                    fixture.Base(), stats, detail) ==
                AC::SceneManifestStatus::COUNT_MISMATCH);
        REQUIRE(!std::filesystem::exists(ManifestPath(fixture.Base())));
    }
}

CORSAIRS_TEST(SceneManifest_RejectsUnknownPartitionKeysAndStickyTerrainError) {
    const auto requireSelectionFailure = [&](
        const std::function<void(SceneManifestFixture&)>& mutate,
        AC::SceneManifestStatus expected) {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        mutate(fixture);
        const AC::SceneManifestStats sentinel = SentinelManifestStats();
        AC::SceneManifestStats stats = sentinel;
        std::string detail;
        REQUIRE(AC::WriteSceneSourceManifest(
                    fixture.Selection(), fixture.Source(), fixture.Context(),
                    fixture.Base(), stats, detail) == expected);
        REQUIRE(!detail.empty());
        REQUIRE(SameManifestStats(stats, sentinel));
        REQUIRE(!std::filesystem::exists(ManifestPath(fixture.Base())));
    };

    requireSelectionFailure([](SceneManifestFixture& fixture) {
        fixture.Selection().Models.front().Info.TypeId =
            static_cast<std::int16_t>(2u << 14u);
    }, AC::SceneManifestStatus::UNKNOWN_OBJECT_TYPE);
    requireSelectionFailure([](SceneManifestFixture& fixture) {
        fixture.Selection().Models.front().Info.TypeId =
            static_cast<std::int16_t>(1u << 14u);
    }, AC::SceneManifestStatus::INVALID_SOURCE_CONTEXT);
    requireSelectionFailure([](SceneManifestFixture& fixture) {
        fixture.Selection().DeferredEffects.front().Source =
            fixture.Selection().Models.front().Source;
    }, AC::SceneManifestStatus::INVALID_SOURCE_CONTEXT);
    requireSelectionFailure([](SceneManifestFixture& fixture) {
        AC::PlacedObject second = fixture.Selection().DeferredEffects.front();
        second.Info.TypeId = 1;
        fixture.Selection().DeferredEffects.clear();
        fixture.Selection().Models.push_back(second);
        std::swap(fixture.Selection().Models[0],
                  fixture.Selection().Models[1]);
        fixture.Context().ExpectedSceneModelCount = 2u;
        fixture.Context().ExpectedDeferredEffectCount = 0u;
    }, AC::SceneManifestStatus::INVALID_SOURCE_CONTEXT);
    requireSelectionFailure([](SceneManifestFixture& fixture) {
        fixture.Selection().ReferenceKeys.push_back(
            fixture.Selection().DeferredEffects.front().Source);
        fixture.Context().ExpectedReferenceObjectCount = 2u;
    }, AC::SceneManifestStatus::INVALID_SOURCE_CONTEXT);

    {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        fixture.Source().InjectFailure(0, 0);
        const AC::SceneManifestStats sentinel = SentinelManifestStats();
        AC::SceneManifestStats stats = sentinel;
        std::string detail;
        REQUIRE(AC::WriteSceneSourceManifest(
                    fixture.Selection(), fixture.Source(), fixture.Context(),
                    fixture.Base(), stats, detail) ==
                AC::SceneManifestStatus::TERRAIN_READ_FAILED);
        REQUIRE(detail.contains("injected manifest terrain read failure"));
        REQUIRE(SameManifestStats(stats, sentinel));
        REQUIRE(!std::filesystem::exists(ManifestPath(fixture.Base())));
    }
}

CORSAIRS_TEST(SceneManifest_BackupBarrierFailureNeverReplacesPriorDestination) {
    SceneManifestFixture fixture;
    REQUIRE(fixture.Ready());
    const std::filesystem::path destination = ManifestPath(fixture.Base());
    constexpr std::string_view priorBytes = "literal prior manifest bytes";
    REQUIRE(WriteText(destination, priorBytes));
    const std::filesystem::perms priorMode =
        std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write |
        std::filesystem::perms::group_read;
    std::error_code modeError;
    std::filesystem::permissions(
        destination, priorMode, std::filesystem::perm_options::replace,
        modeError);
    REQUIRE(!modeError);
    const std::filesystem::perms recordedMode =
        std::filesystem::status(destination, modeError).permissions();
    REQUIRE(!modeError);

    bool failedBarrier = false;
    std::size_t destinationReplaceCalls = 0u;
    AC::SceneManifestStats stats = SentinelManifestStats();
    const AC::SceneManifestStats before = stats;
    std::string detail;
    const AC::SceneManifestStatus status =
        AC::WriteSceneSourceManifestForTesting(
            fixture.Selection(), fixture.Source(), fixture.Context(),
            fixture.Base(), stats, detail,
            [&](std::string_view point) {
                if (point == "DESTINATION_REPLACE") {
                    ++destinationReplaceCalls;
                }
                if (!failedBarrier && point == "BACKUP_PARENT_FSYNC") {
                    failedBarrier = true;
                    return AC::SceneManifestFaultAction::FAIL;
                }
                return AC::SceneManifestFaultAction::NONE;
            });
    REQUIRE(failedBarrier);
    REQUIRE_EQ(destinationReplaceCalls, 0u);
    REQUIRE(status == AC::SceneManifestStatus::WRITE_FAILED);
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{priorBytes});
    REQUIRE(std::filesystem::status(destination, modeError).permissions() ==
            recordedMode);
    REQUIRE(!modeError);
    REQUIRE(PublicationArtifacts(fixture.Root(), destination).empty());
    REQUIRE(SameManifestStats(stats, before));
}

CORSAIRS_TEST(SceneManifest_PersistentRollbackFaultsRetainVerifiedPriorBackup) {
    constexpr std::array<std::string_view, 3> rollbackPoints{
        "ROLLBACK_REPLACE", "ROLLBACK_PARENT_FSYNC", "ROLLBACK_VERIFY"};
    for (const std::string_view rollbackPoint : rollbackPoints) {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        const std::filesystem::path destination = ManifestPath(fixture.Base());
        constexpr std::string_view priorBytes = "literal prior manifest bytes";
        REQUIRE(WriteText(destination, priorBytes));
        const std::filesystem::perms priorMode =
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read;
        std::error_code modeError;
        std::filesystem::permissions(
            destination, priorMode, std::filesystem::perm_options::replace,
            modeError);
        REQUIRE(!modeError);
        const std::filesystem::perms recordedMode =
            std::filesystem::status(destination, modeError).permissions();
        REQUIRE(!modeError);

        bool publicationFault = false;
        std::size_t persistentFaults = 0u;
        AC::SceneManifestStats stats = SentinelManifestStats();
        std::string detail;
        const AC::SceneManifestStatus status =
            AC::WriteSceneSourceManifestForTesting(
                fixture.Selection(), fixture.Source(), fixture.Context(),
                fixture.Base(), stats, detail,
                [&](std::string_view point) {
                    if (!publicationFault && point == "AFTER_REPLACE") {
                        publicationFault = true;
                        return AC::SceneManifestFaultAction::FAIL;
                    }
                    if (point == rollbackPoint) {
                        ++persistentFaults;
                        return AC::SceneManifestFaultAction::FAIL;
                    }
                    return AC::SceneManifestFaultAction::NONE;
                });
        REQUIRE(publicationFault);
        REQUIRE_EQ(persistentFaults, 1u);
        REQUIRE(status == AC::SceneManifestStatus::RECOVERY_REQUIRED);
        const auto backup = FindArtifact(fixture.Root(), ".backup.");
        REQUIRE(backup.has_value());
        REQUIRE_EQ(ReadText(*backup).value_or(""), std::string{priorBytes});
        REQUIRE(std::filesystem::status(*backup, modeError).permissions() ==
                recordedMode);
        REQUIRE(!modeError);
        REQUIRE(detail.contains(backup->generic_string()));
    }
}

CORSAIRS_TEST(SceneManifest_OrdinaryPriorFaultsRestoreExactBytesAndMode) {
    constexpr std::array<std::string_view, 4> publicationPoints{
        "AFTER_TEMP_SERIALIZATION", "AFTER_TEMP_FSYNC", "BEFORE_REPLACE",
        "POST_REPLACE_PARENT_FSYNC"};
    for (const std::string_view publicationPoint : publicationPoints) {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        const std::filesystem::path destination = ManifestPath(fixture.Base());
        constexpr std::string_view priorBytes = "literal prior manifest bytes";
        REQUIRE(WriteText(destination, priorBytes));
        const std::filesystem::perms priorMode =
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read;
        std::error_code modeError;
        std::filesystem::permissions(
            destination, priorMode, std::filesystem::perm_options::replace,
            modeError);
        REQUIRE(!modeError);
        const std::filesystem::perms recordedMode =
            std::filesystem::status(destination, modeError).permissions();
        REQUIRE(!modeError);

        bool injected = false;
        AC::SceneManifestStats stats = SentinelManifestStats();
        const AC::SceneManifestStats before = stats;
        std::string detail;
        const AC::SceneManifestStatus status =
            AC::WriteSceneSourceManifestForTesting(
                fixture.Selection(), fixture.Source(), fixture.Context(),
                fixture.Base(), stats, detail,
                [&](std::string_view point) {
                    if (!injected && point == publicationPoint) {
                        injected = true;
                        return AC::SceneManifestFaultAction::FAIL;
                    }
                    return AC::SceneManifestFaultAction::NONE;
                });
        REQUIRE(injected);
        REQUIRE(status == AC::SceneManifestStatus::WRITE_FAILED);
        REQUIRE_EQ(ReadText(destination).value_or(""), std::string{priorBytes});
        REQUIRE(std::filesystem::status(destination, modeError).permissions() ==
                recordedMode);
        REQUIRE(!modeError);
        REQUIRE(PublicationArtifacts(fixture.Root(), destination).empty());
        REQUIRE(SameManifestStats(stats, before));
    }
}

CORSAIRS_TEST(SceneManifest_NoPriorOrdinaryFailuresRestoreVerifiedAbsence) {
    constexpr std::array<std::string_view, 5> publicationPoints{
        "AFTER_TEMP_SERIALIZATION", "AFTER_TEMP_FSYNC", "BEFORE_REPLACE",
        "AFTER_REPLACE", "POST_REPLACE_PARENT_FSYNC"};
    for (const std::string_view publicationPoint : publicationPoints) {
        SceneManifestFixture fixture;
        REQUIRE(fixture.Ready());
        const std::filesystem::path destination = ManifestPath(fixture.Base());
        bool injected = false;
        AC::SceneManifestStats stats = SentinelManifestStats();
        const AC::SceneManifestStats before = stats;
        std::string detail;
        const AC::SceneManifestStatus status =
            AC::WriteSceneSourceManifestForTesting(
                fixture.Selection(), fixture.Source(), fixture.Context(),
                fixture.Base(), stats, detail,
                [&](std::string_view point) {
                    if (!injected && point == publicationPoint) {
                        injected = true;
                        return AC::SceneManifestFaultAction::FAIL;
                    }
                    return AC::SceneManifestFaultAction::NONE;
                });
        REQUIRE(injected);
        REQUIRE(status == AC::SceneManifestStatus::WRITE_FAILED);
        REQUIRE(!std::filesystem::exists(destination));
        REQUIRE(PublicationArtifacts(fixture.Root(), destination).empty());
        REQUIRE(SameManifestStats(stats, before));
        REQUIRE(!detail.contains("priorBackup="));
    }
}

CORSAIRS_TEST(SceneManifest_NoPriorPersistentCleanupFaultsNameEveryPath) {
    constexpr std::array<std::string_view, 2> publicationPoints{
        "AFTER_TEMP_FSYNC", "AFTER_REPLACE"};
    constexpr std::array<std::string_view, 3> cleanupPoints{
        "NO_PRIOR_REMOVE", "NO_PRIOR_PARENT_FSYNC",
        "NO_PRIOR_VERIFY_ABSENT"};
    for (const std::string_view publicationPoint : publicationPoints) {
        for (const std::string_view cleanupPoint : cleanupPoints) {
            SceneManifestFixture fixture;
            REQUIRE(fixture.Ready());
            const std::filesystem::path destination =
                ManifestPath(fixture.Base());
            bool publicationFault = false;
            std::size_t cleanupFaults = 0u;
            AC::SceneManifestStats stats;
            std::string detail;
            const AC::SceneManifestStatus status =
                AC::WriteSceneSourceManifestForTesting(
                    fixture.Selection(), fixture.Source(), fixture.Context(),
                    fixture.Base(), stats, detail,
                    [&](std::string_view point) {
                        if (!publicationFault && point == publicationPoint) {
                            publicationFault = true;
                            return AC::SceneManifestFaultAction::FAIL;
                        }
                        if (publicationFault && point == cleanupPoint) {
                            ++cleanupFaults;
                            return AC::SceneManifestFaultAction::FAIL;
                        }
                        return AC::SceneManifestFaultAction::NONE;
                    });
            REQUIRE(publicationFault);
            REQUIRE(cleanupFaults >= 1u);
            REQUIRE(status == AC::SceneManifestStatus::RECOVERY_REQUIRED);
            REQUIRE(detail.contains("priorBackup=null"));
            REQUIRE(detail.contains(destination.generic_string()));
            REQUIRE(detail.contains("cleanup/retry"));
            REQUIRE(!FindArtifact(fixture.Root(), ".backup.").has_value());
        }
    }
}

CORSAIRS_TEST(SceneManifest_PostCommitBackupCleanupFailureKeepsSuccessAndEvidence) {
    SceneManifestFixture fixture;
    REQUIRE(fixture.Ready());
    const std::filesystem::path destination = ManifestPath(fixture.Base());
    constexpr std::string_view priorBytes = "literal prior manifest bytes";
    REQUIRE(WriteText(destination, priorBytes));

    bool cleanupFault = false;
    AC::SceneManifestStats stats;
    std::string detail;
    const AC::SceneManifestStatus status =
        AC::WriteSceneSourceManifestForTesting(
            fixture.Selection(), fixture.Source(), fixture.Context(),
            fixture.Base(), stats, detail,
            [&](std::string_view point) {
                if (!cleanupFault && point == "BACKUP_REMOVE") {
                    cleanupFault = true;
                    return AC::SceneManifestFaultAction::FAIL;
                }
                return AC::SceneManifestFaultAction::NONE;
            });
    REQUIRE(cleanupFault);
    REQUIRE(status == AC::SceneManifestStatus::OK);
    REQUIRE(ReadText(destination).value_or("") != std::string{priorBytes});
    const auto backup = FindArtifact(fixture.Root(), ".backup.");
    REQUIRE(backup.has_value());
    REQUIRE_EQ(ReadText(*backup).value_or(""), std::string{priorBytes});
    REQUIRE(detail.contains(backup->generic_string()));
    REQUIRE_EQ(stats.SourceRecordCount, 2u);
}

CORSAIRS_TEST(SceneManifest_ReadOnlyPriorCanBeBackedUpAndRolledBackExactly) {
    SceneManifestFixture fixture;
    REQUIRE(fixture.Ready());
    const std::filesystem::path destination = ManifestPath(fixture.Base());
    [[maybe_unused]] const WritablePathGuard writableCleanup{destination};
    constexpr std::string_view priorBytes = "literal readonly prior manifest";
    REQUIRE(WriteText(destination, priorBytes));
    const std::filesystem::perms readOnlyMode =
        std::filesystem::perms::owner_read;
    std::error_code modeError;
    std::filesystem::permissions(
        destination, readOnlyMode, std::filesystem::perm_options::replace,
        modeError);
    REQUIRE(!modeError);
    const std::filesystem::perms recordedMode =
        std::filesystem::status(destination, modeError).permissions();
    REQUIRE(!modeError);

    bool postReplaceBarrierFault = false;
    const AC::SceneManifestStats sentinel = SentinelManifestStats();
    AC::SceneManifestStats stats = sentinel;
    std::string detail;
    const AC::SceneManifestStatus status =
        AC::WriteSceneSourceManifestForTesting(
            fixture.Selection(), fixture.Source(), fixture.Context(),
            fixture.Base(), stats, detail,
            [&](std::string_view point) {
                if (!postReplaceBarrierFault &&
                    point == "POST_REPLACE_PARENT_FSYNC") {
                    postReplaceBarrierFault = true;
                    return AC::SceneManifestFaultAction::FAIL;
                }
                return AC::SceneManifestFaultAction::NONE;
            });

    REQUIRE(postReplaceBarrierFault);
    REQUIRE(status == AC::SceneManifestStatus::WRITE_FAILED);
    REQUIRE_EQ(ReadText(destination).value_or(""), std::string{priorBytes});
    REQUIRE(std::filesystem::status(destination, modeError).permissions() ==
            recordedMode);
    REQUIRE(!modeError);
    REQUIRE(PublicationArtifacts(fixture.Root(), destination).empty());
    REQUIRE(SameManifestStats(stats, sentinel));
}

CORSAIRS_TEST(SceneManifest_CleanupFsyncFailuresRetainExactPriorBackupTruthfully) {
    enum class FailurePhase {
        PRE_REPLACE,
        ROLLBACK,
        POST_COMMIT,
    };
    struct FailureCase {
        FailurePhase Phase;
        std::string_view CleanupPoint;
        AC::SceneManifestStatus ExpectedStatus;
    };
    constexpr std::array cases{
        FailureCase{
            FailurePhase::PRE_REPLACE,
            "PRIOR_PRE_REPLACE_CLEANUP_PARENT_FSYNC",
            AC::SceneManifestStatus::RECOVERY_REQUIRED},
        FailureCase{
            FailurePhase::ROLLBACK,
            "PRIOR_ROLLBACK_CLEANUP_PARENT_FSYNC",
            AC::SceneManifestStatus::RECOVERY_REQUIRED},
        FailureCase{
            FailurePhase::POST_COMMIT,
            "POST_COMMIT_BACKUP_CLEANUP_PARENT_FSYNC",
            AC::SceneManifestStatus::OK},
    };

    constexpr std::array persistentRecoveryFaults{false, true};
    for (const FailureCase& failureCase : cases) {
        for (const bool persistentRecoveryFault : persistentRecoveryFaults) {
            SceneManifestFixture fixture;
            REQUIRE(fixture.Ready());
            const std::filesystem::path destination =
                ManifestPath(fixture.Base());
            constexpr std::string_view priorBytes =
                "literal cleanup-fsync prior manifest";
            REQUIRE(WriteText(destination, priorBytes));
            const std::filesystem::perms priorMode =
                std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write |
                std::filesystem::perms::group_read;
            std::error_code modeError;
            std::filesystem::permissions(
                destination, priorMode, std::filesystem::perm_options::replace,
                modeError);
            REQUIRE(!modeError);
            const std::filesystem::perms recordedMode =
                std::filesystem::status(destination, modeError).permissions();
            REQUIRE(!modeError);

            bool publicationFault = false;
            std::size_t cleanupPointCalls = 0u;
            bool firstCallSawBackupAbsent = false;
            bool secondCallSawBackupRecreated = false;
            AC::SceneManifestStats stats = SentinelManifestStats();
            const AC::SceneManifestStats before = stats;
            std::string detail;
            const AC::SceneManifestStatus status =
                AC::WriteSceneSourceManifestForTesting(
                    fixture.Selection(), fixture.Source(), fixture.Context(),
                    fixture.Base(), stats, detail,
                    [&](std::string_view point) {
                        if (!publicationFault &&
                            failureCase.Phase == FailurePhase::PRE_REPLACE &&
                            point == "BACKUP_PARENT_FSYNC") {
                            publicationFault = true;
                            return AC::SceneManifestFaultAction::FAIL;
                        }
                        if (!publicationFault &&
                            failureCase.Phase == FailurePhase::ROLLBACK &&
                            point == "AFTER_REPLACE") {
                            publicationFault = true;
                            return AC::SceneManifestFaultAction::FAIL;
                        }
                        if (point == failureCase.CleanupPoint) {
                            ++cleanupPointCalls;
                            const bool backupPresent =
                                FindArtifact(fixture.Root(), ".backup.")
                                    .has_value();
                            if (cleanupPointCalls == 1u) {
                                firstCallSawBackupAbsent = !backupPresent;
                                return AC::SceneManifestFaultAction::FAIL;
                            }
                            if (cleanupPointCalls == 2u) {
                                secondCallSawBackupRecreated = backupPresent;
                                return persistentRecoveryFault
                                    ? AC::SceneManifestFaultAction::FAIL
                                    : AC::SceneManifestFaultAction::NONE;
                            }
                        }
                        return AC::SceneManifestFaultAction::NONE;
                    });

            if (failureCase.Phase == FailurePhase::POST_COMMIT) {
                REQUIRE(!publicationFault);
            }
            else {
                REQUIRE(publicationFault);
            }
            REQUIRE_EQ(cleanupPointCalls, 2u);
            REQUIRE(firstCallSawBackupAbsent);
            REQUIRE(secondCallSawBackupRecreated);
            REQUIRE(status == failureCase.ExpectedStatus);
            const auto backup = FindArtifact(fixture.Root(), ".backup.");
            REQUIRE(backup.has_value());
            REQUIRE_EQ(ReadText(*backup).value_or(""),
                       std::string{priorBytes});
            REQUIRE(std::filesystem::status(*backup, modeError).permissions() ==
                    recordedMode);
            REQUIRE(!modeError);
            const std::string backupPath = backup->generic_string();
            REQUIRE(!detail.contains("priorBackup=null"));
            if (persistentRecoveryFault) {
                REQUIRE(!detail.contains("retained verified prior backup"));
                if (failureCase.Phase == FailurePhase::POST_COMMIT) {
                    REQUIRE(detail.contains(
                        std::format("prior backup is unavailable at {}",
                                    backupPath)));
                }
                else {
                    REQUIRE(detail.contains("priorBackup=unavailable"));
                    REQUIRE(detail.contains(
                        std::format("expectedPath={}", backupPath)));
                    REQUIRE(!detail.contains(
                        std::format("priorBackup={}", backupPath)));
                }
            }
            else {
                REQUIRE(!detail.contains("unavailable"));
                if (failureCase.Phase == FailurePhase::POST_COMMIT) {
                    REQUIRE(detail.contains(std::format(
                        "retained verified prior backup {}", backupPath)));
                }
                else {
                    REQUIRE(detail.contains(
                        std::format("priorBackup={}", backupPath)));
                }
            }
            if (failureCase.Phase == FailurePhase::POST_COMMIT) {
                REQUIRE_EQ(stats.SourceRecordCount, 2u);
                REQUIRE(ReadText(destination).value_or("") !=
                        std::string{priorBytes});
            }
            else {
                REQUIRE(SameManifestStats(stats, before));
            }
        }
    }
}

} // namespace
