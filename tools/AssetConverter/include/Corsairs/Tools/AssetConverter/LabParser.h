#pragma once

#include "Corsairs/Tools/AssetConverter/LabTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

enum class LabStatus : std::uint32_t {
    OK = 0,
    OK_WITH_TRAILING_DATA,
    VERSION_TRUNCATED,
    VERSION_UNSUPPORTED,
    HEADER_TRUNCATED,
    KEY_TYPE_UNSUPPORTED,
    DATA_MALFORMED,
};

[[nodiscard]] std::string_view ToString(LabStatus status);

struct LabDiagnostics {
    LabStatus Status{LabStatus::OK};
    std::string Detail;
    std::uint32_t Version{0};
};

// Ключи одной кости. Заполняются в зависимости от типа ключей файла:
// QUAT даёт Positions + Rotations, MAT43/MAT44 — Matrices (по 16 float,
// приведённые к 4x4). Длина каждого непустого массива равна FrameNum.
struct LabBoneTrack {
    std::vector<Vector3> Positions;
    std::vector<Quaternion> Rotations;
    std::vector<float> Matrices;
};

struct LabAnimation {
    std::uint32_t Version{0};
    BoneInfoHeader Header{};
    std::vector<BoneBaseInfo> Bones;
    // Обратные bind-матрицы, по 16 float на кость.
    std::vector<float> InverseBindMatrices;
    std::vector<BoneDummyInfo> Dummies;
    std::vector<LabBoneTrack> Tracks;

    [[nodiscard]] BoneKeyType KeyType() const {
        return static_cast<BoneKeyType>(Header.KeyType);
    }

    [[nodiscard]] float DurationSeconds() const {
        return Header.FrameNum > 0
            ? static_cast<float>(Header.FrameNum - 1) / kAnimFramesPerSecond
            : 0.0f;
    }
};

// Имя кости как записано в файле, с ограничением по kMaxName.
[[nodiscard]] std::string BoneName(const BoneBaseInfo& bone);

// Разбирает .lab целиком. std::nullopt — файл непригоден; причина в diag.
[[nodiscard]] std::optional<LabAnimation> ParseLab(std::span<const std::uint8_t> bytes,
                                                   LabDiagnostics& diag);

} // namespace Corsairs::Tools::AssetConverter
