#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <algorithm>
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
    case LgoStatus::HELPER_BLOCK_MALFORMED:   return "HELPER_BLOCK_MALFORMED";
    case LgoStatus::HELPER_SECTION_UNSUPPORTED: return "HELPER_SECTION_UNSUPPORTED";
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

// Читает vector<T> длиной count. false — данные за границей буфера.
template <typename T>
bool ReadVector(BinaryReader& reader, std::vector<T>& out, std::uint32_t count) {
    if (count == 0) {
        return true;
    }
    out.resize(count);
    return reader.ReadArray(out.data(), count);
}

// Эффективная версия блока. Для внешней версии 0x0000 настоящая версия лежит
// вложенным DWORD в начале блока; для остальных совпадает с внешней.
bool ReadEffectiveVersion(BinaryReader& reader, std::uint32_t outerVersion,
                          std::uint32_t& effective, std::string_view what,
                          LgoStatus failStatus, LgoDiagnostics& diag) {
    if (outerVersion != kLegacyVersion) {
        effective = outerVersion;
        return true;
    }
    if (!reader.Read(effective)) {
        diag.Status = failStatus;
        diag.Detail = std::format("не прочитана вложенная версия блока {}", what);
        return false;
    }
    return true;
}

// Размер одной записи материала на диске для данной эффективной версии.
// 0x0000 — 1028 байт (нет прозрачности), 0x0001 — 1164 (расширенные текстуры),
// 0x0002 и 0x1000+ — 1004 (текущий формат).
bool MaterialRecordSize(std::uint32_t effective, std::size_t& size) {
    if (effective >= 0x1000u || effective == 0x0002u) {
        size = kMtlTexInfoSize;
        return true;
    }
    if (effective == 0x0001u) {
        size = sizeof(MtlTexInfoV1);
        return true;
    }
    if (effective == 0x0000u) {
        size = kMtlTexInfoV0Size;
        return true;
    }
    return false;
}

// Копирует имена текстур и параметры материала в версионно-независимый вид.
template <typename RawT>
void FillMaterial(const RawT& raw, LgoMaterial& out) {
    out.Mtl = raw.Mtl;
    for (std::size_t stage = 0; stage < kMaxTextureStageNum; ++stage) {
        out.Textures[stage] = FixedName(raw.TexSeq[stage].FileName);
    }
}

