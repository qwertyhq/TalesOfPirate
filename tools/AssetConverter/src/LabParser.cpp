#include "Corsairs/Tools/AssetConverter/LabParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <cmath>
#include <format>
#include <limits>

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

bool BytesFit(std::size_t count, std::size_t elementSize,
              std::size_t& byteCount) {
    if (count > std::numeric_limits<std::size_t>::max() / elementSize) {
        return false;
    }
    byteCount = count * elementSize;
    return true;
}

bool CanReadBodyBytes(const BinaryReader& reader, std::size_t bodyStart,
                      std::size_t availableBytes, std::size_t byteCount) {
    if (reader.Offset() < bodyStart) {
        return false;
    }
    const std::size_t consumed = reader.Offset() - bodyStart;
    return consumed <= availableBytes &&
           byteCount <= availableBytes - consumed &&
           reader.CanRead(byteCount);
}

template <typename T>
bool ReadBodyValue(BinaryReader& reader, std::size_t bodyStart,
                   std::size_t availableBytes, T& value) {
    return CanReadBodyBytes(reader, bodyStart, availableBytes, sizeof(T)) &&
           reader.Read(value);
}

template <typename T>
bool ReadBodyVector(BinaryReader& reader, std::size_t bodyStart,
                    std::size_t availableBytes, std::vector<T>& out,
                    std::uint32_t count) {
    std::size_t byteCount = 0;
    if (!BytesFit(count, sizeof(T), byteCount) ||
        !CanReadBodyBytes(reader, bodyStart, availableBytes, byteCount)) {
        return false;
    }
    out.resize(count);
    return reader.ReadArray(out.data(), out.size());
}

bool AllFinite(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

bool ValidateHierarchy(const LabAnimation& animation, std::string& detail) {
    const std::uint32_t boneNum = animation.Header.BoneNum;
    // Сначала проверяем весь индексный граф. Иначе допустимая forward-ссылка
    // может привести cycle walk к ещё не проверенному out-of-range parent.
    for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
        const BoneBaseInfo& info = animation.Bones[bone];
        if (info.Id != bone) {
            detail = std::format("кость {} имеет несовпадающий id={}", bone, info.Id);
            return false;
        }
        if (info.ParentId != kNoParent && info.ParentId >= boneNum) {
            detail = std::format("кость {} имеет parentId={} вне {} костей",
                                 bone, info.ParentId, boneNum);
            return false;
        }
    }

    for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
        std::uint32_t cursor = bone;
        bool reachesRoot = false;
        for (std::uint32_t step = 0; step <= boneNum; ++step) {
            if (cursor >= boneNum) {
                detail = std::format(
                    "иерархия кости {} вышла на индекс {} вне {} костей",
                    bone, cursor, boneNum);
                return false;
            }
            const std::uint32_t parent = animation.Bones[cursor].ParentId;
            if (parent == kNoParent) {
                reachesRoot = true;
                break;
            }
            cursor = parent;
        }
        if (!reachesRoot) {
            detail = std::format("иерархия кости {} содержит цикл", bone);
            return false;
        }
    }

    for (std::size_t dummy = 0; dummy < animation.Dummies.size(); ++dummy) {
        const BoneDummyInfo& info = animation.Dummies[dummy];
        if (info.ParentBoneId >= boneNum) {
            detail = std::format("bone dummy {} имеет parentBoneId={} вне {} костей",
                                 dummy, info.ParentBoneId, boneNum);
            return false;
        }
        if (!AllFinite(info.Mat, 16)) {
            detail = std::format("bone dummy {} содержит нечисловую матрицу", dummy);
            return false;
        }
    }
    return true;
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

