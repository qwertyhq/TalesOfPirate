#pragma once

#include "Corsairs/Tools/AssetConverter/LabParser.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Corsairs::Tools::AssetConverter {

enum class GltfSkeletonStatus : std::uint32_t {
    OK = 0,
    EMPTY_SKELETON,
    WRITE_FAILED,
};

// Разложение матрицы трансформации на перенос, поворот и масштаб.
// Матрица ожидается уже в системе координат glTF (column-major).
struct Trs {
    float Translation[3];
    float Rotation[4];   // кватернион (x, y, z, w)
    float Scale[3];
};

// Раскладывает 4x4-матрицу в column-major раскладке glTF на TRS.
// glTF не умеет матричные каналы анимации, поэтому файлы с матричными
// ключами (MAT43/MAT44) обязаны пройти через это разложение.
[[nodiscard]] Trs DecomposeGltfMatrix(const float* matrix);

// Переводит кватернион поворота из левосторонней системы в правостороннюю.
//
// Отражение S = diag(1,1,-1) меняет ориентацию пространства, поэтому
// сопряжение S*R*S переводит поворот вокруг оси n на угол θ в поворот вокруг
// S*n на угол −θ. В компонентах это даёт (x, y, z, w) -> (−x, −y, z, w):
// отрицаются X и Y, а не Z. Наивное отрицание Z дало бы анимацию, вращающуюся
// в обратную сторону вокруг вертикальной оси.
void ConvertQuaternionToGltf(const Quaternion& in, float* out);

// Пишет скелет и одну анимационную дорожку в gltfPath плюс парный .bin.
// Меш не включается: привязка меша к скелету — отдельный этап, а обратные
// bind-матрицы сохраняются в skin, чтобы её можно было выполнить позже.
[[nodiscard]] GltfSkeletonStatus WriteSkeletonGltf(const LabAnimation& anim,
                                                   std::string_view animationName,
                                                   const std::filesystem::path& gltfPath,
                                                   std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
