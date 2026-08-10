#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"
#include "Corsairs/Tools/AssetConverter/LmoParser.h"

#include "TestHarness.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path ModelPath(const char* relative) {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "model" / relative;
}

std::filesystem::path SampleLgoPath() {
    return ModelPath("character/0066000000.lgo");
}

template <typename T>
void AppendPod(std::vector<std::uint8_t>& bytes, const T& value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + sizeof(T));
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename RawMaterial>
std::vector<std::uint8_t> MaterialOnlyLgo(std::uint32_t outerVersion,
                                          std::uint32_t materialVersion,
                                          const RawMaterial& material) {
    std::vector<std::uint8_t> bytes;
    AppendPod(bytes, outerVersion);

    AC::GeomObjHeader header{};
    header.MatLocal[0] = 1.0f;
    header.MatLocal[5] = 1.0f;
    header.MatLocal[10] = 1.0f;
    header.MatLocal[15] = 1.0f;
    header.MtlSize = static_cast<std::uint32_t>(sizeof(std::uint32_t) +
        sizeof(RawMaterial) +
        (outerVersion == AC::kLegacyVersion ? sizeof(std::uint32_t) : 0u));
    AppendPod(bytes, header);

    if (outerVersion == AC::kLegacyVersion) {
        AppendPod(bytes, materialVersion);
    }
    constexpr std::uint32_t materialCount = 1;
    AppendPod(bytes, materialCount);
    AppendPod(bytes, material);
    return bytes;
}

void FillLegacyInvalidStates(AC::RenderStateSet2x8& states) {
    for (auto& set : states.Rsv) {
        for (AC::RenderStateValue& value : set) {
            value.State = 0xFFFFFFFFu;
            value.Value = 0u;
        }
    }
}

