#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LegacySceneBake.h"
#include "Corsairs/Tools/AssetConverter/LmoParser.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"

#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path FountainModel() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
        "Client/model/scene/by-bd015.lmo";
}

std::filesystem::path SceneModel(const std::string& name) {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
        "Client/model/scene" / name;
}

std::string ReadText(const std::filesystem::path& path) {
    const auto bytes = AC::ReadWholeFile(path);
    if (!bytes.has_value()) {
        return {};
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

std::size_t CountOccurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::array<float, 16> IdentityMatrix() {
    return {1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1};
}

CORSAIRS_TEST(LegacySceneBake_OwnedTick120SamplesPriorZeroBasedFrame) {
    // Mutation caught: `tick % frameCount` would return 20/19/58 instead.
    REQUIRE_EQ(AC::LegacySampleFrameAfterOwnedTicks(120u, 100u), 19u);
    REQUIRE_EQ(AC::LegacySampleFrameAfterOwnedTicks(120u, 101u), 18u);
    REQUIRE_EQ(AC::LegacySampleFrameAfterOwnedTicks(120u, 61u), 58u);
}

CORSAIRS_TEST(LegacySceneBake_ParsesExactRequiredEmbeddedBoneCorpus) {
    // Mutation caught: skipping BoneDataSize still reports the byte count, but
    // cannot produce a skeleton, hierarchy or animation for SceneMap glTF.
    struct Case {
        const char* Model;
        std::size_t Object;
        std::uint32_t Version;
        std::uint32_t BoneDataSize;
        std::uint32_t BoneNum;
        std::uint32_t FrameNum;
        std::uint32_t DummyNum;
    };
    constexpr std::array<Case, 10> cases{{
        {"by-bd002.lmo", 8u, 0x1004u, 9656u, 4u, 47u, 1u},
        {"nml-bd199.lmo", 0u, 0x1004u, 39224u, 4u, 201u, 1u},
        {"nml-bd199.lmo", 1u, 0x1004u, 39224u, 4u, 201u, 1u},
        {"nml-bd199.lmo", 2u, 0x1004u, 39224u, 4u, 201u, 1u},
        {"nml-bd199.lmo", 3u, 0x1004u, 39224u, 4u, 201u, 1u},
        {"nml-bd199.lmo", 4u, 0x1004u, 39224u, 4u, 201u, 1u},
        {"nml-bd025.lmo", 0u, 0x1005u, 29992u, 6u, 101u, 1u},
        {"by-bd035.lmo", 1u, 0x1004u, 38424u, 8u, 97u, 1u},
        {"by-bd032.lmo", 1u, 0x1004u, 34976u, 7u, 101u, 1u},
        {"by-bd033.lmo", 1u, 0x1004u, 38424u, 8u, 97u, 1u},
    }};

    for (const Case& testCase : cases) {
        const auto bytes = AC::ReadWholeFile(SceneModel(testCase.Model));
        REQUIRE(bytes.has_value());
        AC::LgoDiagnostics diag;
        const auto model = AC::ParseLmo(*bytes, diag);
        REQUIRE(model.has_value());
        REQUIRE(testCase.Object < model->Objects.size());

        const AC::LgoGeomObj& object = model->Objects[testCase.Object];
        REQUIRE_EQ(object.Animation.BoneDataSize, testCase.BoneDataSize);
        REQUIRE(object.Animation.Bone.has_value());
        const AC::LabAnimation& bone = *object.Animation.Bone;
        REQUIRE_EQ(bone.Version, testCase.Version);
        REQUIRE_EQ(bone.Header.BoneNum, testCase.BoneNum);
        REQUIRE_EQ(bone.Header.FrameNum, testCase.FrameNum);
        REQUIRE_EQ(bone.Header.DummyNum, testCase.DummyNum);
        REQUIRE_EQ(static_cast<std::uint32_t>(bone.KeyType()),
                   static_cast<std::uint32_t>(AC::BoneKeyType::MAT43));
        REQUIRE_EQ(bone.Bones.size(), testCase.BoneNum);
        REQUIRE_EQ(bone.Tracks.size(), testCase.BoneNum);
        REQUIRE_EQ(bone.InverseBindMatrices.size(),
                   static_cast<std::size_t>(testCase.BoneNum) * 16u);
        REQUIRE_EQ(bone.Dummies.size(), testCase.DummyNum);
        REQUIRE_EQ(object.Mesh.Blends.size(), object.Mesh.Positions.size());
        REQUIRE(!object.Mesh.BoneIndices.empty());
        for (const std::uint32_t boneIndex : object.Mesh.BoneIndices) {
            REQUIRE(boneIndex < testCase.BoneNum);
        }
    }
}

CORSAIRS_TEST(LegacySceneBake_PreserveAnimatedSamplesTick119ForRequiredCorpus) {
    // Mutation caught: using captureTick % FrameNum shifts every required
    // animated part by one source frame.
    struct Case {
        const char* Model;
        std::size_t Object;
        std::uint32_t FrameNum;
        std::uint32_t SampleFrame;
    };
    constexpr std::array<Case, 10> cases{{
        {"by-bd002.lmo", 8u, 47u, 25u},
        {"nml-bd199.lmo", 0u, 201u, 119u},
        {"nml-bd199.lmo", 1u, 201u, 119u},
        {"nml-bd199.lmo", 2u, 201u, 119u},
        {"nml-bd199.lmo", 3u, 201u, 119u},
        {"nml-bd199.lmo", 4u, 201u, 119u},
        {"nml-bd025.lmo", 0u, 101u, 18u},
        {"by-bd035.lmo", 1u, 97u, 22u},
        {"by-bd032.lmo", 1u, 101u, 18u},
        {"by-bd033.lmo", 1u, 97u, 22u},
    }};

    for (const Case& testCase : cases) {
        const auto bytes = AC::ReadWholeFile(SceneModel(testCase.Model));
        REQUIRE(bytes.has_value());
        AC::LgoDiagnostics diag;
        auto model = AC::ParseLmo(*bytes, diag);
        REQUIRE(model.has_value());
        REQUIRE(testCase.Object < model->Objects.size());

        AC::LgoGeomObj& object = model->Objects[testCase.Object];
        std::string detail;
        REQUIRE_EQ(static_cast<std::uint32_t>(AC::BakeLegacyCaptureState(
                       object, 120u, detail,
                       AC::LegacyBoneCapturePolicy::PreserveAnimated)),
                   static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));
        REQUIRE(object.CaptureBake.BonePreserveAnimated);
        REQUIRE(!object.CaptureBake.BoneStaticReferencePose);
        REQUIRE_EQ(object.CaptureBake.BoneFrameCount, testCase.FrameNum);
        REQUIRE_EQ(object.CaptureBake.BoneSampleFrame, testCase.SampleFrame);
    }
}

CORSAIRS_TEST(LegacySceneBake_SceneMapWritesAnimatedSkinWithScaleAndOwnedRoot) {
    // Mutation caught: the old SceneMap guard rejects every skinned part; the
    // old node offsets also attach root bone 0 to the mesh node.
    AC::LgoGeomObj object;
    object.Version = 0x1004u;
    object.Mesh.Positions = {
        AC::Vector3{0.0f, 0.0f, 0.0f},
        AC::Vector3{1.0f, 0.0f, 0.0f},
        AC::Vector3{0.0f, 1.0f, 0.0f},
    };
    object.Mesh.Indices = {0u, 1u, 2u};
    object.Mesh.Subsets = {AC::SubsetInfo{1u, 0u, 3u, 0u}};
    object.Mesh.BoneIndices = {0u, 1u};
    object.Mesh.Blends.resize(3);
    for (AC::BlendInfo& blend : object.Mesh.Blends) {
        blend.Index[0] = 0u;
        blend.Weight[0] = 1.0f;
    }
    object.MatModel[12] = 10.0f;
    object.MatModel[13] = 20.0f;
    object.MatModel[14] = 30.0f;

    AC::HelperDummyInfo modelDummy{};
    const auto identity = IdentityMatrix();
    std::copy(identity.begin(), identity.end(), modelDummy.Mat);
    object.Helper.Dummies.push_back(modelDummy);

    AC::LabAnimation animation;
    animation.Version = 0x1004u;
    animation.Header = AC::BoneInfoHeader{
        2u, 2u, 1u, static_cast<std::uint32_t>(AC::BoneKeyType::MAT43)};
    animation.Bones.resize(2);
    std::copy_n("root", 4u, animation.Bones[0].Name);
    animation.Bones[0].Id = 0u;
    animation.Bones[0].ParentId = AC::kNoParent;
    std::copy_n("child", 5u, animation.Bones[1].Name);
    animation.Bones[1].Id = 1u;
    animation.Bones[1].ParentId = 0u;
    animation.InverseBindMatrices.insert(
        animation.InverseBindMatrices.end(), identity.begin(), identity.end());
    animation.InverseBindMatrices.insert(
        animation.InverseBindMatrices.end(), identity.begin(), identity.end());
    AC::BoneDummyInfo boneDummy{};
    boneDummy.Id = 7u;
    boneDummy.ParentBoneId = 1u;
    std::copy(identity.begin(), identity.end(), boneDummy.Mat);
    animation.Dummies.push_back(boneDummy);
    animation.Tracks.resize(2);
    const std::array<float, 16> scaledFrame{
        2, 0, 0, 0,
        0, 3, 0, 0,
        0, 0, 4, 0,
        5, 6, 7, 1};
    animation.Tracks[0].Matrices.insert(
        animation.Tracks[0].Matrices.end(), scaledFrame.begin(), scaledFrame.end());
    animation.Tracks[0].Matrices.insert(
        animation.Tracks[0].Matrices.end(), identity.begin(), identity.end());
    animation.Tracks[1].Matrices.insert(
        animation.Tracks[1].Matrices.end(), identity.begin(), identity.end());
    animation.Tracks[1].Matrices.insert(
        animation.Tracks[1].Matrices.end(), identity.begin(), identity.end());
    object.Animation.Bone = animation;
    object.Animation.BoneDataSize = 424u;

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::BakeLegacyCaptureState(
                   object, 120u, detail,
                   AC::LegacyBoneCapturePolicy::PreserveAnimated)),
               static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));

    const std::filesystem::path outputRoot =
        std::filesystem::temp_directory_path() / "corsairs-scene-bone-writer-test";
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    std::filesystem::create_directories(outputRoot, error);
    REQUIRE(!error);
    const std::filesystem::path gltfPath = outputRoot / "animated.gltf";

    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, gltfPath, detail, {}, &*object.Animation.Bone,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::Preserve)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    const std::string gltf = ReadText(gltfPath);
    REQUIRE(gltf.find(R"("JOINTS_0")") != std::string::npos);
    REQUIRE(gltf.find(R"("WEIGHTS_0")") != std::string::npos);
    REQUIRE(gltf.find(R"("inverseBindMatrices")") != std::string::npos);
    REQUIRE(gltf.find(R"("animations":[{"name":"legacy_bone")") !=
            std::string::npos);
    REQUIRE_EQ(CountOccurrences(gltf, R"("path":"translation")"), 2u);
    REQUIRE_EQ(CountOccurrences(gltf, R"("path":"rotation")"), 2u);
    REQUIRE_EQ(CountOccurrences(gltf, R"("path":"scale")"), 2u);
    REQUIRE(gltf.find(
        R"("name":"part_root","children":[1,2,3],"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,10,30,20,1])") !=
            std::string::npos);
    REQUIRE(gltf.find(R"("joints":[3,4],"skeleton":3)") !=
            std::string::npos);
    REQUIRE(gltf.find(R"("target":{"node":3,"path":"scale"})") !=
            std::string::npos);
    REQUIRE(gltf.find(R"("scale":[2,4,3])") != std::string::npos);
    // glTF allows bufferView.target only for ARRAY_BUFFER/ELEMENT_ARRAY_BUFFER;
    // animation and inverse-bind views must omit it rather than emit zero.
    REQUIRE(gltf.find(R"("target":0)") == std::string::npos);

    std::filesystem::remove_all(outputRoot, error);
}