bool ParseBoneAnimationBody(BinaryReader& reader, std::uint32_t version,
                            std::size_t availableBytes,
                            LabAnimation& animation,
                            std::string& detail) {
    detail.clear();
    animation = {};
    animation.Version = version;
    const std::size_t bodyStart = reader.Offset();

    if (version == kLegacyVersion) {
        std::uint32_t legacyVersion = 0;
        if (!ReadBodyValue(reader, bodyStart, availableBytes, legacyVersion)) {
            detail = "BONE v0 не содержит вложенную версию";
            return false;
        }
    }

    if (!ReadBodyValue(reader, bodyStart, availableBytes, animation.Header)) {
        detail = "не прочитан BoneInfoHeader";
        return false;
    }

    const std::uint32_t boneNum = animation.Header.BoneNum;
    const std::uint32_t frameNum = animation.Header.FrameNum;
    const std::uint32_t dummyNum = animation.Header.DummyNum;
    if (boneNum == 0 || frameNum == 0 || boneNum > 256u) {
        detail = std::format("недопустимый BONE header: bones={}, frames={}",
                             boneNum, frameNum);
        return false;
    }

    const BoneKeyType keyType = animation.KeyType();
    if (keyType != BoneKeyType::MAT43 && keyType != BoneKeyType::MAT44 &&
        keyType != BoneKeyType::QUAT) {
        detail = std::format("KeyType={} (ожидались 1, 2 или 3)",
                             animation.Header.KeyType);
        return false;
    }

    std::size_t boneBytes = 0;
    std::size_t inverseBindBytes = 0;
    std::size_t dummyBytes = 0;
    if (!BytesFit(boneNum, sizeof(BoneBaseInfo), boneBytes) ||
        !BytesFit(static_cast<std::size_t>(boneNum) * 16u, sizeof(float),
                  inverseBindBytes) ||
        !BytesFit(dummyNum, sizeof(BoneDummyInfo), dummyBytes)) {
        detail = "размер базовых массивов BONE переполнен";
        return false;
    }
    const std::size_t consumed = reader.Offset() - bodyStart;
    if (consumed > availableBytes || boneBytes > availableBytes - consumed ||
        inverseBindBytes > availableBytes - consumed - boneBytes ||
        dummyBytes > availableBytes - consumed - boneBytes - inverseBindBytes) {
        detail = "базовые массивы BONE выходят за declared size";
        return false;
    }

    if (!ReadBodyVector(reader, bodyStart, availableBytes,
                        animation.Bones, boneNum)) {
        detail = "не прочитан массив костей";
        return false;
    }

    animation.InverseBindMatrices.resize(static_cast<std::size_t>(boneNum) * 16u);
    if (!CanReadBodyBytes(reader, bodyStart, availableBytes, inverseBindBytes) ||
        !reader.ReadArray(animation.InverseBindMatrices.data(),
                          animation.InverseBindMatrices.size())) {
        detail = "не прочитаны обратные bind-матрицы";
        return false;
    }
    if (!AllFinite(animation.InverseBindMatrices.data(),
                   animation.InverseBindMatrices.size())) {
        detail = "обратные bind-матрицы содержат нечисловые значения";
        return false;
    }

    if (!ReadBodyVector(reader, bodyStart, availableBytes,
                        animation.Dummies, dummyNum)) {
        detail = "не прочитан массив bone dummy";
        return false;
    }
    if (!ValidateHierarchy(animation, detail)) {
        return false;
    }

    animation.Tracks.resize(boneNum);
    switch (keyType) {
    case BoneKeyType::MAT43:
        for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
            std::vector<Matrix43> raw;
            if (!ReadBodyVector(reader, bodyStart, availableBytes, raw, frameNum)) {
                detail = std::format("не прочитаны MAT43 ключи кости {}", bone);
                return false;
            }
            LabBoneTrack& track = animation.Tracks[bone];
            track.Matrices.resize(static_cast<std::size_t>(frameNum) * 16u);
            for (std::uint32_t frame = 0; frame < frameNum; ++frame) {
                if (!AllFinite(raw[frame].M, 12)) {
                    detail = std::format("MAT43 кости {} frame {} нечисловой",
                                         bone, frame);
                    return false;
                }
                Expand43To44(raw[frame],
                             track.Matrices.data() +
                                 static_cast<std::size_t>(frame) * 16u);
            }
        }
        break;

    case BoneKeyType::MAT44:
        for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
            LabBoneTrack& track = animation.Tracks[bone];
            const std::size_t valueCount = static_cast<std::size_t>(frameNum) * 16u;
            std::size_t byteCount = 0;
            if (!BytesFit(valueCount, sizeof(float), byteCount) ||
                !CanReadBodyBytes(reader, bodyStart, availableBytes, byteCount)) {
                detail = std::format("MAT44 ключи кости {} выходят за declared size",
                                     bone);
                return false;
            }
            track.Matrices.resize(valueCount);
            if (!reader.ReadArray(track.Matrices.data(), track.Matrices.size()) ||
                !AllFinite(track.Matrices.data(), track.Matrices.size())) {
                detail = std::format("MAT44 ключи кости {} повреждены", bone);
                return false;
            }
        }
        break;

    case BoneKeyType::QUAT:
        for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
            LabBoneTrack& track = animation.Tracks[bone];
            const std::uint32_t positionCount =
                version >= 0x1003u || animation.Bones[bone].ParentId == kNoParent
                    ? frameNum
                    : 1u;
            if (!ReadBodyVector(reader, bodyStart, availableBytes,
                                track.Positions, positionCount)) {
                detail = std::format("не прочитаны QUAT позиции кости {}", bone);
                return false;
            }
            if (positionCount == 1u && frameNum > 1u) {
                track.Positions.resize(frameNum, track.Positions.front());
            }
            if (!ReadBodyVector(reader, bodyStart, availableBytes,
                                track.Rotations, frameNum)) {
                detail = std::format("не прочитаны QUAT повороты кости {}", bone);
                return false;
            }
            for (std::uint32_t frame = 0; frame < frameNum; ++frame) {
                const Vector3& position = track.Positions[frame];
                const Quaternion& rotation = track.Rotations[frame];
                if (!std::isfinite(position.X) || !std::isfinite(position.Y) ||
                    !std::isfinite(position.Z) || !std::isfinite(rotation.X) ||
                    !std::isfinite(rotation.Y) || !std::isfinite(rotation.Z) ||
                    !std::isfinite(rotation.W)) {
                    detail = std::format("QUAT кости {} frame {} нечисловой",
                                         bone, frame);
                    return false;
                }
            }
        }
        break;
    }

    return true;
}

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

    if (reader.Remaining() < sizeof(BoneInfoHeader)) {
        diag.Status = LabStatus::HEADER_TRUNCATED;
        diag.Detail = "не прочитан BoneInfoHeader";
        return std::nullopt;
    }

    std::string bodyDetail;
    if (!ParseBoneAnimationBody(reader, anim.Version, reader.Remaining(),
                                anim, bodyDetail)) {
        const BoneKeyType keyType = anim.KeyType();
        diag.Status = keyType != BoneKeyType::MAT43 &&
                              keyType != BoneKeyType::MAT44 &&
                              keyType != BoneKeyType::QUAT
                          ? LabStatus::KEY_TYPE_UNSUPPORTED
                          : LabStatus::DATA_MALFORMED;
        diag.Detail = std::move(bodyDetail);
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
