#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <format>

namespace Corsairs::Tools::AssetConverter {

std::string_view ToString(LgoStatus status) {
    switch (status) {
    case LgoStatus::OK:                       return "OK";
    case LgoStatus::OK_WITH_TRAILING_DATA:    return "OK_WITH_TRAILING_DATA";
    case LgoStatus::VERSION_TRUNCATED:        return "VERSION_TRUNCATED";
    case LgoStatus::VERSION_UNKNOWN:          return "VERSION_UNKNOWN";
    case LgoStatus::VERSION_UNSUPPORTED:      return "VERSION_UNSUPPORTED";
    case LgoStatus::HEADER_TRUNCATED:         return "HEADER_TRUNCATED";
    case LgoStatus::BLOCK_SIZES_INCONSISTENT: return "BLOCK_SIZES_INCONSISTENT";
    case LgoStatus::MTL_BLOCK_MALFORMED:      return "MTL_BLOCK_MALFORMED";
    case LgoStatus::MESH_BLOCK_MALFORMED:     return "MESH_BLOCK_MALFORMED";
    }
    return "UNKNOWN";
}

std::string LgoMaterial::TextureName(std::size_t stage) const {
    if (stage >= kMaxTextureStageNum) {
        return {};
    }
    return Textures[stage];
}

namespace {

// Поле имени на диске может не иметь завершающего нуля, поэтому длина ищется
// вручную с явным пределом. strnlen не используется: он объявлен в разных
// заголовках на разных платформах.
std::string FixedName(const char* name) {
    std::size_t length = 0;
    while (length < kMaxName && name[length] != '\0') {
        ++length;
    }
    return std::string{name, length};
}

// Разбирает блок материалов и проверяет, что потрачено ровно mtlSize байт.
bool ParseMaterialBlock(BinaryReader& reader, std::uint32_t mtlSize,
                        std::vector<LgoMaterial>& out, LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    std::uint32_t mtlNum = 0;
    if (!reader.Read(mtlNum)) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = "не прочитан MtlNum";
        return false;
    }

    const std::size_t expected = 4 + kMtlTexInfoSize * mtlNum;
    if (expected != mtlSize) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "MtlNum={} даёт {} байт, заголовок объявил MtlSize={}",
            mtlNum, expected, mtlSize);
        return false;
    }

    out.resize(mtlNum);
    for (std::uint32_t i = 0; i < mtlNum; ++i) {
        MtlTexInfo raw{};
        if (!reader.Read(raw)) {
            diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
            diag.Detail = std::format("не прочитан MtlTexInfo[{}]", i);
            return false;
        }
        out[i].Opacity = raw.Opacity;
        out[i].TranspType = raw.TranspType;
        out[i].Mtl = raw.Mtl;
        for (std::size_t stage = 0; stage < kMaxTextureStageNum; ++stage) {
            out[i].Textures[stage] = FixedName(raw.TexSeq[stage].FileName);
        }
    }

    const std::size_t consumed = reader.Offset() - blockStart;
    if (consumed != mtlSize) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format("прочитано {} байт, объявлено {}", consumed, mtlSize);
        return false;
    }
    return true;
}