std::vector<std::uint8_t> AnimationOnlyLgo(
    const std::optional<std::array<float, 12>>& matrix,
    const std::optional<std::array<float, 16>>& texUv) {
    constexpr std::uint32_t version = 0x1004u;
    constexpr std::uint32_t tableSize =
        sizeof(std::uint32_t) * (2u + 64u + 64u);
    const std::uint32_t matrixSize = matrix.has_value()
        ? sizeof(std::uint32_t) + sizeof(*matrix)
        : 0u;
    const std::uint32_t texUvSize = texUv.has_value()
        ? sizeof(std::uint32_t) + sizeof(*texUv)
        : 0u;

    AC::GeomObjHeader header{};
    header.MatLocal[0] = 1.0f;
    header.MatLocal[5] = 1.0f;
    header.MatLocal[10] = 1.0f;
    header.MatLocal[15] = 1.0f;
    header.AnimSize = tableSize + matrixSize + texUvSize;

    std::vector<std::uint8_t> bytes;
    AppendPod(bytes, version);
    AppendPod(bytes, header);
    AppendPod(bytes, std::uint32_t{0u});
    AppendPod(bytes, matrixSize);
    std::array<std::uint32_t, 64> texUvSizes{};
    texUvSizes[0] = texUvSize;
    AppendPod(bytes, texUvSizes);
    AppendPod(bytes, std::array<std::uint32_t, 64>{});
    if (matrix.has_value()) {
        AppendPod(bytes, std::uint32_t{1u});
        AppendPod(bytes, *matrix);
    }
    if (texUv.has_value()) {
        AppendPod(bytes, std::uint32_t{1u});
        AppendPod(bytes, *texUv);
    }
    return bytes;
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

CORSAIRS_TEST(LgoParser_ModernMaterialPreservesRawEffectiveAndFullRenderStates) {
    AC::MtlTexInfo raw{};
    raw.Opacity = 0.75f;
    raw.TranspType = 2u;
    raw.RsSet[0] = AC::RenderStateAtom{100u, 200u, 300u};
    raw.RsSet[1] = AC::RenderStateAtom{101u, 201u, 301u};
    raw.RsSet[2] = AC::RenderStateAtom{102u, 202u, 302u};
    raw.RsSet[3] = AC::RenderStateAtom{103u, 203u, 303u};
    raw.RsSet[4] = AC::RenderStateAtom{104u, 204u, 304u};
    raw.RsSet[5] = AC::RenderStateAtom{105u, 205u, 305u};
    raw.RsSet[6] = AC::RenderStateAtom{106u, 206u, 306u};
    raw.RsSet[7] = AC::RenderStateAtom{107u, 207u, 307u};

    const auto bytes = MaterialOnlyLgo(0x1004u, 0x1004u, raw);
    AC::LgoDiagnostics diag;
    const auto object = AC::ParseLgo(bytes, diag);
    REQUIRE(object.has_value());
    REQUIRE_EQ(object->Materials.size(), 1u);

    const AC::LgoMaterial& material = object->Materials[0];
    REQUIRE_EQ(material.Opacity, 0.75f);
    REQUIRE_EQ(material.RawTranspType, 2u);
    REQUIRE_EQ(material.EffectiveTranspType, 5u);
    REQUIRE_EQ(material.RenderStates[0].State, 100u);
    REQUIRE_EQ(material.RenderStates[0].Value0, 200u);
    REQUIRE_EQ(material.RenderStates[0].Value1, 300u);
    REQUIRE_EQ(material.RenderStates[1].State, 101u);
    REQUIRE_EQ(material.RenderStates[1].Value0, 201u);
    REQUIRE_EQ(material.RenderStates[1].Value1, 301u);
    REQUIRE_EQ(material.RenderStates[2].State, 102u);
    REQUIRE_EQ(material.RenderStates[2].Value0, 202u);
    REQUIRE_EQ(material.RenderStates[2].Value1, 302u);
    REQUIRE_EQ(material.RenderStates[3].State, 103u);
    REQUIRE_EQ(material.RenderStates[3].Value0, 203u);
    REQUIRE_EQ(material.RenderStates[3].Value1, 303u);
    REQUIRE_EQ(material.RenderStates[4].State, 104u);
    REQUIRE_EQ(material.RenderStates[4].Value0, 204u);
    REQUIRE_EQ(material.RenderStates[4].Value1, 304u);
    REQUIRE_EQ(material.RenderStates[5].State, 105u);
    REQUIRE_EQ(material.RenderStates[5].Value0, 205u);
    REQUIRE_EQ(material.RenderStates[5].Value1, 305u);
    REQUIRE_EQ(material.RenderStates[6].State, 106u);
    REQUIRE_EQ(material.RenderStates[6].Value0, 206u);
    REQUIRE_EQ(material.RenderStates[6].Value1, 306u);
    REQUIRE_EQ(material.RenderStates[7].State, 107u);
    REQUIRE_EQ(material.RenderStates[7].Value0, 207u);
    REQUIRE_EQ(material.RenderStates[7].Value1, 307u);
}

CORSAIRS_TEST(LgoParser_LegacyV1UpgradesAlphaStatesAndKeepsTerminator) {
    AC::MtlTexInfoV1 raw{};
    raw.Opacity = 1.0f;
    raw.TranspType = 1u;
    FillLegacyInvalidStates(raw.RsSet);
    raw.RsSet.Rsv[0][0] = AC::RenderStateValue{25u, 8u};
    raw.RsSet.Rsv[0][1] = AC::RenderStateValue{24u, 17u};
    raw.RsSet.Rsv[0][2] = AC::RenderStateValue{19u, 2u};

    const auto bytes = MaterialOnlyLgo(AC::kLegacyVersion, 0x0001u, raw);
    AC::LgoDiagnostics diag;
    const auto object = AC::ParseLgo(bytes, diag);
    REQUIRE(object.has_value());

    const AC::LgoMaterial& material = object->Materials[0];
    REQUIRE_EQ(material.RawTranspType, 1u);
    REQUIRE_EQ(material.EffectiveTranspType, 1u);
    REQUIRE_EQ(material.RenderStates[0].State, 25u);
    REQUIRE_EQ(material.RenderStates[0].Value0, 5u);
    REQUIRE_EQ(material.RenderStates[0].Value1, 5u);
    REQUIRE_EQ(material.RenderStates[1].State, 24u);
    REQUIRE_EQ(material.RenderStates[1].Value0, 129u);
    REQUIRE_EQ(material.RenderStates[1].Value1, 129u);
    REQUIRE_EQ(material.RenderStates[2].State, 19u);
    REQUIRE_EQ(material.RenderStates[2].Value0, 2u);
    REQUIRE_EQ(material.RenderStates[3].State, 0xFFFFFFFFu);
    REQUIRE_EQ(material.RenderStates[3].Value0, 0u);
    REQUIRE_EQ(material.RenderStates[3].Value1, 0u);
}

CORSAIRS_TEST(LgoParser_LegacyV0UpgradesAlphaStatesAndDefaultsTransparency) {
    AC::MtlTexInfoV0 raw{};
    FillLegacyInvalidStates(raw.RsSet);
    raw.RsSet.Rsv[0][0] = AC::RenderStateValue{25u, 1u};
    raw.RsSet.Rsv[0][1] = AC::RenderStateValue{24u, 255u};

    const auto bytes = MaterialOnlyLgo(AC::kLegacyVersion, 0x0000u, raw);
    AC::LgoDiagnostics diag;
    const auto object = AC::ParseLgo(bytes, diag);
    REQUIRE(object.has_value());

    const AC::LgoMaterial& material = object->Materials[0];
    REQUIRE_EQ(material.RawTranspType, 0u);
    REQUIRE_EQ(material.EffectiveTranspType, 0u);
    REQUIRE_EQ(material.RenderStates[0].State, 25u);
    REQUIRE_EQ(material.RenderStates[0].Value0, 5u);
    REQUIRE_EQ(material.RenderStates[1].State, 24u);
    REQUIRE_EQ(material.RenderStates[1].Value0, 129u);
    REQUIRE_EQ(material.RenderStates[2].State, 0xFFFFFFFFu);
}

CORSAIRS_TEST(LgoParser_RealByBd001RawTypeTwoResolvesSubtractive) {
    const auto bytes = AC::ReadWholeFile(ModelPath("scene/by-bd001.lmo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto model = AC::ParseLmo(*bytes, diag);
    REQUIRE(model.has_value());
    REQUIRE_EQ(model->Objects.size(), 4u);
    REQUIRE(model->Objects[0].Materials.size() > 5u);

    const AC::LgoMaterial& material = model->Objects[0].Materials[5];
    REQUIRE(model->Objects[0].Mesh.Subsets.size() > 5u);
    REQUIRE(model->Objects[0].Mesh.Subsets[5].PrimitiveNum > 0u);
    REQUIRE_EQ(material.TextureName(0), std::string{"010038.bmp"});
    REQUIRE_EQ(material.RawTranspType, 2u);
    REQUIRE_EQ(material.EffectiveTranspType, 5u);
    REQUIRE_EQ(material.RenderStates[0].State, 19u);
    REQUIRE_EQ(material.RenderStates[0].Value0, 1u);
    REQUIRE_EQ(material.RenderStates[1].State, 20u);
    REQUIRE_EQ(material.RenderStates[1].Value0, 4u);

    AC::LegacyMaterialMetadata metadata;
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(
                   AC::ResolveLegacyMaterial(material, metadata, detail)),
               static_cast<std::uint32_t>(AC::LegacyMaterialStatus::OK));
    REQUIRE_EQ(static_cast<std::uint32_t>(metadata.Mode),
               static_cast<std::uint32_t>(AC::LegacyMaterialMode::Subtractive));
    REQUIRE_EQ(metadata.RawTranspType, 2u);
    REQUIRE_EQ(metadata.EffectiveTranspType, 5u);
    REQUIRE_EQ(metadata.SrcBlend, 1u);
    REQUIRE_EQ(metadata.DestBlend, 4u);
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

CORSAIRS_TEST(LgoParser_RejectsNonFiniteMatController) {
    std::array<float, 12> matrix{
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
        0, 0, 0};
    matrix[7] = std::numeric_limits<float>::quiet_NaN();

    AC::LgoDiagnostics diag;
    const auto object = AC::ParseLgo(AnimationOnlyLgo(matrix, std::nullopt), diag);
    REQUIRE(!object.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::ANIM_BLOCK_MALFORMED));
}

CORSAIRS_TEST(LgoParser_RejectsNonFiniteTexUvController) {
    std::array<float, 16> matrix{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1};
    matrix[15] = std::numeric_limits<float>::infinity();

    AC::LgoDiagnostics diag;
    const auto object = AC::ParseLgo(AnimationOnlyLgo(std::nullopt, matrix), diag);
    REQUIRE(!object.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::ANIM_BLOCK_MALFORMED));
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

// --- Легаси-формат version = 0x0000 -----------------------------------------
// У него вложенная версия в начале каждого блока, другая раскладка материалов
// (1028 байт вместо 1004) и другой порядок массивов геометрии: подсеты идут
// первыми, а индексы костей однобайтовые.

CORSAIRS_TEST(LgoParser_ParsesLegacyVersionZeroSimpleMesh) {
    const auto bytes = AC::ReadWholeFile(ModelPath("character/2000000003.lgo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::OK));

    REQUIRE_EQ(obj->Version, 0x0000u);
    REQUIRE_EQ(obj->Materials.size(), 1u);

    const AC::LgoMesh& mesh = obj->Mesh;
    REQUIRE_EQ(mesh.Header.Fvf, 0x0112u);
    REQUIRE_EQ(mesh.Header.VertexNum, 8u);
    REQUIRE_EQ(mesh.Header.IndexNum, 12u);
    REQUIRE_EQ(mesh.Header.SubsetNum, 1u);
    REQUIRE_EQ(mesh.Positions.size(), 8u);
    REQUIRE_EQ(mesh.Normals.size(), 8u);
    REQUIRE_EQ(mesh.Texcoords[0].size(), 8u);
    REQUIRE_EQ(mesh.Indices.size(), 12u);
    REQUIRE_EQ(mesh.Subsets.size(), 1u);
}

CORSAIRS_TEST(LgoParser_ParsesLegacyVersionZeroWithVertexColors) {
    const auto bytes = AC::ReadWholeFile(ModelPath("item/01020007.lgo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    // 0x0152 = XYZ | NORMAL | DIFFUSE | TEX1
    REQUIRE_EQ(mesh.Header.Fvf, 0x0152u);
    REQUIRE(AC::HasFvf(mesh.Header.Fvf, AC::FvfFlag::DIFFUSE));
    REQUIRE_EQ(mesh.Header.VertexNum, 334u);
    REQUIRE_EQ(mesh.VertexColors.size(), 334u);
}

CORSAIRS_TEST(LgoParser_ParsesLegacyVersionZeroWithByteBoneIndices) {
    const auto bytes = AC::ReadWholeFile(ModelPath("character/0018000002.lgo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    const AC::LgoMesh& mesh = obj->Mesh;
    // 0x1118 содержит LASTBETA_UBYTE4 — скиннинг с однобайтовыми индексами,
    // которые парсер обязан расширить до 32 бит.
    REQUIRE(AC::HasFvf(mesh.Header.Fvf, AC::FvfFlag::LASTBETA_UBYTE4));
    REQUIRE_EQ(mesh.Header.VertexNum, 336u);
    REQUIRE_EQ(mesh.Header.BoneIndexNum, 15u);
    REQUIRE_EQ(mesh.Blends.size(), 336u);
    REQUIRE_EQ(mesh.BoneIndices.size(), 15u);
}

CORSAIRS_TEST(LgoParser_LegacyMaterialCarriesTextureName) {
    const auto bytes = AC::ReadWholeFile(ModelPath("character/2000000003.lgo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());
    REQUIRE_EQ(obj->Materials.size(), 1u);
    REQUIRE(!obj->Materials[0].TextureName(0).empty());
}

// --- Helper-блок: точки крепления и объёмы -----------------------------------

CORSAIRS_TEST(LgoParser_ParsesBoundingSphereHelper) {
    const auto bytes = AC::ReadWholeFile(SampleLgoPath());
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    // helperSize=92 = 4 (type) + 4 (num) + 84 (одна сфера)
    REQUIRE_EQ(obj->Helper.Type, 0x0020u);
    REQUIRE_EQ(obj->Helper.BoundingSpheres.size(), 1u);
    REQUIRE(obj->Helper.Dummies.empty());
    REQUIRE(obj->Helper.BoundingSpheres[0].BoundSphere.Radius > 0.0f);
}

CORSAIRS_TEST(LgoParser_ParsesDummyAttachPointsWithBoundingBox) {
    const auto bytes = AC::ReadWholeFile(ModelPath("character/04090084.lgo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    // helperSize=804 = 4 (type) + 4 + 140*5 (dummy) + 4 + 92*1 (bbox)
    REQUIRE_EQ(obj->Helper.Type, 0x0011u);
    REQUIRE_EQ(obj->Helper.Dummies.size(), 5u);
    REQUIRE_EQ(obj->Helper.BoundingBoxes.size(), 1u);
}

CORSAIRS_TEST(LgoParser_ParsesDummyOnlyHelper) {
    const auto bytes = AC::ReadWholeFile(ModelPath("character/0009000100.lgo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    // helperSize=428 = 4 (type) + 4 + 140*3
    REQUIRE_EQ(obj->Helper.Type, 0x0001u);
    REQUIRE_EQ(obj->Helper.Dummies.size(), 3u);
}

CORSAIRS_TEST(LgoParser_ParsesLegacyDummyWithShortLayout) {
    const auto bytes = AC::ReadWholeFile(ModelPath("character/2000000000.lgo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    REQUIRE(obj.has_value());

    // В версии 0x0000 dummy занимает 68 байт (id + mat), а не 140:
    // helperSize=388 = 4 (вложенная версия) + 4 (type) + 4 + 68*3 + 4 + 84*2
    REQUIRE_EQ(obj->Version, 0x0000u);
    REQUIRE_EQ(obj->Helper.Type, 0x0021u);
    REQUIRE_EQ(obj->Helper.Dummies.size(), 3u);
    REQUIRE_EQ(obj->Helper.BoundingSpheres.size(), 2u);
}

} // namespace
