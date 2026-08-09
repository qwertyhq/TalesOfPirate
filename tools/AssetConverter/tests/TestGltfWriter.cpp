#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"

#include "TestHarness.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path SampleLgo() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
           "Client" / "model" / "character" / "0066000000.lgo";
}

std::filesystem::path OutputDir() {
    return std::filesystem::temp_directory_path() / "corsairs-gltf-tests";
}

AC::LgoMaterial MaterialWithStates(
    std::uint32_t rawTranspType,
    std::uint32_t effectiveTranspType,
    float opacity,
    std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> states) {
    AC::LgoMaterial material;
    material.Opacity = opacity;
    material.RawTranspType = rawTranspType;
    material.EffectiveTranspType = effectiveTranspType;
    material.Mtl.Dif = AC::ColorValue4f{1.0f, 1.0f, 1.0f, 1.0f};
    for (AC::RenderStateAtom& atom : material.RenderStates) {
        atom = AC::RenderStateAtom{0xFFFFFFFFu, 0u, 0u};
    }

    std::size_t index = 0;
    for (const auto& [state, value] : states) {
        material.RenderStates[index++] = AC::RenderStateAtom{state, value, value};
    }
    return material;
}

AC::LgoGeomObj MaterialFixtureObject(std::vector<AC::LgoMaterial> materials) {
    AC::LgoGeomObj object;
    object.Version = 0x1004u;
    object.Mesh.Positions = {
        AC::Vector3{0.0f, 0.0f, 0.0f},
        AC::Vector3{1.0f, 0.0f, 0.0f},
        AC::Vector3{0.0f, 1.0f, 0.0f},
    };
    object.Mesh.Indices = {0u, 1u, 2u};
    for (std::size_t i = 0; i < materials.size(); ++i) {
        object.Mesh.Subsets.push_back(AC::SubsetInfo{1u, 0u, 3u, 0u});
    }
    object.Materials = std::move(materials);
    return object;
}

std::string ReadText(const std::filesystem::path& path) {
    const auto bytes = AC::ReadWholeFile(path);
    if (!bytes) {
        return {};
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

CORSAIRS_TEST(LegacyMaterialResolver_NormalizesLiteralRawTypes) {
    std::uint32_t effective = 99u;
    std::string detail;

    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::NormalizeLegacyTransparencyType(0u, effective, detail)),
               static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
    REQUIRE_EQ(effective, 0u);
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::NormalizeLegacyTransparencyType(1u, effective, detail)),
               static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
    REQUIRE_EQ(effective, 1u);
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::NormalizeLegacyTransparencyType(2u, effective, detail)),
               static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
    REQUIRE_EQ(effective, 5u);
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::NormalizeLegacyTransparencyType(8u, effective, detail)),
               static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
    REQUIRE_EQ(effective, 8u);
}