CORSAIRS_TEST(LegacySceneBake_SceneMapBoneQuaternionUsesGenericBasis) {
    AC::LgoGeomObj object;
    object.Version = 0x1004u;
    object.Mesh.Positions = {
        AC::Vector3{0.0f, 0.0f, 0.0f},
        AC::Vector3{1.0f, 0.0f, 0.0f},
        AC::Vector3{0.0f, 1.0f, 0.0f},
    };
    object.Mesh.Indices = {0u, 1u, 2u};
    object.Mesh.Subsets = {AC::SubsetInfo{1u, 0u, 3u, 0u}};
    object.Mesh.BoneIndices = {0u};
    object.Mesh.Blends.resize(3);
    for (AC::BlendInfo& blend : object.Mesh.Blends) {
        blend.Index[0] = 0u;
        blend.Weight[0] = 1.0f;
    }

    AC::LabAnimation animation;
    animation.Version = 0x1004u;
    animation.Header = AC::BoneInfoHeader{
        1u, 1u, 0u, static_cast<std::uint32_t>(AC::BoneKeyType::QUAT)};
    animation.Bones.resize(1);
    animation.Bones[0].Id = 0u;
    animation.Bones[0].ParentId = AC::kNoParent;
    const auto identity = IdentityMatrix();
    animation.InverseBindMatrices.insert(
        animation.InverseBindMatrices.end(), identity.begin(), identity.end());
    animation.Tracks.resize(1);
    animation.Tracks[0].Positions.push_back(AC::Vector3{0.0f, 0.0f, 0.0f});
    constexpr float halfRoot = 0.7071067811865475f;
    animation.Tracks[0].Rotations.push_back(
        AC::Quaternion{halfRoot, 0.0f, 0.0f, halfRoot});
    object.Animation.Bone = animation;

    const std::filesystem::path outputRoot =
        std::filesystem::temp_directory_path() /
        "corsairs-scene-quat-basis-test";
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    std::filesystem::create_directories(outputRoot, error);
    REQUIRE(!error);
    const std::filesystem::path gltfPath = outputRoot / "quat.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, gltfPath, detail, {}, &*object.Animation.Bone,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::Preserve)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));

    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");
    const auto binary = AC::ReadWholeFile(binPath);
    REQUIRE(binary.has_value());
    // Layout для этого literal fixture: POSITION 36, JOINTS 12, WEIGHTS 48,
    // IBM 64, indices 12, time 4, translation 12, затем rotation VEC4.
    constexpr std::size_t rotationOffset = 188u;
    REQUIRE(binary->size() >= rotationOffset + sizeof(float) * 4u);
    std::array<float, 4> rotation{};
    std::memcpy(rotation.data(), binary->data() + rotationOffset,
                sizeof(rotation));
    REQUIRE(std::fabs(rotation[0] + halfRoot) < 0.000001f);
    REQUIRE_EQ(rotation[1], 0.0f);
    REQUIRE_EQ(rotation[2], 0.0f);
    REQUIRE(std::fabs(rotation[3] - halfRoot) < 0.000001f);
    REQUIRE(ReadText(gltfPath).find(R"("animations":[{"name":"legacy_bone")") !=
            std::string::npos);

    std::filesystem::remove_all(outputRoot, error);
}

