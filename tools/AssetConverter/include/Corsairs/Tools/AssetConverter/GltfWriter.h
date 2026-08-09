#pragma once

#include "Corsairs/Tools/AssetConverter/LabParser.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

enum class GltfCoordinateProfile : std::uint8_t {
    Generic,
    SceneMap,
};

enum class GltfSkinPolicy : std::uint8_t {
    Preserve,
    StaticReferencePose,
};

enum class GltfStatus : std::uint32_t {
    OK = 0,
    EMPTY_MESH,
    WRITE_FAILED,
};

// Переводит матрицу трансформации из системы MindPower3D в glTF.
//
// Generic выполняет замену базиса P*M*P, где P переставляет Y и Z. SceneMap
// после неё применяет зеркало G=diag(1,1,-1,1): G*(P*M*P)*G.
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
void ConvertMatrixToGltf(
    const float* in,
    float* out,
    GltfCoordinateProfile profile = GltfCoordinateProfile::Generic);

// Настройки записи текстур. Если ResolvedTextures пуст, материалы пишутся без
// изображений — с именами и цветами, но без ссылок на файлы.
struct GltfTextureOptions {
    // Разрешённые пути к файлам текстур по стадиям каждого материала.
    // Внешний индекс — материал, внутренний — стадия (0..3). Пустой путь
    // означает «текстуры для этой стадии нет».
    std::vector<std::vector<std::filesystem::path>> ResolvedTextures;
    // Куда копировать текстуры. Пустой путь — не копировать, ссылаться на
    // исходное расположение относительным путём.
    std::filesystem::path CopyTo;
};

// Пишет gltfPath и парный .bin рядом (то же имя, расширение .bin).
// detail заполняется человекочитаемой причиной при неуспехе.
//
// Преобразование системы координат: MindPower3D держит высоту по Z и
// левосторонняя, glTF — правосторонняя с высотой по Y, поэтому оси Y и Z
// меняются местами, а порядок обхода треугольника — на противоположный. Обе
// операции обязательны вместе.
//
// `skeleton` — скелет из `.lab`, к которому привязан меш. Когда он передан и
// меш несёт скиннинг, в файл попадает полная иерархия костей с настоящими
// именами и обратными bind-матрицами, а `JOINTS_0` ссылается на позиции в ней.
// Без скелета суставы выходят плоским списком тех костей, которыми меш
// пользуется, — импортёр строит по нему отдельный скелет, и дорожки анимации
// из `.lab` к такому мешу не применяются.
[[nodiscard]] GltfStatus WriteGltf(const LgoGeomObj& obj,
                                   const std::filesystem::path& gltfPath,
                                   std::string& detail,
                                   const GltfTextureOptions& textures = {},
                                   const LabAnimation* skeleton = nullptr,
                                   GltfCoordinateProfile profile =
                                       GltfCoordinateProfile::Generic,
                                   GltfSkinPolicy skinPolicy =
                                       GltfSkinPolicy::Preserve);

} // namespace Corsairs::Tools::AssetConverter