// Легаси-вариант блока материалов (внешняя version == 0x0000). Отличия от
// современного: в начале блока лежит вложенная версия, а сама структура
// материала занимает 1028 байт вместо 1004 (нет Opacity/TranspType, другой
// формат render states и текстур).
bool ParseMaterialBlockV0(BinaryReader& reader, std::uint32_t mtlSize,
                          std::vector<LgoMaterial>& out, LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    std::uint32_t innerVersion = 0;
    if (!reader.Read(innerVersion)) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = "не прочитана вложенная версия блока материалов";
        return false;
    }

    if (innerVersion != 0) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "вложенная версия блока материалов 0x{:04X} не поддерживается "
            "(реализована только 0x0000)", innerVersion);
        return false;
    }

    std::uint32_t mtlNum = 0;
    if (!reader.Read(mtlNum)) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = "не прочитан MtlNum (легаси)";
        return false;
    }

    const std::size_t expected = 8 + kMtlTexInfoV0Size * mtlNum;
    if (expected != mtlSize) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "легаси: MtlNum={} даёт {} байт, заголовок объявил MtlSize={}",
            mtlNum, expected, mtlSize);
        return false;
    }

    out.resize(mtlNum);
    for (std::uint32_t i = 0; i < mtlNum; ++i) {
        MtlTexInfoV0 raw{};
        if (!reader.Read(raw)) {
            diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
            diag.Detail = std::format("не прочитан MtlTexInfoV0[{}]", i);
            return false;
        }
        // В легаси-структуре нет прозрачности — движок оставляет значения по
        // умолчанию, делаем так же.
        out[i].Mtl = raw.Mtl;
        for (std::size_t stage = 0; stage < kMaxTextureStageNum; ++stage) {
            out[i].Textures[stage] = FixedName(raw.TexSeq[stage].FileName);
        }
    }

    const std::size_t consumed = reader.Offset() - blockStart;
    if (consumed != mtlSize) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format("легаси: прочитано {} байт, объявлено {}", consumed, mtlSize);
        return false;
    }
    return true;
}

// Читает vector<T> длиной count. false — данные за границей буфера.
template <typename T>
bool ReadVector(BinaryReader& reader, std::vector<T>& out, std::uint32_t count) {
    if (count == 0) {
        return true;
    }
    out.resize(count);
    return reader.ReadArray(out.data(), count);
}