CORSAIRS_TEST(LegacySceneBake_RequiredTenPartsWriteTrueBoneAnimations) {
    struct Case {
        const char* Model;
        std::size_t Object;
        std::uint32_t BoneNum;
        std::uint32_t FrameNum;
        std::uint32_t SampleFrame;
        std::uint32_t PlacementCount;
    };
    constexpr std::array<Case, 10> cases{{
        {"by-bd002.lmo", 8u, 4u, 47u, 25u, 1u},
        {"nml-bd199.lmo", 0u, 4u, 201u, 119u, 5u},
        {"nml-bd199.lmo", 1u, 4u, 201u, 119u, 5u},
        {"nml-bd199.lmo", 2u, 4u, 201u, 119u, 5u},
        {"nml-bd199.lmo", 3u, 4u, 201u, 119u, 5u},
        {"nml-bd199.lmo", 4u, 4u, 201u, 119u, 5u},
        {"nml-bd025.lmo", 0u, 6u, 101u, 18u, 5u},
        {"by-bd035.lmo", 1u, 8u, 97u, 22u, 1u},
        {"by-bd032.lmo", 1u, 7u, 101u, 18u, 1u},
        {"by-bd033.lmo", 1u, 8u, 97u, 22u, 2u},
    }};

    const std::filesystem::path outputRoot =
        std::filesystem::temp_directory_path() / "corsairs-required-true-bone-test";
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    std::filesystem::create_directories(outputRoot, error);
    REQUIRE(!error);

    std::uint32_t placementCount = 0;
    std::uint32_t boneTrackCount = 0;
    for (const Case& testCase : cases) {
        const auto bytes = AC::ReadWholeFile(SceneModel(testCase.Model));
        REQUIRE(bytes.has_value());
        AC::LgoDiagnostics diag;
        auto model = AC::ParseLmo(*bytes, diag);
        REQUIRE(model.has_value());
        REQUIRE(testCase.Object < model->Objects.size());
        AC::LgoGeomObj& object = model->Objects[testCase.Object];
        REQUIRE(object.Animation.Bone.has_value());

        std::string detail;
        REQUIRE_EQ(static_cast<std::uint32_t>(AC::BakeLegacyCaptureState(
                       object, 120u, detail,
                       AC::LegacyBoneCapturePolicy::PreserveAnimated)),
                   static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));

        const std::filesystem::path gltfPath = outputRoot /
            std::format("{}_{}.gltf",
                        std::filesystem::path{testCase.Model}.stem().string(),
                        testCase.Object);
        REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                       object, gltfPath, detail, {}, &*object.Animation.Bone,
                       AC::GltfCoordinateProfile::SceneMap,
                       AC::GltfSkinPolicy::Preserve)),
                   static_cast<std::uint32_t>(AC::GltfStatus::OK));

        const std::string gltf = ReadText(gltfPath);
        REQUIRE(gltf.find(R"("skins")") != std::string::npos);
        REQUIRE(gltf.find(R"("animations")") != std::string::npos);
        REQUIRE_EQ(CountOccurrences(gltf, R"("path":"translation")"),
                   testCase.BoneNum);
        REQUIRE_EQ(CountOccurrences(gltf, R"("path":"rotation")"),
                   testCase.BoneNum);
        REQUIRE_EQ(CountOccurrences(gltf, R"("path":"scale")"),
                   testCase.BoneNum);
        REQUIRE(gltf.find(std::format(
                    R"("policy":"preserveAnimated","frameCount":{},"sampleFrame":{})",
                    testCase.FrameNum, testCase.SampleFrame)) !=
                std::string::npos);

        placementCount += testCase.PlacementCount;
        boneTrackCount += testCase.BoneNum;
    }
    REQUIRE_EQ(placementCount, 35u);
    REQUIRE_EQ(boneTrackCount, 53u);

    std::filesystem::remove_all(outputRoot, error);
}

