#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path SampleLgoPath() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} /
           "Client" / "model" / "character" / "0066000000.lgo";
}

CORSAIRS_TEST(LgoParser_ParsesHeaderOfRealFile) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());
    REQUIRE_EQ(bytes->size(), 18044u);

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::OK));

    REQUIRE_EQ(obj->Version, 0x1004u);
    REQUIRE_EQ(obj->Header.Id, 0u);
    REQUIRE_EQ(obj->Header.ParentId, 0xFFFFFFFFu);
    REQUIRE_EQ(obj->Header.MtlSize, 1008u);
    REQUIRE_EQ(obj->Header.MeshSize, 16824u);
    REQUIRE_EQ(obj->Header.HelperSize, 92u);
    REQUIRE_EQ(obj->Header.AnimSize, 0u);
}

CORSAIRS_TEST(LgoParser_HeaderMatLocalIsIdentityForSample) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    REQUIRE_EQ(obj->Header.MatLocal[0], 1.0f);
    REQUIRE_EQ(obj->Header.MatLocal[5], 1.0f);
    REQUIRE_EQ(obj->Header.MatLocal[10], 1.0f);
    REQUIRE_EQ(obj->Header.MatLocal[15], 1.0f);
    REQUIRE_EQ(obj->Header.MatLocal[1], 0.0f);
}

CORSAIRS_TEST(LgoParser_ParsesMaterialBlockAndConsumesExactSize) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    // MtlSize 1008 = 4 (MtlNum) + 1004 * 1
    REQUIRE_EQ(obj->Materials.size(), 1u);
    // Проверено на реальном файле: имя текстуры лежит по смещению 0x154
    // и равно "0066000000.BMP". Расширение намеренно НЕ нормализуется —
    // на диске файл называется 0066000000.png.
    REQUIRE_EQ(obj->Materials[0].TextureName(0), std::string{"0066000000.BMP"});
}

CORSAIRS_TEST(LgoParser_RejectsTruncatedVersion) {
    const std::vector<std::uint8_t> bytes{0x04, 0x10};

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(bytes, diag);
    REQUIRE(!obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::VERSION_TRUNCATED));
}

CORSAIRS_TEST(LgoParser_RejectsUnknownVersion) {
    std::vector<std::uint8_t> bytes(200, 0);
    bytes[0] = 0xEF;
    bytes[1] = 0xBE;
    bytes[2] = 0xAD;
    bytes[3] = 0xDE;

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(bytes, diag);
    REQUIRE(!obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::VERSION_UNKNOWN));
}

CORSAIRS_TEST(LgoParser_RejectsBlockSizesLargerThanFile) {
    std::vector<std::uint8_t> bytes(4 + AC::kGeomObjHeaderSize, 0);
    bytes[0] = 0x04;
    bytes[1] = 0x10;

    // MtlSize по смещению 4 + 100 = 104 (поле идёт после Id/ParentId/Type/
    // MatLocal[64]/Rcci[16]/StateCtrl[8] = 4+4+4+64+16+8 = 100).
    bytes[4 + 100] = 0xFF;
    bytes[4 + 101] = 0xFF;

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(bytes, diag);
    REQUIRE(!obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::BLOCK_SIZES_INCONSISTENT));
}

CORSAIRS_TEST(LgoParser_ParsesMeshBlockOfRealFile) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    REQUIRE(mesh.Header.VertexNum > 0u);
    REQUIRE(mesh.Header.IndexNum > 0u);
    REQUIRE(mesh.Header.SubsetNum > 0u);

    REQUIRE_EQ(mesh.Positions.size(), static_cast<std::size_t>(mesh.Header.VertexNum));
    REQUIRE_EQ(mesh.Indices.size(), static_cast<std::size_t>(mesh.Header.IndexNum));
    REQUIRE_EQ(mesh.Subsets.size(), static_cast<std::size_t>(mesh.Header.SubsetNum));
}

CORSAIRS_TEST(LgoParser_MeshNormalsPresentWhenFvfSaysSo) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    if (AC::HasFvf(mesh.Header.Fvf, AC::FvfFlag::NORMAL)) {
        REQUIRE_EQ(mesh.Normals.size(), static_cast<std::size_t>(mesh.Header.VertexNum));
    }
    else {
        REQUIRE(mesh.Normals.empty());
    }
}

CORSAIRS_TEST(LgoParser_MeshIndicesStayInVertexRange) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    for (std::uint32_t index : mesh.Indices) {
        REQUIRE(index < mesh.Header.VertexNum);
    }
}

CORSAIRS_TEST(LgoParser_SubsetsCoverIndexBuffer) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    for (const AC::SubsetInfo& subset : mesh.Subsets) {
        const std::uint64_t last =
            static_cast<std::uint64_t>(subset.StartIndex) +
            static_cast<std::uint64_t>(subset.PrimitiveNum) * 3ull;
        REQUIRE(last <= static_cast<std::uint64_t>(mesh.Header.IndexNum));
    }
}

} // namespace