// Разбирает блок материалов любой известной версии и проверяет, что потрачено
// ровно mtlSize байт.
bool ParseMaterialBlock(BinaryReader& reader, std::uint32_t mtlSize,
                        std::uint32_t outerVersion, std::vector<LgoMaterial>& out,
                        LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    std::uint32_t effective = 0;
    if (!ReadEffectiveVersion(reader, outerVersion, effective, "материалов",
                              LgoStatus::MTL_BLOCK_MALFORMED, diag)) {
        return false;
    }

    std::size_t recordSize = 0;
    if (!MaterialRecordSize(effective, recordSize)) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format("версия блока материалов 0x{:04X} не поддерживается",
                                  effective);
        return false;
    }

    std::uint32_t mtlNum = 0;
    if (!reader.Read(mtlNum)) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = "не прочитан MtlNum";
        return false;
    }

    const std::size_t prefix = reader.Offset() - blockStart;
    const std::size_t expected = prefix + recordSize * mtlNum;
    if (expected != mtlSize) {
        diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "версия 0x{:04X}: MtlNum={} даёт {} байт, заголовок объявил MtlSize={}",
            effective, mtlNum, expected, mtlSize);
        return false;
    }

    out.resize(mtlNum);
    for (std::uint32_t i = 0; i < mtlNum; ++i) {
        bool ok = false;
        if (recordSize == kMtlTexInfoSize) {
            MtlTexInfo raw{};
            ok = reader.Read(raw);
            if (ok) {
                out[i].Opacity = raw.Opacity;
                out[i].TranspType = raw.TranspType;
                FillMaterial(raw, out[i]);
            }
        }
        else if (recordSize == sizeof(MtlTexInfoV1)) {
            MtlTexInfoV1 raw{};
            ok = reader.Read(raw);
            if (ok) {
                out[i].Opacity = raw.Opacity;
                out[i].TranspType = raw.TranspType;
                FillMaterial(raw, out[i]);
            }
        }
        else {
            // Версия 0x0000: полей прозрачности нет, движок оставляет
            // значения по умолчанию — делаем так же.
            MtlTexInfoV0 raw{};
            ok = reader.Read(raw);
            if (ok) {
                FillMaterial(raw, out[i]);
            }
        }

        if (!ok) {
            diag.Status = LgoStatus::MTL_BLOCK_MALFORMED;
            diag.Detail = std::format("не прочитан материал {}", i);
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

// Сколько наборов UV несёт вершина при данном FVF. Порядок проверок повторяет
// цепочку if/else if движка: TEX1 проверяется первым, поэтому набор из четырёх
// UV имеет флаг TEX4 и не совпадает с TEX1.
std::uint32_t TexcoordSetCount(std::uint32_t fvf) {
    if (HasFvf(fvf, FvfFlag::TEX1)) {
        return 1;
    }
    if (HasFvf(fvf, FvfFlag::TEX2)) {
        return 2;
    }
    if (HasFvf(fvf, FvfFlag::TEX3)) {
        return 3;
    }
    if (HasFvf(fvf, FvfFlag::TEX4)) {
        return 4;
    }
    return 0;
}

// Разбирает блок геометрии любой известной версии и проверяет, что потрачено
// ровно meshSize байт.
//
// Различий между версиями два, и оба существенные:
//   * заголовок — 128 байт в 0x1004+, 120 в промежуточных, 152 в 0x0000;
//   * порядок массивов — в 0x1004+ подсеты идут последними, в остальных
//     первыми, а индексы костей там однобайтовые и определяются флагом
//     LASTBETA_UBYTE4, а не полем BoneIndexNum.
bool ParseMeshBlock(BinaryReader& reader, std::uint32_t meshSize,
                    std::uint32_t outerVersion, LgoMesh& mesh, LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    std::uint32_t effective = 0;
    if (!ReadEffectiveVersion(reader, outerVersion, effective, "геометрии",
                              LgoStatus::MESH_BLOCK_MALFORMED, diag)) {
        return false;
    }

    const bool isModern = effective >= kMinSupportedVersion;

    if (isModern) {
        if (!reader.Read(mesh.Header)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитан MeshInfoHeader";
            return false;
        }
    }
    else if (effective >= 0x1000u || effective == 0x0001u) {
        MeshInfoHeaderV3 header{};
        if (!reader.Read(header)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитан MeshInfoHeaderV3";
            return false;
        }
        mesh.Header = MeshInfoHeader{};
        mesh.Header.Fvf = header.Fvf;
        mesh.Header.PtType = header.PtType;
        mesh.Header.VertexNum = header.VertexNum;
        mesh.Header.IndexNum = header.IndexNum;
        mesh.Header.SubsetNum = header.SubsetNum;
        mesh.Header.BoneIndexNum = header.BoneIndexNum;
        mesh.Header.BoneInflFactor = header.BoneIndexNum > 0 ? 2u : 0u;
    }
    else if (effective == 0x0000u) {
        MeshInfoHeaderV0 header{};
        if (!reader.Read(header)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитан MeshInfoHeaderV0";
            return false;
        }
        mesh.Header = MeshInfoHeader{};
        mesh.Header.Fvf = header.Fvf;
        mesh.Header.PtType = header.PtType;
        mesh.Header.VertexNum = header.VertexNum;
        mesh.Header.IndexNum = header.IndexNum;
        mesh.Header.SubsetNum = header.SubsetNum;
        mesh.Header.BoneIndexNum = header.BoneIndexNum;
        mesh.Header.BoneInflFactor = header.BoneIndexNum > 0 ? 2u : 0u;
    }
    else {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = std::format("версия блока геометрии 0x{:04X} не поддерживается",
                                  effective);
        return false;
    }

    const std::uint32_t vertexNum = mesh.Header.VertexNum;
    const std::uint32_t texcoordSets = TexcoordSetCount(mesh.Header.Fvf);

    // Старые версии кладут подсеты перед вершинами.
    if (!isModern && !ReadVector(reader, mesh.Subsets, mesh.Header.SubsetNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны подсеты";
        return false;
    }

    if (isModern &&
        !ReadVector(reader, mesh.VertexElements, mesh.Header.VertexElementNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан VertexElements";
        return false;
    }

    if (!ReadVector(reader, mesh.Positions, vertexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны позиции вершин";
        return false;
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::NORMAL) &&
        !ReadVector(reader, mesh.Normals, vertexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны нормали";
        return false;
    }

    for (std::uint32_t set = 0; set < texcoordSets; ++set) {
        if (!ReadVector(reader, mesh.Texcoords[set], vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = std::format("не прочитан UV-набор {}", set);
            return false;
        }
    }

    if (HasFvf(mesh.Header.Fvf, FvfFlag::DIFFUSE) &&
        !ReadVector(reader, mesh.VertexColors, vertexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны цвета вершин";
        return false;
    }

    // Наличие скиннинга: в новых версиях по счётчику костей, в старых — по
    // флагу FVF. Индексы костей там же однобайтовые.
    const bool hasSkin = isModern
        ? mesh.Header.BoneIndexNum > 0
        : HasFvf(mesh.Header.Fvf, FvfFlag::LASTBETA_UBYTE4);

    if (hasSkin) {
        if (!ReadVector(reader, mesh.Blends, vertexNum)) {
            diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
            diag.Detail = "не прочитаны веса скиннинга";
            return false;
        }

        if (isModern) {
            if (!ReadVector(reader, mesh.BoneIndices, mesh.Header.BoneIndexNum)) {
                diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
                diag.Detail = "не прочитаны индексы костей";
                return false;
            }
        }
        else {
            std::vector<std::uint8_t> byteIndices;
            if (!ReadVector(reader, byteIndices, mesh.Header.BoneIndexNum)) {
                diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
                diag.Detail = "не прочитаны однобайтовые индексы костей";
                return false;
            }
            mesh.BoneIndices.assign(byteIndices.begin(), byteIndices.end());
        }
    }

    if (!ReadVector(reader, mesh.Indices, mesh.Header.IndexNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитан индексный буфер";
        return false;
    }

    // В новых версиях подсеты идут последними.
    if (isModern && !ReadVector(reader, mesh.Subsets, mesh.Header.SubsetNum)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "не прочитаны подсеты";
        return false;
    }

    const std::size_t consumed = reader.Offset() - blockStart;
    if (consumed != meshSize) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "версия 0x{:04X}: прочитано {} байт, заголовок объявил MeshSize={} "
            "(fvf=0x{:08X}, vertexNum={}, indexNum={}, subsetNum={}, boneIndexNum={})",
            effective, consumed, meshSize, mesh.Header.Fvf, mesh.Header.VertexNum,
            mesh.Header.IndexNum, mesh.Header.SubsetNum, mesh.Header.BoneIndexNum);
        return false;
    }
    return true;
}

// В версиях <= 0x1001 бокс хранился как (точка, размер); приводим к
// (центр, радиус), как делает движок.
void ConvertBoxPointSizeToCenterRadius(Box& box) {
    const Vector3 point = box.Center;
    const Vector3 size = box.Radius;
    box.Radius = Vector3{size.X / 2.0f, size.Y / 2.0f, size.Z / 2.0f};
    box.Center = Vector3{point.X + box.Radius.X,
                         point.Y + box.Radius.Y,
                         point.Z + box.Radius.Z};
}

} // namespace

// Разбирает helper-блок. Секции идут в фиксированном порядке по битам Type,
// каждая начинается со своего счётчика. Проверяется точный расход байт.
//
// Раскладка dummy зависит от версии: в 0x1001+ это 140 байт (Mat + MatLocal +
// родитель), в 0x0000..0x1000 — 68 байт (только Id + Mat).
bool ParseHelperBlock(BinaryReader& reader, std::uint32_t helperSize,
                      std::uint32_t version, LgoHelper& helper, LgoDiagnostics& diag) {
    const std::size_t blockStart = reader.Offset();

    if (version == kLegacyVersion) {
        std::uint32_t innerVersion = 0;
        if (!reader.Read(innerVersion)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитана вложенная версия helper-блока";
            return false;
        }
    }

    if (!reader.Read(helper.Type)) {
        diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
        diag.Detail = "не прочитан Type helper-блока";
        return false;
    }

    if (HasHelper(helper.Type, HelperType::DUMMY)) {
        std::uint32_t count = 0;
        if (!reader.Read(count)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитан счётчик dummy";
            return false;
        }

        helper.Dummies.resize(count);
        if (version >= 0x1001u) {
            if (!reader.ReadArray(helper.Dummies.data(), count)) {
                diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
                diag.Detail = "не прочитан массив dummy";
                return false;
            }
        }
        else {
            // Короткая раскладка: локальная матрица и родитель отсутствуют,
            // движок оставляет их нулевыми — делаем так же.
            std::vector<HelperDummyInfoV1000> legacy(count);
            if (count > 0 && !reader.ReadArray(legacy.data(), count)) {
                diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
                diag.Detail = "не прочитан массив dummy (короткая раскладка)";
                return false;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                helper.Dummies[i] = HelperDummyInfo{};
                helper.Dummies[i].Id = legacy[i].Id;
                for (std::size_t k = 0; k < 16; ++k) {
                    helper.Dummies[i].Mat[k] = legacy[i].Mat[k];
                }
            }
        }
    }

    if (HasHelper(helper.Type, HelperType::BOX)) {
        std::uint32_t count = 0;
        if (!reader.Read(count)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитан счётчик helper-боксов";
            return false;
        }
        if (!ReadVector(reader, helper.Boxes, count)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитан массив helper-боксов";
            return false;
        }
        if (version <= 0x1001u) {
            for (HelperBoxInfo& box : helper.Boxes) {
                ConvertBoxPointSizeToCenterRadius(box.BoundBox);
            }
        }
    }

    // Секция MESH переменной длины: у каждого меша свой заголовок, за которым
    // идут вершины и грани. Встречается в .lmo сцен (530 из 639 файлов).
    if (HasHelper(helper.Type, HelperType::MESH)) {
        std::uint32_t count = 0;
        if (!reader.Read(count)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитан счётчик helper-мешей";
            return false;
        }

        helper.Meshes.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            LgoHelperMesh& mesh = helper.Meshes[i];
            if (!reader.Read(mesh.Header)) {
                diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
                diag.Detail = std::format("не прочитан заголовок helper-меша {}", i);
                return false;
            }
            if (!ReadVector(reader, mesh.Vertices, mesh.Header.VertexNum)) {
                diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
                diag.Detail = std::format("не прочитаны вершины helper-меша {}", i);
                return false;
            }
            if (!ReadVector(reader, mesh.Faces, mesh.Header.FaceNum)) {
                diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
                diag.Detail = std::format("не прочитаны грани helper-меша {}", i);
                return false;
            }
        }

        if (version <= 0x1001u) {
            for (LgoHelperMesh& mesh : helper.Meshes) {
                ConvertBoxPointSizeToCenterRadius(mesh.Header.BoundBox);
            }
        }
    }

    if (HasHelper(helper.Type, HelperType::BOUNDING_BOX)) {
        std::uint32_t count = 0;
        if (!reader.Read(count)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитан счётчик bounding box";
            return false;
        }
        if (!ReadVector(reader, helper.BoundingBoxes, count)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитан массив bounding box";
            return false;
        }

        if (version <= 0x1001u) {
            for (BoundingBoxInfo& box : helper.BoundingBoxes) {
                ConvertBoxPointSizeToCenterRadius(box.BoundBox);
            }
        }
    }

    if (HasHelper(helper.Type, HelperType::BOUNDING_SPHERE)) {
        std::uint32_t count = 0;
        if (!reader.Read(count)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитан счётчик bounding sphere";
            return false;
        }
        if (!ReadVector(reader, helper.BoundingSpheres, count)) {
            diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
            diag.Detail = "не прочитан массив bounding sphere";
            return false;
        }
    }

    const std::size_t consumed = reader.Offset() - blockStart;
    if (consumed != helperSize) {
        diag.Status = LgoStatus::HELPER_BLOCK_MALFORMED;
        diag.Detail = std::format(
            "прочитано {} байт, заголовок объявил HelperSize={} (Type=0x{:04X}, "
            "dummy={}, bbox={}, bsphere={})",
            consumed, helperSize, helper.Type, helper.Dummies.size(),
            helper.BoundingBoxes.size(), helper.BoundingSpheres.size());
        return false;
    }
    return true;
}

bool IsSupportedGeomVersion(std::uint32_t version) {
    // Реализованы все версии, встречающиеся в датасете: легаси 0x0000 с
    // вложенными версиями блоков, промежуточные 0x1000..0x1003 и текущие
    // 0x1004..0x1005.
    return IsKnownVersion(version);
}

bool ParseGeomObjBody(BinaryReader& reader, std::uint32_t version,
                      std::size_t availableBytes, LgoGeomObj& obj, LgoDiagnostics& diag) {
    obj.Version = version;

    if (!reader.Read(obj.Header)) {
        diag.Status = LgoStatus::HEADER_TRUNCATED;
        diag.Detail = std::format("нужно {} байт заголовка, доступно {}",
                                  kGeomObjHeaderSize, reader.Remaining());
        return false;
    }

    // Пока родитель неизвестен, положение в модели равно локальному. Для
    // `.lgo` так и остаётся, для `.lmo` цепочку родителей досчитывает ParseLmo.
    std::copy_n(obj.Header.MatLocal, 16, obj.MatModel);

    const std::uint64_t blocksSum =
        static_cast<std::uint64_t>(obj.Header.MtlSize) +
        static_cast<std::uint64_t>(obj.Header.MeshSize) +
        static_cast<std::uint64_t>(obj.Header.HelperSize) +
        static_cast<std::uint64_t>(obj.Header.AnimSize);
    const std::uint64_t expectedTotal = blocksSum + kGeomObjHeaderSize;

    if (expectedTotal > availableBytes) {
        diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
        diag.Detail = std::format(
            "mtl={} mesh={} helper={} anim={}; объекту нужно {} байт, доступно {}",
            obj.Header.MtlSize, obj.Header.MeshSize, obj.Header.HelperSize,
            obj.Header.AnimSize, expectedTotal, availableBytes);
        return false;
    }

    if (obj.Header.MtlSize > 0) {
        if (!ParseMaterialBlock(reader, obj.Header.MtlSize, version, obj.Materials, diag)) {
            return false;
        }
    }

    if (obj.Header.MeshSize > 0) {
        if (!ParseMeshBlock(reader, obj.Header.MeshSize, version, obj.Mesh, diag)) {
            return false;
        }
    }

    if (obj.Header.HelperSize > 0) {
        if (!ParseHelperBlock(reader, obj.Header.HelperSize, version, obj.Helper, diag)) {
            return false;
        }
    }

    if (obj.Header.AnimSize > 0 && !reader.Skip(obj.Header.AnimSize)) {
        diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
        diag.Detail = "блок анимации выходит за границы объекта";
        return false;
    }

    diag.Status = expectedTotal < availableBytes
        ? LgoStatus::OK_WITH_TRAILING_DATA
        : LgoStatus::OK;
    return true;
}

std::optional<LgoGeomObj> ParseLgo(std::span<const std::uint8_t> bytes,
                                   LgoDiagnostics& diag) {
    diag = {};
    BinaryReader reader{bytes};

    std::uint32_t version = 0;
    if (!reader.Read(version)) {
        diag.Status = LgoStatus::VERSION_TRUNCATED;
        diag.Detail = "файл короче 4 байт";
        return std::nullopt;
    }
    diag.Version = version;

    if (!IsKnownVersion(version)) {
        diag.Status = LgoStatus::VERSION_UNKNOWN;
        diag.Detail = std::format("version=0x{:08X}, ожидалось 0x0000 или 0x1000..0x1005",
                                  version);
        return std::nullopt;
    }

    // Поддерживаются две раскладки: современная (0x1004+) и легаси (0x0000).
    // Версии 0x1000..0x1003 в датасете отсутствуют и не реализованы.
    if (!IsSupportedGeomVersion(version)) {
        diag.Status = LgoStatus::VERSION_UNSUPPORTED;
        diag.Detail = std::format(
            "version=0x{:08X}: поддерживаются 0x0000 и 0x{:04X}+",
            version, kMinSupportedVersion);
        return std::nullopt;
    }

    LgoGeomObj obj;
    if (!ParseGeomObjBody(reader, version, bytes.size() - 4, obj, diag)) {
        return std::nullopt;
    }

    if (diag.Status == LgoStatus::OK_WITH_TRAILING_DATA) {
        const std::uint64_t used =
            4 + kGeomObjHeaderSize + obj.Header.MtlSize + obj.Header.MeshSize +
            obj.Header.HelperSize + obj.Header.AnimSize;
        diag.Detail = std::format("трейлер {} байт", bytes.size() - used);
    }
    return obj;
}

} // namespace Corsairs::Tools::AssetConverter