CORSAIRS_TEST(LegacySceneBake_SceneMapRejectsCyclicBoneHierarchyWithoutOutput) {
    const auto bytes = AC::ReadWholeFile(SceneModel("by-bd002.lmo"));
    REQUIRE(bytes.has_value());
    AC::LgoDiagnostics diag;
    auto model = AC::ParseLmo(*bytes, diag);
    REQUIRE(model.has_value());
    AC::LgoGeomObj object = model->Objects[8];
    REQUIRE(object.Animation.Bone.has_value());
    REQUIRE(object.Animation.Bone->Bones.size() > 1u);
    object.Animation.Bone->Bones[1].ParentId = 1u;

    const std::filesystem::path outputRoot =
        std::filesystem::temp_directory_path() / "corsairs-invalid-bone-writer-test";
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    std::filesystem::create_directories(outputRoot, error);
    REQUIRE(!error);
    const std::filesystem::path gltfPath = outputRoot / "cyclic.gltf";
    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, gltfPath, detail, {}, &*object.Animation.Bone,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::Preserve)),
               static_cast<std::uint32_t>(AC::GltfStatus::INVALID_SKIN_DATA));
    REQUIRE(!std::filesystem::exists(gltfPath));
    REQUIRE(!std::filesystem::exists(binPath));

    std::filesystem::remove_all(outputRoot, error);
}

CORSAIRS_TEST(LegacySceneBake_SceneMapRejectsForwardParentWithInvalidAncestor) {
    const auto bytes = AC::ReadWholeFile(SceneModel("by-bd002.lmo"));
    REQUIRE(bytes.has_value());
    AC::LgoDiagnostics diag;
    auto model = AC::ParseLmo(*bytes, diag);
    REQUIRE(model.has_value());
    AC::LgoGeomObj object = model->Objects[8];
    REQUIRE(object.Animation.Bone.has_value());
    REQUIRE(object.Animation.Bone->Bones.size() > 1u);
    object.Animation.Bone->Bones[0].ParentId = 1u;
    object.Animation.Bone->Bones[1].ParentId = 999u;

    const std::filesystem::path outputRoot =
        std::filesystem::temp_directory_path() /
        "corsairs-forward-invalid-bone-writer-test";
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    std::filesystem::create_directories(outputRoot, error);
    REQUIRE(!error);
    const std::filesystem::path gltfPath = outputRoot / "invalid.gltf";
    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, gltfPath, detail, {}, &*object.Animation.Bone,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::Preserve)),
               static_cast<std::uint32_t>(AC::GltfStatus::INVALID_SKIN_DATA));
    REQUIRE_EQ(detail, std::string{"BONE hierarchy повреждена на кости 1"});
    REQUIRE(!std::filesystem::exists(gltfPath));
    REQUIRE(!std::filesystem::exists(binPath));

    std::filesystem::remove_all(outputRoot, error);
}

