#pragma once

#include "Corsairs/Tools/AssetConverter/LgoTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

// Причина отказа ParseLgo. OK — успех; OK_WITH_TRAILING_DATA — данные валидны,
// но в файле остался хвост за границей объявленных блоков (движок его тоже
// игнорирует, поэтому это предупреждение, а не ошибка).
enum class LgoStatus : std::uint32_t {
    OK = 0,
    OK_WITH_TRAILING_DATA,
    VERSION_TRUNCATED,
    VERSION_UNKNOWN,
    VERSION_UNSUPPORTED,
    HEADER_TRUNCATED,
    BLOCK_SIZES_INCONSISTENT,
    MTL_BLOCK_MALFORMED,
    MESH_BLOCK_MALFORMED,
    HELPER_BLOCK_MALFORMED,
    HELPER_SECTION_UNSUPPORTED,
};

[[nodiscard]] std::string_view ToString(LgoStatus status);

struct LgoDiagnostics {
    LgoStatus Status{LgoStatus::OK};
    std::string Detail;
    std::uint32_t Version{0};
};

// Материал в представлении, не зависящем от версии файла на диске. Раскладки
// 0x1004+ и легаси 0x0000 различаются набором полей, поэтому обе приводятся
// сюда парсером.
struct LgoMaterial {
    float Opacity{1.0f};
    std::uint32_t TranspType{0};
    Material Mtl{};
    // Имя файла текстуры по стадиям, как записано в файле, без нормализации
    // расширения: модели ссылаются на .BMP, тогда как на диске лежат .png.
    std::string Textures[kMaxTextureStageNum];

    // Пустая строка — стадия не задана или индекс вне диапазона.
    [[nodiscard]] std::string TextureName(std::size_t stage) const;
};

struct LgoMesh {
    MeshInfoHeader Header{};
    std::vector<Vector3> Positions;
    std::vector<Vector3> Normals;
    std::vector<Vector2> Texcoords[kMaxTextureStageNum];
    std::vector<std::uint32_t> VertexColors;
    std::vector<BlendInfo> Blends;
    std::vector<std::uint32_t> BoneIndices;
    std::vector<std::uint32_t> Indices;
    std::vector<SubsetInfo> Subsets;
    std::vector<VertexElement> VertexElements;
};

// Helper-меш: геометрия столкновений или зоны, отдельная от видимой модели.
struct LgoHelperMesh {
    HelperMeshHeader Header{};
    std::vector<Vector3> Vertices;
    std::vector<HelperMeshFaceInfo> Faces;
};

// Helper-данные объекта. Dummy — точки крепления оружия и эффектов; bounding
// box/sphere — объёмы для отсечения и попаданий; Meshes — геометрия зон.
struct LgoHelper {
    std::uint32_t Type{0};
    std::vector<HelperDummyInfo> Dummies;
    std::vector<HelperBoxInfo> Boxes;
    std::vector<LgoHelperMesh> Meshes;
    std::vector<BoundingBoxInfo> BoundingBoxes;
    std::vector<BoundingSphereInfo> BoundingSpheres;
};

struct LgoGeomObj {
    std::uint32_t Version{0};
    GeomObjHeader Header{};
    std::vector<LgoMaterial> Materials;
    LgoMesh Mesh;
    LgoHelper Helper;

    // Положение объекта в пространстве модели. Для `.lgo`, где объект один,
    // совпадает с `Header.MatLocal`; для `.lmo` в неё свёрнута цепочка
    // родителей по `ParentId`. Движок делает то же самое: `lwNodeObject`
    // держит `_mat_local` из файла и вычисляет
    // `_mat_world = _mat_local * mat_parent`, а без родителя просто
    // `_mat_world = _mat_local`.
    //
    // Поле отдельное, а не переписанный `Header.MatLocal`: заголовок должен
    // оставаться тем, что лежит на диске, иначе разбор перестаёт быть
    // проверяемым по исходным байтам.
    float MatModel[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// Разбирает .lgo целиком. std::nullopt — файл непригоден; причина в diag.
// Блок anim пропускается по объявленному размеру: он не нужен для статической
// геометрии, но его размер участвует в проверке целостности.
[[nodiscard]] std::optional<LgoGeomObj> ParseLgo(std::span<const std::uint8_t> bytes,
                                                 LgoDiagnostics& diag);

// Реализована ли раскладка блоков для этой версии.
[[nodiscard]] bool IsSupportedGeomVersion(std::uint32_t version);

// Разбирает тело геометрического объекта — заголовок и четыре блока — из
// текущей позиции reader'а. Версия передаётся снаружи: в `.lgo` она лежит в
// начале файла, в `.lmo` относится ко всем объектам сразу.
//
// `availableBytes` — сколько байт отведено объекту: для `.lgo` это размер
// файла минус префикс версии, для `.lmo` — поле `Size` записи оглавления.
// Именно против него проверяется согласованность размеров блоков.
[[nodiscard]] bool ParseGeomObjBody(class BinaryReader& reader, std::uint32_t version,
                                    std::size_t availableBytes, LgoGeomObj& obj,
                                    LgoDiagnostics& diag);

// Разбирает helper-блок из текущей позиции reader'а и проверяет, что потрачено
// ровно helperSize байт. Используется и внутри геометрического объекта, и как
// отдельная запись оглавления `.lmo`.
[[nodiscard]] bool ParseHelperBlock(class BinaryReader& reader, std::uint32_t helperSize,
                                    std::uint32_t version, LgoHelper& helper,
                                    LgoDiagnostics& diag);

} // namespace Corsairs::Tools::AssetConverter
