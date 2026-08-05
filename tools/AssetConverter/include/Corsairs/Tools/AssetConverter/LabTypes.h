#pragma once

#include "Corsairs/Tools/AssetConverter/LgoTypes.h"

#include <cstddef>
#include <cstdint>

namespace Corsairs::Tools::AssetConverter {

// Тип ключей анимации костей. Источник — lwBoneKeyInfoType в
// sources/Engine/Model/lwExpObj.h.
enum class BoneKeyType : std::uint32_t {
    MAT43 = 1,
    MAT44 = 2,
    QUAT  = 3,
};

// Опорная частота кадров анимаций MindPower3D. Движок нормализует
// проигрывание относительно 30 FPS (см. комментарий в
// sources/Engine/Model/lwPrimitive.cpp).
inline constexpr float kAnimFramesPerSecond = 30.0f;

#pragma pack(push, 1)

struct Quaternion {
    float X;
    float Y;
    float Z;
    float W;
};

// 4x3: три базисных вектора и перенос, построчно.
struct Matrix43 {
    float M[12];
};

struct BoneInfoHeader {
    std::uint32_t BoneNum;
    std::uint32_t FrameNum;
    std::uint32_t DummyNum;
    std::uint32_t KeyType;
};

struct BoneBaseInfo {
    char Name[kMaxName];
    std::uint32_t Id;
    std::uint32_t ParentId;
};

struct BoneDummyInfo {
    std::uint32_t Id;
    std::uint32_t ParentBoneId;
    float Mat[16];
};

#pragma pack(pop)

// Размеры проверены на всех 670 файлах `Client/animation/*.lab`: расчётный
// размер файла совпал с фактическим для обоих типов ключей.
static_assert(sizeof(Quaternion) == 16, "Quaternion: раскладка на диске 16 байт");
static_assert(sizeof(Matrix43) == 48, "Matrix43: раскладка на диске 48 байт");
static_assert(sizeof(BoneInfoHeader) == 16, "BoneInfoHeader: раскладка на диске 16 байт");
static_assert(sizeof(BoneBaseInfo) == 72, "BoneBaseInfo: раскладка на диске 72 байта");
static_assert(sizeof(BoneDummyInfo) == 72, "BoneDummyInfo: раскладка на диске 72 байта");

// Сентинел «нет родителя» в ParentId — то же значение, что LW_INVALID_INDEX.
inline constexpr std::uint32_t kNoParent = 0xFFFFFFFFu;

} // namespace Corsairs::Tools::AssetConverter