CORSAIRS_TEST(LegacySceneBake_RejectsNonFiniteMatBeforeMutation) {
    AC::LgoGeomObj object;
    const auto identity = IdentityMatrix();
    std::copy(identity.begin(), identity.end(), object.MatModel);
    object.Animation.Matrix.emplace();
    object.Animation.Matrix->Frames.push_back(identity);
    object.Animation.Matrix->Frames[0][7] =
        std::numeric_limits<float>::quiet_NaN();

    std::string detail;
    const AC::LegacyBakeStatus status =
        AC::BakeLegacyCaptureState(object, 120u, detail);
    REQUIRE(status != AC::LegacyBakeStatus::OK);
    REQUIRE(!object.CaptureBake.Applied);
    REQUIRE(std::equal(identity.begin(), identity.end(), object.MatModel));
}

CORSAIRS_TEST(LegacySceneBake_RejectsNonFiniteTexUvBeforeMutation) {
    AC::LgoGeomObj object;
    object.Mesh.Texcoords[0] = {
        AC::Vector2{0.0f, 0.0f},
        AC::Vector2{1.0f, 0.0f},
        AC::Vector2{0.0f, 1.0f},
    };
    object.Mesh.Indices = {0u, 1u, 2u};
    object.Mesh.Subsets = {AC::SubsetInfo{1u, 0u, 3u, 0u}};
    AC::LgoTexUvAnimation controller;
    controller.Subset = 0u;
    controller.Stage = 0u;
    controller.Frames.push_back(IdentityMatrix());
    controller.Frames[0][15] = std::numeric_limits<float>::infinity();
    object.Animation.TexUv.push_back(controller);
    const auto originalTexcoords = object.Mesh.Texcoords[0];

    std::string detail;
    const AC::LegacyBakeStatus status =
        AC::BakeLegacyCaptureState(object, 120u, detail);
    REQUIRE(status != AC::LegacyBakeStatus::OK);
    REQUIRE(!object.CaptureBake.Applied);
    REQUIRE_EQ(object.Mesh.Texcoords[0].size(), originalTexcoords.size());
    for (std::size_t index = 0; index < originalTexcoords.size(); ++index) {
        REQUIRE_EQ(object.Mesh.Texcoords[0][index].X, originalTexcoords[index].X);
        REQUIRE_EQ(object.Mesh.Texcoords[0][index].Y, originalTexcoords[index].Y);
    }
}

CORSAIRS_TEST(LegacySceneBake_RejectsFiniteTexUvWhoseBakeOverflows) {
    AC::LgoGeomObj object;
    object.Mesh.Texcoords[0] = {
        AC::Vector2{std::numeric_limits<float>::max(), 0.0f},
        AC::Vector2{0.0f, 0.0f},
        AC::Vector2{0.0f, 1.0f},
    };
    object.Mesh.Indices = {0u, 1u, 2u};
    object.Mesh.Subsets = {AC::SubsetInfo{1u, 0u, 3u, 0u}};
    AC::LgoTexUvAnimation controller;
    controller.Subset = 0u;
    controller.Stage = 0u;
    controller.Frames.push_back(IdentityMatrix());
    controller.Frames[0][0] = std::numeric_limits<float>::max();
    object.Animation.TexUv.push_back(controller);
    const auto originalTexcoords = object.Mesh.Texcoords[0];

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BakeLegacyCaptureState(object, 120u, detail)),
               static_cast<std::uint32_t>(
                   AC::LegacyBakeStatus::NON_FINITE_CONTROLLER));
    REQUIRE(!object.CaptureBake.Applied);
    REQUIRE_EQ(object.Mesh.Texcoords[0][0].X, originalTexcoords[0].X);
    REQUIRE_EQ(object.Mesh.Texcoords[0][0].Y, originalTexcoords[0].Y);
}

CORSAIRS_TEST(LegacySceneBake_RejectsTexUvSharedWithUncontrolledSubset) {
    AC::LgoGeomObj object;
    object.Mesh.Texcoords[0] = {
        AC::Vector2{0.25f, 0.5f},
        AC::Vector2{1.0f, 0.0f},
        AC::Vector2{0.0f, 1.0f},
        AC::Vector2{1.0f, 1.0f},
        AC::Vector2{2.0f, 1.0f},
    };
    // Vertex 0 принадлежит и animated subset 0, и uncontrolled subset 1.
    object.Mesh.Indices = {0u, 1u, 2u, 0u, 3u, 4u};
    object.Mesh.Subsets = {
        AC::SubsetInfo{1u, 0u, 3u, 0u},
        AC::SubsetInfo{1u, 3u, 3u, 0u},
    };
    AC::LgoTexUvAnimation controller;
    controller.Subset = 0u;
    controller.Stage = 0u;
    controller.Frames.push_back(IdentityMatrix());
    controller.Frames[0][8] = 2.0f;
    object.Animation.TexUv.push_back(controller);
    const auto originalTexcoords = object.Mesh.Texcoords[0];

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BakeLegacyCaptureState(object, 120u, detail)),
               static_cast<std::uint32_t>(
                   AC::LegacyBakeStatus::CONFLICTING_SHARED_VERTEX));
    REQUIRE(!object.CaptureBake.Applied);
    REQUIRE_EQ(object.Mesh.Texcoords[0][0].X, originalTexcoords[0].X);
    REQUIRE_EQ(object.Mesh.Texcoords[0][0].Y, originalTexcoords[0].Y);
}