// Разбирает блок геометрии для version >= 0x1004 и проверяет, что потрачено
// ровно meshSize байт. Порядок массивов задан LgoLoader::LoadMeshInfo
// (sources/Engine/Asset/AssetLoaders.cpp) и обязателен к соблюдению.
bool ParseMeshBlock(BinaryReader& reader, std::uint32_t meshSize, LgoMesh& mesh,
                    LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    if (!reader.Read(mesh.Header)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан MeshInfoHeader";
        return false;
    }

    const std::uint32_t vertexNum = mesh.Header.VertexNum;

    if (!ReadVector(reader, mesh.VertexElements, mesh.Header.VertexElementNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан VertexElements";
        return false;
    }

    if (!ReadVector(reader, mesh.Positions, vertexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны позиции вершин";
        return false;
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::NORMAL)) {
        if (!ReadVector(reader, mesh.Normals, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны нормали";
            return false;
        }
    }

    // Порядок проверок повторяет цепочку if/else if движка: TEX1 проверяется
    // первым, поэтому набор с четырьмя UV имеет флаг TEX4 и не совпадает с TEX1.
    std::uint32_t texcoordSets = 0;
    if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX1)) {
        texcoordSets = 1;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX2)) {
        texcoordSets = 2;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX3)) {
        texcoordSets = 3;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX4)) {
        texcoordSets = 4;
    }

    for (std::uint32_t set = 0; set < texcoordSets; ++set) {
        if (!ReadVector(reader, mesh.Texcoords[set], vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = std::format("не прочитан UV-набор {}", set);
            return false;
        }
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::DIFFUSE)) {
        if (!ReadVector(reader, mesh.VertexColors, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны цвета вершин";
            return false;
        }
    }

    if (mesh.Header.BoneIndexNum > 0) {
        if (!ReadVector(reader, mesh.Blends, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны веса скиннинга";
            return false;
        }
        if (!ReadVector(reader, mesh.BoneIndices, mesh.Header.BoneIndexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны индексы костей";
            return false;
        }
    }

    if (!ReadVector(reader, mesh.Indices, mesh.Header.IndexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан индексный буфер";
        return false;
    }

    if (!ReadVector(reader, mesh.Subsets, mesh.Header.SubsetNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны подсеты";
        return false;
    }

    const std::size_t consumed = reader.Offset() - blockStart;
    if (consumed != meshSize) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "прочитано {} байт, заголовок объявил MeshSize={} (fvf=0x{:08X}, "
            "vertexNum={}, indexNum={}, subsetNum={}, boneIndexNum={})",
            consumed, meshSize, mesh.Header.Fvf, mesh.Header.VertexNum,
            mesh.Header.IndexNum, mesh.Header.SubsetNum, mesh.Header.BoneIndexNum);
        return false;
    }
    return true;
}

// Легаси-вариант блока геометрии (внешняя version == 0x0000). Отличия от
// современного, все три существенные:
//   1. в начале блока вложенная версия;
//   2. подсеты читаются ПЕРВЫМИ, а не последними;
//   3. индексы костей однобайтовые, а наличие скиннинга определяется флагом
//      LASTBETA_UBYTE4, а не полем BoneIndexNum.
bool ParseMeshBlockV0(BinaryReader& reader, std::uint32_t meshSize, LgoMesh& mesh,
                      LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    std::uint32_t innerVersion = 0;
    if (!reader.Read(innerVersion)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитана вложенная версия блока геометрии";
        return false;
    }

    if (innerVersion != 0) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "вложенная версия блока геометрии 0x{:04X} не поддерживается "
            "(реализована только 0x0000)", innerVersion);
        return false;
    }

    MeshInfoHeaderV0 legacyHeader{};
    if (!reader.Read(legacyHeader)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан MeshInfoHeaderV0";
        return false;
    }

    mesh.Header.Fvf = legacyHeader.Fvf;
    mesh.Header.PtType = legacyHeader.PtType;
    mesh.Header.VertexNum = legacyHeader.VertexNum;
    mesh.Header.IndexNum = legacyHeader.IndexNum;
    mesh.Header.SubsetNum = legacyHeader.SubsetNum;
    mesh.Header.BoneIndexNum = legacyHeader.BoneIndexNum;
    mesh.Header.BoneInflFactor = legacyHeader.BoneIndexNum > 0 ? 2u : 0u;
    mesh.Header.VertexElementNum = 0;

    const std::uint32_t vertexNum = mesh.Header.VertexNum;

    if (!ReadVector(reader, mesh.Subsets, mesh.Header.SubsetNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "легаси: не прочитаны подсеты";
        return false;
    }

    if (!ReadVector(reader, mesh.Positions, vertexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "легаси: не прочитаны позиции вершин";
        return false;
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::NORMAL)) {
        if (!ReadVector(reader, mesh.Normals, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "легаси: не прочитаны нормали";
            return false;
        }
    }

    std::uint32_t texcoordSets = 0;
    if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX1)) {
        texcoordSets = 1;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX2)) {
        texcoordSets = 2;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX3)) {
        texcoordSets = 3;
    }
    else if (HasFvf(mesh.Header.Fvf, FvfFlag::TEX4)) {
        texcoordSets = 4;
    }

    for (std::uint32_t set = 0; set < texcoordSets; ++set) {
        if (!ReadVector(reader, mesh.Texcoords[set], vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = std::format("легаси: не прочитан UV-набор {}", set);
            return false;
        }
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::DIFFUSE)) {
        if (!ReadVector(reader, mesh.VertexColors, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "легаси: не прочитаны цвета вершин";
            return false;
        }
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::LASTBETA_UBYTE4)) {
        if (!ReadVector(reader, mesh.Blends, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "легаси: не прочитаны веса скиннинга";
            return false;
        }

        // Индексы костей на диске однобайтовые; расширяем до 32 бит, чтобы
        // остальной код не различал версии.
        std::vector<std::uint8_t> byteIndices;
        if (!ReadVector(reader, byteIndices, mesh.Header.BoneIndexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "легаси: не прочитаны однобайтовые индексы костей";
            return false;
        }
        mesh.BoneIndices.assign(byteIndices.begin(), byteIndices.end());
    }

    if (!ReadVector(reader, mesh.Indices, mesh.Header.IndexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "легаси: не прочитан индексный буфер";
        return false;
    }

    const std::size_t consumed = reader.Offset() - blockStart;
    if (consumed != meshSize) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "легаси: прочитано {} байт, заголовок объявил MeshSize={} (fvf=0x{:08X}, "
            "vertexNum={}, indexNum={}, subsetNum={}, boneIndexNum={})",
            consumed, meshSize, mesh.Header.Fvf, mesh.Header.VertexNum,
            mesh.Header.IndexNum, mesh.Header.SubsetNum, mesh.Header.BoneIndexNum);
        return false;
    }
    return true;
}

} // namespace

