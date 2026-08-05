#include "Corsairs/Tools/AssetConverter/LabParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <format>

namespace Corsairs::Tools::AssetConverter {

std::string_view ToString(LabStatus status) {
    switch (status) {
    case LabStatus::OK:                    return "OK";
    case LabStatus::OK_WITH_TRAILING_DATA: return "OK_WITH_TRAILING_DATA";
    case LabStatus::VERSION_TRUNCATED:     return "VERSION_TRUNCATED";
    case LabStatus::VERSION_UNSUPPORTED:   return "VERSION_UNSUPPORTED";
    case LabStatus::HEADER_TRUNCATED:      return "HEADER_TRUNCATED";
    case LabStatus::KEY_TYPE_UNSUPPORTED:  return "KEY_TYPE_UNSUPPORTED";
    case LabStatus::DATA_MALFORMED:        return "DATA_MALFORMED";
    }
    return "UNKNOWN";
}

std::string BoneName(const BoneBaseInfo& bone) {
    std::size_t length = 0;
    while (length < kMaxName && bone.Name[length] != '\0') {
        ++length;
    }
    return std::string{bone.Name, length};
}

namespace {

template <typename T>
bool ReadVector(BinaryReader& reader, std::vector<T>& out, std::uint32_t count) {
    if (count == 0) {
        return true;
    }
    out.resize(count);
    return reader.ReadArray(out.data(), count);
}

// Расширяет 4x3 до 4x4, дописывая столбец (0,0,0,1). Исходная раскладка —
// построчная: три базисных вектора, затем перенос.
void Expand43To44(const Matrix43& src, float* dst) {
    for (int row = 0; row < 4; ++row) {
        dst[row * 4 + 0] = src.M[row * 3 + 0];
        dst[row * 4 + 1] = src.M[row * 3 + 1];
        dst[row * 4 + 2] = src.M[row * 3 + 2];
        dst[row * 4 + 3] = (row == 3) ? 1.0f : 0.0f;
    }
}

} // namespace

std::optional<LabAnimation> ParseLab(std::span<const std::uint8_t> bytes,
                                     LabDiagnostics& diag) {
    diag = {};
    BinaryReader reader{bytes};

    LabAnimation anim;

    if (!reader.Read(anim.Version)) {
        diag.Status = LabStatus::VERSION_TRUNCATED;
        diag.Detail = "файл короче 4 байт";
        return std::nullopt;
    }
    diag.Version = anim.Version;

    // В датасете встречаются только 0x1004 и 0x1005. Движок для версий ниже
    // 0x1000 требует переэкспорта, а различия 0x1000..0x1003 касаются лишь
    // ветки QUAT (позиции хранились по одной на не-корневую кость).
    if (anim.Version < 0x1004u) {
        diag.Status = LabStatus::VERSION_UNSUPPORTED;
        diag.Detail = std::format(
            "version=0x{:08X}: поддерживаются 0x1004 и 0x1005", anim.Version);
        return std::nullopt;
    }

    if (!reader.Read(anim.Header)) {
        diag.Status = LabStatus::HEADER_TRUNCATED;
        diag.Detail = "не прочитан BoneInfoHeader";
        return std::nullopt;
    }

    const std::uint32_t boneNum = anim.Header.BoneNum;
    const std::uint32_t frameNum = anim.Header.FrameNum;

    if (!ReadVector(reader, anim.Bones, boneNum)) {
        diag.Status = LabStatus::DATA_MALFORMED;
        diag.Detail = "не прочитан массив костей";
        return std::nullopt;
    }

    anim.InverseBindMatrices.resize(static_cast<std::size_t>(boneNum) * 16);
    if (boneNum > 0 &&
        !reader.ReadArray(anim.InverseBindMatrices.data(),
                          static_cast<std::size_t>(boneNum) * 16)) {
        diag.Status = LabStatus::DATA_MALFORMED;
        diag.Detail = "не прочитаны обратные bind-матрицы";
        return std::nullopt;
    }

    if (!ReadVector(reader, anim.Dummies, anim.Header.DummyNum)) {
        diag.Status = LabStatus::DATA_MALFORMED;
        diag.Detail = "не прочитаны dummy костей";
        return std::nullopt;
    }

    anim.Tracks.resize(boneNum);

    switch (anim.KeyType()) {
    case BoneKeyType::QUAT:
        for (std::uint32_t i = 0; i < boneNum; ++i) {
            if (!ReadVector(reader, anim.Tracks[i].Positions, frameNum)) {
                diag.Status = LabStatus::DATA_MALFORMED;
                diag.Detail = std::format("не прочитаны позиции кости {}", i);
                return std::nullopt;
            }
            if (!ReadVector(reader, anim.Tracks[i].Rotations, frameNum)) {
                diag.Status = LabStatus::DATA_MALFORMED;
                diag.Detail = std::format("не прочитаны кватернионы кости {}", i);
                return std::nullopt;
            }
        }
        break;

    case BoneKeyType::MAT43:
        for (std::uint32_t i = 0; i < boneNum; ++i) {
            std::vector<Matrix43> raw;
            if (!ReadVector(reader, raw, frameNum)) {
                diag.Status = LabStatus::DATA_MALFORMED;
                diag.Detail = std::format("не прочитаны матрицы 4x3 кости {}", i);
                return std::nullopt;
            }
            anim.Tracks[i].Matrices.resize(static_cast<std::size_t>(frameNum) * 16);
            for (std::uint32_t f = 0; f < frameNum; ++f) {
                Expand43To44(raw[f], anim.Tracks[i].Matrices.data() + f * 16);
            }
        }
        break;

    case BoneKeyType::MAT44:
        for (std::uint32_t i = 0; i < boneNum; ++i) {
            anim.Tracks[i].Matrices.resize(static_cast<std::size_t>(frameNum) * 16);
            if (frameNum > 0 &&
                !reader.ReadArray(anim.Tracks[i].Matrices.data(),
                                  static_cast<std::size_t>(frameNum) * 16)) {
                diag.Status = LabStatus::DATA_MALFORMED;
                diag.Detail = std::format("не прочитаны матрицы 4x4 кости {}", i);
                return std::nullopt;
            }
        }
        break;

    default:
        diag.Status = LabStatus::KEY_TYPE_UNSUPPORTED;
        diag.Detail = std::format("KeyType={} (ожидались 1, 2 или 3)", anim.Header.KeyType);
        return std::nullopt;
    }

    if (reader.Remaining() > 0) {
        diag.Status = LabStatus::OK_WITH_TRAILING_DATA;
        diag.Detail = std::format("трейлер {} байт", reader.Remaining());
        return anim;
    }

    diag.Status = LabStatus::OK;
    return anim;
}

} // namespace Corsairs::Tools::AssetConverter