CORSAIRS_TEST(LegacyMaterialResolver_ResolvesFiveLiteralModes) {
    struct Case {
        AC::LgoMaterial Material;
        AC::LegacyMaterialMode Mode;
        bool AlphaTestEnabled;
        std::uint32_t AlphaRef;
        std::uint32_t AlphaFunc;
        bool AlphaBlendEnabled;
        std::uint32_t SrcBlend;
        std::uint32_t DestBlend;
    };

    const std::array<Case, 5> cases{{
        {MaterialWithStates(0u, 0u, 1.0f, {{15u, 0u}, {27u, 0u}}),
         AC::LegacyMaterialMode::Opaque, false, 0u, 0u, false, 0u, 0u},
        {MaterialWithStates(0u, 0u, 1.0f,
                            {{15u, 1u}, {25u, 5u}, {24u, 129u}}),
         AC::LegacyMaterialMode::Masked, true, 129u, 5u, false, 0u, 0u},
        {MaterialWithStates(0u, 0u, 0.5f, {{19u, 5u}, {20u, 6u}}),
         AC::LegacyMaterialMode::Alpha, false, 0u, 0u, true, 5u, 6u},
        {MaterialWithStates(1u, 1u, 1.0f, {{19u, 2u}, {20u, 2u}}),
         AC::LegacyMaterialMode::Additive, false, 0u, 0u, true, 2u, 2u},
        {MaterialWithStates(2u, 5u, 1.0f, {{19u, 1u}, {20u, 4u}}),
         AC::LegacyMaterialMode::Subtractive, false, 0u, 0u, true, 1u, 4u},
    }};

    for (const Case& testCase : cases) {
        AC::LegacyMaterialMetadata metadata;
        std::string detail;
        REQUIRE_EQ(static_cast<std::uint32_t>(
                       AC::ResolveLegacyMaterial(testCase.Material, metadata, detail)),
                   static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
        REQUIRE_EQ(static_cast<std::uint32_t>(metadata.Mode),
                   static_cast<std::uint32_t>(testCase.Mode));
        REQUIRE_EQ(metadata.Opacity, testCase.Material.Opacity);
        REQUIRE_EQ(metadata.RawTranspType, testCase.Material.RawTranspType);
        REQUIRE_EQ(metadata.EffectiveTranspType,
                   testCase.Material.EffectiveTranspType);
        REQUIRE_EQ(metadata.AlphaTestEnabled, testCase.AlphaTestEnabled);
        REQUIRE_EQ(metadata.AlphaRef, testCase.AlphaRef);
        REQUIRE_EQ(metadata.AlphaFunc, testCase.AlphaFunc);
        REQUIRE_EQ(metadata.AlphaBlendEnabled, testCase.AlphaBlendEnabled);
        REQUIRE_EQ(metadata.SrcBlend, testCase.SrcBlend);
        REQUIRE_EQ(metadata.DestBlend, testCase.DestBlend);
    }
}

CORSAIRS_TEST(LegacyMaterialResolver_RejectsLiteralContradictions) {
    auto conflicting = MaterialWithStates(
        0u, 0u, 1.0f, {{19u, 5u}, {19u, 2u}, {20u, 6u}});
    auto maskedAndBlended = MaterialWithStates(
        0u, 0u, 1.0f,
        {{15u, 1u}, {25u, 5u}, {24u, 129u}, {19u, 5u}, {20u, 6u}});
    auto unsupportedAlpha = MaterialWithStates(
        0u, 0u, 1.0f, {{15u, 1u}, {25u, 4u}, {24u, 129u}});
    auto unsupportedBlend = MaterialWithStates(
        0u, 0u, 0.5f, {{19u, 2u}, {20u, 4u}});
    auto normalizationMismatch = MaterialWithStates(
        2u, 2u, 1.0f, {{19u, 1u}, {20u, 4u}});
    auto unsupportedTransparency = MaterialWithStates(7u, 7u, 1.0f, {});

    struct Case {
        const AC::LgoMaterial* Material;
        AC::LegacyMaterialStatus Status;
    };
    const std::array<Case, 6> cases{{
        {&conflicting, AC::LegacyMaterialStatus::CONTRADICTORY_RENDER_STATE},
        {&maskedAndBlended, AC::LegacyMaterialStatus::CONTRADICTORY_RENDER_STATE},
        {&unsupportedAlpha, AC::LegacyMaterialStatus::UNSUPPORTED_ALPHA_TEST},
        {&unsupportedBlend, AC::LegacyMaterialStatus::UNSUPPORTED_BLEND_PAIR},
        {&normalizationMismatch,
         AC::LegacyMaterialStatus::CONTRADICTORY_RENDER_STATE},
        {&unsupportedTransparency,
         AC::LegacyMaterialStatus::UNSUPPORTED_TRANSPARENCY_TYPE},
    }};

    for (const Case& testCase : cases) {
        AC::LegacyMaterialMetadata metadata;
        std::string detail;
        REQUIRE_EQ(static_cast<std::uint32_t>(
                       AC::ResolveLegacyMaterial(*testCase.Material, metadata, detail)),
                   static_cast<std::uint32_t>(testCase.Status));
        REQUIRE(!detail.empty());
    }
}

CORSAIRS_TEST(LegacyMaterialResolver_CanonicalizesEveryStateBeforeTerminator) {
    auto conflictingUnknown = MaterialWithStates(
        0u, 0u, 1.0f, {{137u, 10u}, {137u, 11u}});
    auto repeatedUnknown = MaterialWithStates(
        0u, 0u, 1.0f, {{137u, 10u}, {137u, 10u}});
    auto conflictingEndValue = repeatedUnknown;
    conflictingEndValue.RenderStates[1].Value1 = 11u;
    auto poisonAfterTerminator = MaterialWithStates(0u, 0u, 1.0f, {});
    poisonAfterTerminator.RenderStates[0] =
        AC::RenderStateAtom{137u, 10u, 10u};
    poisonAfterTerminator.RenderStates[1] =
        AC::RenderStateAtom{0xFFFFFFFFu, 0u, 0u};
    poisonAfterTerminator.RenderStates[2] =
        AC::RenderStateAtom{137u, 99u, 99u};
    poisonAfterTerminator.RenderStates[3] =
        AC::RenderStateAtom{15u, 1u, 1u};

    AC::LegacyMaterialMetadata metadata;
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::ResolveLegacyMaterial(conflictingUnknown, metadata, detail)),
               static_cast<std::uint32_t>(
                   AC::LegacyMaterialStatus::CONTRADICTORY_RENDER_STATE));
    REQUIRE(!detail.empty());

    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::ResolveLegacyMaterial(conflictingEndValue, metadata, detail)),
               static_cast<std::uint32_t>(
                   AC::LegacyMaterialStatus::CONTRADICTORY_RENDER_STATE));
    REQUIRE(!detail.empty());

    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::ResolveLegacyMaterial(repeatedUnknown, metadata, detail)),
               static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
    REQUIRE_EQ(static_cast<std::uint32_t>(metadata.Mode),
               static_cast<std::uint32_t>(AC::LegacyMaterialMode::Opaque));

    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::ResolveLegacyMaterial(poisonAfterTerminator, metadata, detail)),
               static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
    REQUIRE_EQ(static_cast<std::uint32_t>(metadata.Mode),
               static_cast<std::uint32_t>(AC::LegacyMaterialMode::Opaque));
    REQUIRE(!metadata.AlphaTestEnabled);
}

