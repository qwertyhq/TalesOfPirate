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

struct LgoGeomObj {
    std::uint32_t Version{0};
    GeomObjHeader Header{};
    std::vector<LgoMaterial> Materials;
    LgoMesh Mesh;
};

// Разбирает .lgo целиком. std::nullopt — файл непригоден; причина в diag.
// Блоки helper и anim пропускаются по объявленному размеру: они не нужны для
// статической геометрии, но их размеры участвуют в проверке целостности.
[[nodiscard]] std::optional<LgoGeomObj> ParseLgo(std::span<const std::uint8_t> bytes,
                                                 LgoDiagnostics& diag);

} // namespace Corsairs::Tools::AssetConverter
