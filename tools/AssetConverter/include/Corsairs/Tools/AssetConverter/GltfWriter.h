#pragma once

#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace Corsairs::Tools::AssetConverter {

enum class GltfStatus : std::uint32_t {
    OK = 0,
    EMPTY_MESH,
    WRITE_FAILED,
};

// Пишет gltfPath и парный .bin рядом (то же имя, расширение .bin).
// detail заполняется человекочитаемой причиной при неуспехе.
//
// Преобразование системы координат: MindPower3D левосторонняя, glTF —
// правосторонняя, поэтому Z инвертируется, а порядок обхода треугольника
// меняется на противоположный. Обе операции обязательны вместе.
[[nodiscard]] GltfStatus WriteGltf(const LgoGeomObj& obj,
                                   const std::filesystem::path& gltfPath,
                                   std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