CORSAIRS_TEST(LegacyMaterialResolver_SubtractiveEquationIsDestinationTimesInverseSource) {
    const AC::LgoMaterial material = MaterialWithStates(
        2u, 5u, 1.0f, {{19u, 1u}, {20u, 4u}});
    AC::LegacyMaterialMetadata metadata;
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::ResolveLegacyMaterial(material, metadata, detail)),
               static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
    REQUIRE_EQ(static_cast<std::uint32_t>(metadata.Mode),
               static_cast<std::uint32_t>(AC::LegacyMaterialMode::Subtractive));
    REQUIRE(static_cast<std::uint32_t>(metadata.Mode) !=
            static_cast<std::uint32_t>(AC::LegacyMaterialMode::Alpha));
    REQUIRE(static_cast<std::uint32_t>(metadata.Mode) !=
            static_cast<std::uint32_t>(AC::LegacyMaterialMode::Additive));

    const std::array<float, 3> source{0.25f, 0.5f, 0.75f};
    const std::array<float, 3> destination{0.8f, 0.6f, 0.4f};
    const std::array<float, 3> result{
        destination[0] * (1.0f - source[0]),
        destination[1] * (1.0f - source[1]),
        destination[2] * (1.0f - source[2]),
    };
    REQUIRE_EQ(result[0], 0.6f);
    REQUIRE_EQ(result[1], 0.3f);
    REQUIRE_EQ(result[2], 0.1f);
}

