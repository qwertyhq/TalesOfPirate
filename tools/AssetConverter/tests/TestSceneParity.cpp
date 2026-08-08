#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"
#include "Corsairs/Tools/AssetConverter/SceneParity.h"

#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
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

} // namespace
