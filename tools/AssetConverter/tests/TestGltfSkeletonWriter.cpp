#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfSkeletonWriter.h"
#include "Corsairs/Tools/AssetConverter/LabParser.h"

#include "TestHarness.h"

#include <cmath>
#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path AnimPath(const char* name) {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "animation" / name;
}

std::filesystem::path OutDir() {
    return std::filesystem::temp_directory_path() / "corsairs-skeleton-tests";
}

bool NearlyEqual(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

CORSAIRS_TEST(GltfSkeleton_QuaternionConversionNegatesXY) {
    const AC::Quaternion in{0.1f, 0.2f, 0.3f, 0.9273618f};
    float out[4]{};
    AC::ConvertQuaternionToGltf(in, out);

    // Отрицаются X и Y, Z и W сохраняют знак.
    REQUIRE(out[0] < 0.0f);
    REQUIRE(out[1] < 0.0f);
    REQUIRE(out[2] > 0.0f);
    REQUIRE(out[3] > 0.0f);

    // Результат нормализован.
    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] +
                                out[2] * out[2] + out[3] * out[3]);
    REQUIRE(NearlyEqual(len, 1.0f));
}

CORSAIRS_TEST(GltfSkeleton_QuaternionConversionHandlesZeroLength) {
    const AC::Quaternion in{0.0f, 0.0f, 0.0f, 0.0f};
    float out[4]{};
    AC::ConvertQuaternionToGltf(in, out);

    // Вырожденный кватернион заменяется на единичный, иначе glTF невалиден.
    REQUIRE_EQ(out[3], 1.0f);
}

CORSAIRS_TEST(GltfSkeleton_DecomposeIdentityMatrix) {
    const float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    const AC::Trs trs = AC::DecomposeGltfMatrix(identity);

    REQUIRE(NearlyEqual(trs.Translation[0], 0.0f));
    REQUIRE(NearlyEqual(trs.Scale[0], 1.0f));
    REQUIRE(NearlyEqual(trs.Scale[1], 1.0f));
    REQUIRE(NearlyEqual(trs.Scale[2], 1.0f));
    REQUIRE(NearlyEqual(trs.Rotation[3], 1.0f));
}

CORSAIRS_TEST(GltfSkeleton_DecomposeTranslationAndScale) {
    // column-major: масштаб по диагонали, перенос в индексах 12..14.
    const float m[16] = {
        2, 0, 0, 0,
        0, 3, 0, 0,
        0, 0, 4, 0,
        5, 6, 7, 1,
    };
    const AC::Trs trs = AC::DecomposeGltfMatrix(m);

    REQUIRE(NearlyEqual(trs.Translation[0], 5.0f));
    REQUIRE(NearlyEqual(trs.Translation[1], 6.0f));
    REQUIRE(NearlyEqual(trs.Translation[2], 7.0f));
    REQUIRE(NearlyEqual(trs.Scale[0], 2.0f));
    REQUIRE(NearlyEqual(trs.Scale[1], 3.0f));
    REQUIRE(NearlyEqual(trs.Scale[2], 4.0f));
    REQUIRE(NearlyEqual(trs.Rotation[3], 1.0f));
}

CORSAIRS_TEST(GltfSkeleton_DecomposeRotationRoundTrip) {
    // Поворот на 90° вокруг Z в column-major: столбец 0 -> (0,1,0).
    const float m[16] = {
        0, 1, 0, 0,
       -1, 0, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    const AC::Trs trs = AC::DecomposeGltfMatrix(m);

    // Кватернион поворота на 90° вокруг Z: (0, 0, sin45, cos45).
    REQUIRE(NearlyEqual(trs.Rotation[0], 0.0f));
    REQUIRE(NearlyEqual(trs.Rotation[1], 0.0f));
    REQUIRE(NearlyEqual(trs.Rotation[2], std::sqrt(0.5f)));
    REQUIRE(NearlyEqual(trs.Rotation[3], std::sqrt(0.5f)));
}

CORSAIRS_TEST(GltfSkeleton_WritesQuaternionAnimation) {
    const auto bytes = AC::ReadWholeFile(AnimPath("0301.lab"));
    REQUIRE(bytes.has_value());

    AC::LabDiagnostics diag;
    const auto anim = AC::ParseLab(*bytes, diag);
    REQUIRE(anim.has_value());

    std::filesystem::create_directories(OutDir());
    const std::filesystem::path gltf = OutDir() / "quat.gltf";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::WriteSkeletonGltf(*anim, "walk", gltf, detail)),
               static_cast<std::uint32_t>(AC::GltfSkeletonStatus::OK));

    REQUIRE(std::filesystem::exists(gltf));
    REQUIRE(std::filesystem::exists(OutDir() / "quat.bin"));

    const auto written = AC::ReadWholeFile(gltf);
    REQUIRE(written.has_value());
    const std::string text{reinterpret_cast<const char*>(written->data()), written->size()};

    REQUIRE(text.find(R"("animations")") != std::string::npos);
    REQUIRE(text.find(R"("name":"walk")") != std::string::npos);
    REQUIRE(text.find(R"("skins")") != std::string::npos);
    REQUIRE(text.find(R"("inverseBindMatrices")") != std::string::npos);
    REQUIRE(text.find(R"("Bip01")") != std::string::npos);
    REQUIRE(text.find(R"("path":"rotation")") != std::string::npos);
}

CORSAIRS_TEST(GltfSkeleton_RejectsEmptySkeleton) {
    AC::LabAnimation empty;
    empty.Version = 0x1005u;

    std::filesystem::create_directories(OutDir());
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::WriteSkeletonGltf(empty, "none", OutDir() / "empty.gltf", detail)),
               static_cast<std::uint32_t>(AC::GltfSkeletonStatus::EMPTY_SKELETON));
}

} // namespace