CORSAIRS_TEST(GltfWriter_SceneMapEmitsAuthoritativeLegacyMaterialExtras) {
    AC::LgoGeomObj object = MaterialFixtureObject({
        MaterialWithStates(0u, 0u, 1.0f, {{15u, 0u}, {27u, 0u}}),
        MaterialWithStates(0u, 0u, 1.0f,
                           {{15u, 1u}, {25u, 5u}, {24u, 129u}}),
        MaterialWithStates(0u, 0u, 0.5f, {{19u, 5u}, {20u, 6u}}),
        MaterialWithStates(1u, 1u, 1.0f, {{19u, 2u}, {20u, 2u}}),
        MaterialWithStates(2u, 5u, 1.0f, {{19u, 1u}, {20u, 4u}}),
    });

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path path = OutputDir() / "material-modes.gltf";
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, path, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const std::string text = ReadText(path);
    REQUIRE(!text.empty());
    REQUIRE(text.find(R"("alphaMode":"OPAQUE")") != std::string::npos);
    REQUIRE(text.find(
        R"("alphaMode":"MASK","alphaCutoff":0.5098039215686274)") !=
            std::string::npos);
    REQUIRE(text.find(R"("alphaMode":"BLEND")") != std::string::npos);
    REQUIRE(text.find(
        R"("corsairsLegacyMaterial":{"schemaVersion":1,"mode":"opaque","opacity":1,"rawTranspType":0,"effectiveTranspType":0,"alphaTestEnabled":false,"alphaRef":0,"alphaFunc":0,"alphaBlendEnabled":false,"srcBlend":0,"destBlend":0})") !=
            std::string::npos);
    REQUIRE(text.find(
        R"("corsairsLegacyMaterial":{"schemaVersion":1,"mode":"masked","opacity":1,"rawTranspType":0,"effectiveTranspType":0,"alphaTestEnabled":true,"alphaRef":129,"alphaFunc":5,"alphaBlendEnabled":false,"srcBlend":0,"destBlend":0})") !=
            std::string::npos);
    REQUIRE(text.find(
        R"("corsairsLegacyMaterial":{"schemaVersion":1,"mode":"alpha","opacity":0.5,"rawTranspType":0,"effectiveTranspType":0,"alphaTestEnabled":false,"alphaRef":0,"alphaFunc":0,"alphaBlendEnabled":true,"srcBlend":5,"destBlend":6})") !=
            std::string::npos);
    REQUIRE(text.find(
        R"("corsairsLegacyMaterial":{"schemaVersion":1,"mode":"additive","opacity":1,"rawTranspType":1,"effectiveTranspType":1,"alphaTestEnabled":false,"alphaRef":0,"alphaFunc":0,"alphaBlendEnabled":true,"srcBlend":2,"destBlend":2})") !=
            std::string::npos);
    REQUIRE(text.find(
        R"("corsairsLegacyMaterial":{"schemaVersion":1,"mode":"subtractive","opacity":1,"rawTranspType":2,"effectiveTranspType":5,"alphaTestEnabled":false,"alphaRef":0,"alphaFunc":0,"alphaBlendEnabled":true,"srcBlend":1,"destBlend":4})") !=
            std::string::npos);
}

CORSAIRS_TEST(GltfWriter_UnsupportedSceneMapMaterialLeavesNoOutput) {
    AC::LgoGeomObj object = MaterialFixtureObject({
        MaterialWithStates(7u, 7u, 1.0f, {}),
    });
    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path path = OutputDir() / "unsupported-material.gltf";
    std::filesystem::path binPath = path;
    binPath.replace_extension(".bin");
    std::filesystem::remove(path);
    std::filesystem::remove(binPath);

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, path, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap)),
               static_cast<std::uint32_t>(
                   AC::GltfStatus::UNSUPPORTED_MATERIAL_MODE));
    REQUIRE(!detail.empty());
    REQUIRE(!std::filesystem::exists(path));
    REQUIRE(!std::filesystem::exists(binPath));
}

CORSAIRS_TEST(GltfWriter_GltfPublicationFailureLeavesNoOrphanPairOrTemps) {
    const std::filesystem::path root = OutputDir() / "atomic-pair-failure";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    REQUIRE(std::filesystem::create_directories(root));

    const std::filesystem::path path = root / "blocked.gltf";
    REQUIRE(std::filesystem::create_directory(path));
    std::filesystem::path binPath = path;
    binPath.replace_extension(".bin");

    AC::LgoGeomObj object = MaterialFixtureObject({
        MaterialWithStates(0u, 0u, 1.0f, {}),
    });
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(object, path, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::WRITE_FAILED));
    REQUIRE(!detail.empty());
    REQUIRE(std::filesystem::is_directory(path));
    REQUIRE(!std::filesystem::exists(binPath));

    std::size_t unexpectedEntries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path() != path) {
            ++unexpectedEntries;
        }
    }
    REQUIRE_EQ(unexpectedEntries, 0u);
}