CORSAIRS_TEST(LegacySceneBake_ModelRecomposesAnimatedChildWithParent) {
    auto makeObject = [](std::uint32_t id, std::uint32_t parent,
                         float localX) {
        AC::LgoGeomObj object;
        object.Header.Id = id;
        object.Header.ParentId = parent;
        const auto identity = IdentityMatrix();
        std::copy(identity.begin(), identity.end(), object.Header.MatLocal);
        object.Header.MatLocal[12] = localX;
        std::copy_n(object.Header.MatLocal, 16u, object.MatModel);
        return object;
    };
    std::vector<AC::LgoGeomObj> objects{
        makeObject(10u, 0xFFFFFFFFu, 10.0f),
        makeObject(20u, 10u, 5.0f),
    };
    AC::ResolveModelMatrices(objects);
    REQUIRE_EQ(objects[1].MatModel[12], 15.0f);

    auto animatedLocal = IdentityMatrix();
    animatedLocal[12] = 7.0f;
    objects[1].Animation.Matrix.emplace();
    objects[1].Animation.Matrix->Frames.push_back(animatedLocal);

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::BakeLegacyModelCaptureState(
                   objects, 120u, detail,
                   AC::LegacyBoneCapturePolicy::PreserveAnimated)),
               static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));
    REQUIRE_EQ(objects[1].Header.MatLocal[12], 7.0f);
    REQUIRE_EQ(objects[1].MatModel[12], 17.0f);
    REQUIRE(objects[1].CaptureBake.Applied);
}

CORSAIRS_TEST(LegacySceneBake_SceneMapRejectsFiniteBoneMatrixWhoseTrsOverflows) {
    const auto bytes = AC::ReadWholeFile(SceneModel("by-bd002.lmo"));
    REQUIRE(bytes.has_value());
    AC::LgoDiagnostics diag;
    auto model = AC::ParseLmo(*bytes, diag);
    REQUIRE(model.has_value());
    AC::LgoGeomObj object = model->Objects[8];
    REQUIRE(object.Animation.Bone.has_value());
    REQUIRE(!object.Animation.Bone->Tracks[0].Matrices.empty());
    object.Animation.Bone->Tracks[0].Matrices[0] =
        std::numeric_limits<float>::max();

    const std::filesystem::path outputRoot =
        std::filesystem::temp_directory_path() /
        "corsairs-overflow-bone-writer-test";
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    std::filesystem::create_directories(outputRoot, error);
    REQUIRE(!error);
    const std::filesystem::path gltfPath = outputRoot / "invalid.gltf";
    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   object, gltfPath, detail, {}, &*object.Animation.Bone,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::Preserve)),
               static_cast<std::uint32_t>(AC::GltfStatus::INVALID_SKIN_DATA));
    REQUIRE(detail.find("frame 0") != std::string::npos);
    REQUIRE(!std::filesystem::exists(gltfPath));
    REQUIRE(!std::filesystem::exists(binPath));

    std::filesystem::remove_all(outputRoot, error);
}

