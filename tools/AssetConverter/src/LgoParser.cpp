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

    // Блок геометрии наполняется в Task 4; пока пропускается по объявленному
    // размеру, чтобы проверка целостности файла работала уже сейчас.
    if (obj.Header.MeshSize > 0 && !reader.Skip(obj.Header.MeshSize)) {
        diag.Status = LgoStatus::MESH_BLOCK_MALFORMED;
        diag.Detail = "блок геометрии выходит за границы файла";
        return std::nullopt;
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