CORSAIRS_TEST(GltfWriter_SceneMapMaterialNamesIncludeSourceIndex) {
    auto opaque = MaterialWithStates(0u, 0u, 1.0f, {});
    auto additive = MaterialWithStates(
        1u, 1u, 1.0f, {{19u, 2u}, {20u, 2u}});
    opaque.Textures[0] = "shared.bmp";
    additive.Textures[0] = "shared.bmp";
    AC::LgoGeomObj object = MaterialFixtureObject({opaque, additive});

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path path = OutputDir() / "unique-material-names.gltf";
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, path, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const std::string text = ReadText(path);
    REQUIRE(text.find(R"("name":"shared.bmp0")") !=
            std::string::npos);
    REQUIRE(text.find(R"("name":"shared.bmp1")") !=
            std::string::npos);
    REQUIRE(text.find(R"("name":"material_0_shared.bmp")") ==
            std::string::npos);
    REQUIRE(text.find(R"("name":"shared.bmp")") == std::string::npos);
}

CORSAIRS_TEST(GltfWriter_GenericProfileKeepsLiteralBytes) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());
    AC::LgoDiagnostics diag;
    const auto object = AC::ParseLgo(*bytes, diag);
    REQUIRE(object.has_value());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path path = OutputDir() / "sample.gltf";
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*object, path, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    std::filesystem::path binPath = path;
    binPath.replace_extension(".bin");
    const auto gltfHash = AC::Sha256File(path, detail);
    const auto binHash = AC::Sha256File(binPath, detail);
    REQUIRE(gltfHash.has_value());
    REQUIRE(binHash.has_value());
    REQUIRE_EQ(*gltfHash,
               std::string{"30ae6c2dc2bcc9ab90c6868c21fd5d94b08ee6ce13d5bd4217155e2914b0deb4"});
    REQUIRE_EQ(*binHash,
               std::string{"c6556ee2a67875919aa1bd270fd511caf11710bc516568328feea6862db21057"});
}

CORSAIRS_TEST(GltfWriter_SceneMapMirrorsStaticPartExactlyOnce) {
    AC::LgoGeomObj object;
    object.Version = 0x1004u;
    object.Mesh.Positions = {
        AC::Vector3{0.0f, 0.0f, 0.0f},
        AC::Vector3{2.0f, 0.0f, 0.0f},
        AC::Vector3{0.0f, 1.0f, 0.0f},
    };
    object.Mesh.Normals = {
        AC::Vector3{0.0f, 0.0f, 1.0f},
        AC::Vector3{0.0f, 0.0f, 1.0f},
        AC::Vector3{0.0f, 0.0f, 1.0f},
    };
    object.Mesh.Indices = {0u, 1u, 2u};
    object.Mesh.Subsets.push_back(AC::SubsetInfo{1u, 0u, 3u, 0u});
    object.MatModel[12] = 3.0f;
    object.MatModel[13] = 4.0f;

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "scene-map-static.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, gltfPath, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    auto binPath = gltfPath;
    binPath.replace_extension(".bin");
    const auto binary = AC::ReadWholeFile(binPath);
    REQUIRE(binary.has_value());
    REQUIRE(binary->size() >=
            sizeof(AC::Vector3) * 6 + sizeof(std::uint32_t) * 3);

    std::array<AC::Vector3, 3> positions{};
    std::array<AC::Vector3, 3> normals{};
    std::array<std::uint32_t, 3> indices{};
    std::memcpy(positions.data(), binary->data(), sizeof(positions));
    std::memcpy(normals.data(), binary->data() + sizeof(positions), sizeof(normals));
    std::memcpy(indices.data(),
                binary->data() + sizeof(positions) + sizeof(normals),
                sizeof(indices));

    REQUIRE_EQ(positions[0].X, 0.0f);
    REQUIRE_EQ(positions[0].Y, 0.0f);
    REQUIRE_EQ(positions[0].Z, 0.0f);
    REQUIRE_EQ(positions[1].X, 2.0f);
    REQUIRE_EQ(positions[1].Y, 0.0f);
    REQUIRE_EQ(positions[1].Z, 0.0f);
    REQUIRE_EQ(positions[2].X, 0.0f);
    REQUIRE_EQ(positions[2].Y, 0.0f);
    REQUIRE_EQ(positions[2].Z, -1.0f);
    REQUIRE_EQ(normals[0].X, 0.0f);
    REQUIRE_EQ(normals[0].Y, 1.0f);
    REQUIRE_EQ(normals[0].Z, 0.0f);
    REQUIRE_EQ(indices[0], 0u);
    REQUIRE_EQ(indices[1], 1u);
    REQUIRE_EQ(indices[2], 2u);

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};
    REQUIRE(text.find(
        R"("nodes":[{"name":"part_root","children":[1],"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,3,0,-4,1]},{"mesh":0,"name":"mesh"}])") !=
            std::string::npos);
    REQUIRE(text.find(R"("scenes":[{"nodes":[0]}])") != std::string::npos);
}