CORSAIRS_TEST(LegacySceneBake_RealModel22PreservesAndBakesMatTexUv) {
    const auto bytes = AC::ReadWholeFile(FountainModel());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    auto model = AC::ParseLmo(*bytes, diag);
    REQUIRE(model.has_value());
    REQUIRE_EQ(model->Version, 0x1004u);
    REQUIRE_EQ(model->Objects.size(), 22u);

    std::size_t matrixControllers = 0;
    std::size_t texUvControllers = 0;
    for (const AC::LgoGeomObj& object : model->Objects) {
        matrixControllers += object.Animation.Matrix.has_value() ? 1u : 0u;
        texUvControllers += object.Animation.TexUv.size();
        REQUIRE_EQ(object.Animation.BoneDataSize, 0u);
        REQUIRE_EQ(object.Animation.TexImageControllerCount, 0u);
    }
    REQUIRE_EQ(matrixControllers, 11u);
    REQUIRE_EQ(texUvControllers, 4u);
    REQUIRE_EQ(model->Objects[3].Animation.Matrix->Frames.size(), 701u);
    REQUIRE_EQ(model->Objects[11].Animation.Matrix->Frames.size(), 61u);
    REQUIRE_EQ(model->Objects[20].Animation.TexUv[0].Frames.size(), 100u);

    const AC::LgoGeomObj& sourcePart13 = model->Objects[13];
    const AC::LgoGeomObj& sourcePart17 = model->Objects[17];
    REQUIRE_EQ(sourcePart13.Mesh.VertexColors.size(),
               sourcePart13.Mesh.Positions.size());
    REQUIRE_EQ(sourcePart13.Mesh.VertexColors[0], 0xff123534u);
    REQUIRE_EQ(sourcePart17.Mesh.VertexColors.size(),
               sourcePart17.Mesh.Positions.size());
    const auto tintedPart17 = std::find_if(
        sourcePart17.Mesh.VertexColors.begin(),
        sourcePart17.Mesh.VertexColors.end(),
        [](const std::uint32_t color) { return color != 0xffffffffu; });
    REQUIRE(tintedPart17 != sourcePart17.Mesh.VertexColors.end());

    std::string detail;
    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::BakeLegacyCaptureState(model->Objects[3], 120u, detail)),
        static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));
    REQUIRE_EQ(model->Objects[3].CaptureBake.MatrixSampleFrame, 119u);
    REQUIRE(std::fabs(model->Objects[3].MatModel[5] - (-0.5744382f)) < 0.000001f);

    const float sourceU = model->Objects[20].Mesh.Texcoords[0][0].X;
    const float sourceV = model->Objects[20].Mesh.Texcoords[0][0].Y;
    REQUIRE(std::fabs(sourceU - 0.25024974f) < 0.000001f);
    REQUIRE(std::fabs(sourceV - 0.79970032f) < 0.000001f);

    REQUIRE_EQ(
        static_cast<std::uint32_t>(
            AC::BakeLegacyCaptureState(model->Objects[20], 120u, detail)),
        static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));
    REQUIRE_EQ(model->Objects[20].CaptureBake.TexUvSamples.size(), 1u);
    REQUIRE_EQ(model->Objects[20].CaptureBake.TexUvSamples[0].SampleFrame, 19u);
    REQUIRE(std::fabs(model->Objects[20].Mesh.Texcoords[0][0].X - 0.25024974f) <
            0.000001f);
    REQUIRE(std::fabs(model->Objects[20].Mesh.Texcoords[0][0].Y - 1.5673771f) <
            0.000001f);

    const std::filesystem::path outputRoot =
        std::filesystem::temp_directory_path() / "corsairs-id22-tick120-test";
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    std::filesystem::create_directories(outputRoot, error);
    REQUIRE(!error);

    const std::filesystem::path matrixPath = outputRoot / "by-bd015_3.gltf";
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   model->Objects[3], matrixPath, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::StaticReferencePose)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));
    const std::string matrixGltf = ReadText(matrixPath);
    REQUIRE(matrixGltf.find(
        R"("corsairsLegacyCapture":{"schemaVersion":1,"captureTick":120,"matrix":{"frameCount":701,"sampleFrame":119},"texUv":[]})") !=
        std::string::npos);

    const std::filesystem::path uvPath = outputRoot / "by-bd015_20.gltf";
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   model->Objects[20], uvPath, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::StaticReferencePose)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));
    const std::string uvGltf = ReadText(uvPath);
    REQUIRE(uvGltf.find(
        R"("corsairsLegacyCapture":{"schemaVersion":1,"captureTick":120,"matrix":null,"texUv":[{"subset":0,"stage":0,"frameCount":100,"sampleFrame":19}]})") !=
        std::string::npos);

    // Golden current Generic output: adding legacy vertex tint to SceneMap
    // must not perturb either JSON or binary bytes in the default profile.
    const std::filesystem::path genericPath = outputRoot / "by-bd015_13.gltf";
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   sourcePart13, genericPath, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::Generic)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));
    std::filesystem::path genericBinPath = genericPath;
    genericBinPath.replace_extension(".bin");
    const auto genericGltfHash = AC::Sha256File(genericPath, detail);
    const auto genericBinHash = AC::Sha256File(genericBinPath, detail);
    REQUIRE(genericGltfHash.has_value());
    REQUIRE(genericBinHash.has_value());
    REQUIRE_EQ(*genericGltfHash,
               std::string{"39afc5237df962d87e3b2e5e4f4cbdae4521ab613cf4cd3ed3493d8714d8f268"});
    REQUIRE_EQ(*genericBinHash,
               std::string{"a9718e36c20a9c019c775bda5c8d344b73b5829ee9004a0664c85c5603dfe657"});

    AC::LgoGeomObj scenePart13 = sourcePart13;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BakeLegacyCaptureState(scenePart13, 120u, detail)),
               static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));
    const std::filesystem::path scenePart13Path =
        outputRoot / "scene-by-bd015_13.gltf";
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   scenePart13, scenePart13Path, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::StaticReferencePose)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));
    const std::string scenePart13Gltf = ReadText(scenePart13Path);
    REQUIRE(scenePart13Gltf.find(R"("COLOR_0":3)") != std::string::npos);
    REQUIRE(scenePart13Gltf.find(
        R"("componentType":5121,"count":9,"type":"VEC4","normalized":true)") !=
        std::string::npos);
    std::filesystem::path scenePart13BinPath = scenePart13Path;
    scenePart13BinPath.replace_extension(".bin");
    const auto scenePart13Bin = AC::ReadWholeFile(scenePart13BinPath);
    REQUIRE(scenePart13Bin.has_value());
    const std::size_t part13ColorOffset =
        sourcePart13.Mesh.Positions.size() * sizeof(AC::Vector3) +
        sourcePart13.Mesh.Normals.size() * sizeof(AC::Vector3) +
        sourcePart13.Mesh.Texcoords[0].size() * sizeof(AC::Vector2);
    REQUIRE(scenePart13Bin->size() >= part13ColorOffset + 4u);
    REQUIRE_EQ((*scenePart13Bin)[part13ColorOffset + 0u], 0x12u);
    REQUIRE_EQ((*scenePart13Bin)[part13ColorOffset + 1u], 0x35u);
    REQUIRE_EQ((*scenePart13Bin)[part13ColorOffset + 2u], 0x34u);
    REQUIRE_EQ((*scenePart13Bin)[part13ColorOffset + 3u], 0xffu);

    AC::LgoGeomObj scenePart17 = sourcePart17;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::BakeLegacyCaptureState(scenePart17, 120u, detail)),
               static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));
    const std::filesystem::path scenePart17Path =
        outputRoot / "scene-by-bd015_17.gltf";
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                   scenePart17, scenePart17Path, detail, {}, nullptr,
                   AC::GltfCoordinateProfile::SceneMap,
                   AC::GltfSkinPolicy::StaticReferencePose)),
               static_cast<std::uint32_t>(AC::GltfStatus::OK));
    REQUIRE(ReadText(scenePart17Path).find(R"("COLOR_0":3)") !=
            std::string::npos);
    std::filesystem::path scenePart17BinPath = scenePart17Path;
    scenePart17BinPath.replace_extension(".bin");
    const auto scenePart17Bin = AC::ReadWholeFile(scenePart17BinPath);
    REQUIRE(scenePart17Bin.has_value());
    const std::size_t part17ColorOffset =
        sourcePart17.Mesh.Positions.size() * sizeof(AC::Vector3) +
        sourcePart17.Mesh.Normals.size() * sizeof(AC::Vector3) +
        sourcePart17.Mesh.Texcoords[0].size() * sizeof(AC::Vector2);
    const std::size_t tintedVertex = static_cast<std::size_t>(
        tintedPart17 - sourcePart17.Mesh.VertexColors.begin());
    const std::size_t tintedByteOffset = part17ColorOffset + tintedVertex * 4u;
    REQUIRE(scenePart17Bin->size() >= tintedByteOffset + 4u);
    const std::uint32_t literalTint = *tintedPart17;
    REQUIRE_EQ((*scenePart17Bin)[tintedByteOffset + 0u],
               static_cast<std::uint8_t>((literalTint >> 16u) & 0xffu));
    REQUIRE_EQ((*scenePart17Bin)[tintedByteOffset + 1u],
               static_cast<std::uint8_t>((literalTint >> 8u) & 0xffu));
    REQUIRE_EQ((*scenePart17Bin)[tintedByteOffset + 2u],
               static_cast<std::uint8_t>(literalTint & 0xffu));
    REQUIRE_EQ((*scenePart17Bin)[tintedByteOffset + 3u],
               static_cast<std::uint8_t>((literalTint >> 24u) & 0xffu));

    std::filesystem::remove_all(outputRoot, error);
}

