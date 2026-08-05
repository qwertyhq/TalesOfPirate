#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path SampleLgo() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
           "Client" / "model" / "character" / "0066000000.lgo";
}

std::filesystem::path OutputDir() {
    return std::filesystem::temp_directory_path() / "corsairs-gltf-tests";
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

CORSAIRS_TEST(GltfWriter_MatrixConversionNegatesTranslationZ) {
    // DirectX row-major: перенос в последней строке (индексы 12,13,14).
    const float translate[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        2, 3, 5, 1,
    };
    float out[16]{};
    AC::ConvertMatrixToGltf(translate, out);

    // После транспонирования перенос в glTF column-major остаётся на 12,13,14.
    REQUIRE_EQ(out[12], 2.0f);
    REQUIRE_EQ(out[13], 3.0f);
    REQUIRE_EQ(out[14], -5.0f);
    REQUIRE_EQ(out[15], 1.0f);
}

CORSAIRS_TEST(GltfWriter_MatrixConversionFlipsRotationOffDiagonals) {
    // Поворот на 90° вокруг Y в левосторонней системе.
    const float rotY90[16] = {
        0, 0, -1, 0,
        0, 1,  0, 0,
        1, 0,  0, 0,
        0, 0,  0, 1,
    };
    float out[16]{};
    AC::ConvertMatrixToGltf(rotY90, out);

    // S*M*S отрицает ровно те элементы, где один индекс равен 2: (0,2) и (2,0).
    // Диагональные и не связанные с Z остаются как были.
    REQUIRE_EQ(out[2], 1.0f);    // было -1
    REQUIRE_EQ(out[8], -1.0f);   // было +1
    REQUIRE_EQ(out[5], 1.0f);    // не затронут
    REQUIRE_EQ(out[10], 0.0f);   // m22 не отрицается
    REQUIRE_EQ(out[15], 1.0f);
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

} // namespace