CORSAIRS_TEST(GltfWriter_SceneMapStaticReferencePoseIsExplicitOptIn) {
    AC::LgoGeomObj object;
    object.Version = 0x1004u;
    object.Mesh.Positions = {
        AC::Vector3{0.0f, 0.0f, 0.0f},
        AC::Vector3{1.0f, 0.0f, 0.0f},
        AC::Vector3{0.0f, 1.0f, 0.0f},
    };
    object.Mesh.Indices = {0u, 1u, 2u};
    object.Mesh.Subsets.push_back(AC::SubsetInfo{1u, 0u, 3u, 0u});
    object.Mesh.BoneIndices = {0u};
    object.Mesh.Blends.resize(3);
    for (AC::BlendInfo& blend : object.Mesh.Blends) {
        blend.Weight[0] = 1.0f;
    }

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path rejectedPath =
        OutputDir() / "scene-map-skinned-rejected.gltf";
    auto rejectedBin = rejectedPath;
    rejectedBin.replace_extension(".bin");
    std::filesystem::remove(rejectedPath);
    std::filesystem::remove(rejectedBin);

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, rejectedPath, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap)),
               static_cast<std::uint32_t>(AC::GltfStatus::WRITE_FAILED));
    REQUIRE(!std::filesystem::exists(rejectedPath));
    REQUIRE(!std::filesystem::exists(rejectedBin));

    const std::filesystem::path staticPath =
        OutputDir() / "scene-map-skinned-static.gltf";
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, staticPath, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::StaticReferencePose)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const auto written = AC::ReadWholeFile(staticPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};
    REQUIRE(text.find(R"("name":"part_root")") != std::string::npos);
    REQUIRE(text.find(R"("JOINTS_0")") == std::string::npos);
    REQUIRE(text.find(R"("WEIGHTS_0")") == std::string::npos);
    REQUIRE(text.find(R"("skins")") == std::string::npos);
}

CORSAIRS_TEST(GltfWriter_WritesGltfAndBinForRealFile) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "sample.gltf";
    const std::filesystem::path binPath = OutputDir() / "sample.bin";
    std::filesystem::remove(gltfPath);
    std::filesystem::remove(binPath);

    std::string detail;
    const AC::GltfStatus status = AC::WriteGltf(*obj, gltfPath, detail);
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    REQUIRE(std::filesystem::exists(gltfPath));
    REQUIRE(std::filesystem::exists(binPath));
    REQUIRE(std::filesystem::file_size(binPath) > 0u);
}

CORSAIRS_TEST(GltfWriter_GltfDeclaresVersionAndMeshCounts) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "sample2.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*obj, gltfPath, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("version":"2.0")") != std::string::npos);
    REQUIRE(text.find(R"("POSITION")") != std::string::npos);
    REQUIRE(text.find(R"("meshes")") != std::string::npos);
    // Каждому подсету соответствует своя примитива.
    REQUIRE(text.find(R"("primitives")") != std::string::npos);
}

CORSAIRS_TEST(GltfWriter_MatrixConversionKeepsIdentity) {
    const float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    float out[16]{};
    AC::ConvertMatrixToGltf(identity, out);

    for (int i = 0; i < 16; ++i) {
        REQUIRE_EQ(out[i], identity[i]);
    }
}

CORSAIRS_TEST(GltfWriter_MatrixConversionSwapsTranslationYandZ) {
    // DirectX row-major: перенос в последней строке (индексы 12,13,14).
    // Высота в MindPower3D лежит по Z и обязана оказаться на Y.
    const float translate[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        2, 3, 5, 1,
    };
    float out[16]{};
    AC::ConvertMatrixToGltf(translate, out);

    REQUIRE_EQ(out[12], 2.0f);
    REQUIRE_EQ(out[13], 5.0f);   // была высота Z
    REQUIRE_EQ(out[14], 3.0f);   // был Y
    REQUIRE_EQ(out[15], 1.0f);
}

