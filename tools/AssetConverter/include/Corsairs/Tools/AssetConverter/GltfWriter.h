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

// Переводит матрицу трансформации из левосторонней системы MindPower3D в
// правостороннюю систему glTF.
//
// Выполняется ровно одна операция — смена системы координат: M' = S*M*S, где
// S = diag(1,1,-1,1). Отрицаются элементы, у которых ровно один индекс равен 2,
// то есть смешивающие Z с X/Y, и Z-перенос. Диагональный m22 не меняется.
//
// Транспонирование НЕ выполняется, и это не упущение. DirectX использует
// строки-векторы (p' = p*M) и хранит матрицу по строкам; glTF использует
// столбцы-векторы (p' = M*p) и хранит по столбцам. Матрица действительно
// транспонируется (M_gl = M_dx^T), но элемент M_gl[i][j] = M_dx[j][i] лежит в
// column-major glTF по индексу j*4+i — там же, где он лежит в row-major
// DirectX. Обе перестановки взаимно уничтожаются, плоские массивы совпадают.
// Добавить транспонирование — значит сместить перенос с индексов 12,13,14 на
// 3,7,11 и получить сломанную трансформацию.
//
// `in` и `out` — по 16 float, перекрываться не должны.
void ConvertMatrixToGltf(const float* in, float* out);

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