CORSAIRS_TEST(LegacySceneBake_RequiredSetOwnsTenBoneStaticReferencePoses) {
    struct Case {
        const char* Model;
        std::size_t Object;
        std::uint32_t BoneDataSize;
    };
    constexpr std::array<Case, 10> cases{{
        {"by-bd002.lmo", 8u, 9656u},
        {"nml-bd199.lmo", 0u, 39224u},
        {"nml-bd199.lmo", 1u, 39224u},
        {"nml-bd199.lmo", 2u, 39224u},
        {"nml-bd199.lmo", 3u, 39224u},
        {"nml-bd199.lmo", 4u, 39224u},
        {"nml-bd025.lmo", 0u, 29992u},
        {"by-bd035.lmo", 1u, 38424u},
        {"by-bd032.lmo", 1u, 34976u},
        {"by-bd033.lmo", 1u, 38424u},
    }};

    const std::filesystem::path outputRoot =
        std::filesystem::temp_directory_path() /
        "corsairs-required-scene-bone-static-test";
    std::error_code error;
    std::filesystem::remove_all(outputRoot, error);
    std::filesystem::create_directories(outputRoot, error);
    REQUIRE(!error);

    for (const Case& testCase : cases) {
        const auto bytes = AC::ReadWholeFile(SceneModel(testCase.Model));
        REQUIRE(bytes.has_value());
        AC::LgoDiagnostics diag;
        const auto model = AC::ParseLmo(*bytes, diag);
        REQUIRE(model.has_value());
        REQUIRE(testCase.Object < model->Objects.size());
        const AC::LgoGeomObj& source = model->Objects[testCase.Object];
        REQUIRE_EQ(source.Animation.BoneDataSize, testCase.BoneDataSize);
        REQUIRE_EQ(source.Mesh.Blends.size(), source.Mesh.Positions.size());
        REQUIRE(!source.Mesh.BoneIndices.empty());

        std::string detail;
        AC::LgoGeomObj strict = source;
        REQUIRE_EQ(static_cast<std::uint32_t>(
                       AC::BakeLegacyCaptureState(strict, 120u, detail)),
                   static_cast<std::uint32_t>(
                       AC::LegacyBakeStatus::UNSUPPORTED_BONE_CONTROLLER));

        AC::LgoGeomObj staticPose = source;
        REQUIRE_EQ(static_cast<std::uint32_t>(AC::BakeLegacyCaptureState(
                       staticPose, 120u, detail,
                       AC::LegacyBoneCapturePolicy::StaticReferencePose)),
                   static_cast<std::uint32_t>(AC::LegacyBakeStatus::OK));
        REQUIRE_EQ(staticPose.CaptureBake.BoneDataSize,
                   testCase.BoneDataSize);
        REQUIRE(staticPose.CaptureBake.BoneStaticReferencePose);

        const std::filesystem::path gltfPath = outputRoot /
            std::format("{}_{}.gltf",
                        std::filesystem::path{testCase.Model}.stem().string(),
                        testCase.Object);
        REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteGltf(
                       staticPose, gltfPath, detail, {}, nullptr,
                       AC::GltfCoordinateProfile::SceneMap,
                       AC::GltfSkinPolicy::StaticReferencePose)),
                   static_cast<std::uint32_t>(AC::GltfStatus::OK));
        const std::string gltf = ReadText(gltfPath);
        REQUIRE(gltf.find(std::format(
                    R"("bone":{{"dataSize":{},"policy":"staticReferencePose"}})",
                    testCase.BoneDataSize)) != std::string::npos);
        REQUIRE(gltf.find(R"("JOINTS_0")") == std::string::npos);
        REQUIRE(gltf.find(R"("WEIGHTS_0")") == std::string::npos);
        REQUIRE(gltf.find(R"("skins")") == std::string::npos);
    }

    std::filesystem::remove_all(outputRoot, error);
}

} // namespace