CORSAIRS_TEST(GltfWriter_MatrixConversionMovesRotationToSwappedAxes) {
    // Поворот на 90° вокруг Y в левосторонней системе.
    const float rotY90[16] = {
        0, 0, -1, 0,
        0, 1,  0, 0,
        1, 0,  0, 0,
        0, 0,  0, 1,
    };
    float out[16]{};
    AC::ConvertMatrixToGltf(rotY90, out);

    // P*M*P переставляет индексы строк и столбцов 1 и 2. Элемент (0,2)
    // приходит из (0,1), элемент (0,1) — из (0,2), и так далее.
    REQUIRE_EQ(out[1], -1.0f);   // (0,1) <- (0,2)
    REQUIRE_EQ(out[2], 0.0f);    // (0,2) <- (0,1)
    REQUIRE_EQ(out[4], 1.0f);    // (1,0) <- (2,0)
    REQUIRE_EQ(out[8], 0.0f);    // (2,0) <- (1,0)
    REQUIRE_EQ(out[10], 1.0f);   // (2,2) <- (1,1)
    REQUIRE_EQ(out[15], 1.0f);
}

CORSAIRS_TEST(GltfWriter_MatrixConversionIsItsOwnInverse) {
    // Перестановка двух осей обратна самой себе: двойное применение обязано
    // вернуть исходную матрицу. Это ловит перекос в индексах, который на
    // симметричных примерах незаметен.
    const float source[16] = {
         1,  2,  3,  4,
         5,  6,  7,  8,
         9, 10, 11, 12,
        13, 14, 15, 16,
    };
    float once[16]{};
    float twice[16]{};
    AC::ConvertMatrixToGltf(source, once);
    AC::ConvertMatrixToGltf(once, twice);

    for (int i = 0; i < 16; ++i) {
        REQUIRE_EQ(twice[i], source[i]);
    }
}

CORSAIRS_TEST(GltfWriter_EmitsDummyAttachPointsAsNodes) {
    // character/04090084.lgo содержит 5 dummy-точек крепления.
    const auto bytes = AC::ReadWholeFile(
        std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "model" /
        "character" / "04090084.lgo");
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());
    REQUIRE_EQ(obj->Helper.Dummies.size(), 5u);

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "dummies.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*obj, gltfPath, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("dummy_0")") != std::string::npos);
    REQUIRE(text.find(R"("dummy_4")") != std::string::npos);
    REQUIRE(text.find(R"("matrix")") != std::string::npos);
}

CORSAIRS_TEST(GltfWriter_EmitsSkinningForCharacterMesh) {
    // 0066000000.lgo: 250 вершин, 8 костей, boneInflFactor=2.
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());
    REQUIRE_EQ(obj->Mesh.BoneIndices.size(), 8u);
    REQUIRE_EQ(obj->Mesh.Blends.size(), 250u);

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "skinned.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*obj, gltfPath, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("JOINTS_0")") != std::string::npos);
    REQUIRE(text.find(R"("WEIGHTS_0")") != std::string::npos);
    REQUIRE(text.find(R"("skins")") != std::string::npos);
    REQUIRE(text.find(R"("skin":0)") != std::string::npos);
    // Имена суставов несут глобальные id костей: (3, 0, 6, 1, 7, 4, 5, 2).
    REQUIRE(text.find(R"("bone_3")") != std::string::npos);
    REQUIRE(text.find(R"("bone_2")") != std::string::npos);

    // Суставы обязаны быть в списке узлов сцены, иначе Interchange в UE
    // падает на ensure(SkeletonNodeUid). Узлов всего 1 (меш) + 0 (dummy) +
    // 8 (суставы), значит сцена перечисляет индексы 0..8.
    REQUIRE(text.find(R"("scenes":[{"nodes":[0,1,2,3,4,5,6,7,8]}])") != std::string::npos);
}

