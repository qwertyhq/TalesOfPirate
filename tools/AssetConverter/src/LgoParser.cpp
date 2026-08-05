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
    // Поле на диске может не иметь завершающего нуля, поэтому длина ищется
    // вручную с явным пределом. strnlen не используется: он объявлен в разных
    // заголовках на разных платформах.
    const char* name = Raw.TexSeq[stage].FileName;
    std::size_t length = 0;
    while (length < kMaxName && name[length] != '\0') {
        ++length;
    }
    return std::string{name, length};
}

namespace {

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
        if (!reader.Read(out[i].Raw)) {
            diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
            diag.Detail = std::format("не прочитан MtlTexInfo[{}]", i);
            return false;
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

    if (obj.Version < kMinSupportedVersion) {
        diag.Status = LgoStatus::VERSION_UNSUPPORTED;
        diag.Detail = std::format("version=0x{:08X} ниже поддерживаемой 0x{:08X}",
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
        if (!ParseMaterialBlock(reader, obj.Header.MtlSize, obj.Materials, diag)) {
            return std::nullopt;
        }
    }

    if (obj.Header.MeshSize > 0) {
        if (!ParseMeshBlock(reader, obj.Header.MeshSize, obj.Mesh, diag)) {
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