std::optional<LgoGeomObj> ParseLgo(std::span<const std::uint8_t> bytes,
                                   LgoDiagnostics& diag) {
    diag = {};
    BinaryReader reader{bytes};

    LgoGeomObj obj;

    if (!reader.Read(obj.Version)) {
        diag.Status = LgoStatus::VERSION_TRUNCATED;
        diag.Detail = "файл короче 4 байт";
        return std::nullopt;
    }
    diag.Version = obj.Version;

    if (!IsKnownVersion(obj.Version)) {
        diag.Status = LgoStatus::VERSION_UNKNOWN;
        diag.Detail = std::format("version=0x{:08X}, ожидалось 0x0000 или 0x1000..0x1005",
                                  obj.Version);
        return std::nullopt;
    }

    // Поддерживаются две раскладки: современная (0x1004+) и легаси (0x0000).
    // Версии 0x1000..0x1003 в датасете отсутствуют и не реализованы.
    const bool isLegacy = obj.Version == kLegacyVersion;
    if (!isLegacy && obj.Version < kMinSupportedVersion) {
        diag.Status = LgoStatus::VERSION_UNSUPPORTED;
        diag.Detail = std::format(
            "version=0x{:08X}: поддерживаются 0x0000 и 0x{:04X}+",
            obj.Version, kMinSupportedVersion);
        return std::nullopt;
    }

    if (!reader.Read(obj.Header)) {
        diag.Status = LgoStatus::HEADER_TRUNCATED;
        diag.Detail = std::format("нужно {} байт заголовка, доступно {}",
                                  kGeomObjHeaderSize, reader.Remaining());
        return std::nullopt;
    }

    const std::uint64_t blocksSum =
        static_cast<std::uint64_t>(obj.Header.MtlSize) +
        static_cast<std::uint64_t>(obj.Header.MeshSize) +
        static_cast<std::uint64_t>(obj.Header.HelperSize) +
        static_cast<std::uint64_t>(obj.Header.AnimSize);
    const std::uint64_t expectedTotal = blocksSum + 4 + kGeomObjHeaderSize;

    if (expectedTotal > bytes.size()) {
        diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
        diag.Detail = std::format(
            "mtl={} mesh={} helper={} anim={}; ожидается {} байт, файл {} байт",
            obj.Header.MtlSize, obj.Header.MeshSize, obj.Header.HelperSize,
            obj.Header.AnimSize, expectedTotal, bytes.size());
        return std::nullopt;
    }

    if (obj.Header.MtlSize > 0) {
        const bool ok = isLegacy
            ? ParseMaterialBlockV0(reader, obj.Header.MtlSize, obj.Materials, diag)
            : ParseMaterialBlock(reader, obj.Header.MtlSize, obj.Materials, diag);
        if (!ok) {
            return std::nullopt;
        }
    }

    if (obj.Header.MeshSize > 0) {
        const bool ok = isLegacy
            ? ParseMeshBlockV0(reader, obj.Header.MeshSize, obj.Mesh, diag)
            : ParseMeshBlock(reader, obj.Header.MeshSize, obj.Mesh, diag);
        if (!ok) {
            return std::nullopt;
        }
    }

    if (obj.Header.HelperSize > 0 && !reader.Skip(obj.Header.HelperSize)) {
        diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
        diag.Detail = "блок helper выходит за границы файла";
        return std::nullopt;
    }

    if (obj.Header.AnimSize > 0 && !reader.Skip(obj.Header.AnimSize)) {
        diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
        diag.Detail = "блок анимации выходит за границы файла";
        return std::nullopt;
    }

    if (expectedTotal < bytes.size()) {
        diag.Status = LgoStatus::OK_WITH_TRAILING_DATA;
        diag.Detail = std::format("трейлер {} байт", bytes.size() - expectedTotal);
        return obj;
    }

    diag.Status = LgoStatus::OK;
    return obj;
}

} // namespace Corsairs::Tools::AssetConverter
