#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/LabParser.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path AnimPath(const char* name) {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "animation" / name;
}

CORSAIRS_TEST(LabParser_ParsesQuaternionAnimation) {
    const auto bytes = AC::ReadWholeFile(AnimPath("0301.lab"));
    REQUIRE(bytes.has_value());
    REQUIRE_EQ(bytes->size(), 167060u);

    AC::LabDiagnostics diag;
    const auto anim = AC::ParseLab(*bytes, diag);
    REQUIRE(anim.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LabStatus::OK));

    REQUIRE_EQ(anim->Version, 0x1005u);
    REQUIRE_EQ(anim->Header.BoneNum, 34u);
    REQUIRE_EQ(anim->Header.FrameNum, 170u);
    REQUIRE_EQ(anim->Header.DummyNum, 8u);
    REQUIRE_EQ(static_cast<std::uint32_t>(anim->KeyType()),
               static_cast<std::uint32_t>(AC::BoneKeyType::QUAT));

    REQUIRE_EQ(anim->Bones.size(), 34u);
    REQUIRE_EQ(anim->Dummies.size(), 8u);
    REQUIRE_EQ(anim->InverseBindMatrices.size(), 34u * 16u);
    REQUIRE_EQ(anim->Tracks.size(), 34u);
    REQUIRE_EQ(anim->Tracks[0].Positions.size(), 170u);
    REQUIRE_EQ(anim->Tracks[0].Rotations.size(), 170u);
    REQUIRE(anim->Tracks[0].Matrices.empty());
}

CORSAIRS_TEST(LabParser_FirstBoneIsNamedRoot) {
    const auto bytes = AC::ReadWholeFile(AnimPath("0301.lab"));
    REQUIRE(bytes.has_value());

    AC::LabDiagnostics diag;
    const auto anim = AC::ParseLab(*bytes, diag);
    REQUIRE(anim.has_value());

    // Скелеты экспортированы из 3ds Max, корневая кость называется Bip01.
    REQUIRE_EQ(AC::BoneName(anim->Bones[0]), std::string{"Bip01"});
    REQUIRE_EQ(anim->Bones[0].ParentId, AC::kNoParent);
}

CORSAIRS_TEST(LabParser_HierarchyParentsAreValid) {
    const auto bytes = AC::ReadWholeFile(AnimPath("0301.lab"));
    REQUIRE(bytes.has_value());

    AC::LabDiagnostics diag;
    const auto anim = AC::ParseLab(*bytes, diag);
    REQUIRE(anim.has_value());

    for (const AC::BoneBaseInfo& bone : anim->Bones) {
        if (bone.ParentId != AC::kNoParent) {
            REQUIRE(bone.ParentId < anim->Header.BoneNum);
        }
    }
}

CORSAIRS_TEST(LabParser_DurationUsesThirtyFps) {
    const auto bytes = AC::ReadWholeFile(AnimPath("0301.lab"));
    REQUIRE(bytes.has_value());

    AC::LabDiagnostics diag;
    const auto anim = AC::ParseLab(*bytes, diag);
    REQUIRE(anim.has_value());

    // 170 кадров на 30 FPS: последний кадр в момент 169/30.
    REQUIRE_EQ(anim->DurationSeconds(), 169.0f / 30.0f);
}

CORSAIRS_TEST(LabParser_RejectsTruncatedFile) {
    const std::vector<std::uint8_t> bytes{0x05, 0x10};

    AC::LabDiagnostics diag;
    const auto anim = AC::ParseLab(bytes, diag);
    REQUIRE(!anim.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LabStatus::VERSION_TRUNCATED));
}

CORSAIRS_TEST(LabParser_RejectsOldVersion) {
    std::vector<std::uint8_t> bytes(64, 0);
    bytes[0] = 0x00;
    bytes[1] = 0x10;   // 0x1000 — ниже поддерживаемой

    AC::LabDiagnostics diag;
    const auto anim = AC::ParseLab(bytes, diag);
    REQUIRE(!anim.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LabStatus::VERSION_UNSUPPORTED));
}

} // namespace