CORSAIRS_TEST(GltfWriter_SkipsSkinningForStaticMesh) {
    // Модель сцены без костей не должна получать skin.
    const auto bytes = AC::ReadWholeFile(
        std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "model" /
        "character" / "2000000003.lgo");
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());
    REQUIRE(obj->Mesh.BoneIndices.empty());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "static.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*obj, gltfPath, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("JOINTS_0")") == std::string::npos);
    REQUIRE(text.find(R"("skins")") == std::string::npos);
}

CORSAIRS_TEST(GltfWriter_EmitsMaterialWithTexture) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());
    REQUIRE_EQ(obj->Materials.size(), 1u);

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "textured.gltf";

    AC::GltfTextureOptions options;
    options.ResolvedTextures.resize(1);
    options.ResolvedTextures[0].push_back(
        std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "texture" /
        "character" / "0066000000.png");
    options.CopyTo = OutputDir() / "textures";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*obj, gltfPath, detail, options)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    // Текстура скопирована рядом с результатом.
    REQUIRE(std::filesystem::exists(OutputDir() / "textures" / "0066000000.png"));

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("materials")") != std::string::npos);
    REQUIRE(text.find(R"("images")") != std::string::npos);
    REQUIRE(text.find(R"("textures")") != std::string::npos);
    REQUIRE(text.find(R"("baseColorTexture")") != std::string::npos);
    REQUIRE(text.find(R"("uri":"textures/0066000000.png")") != std::string::npos);
    // Подсет связан с материалом.
    REQUIRE(text.find(R"("material":0)") != std::string::npos);
}

CORSAIRS_TEST(GltfWriter_EmitsMaterialWithoutTextureWhenUnresolved) {
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    std::filesystem::create_directories(OutputDir());
    const std::filesystem::path gltfPath = OutputDir() / "untextured.gltf";

    // Без разрешённых текстур материал всё равно пишется — с именем и цветом.
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*obj, gltfPath, detail)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const auto written = AC::ReadWholeFile(gltfPath);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("materials")") != std::string::npos);
    REQUIRE(text.find(R"("baseColorFactor")") != std::string::npos);
    REQUIRE(text.find(R"("images")") == std::string::npos);
    REQUIRE(text.find(R"("baseColorTexture")") == std::string::npos);
}

CORSAIRS_TEST(GltfWriter_RejectsMeshWithoutVertices) {
    AC::LgoGeomObj empty;
    empty.Version = 0x1004u;

    std::filesystem::create_directories(OutputDir());
    std::string detail;
    const AC::GltfStatus status =
        AC::WriteGltf(empty, OutputDir() / "empty.gltf", detail);
    REQUIRE_EQ(static_cast<std::uint32_t>(status),
               static_cast<std::uint32_t>(AC::GltfStatus::EMPTY_MESH));
}


CORSAIRS_TEST(GltfWriter_ReplacesNonFiniteNormalsAndUvs) {
    // Исходные данные местами содержат NaN и бесконечности: в нормалях у
    // восемнадцати моделей, в развёртке у трёх. Спецификация glTF таких
    // значений не допускает, а Interchange заменяет их нулями — для нормали
    // это означает отсутствие направления, и освещение грани ломается.
    const auto bytes = AC::ReadWholeFile(SampleLgo());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    auto object = AC::ParseLgo(*bytes, diag);
    REQUIRE(object.has_value());
    REQUIRE(!object->Mesh.Normals.empty());

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    object->Mesh.Normals[0] = AC::Vector3{nan, 0.0f, 0.0f};
    object->Mesh.Normals[1] = AC::Vector3{0.0f, inf, 0.0f};
    if (!object->Mesh.Texcoords[0].empty()) {
        object->Mesh.Texcoords[0][0] = AC::Vector2{nan, nan};
    }

    std::filesystem::create_directories(OutputDir());
    const auto path = OutputDir() / "sanitized.gltf";
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(*object, path, detail, {})),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    // Проверяем сам двоичный буфер: JSON нечисловые значения не показывает,
    // они лежат в .bin.
    auto binPath = path;
    binPath.replace_extension(".bin");
    const auto binary = AC::ReadWholeFile(binPath);
    REQUIRE(binary.has_value());

    const float* values = reinterpret_cast<const float*>(binary->data());
    const std::size_t count = binary->size() / sizeof(float);
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(std::isfinite(values[i]));
    }
}

} // namespace
