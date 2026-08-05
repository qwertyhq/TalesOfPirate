#pragma once

#include <cstddef>
#include <cstdint>

namespace Corsairs::Tools::AssetConverter {

inline constexpr std::size_t kMaxName = 64;
inline constexpr std::size_t kMaxTextureStageNum = 4;
inline constexpr std::size_t kTexTssNum = 8;
inline constexpr std::size_t kMtlRsNum = 8;
inline constexpr std::size_t kMeshRsNum = 8;
inline constexpr std::size_t kObjectStateNum = 8;

// Флаги FVF DirectX 9. Значения фиксированы спецификацией D3D9 и продублированы
// здесь, чтобы не тянуть Windows SDK.
enum class FvfFlag : std::uint32_t {
    NORMAL  = 0x0010,
    DIFFUSE = 0x0040,
    TEX1    = 0x0100,
    TEX2    = 0x0200,
    TEX3    = 0x0300,
    TEX4    = 0x0400,
};

[[nodiscard]] inline bool HasFvf(std::uint32_t fvf, FvfFlag flag) {
    const std::uint32_t bits = static_cast<std::uint32_t>(flag);
    return (fvf & bits) == bits;
}

// Версии .lgo, встречающиеся в датасете. Источник — LgoLoader::IsKnownVersion
// в sources/Engine/Asset/AssetLoaders.cpp.
[[nodiscard]] inline bool IsKnownVersion(std::uint32_t version) {
    return version == 0x0000u || (version >= 0x1000u && version <= 0x1005u);
}

// Минимальная версия, чья раскладка блока геометрии реализована в этом плане.
inline constexpr std::uint32_t kMinSupportedVersion = 0x1004u;

#pragma pack(push, 1)

struct Vector2 {
    float X;
    float Y;
};

struct Vector3 {
    float X;
    float Y;
    float Z;
};

struct ColorValue4f {
    float R;
    float G;
    float B;
    float A;
};

struct Material {
    ColorValue4f Dif;
    ColorValue4f Amb;
    ColorValue4f Spe;
    ColorValue4f Emi;
    float Power;
};

struct RenderStateAtom {
    std::uint32_t State;
    std::uint32_t Value0;
    std::uint32_t Value1;
};

struct TexInfo {
    std::uint32_t Stage;
    std::uint32_t Level;
    std::uint32_t Usage;
    std::uint32_t Format;
    std::uint32_t Pool;
    std::uint32_t ByteAlignmentFlag;
    std::uint32_t Type;
    std::uint32_t Width;
    std::uint32_t Height;
    std::uint32_t ColorkeyType;
    std::uint32_t Colorkey;
    char FileName[kMaxName];
    // В движке здесь был `void* data`, менявший размер между x86 и x64 и ломавший
    // разбор файлов. Заменён на 4-байтный плейсхолдер, фиксирующий формат.
    std::uint32_t ReservedData;
    RenderStateAtom TssSet[kTexTssNum];
};

struct MtlTexInfo {
    float Opacity;
    std::uint32_t TranspType;
    Material Mtl;
    RenderStateAtom RsSet[kMtlRsNum];
    TexInfo TexSeq[kMaxTextureStageNum];
};

struct MeshInfoHeader {
    std::uint32_t Fvf;
    std::uint32_t PtType;
    std::uint32_t VertexNum;
    std::uint32_t IndexNum;
    std::uint32_t SubsetNum;
    std::uint32_t BoneIndexNum;
    std::uint32_t BoneInflFactor;
    std::uint32_t VertexElementNum;
    RenderStateAtom RsSet[kMeshRsNum];
};

struct SubsetInfo {
    std::uint32_t PrimitiveNum;
    std::uint32_t StartIndex;
    std::uint32_t VertexNum;
    std::uint32_t MinIndex;
};

struct BlendInfo {
    std::uint8_t Index[4];
    float Weight[4];
};

struct RenderCtrlCreateInfo {
    std::uint32_t CtrlId;
    std::uint32_t DeclId;
    std::uint32_t VsId;
    std::uint32_t PsId;
};

struct GeomObjHeader {
    std::uint32_t Id;
    std::uint32_t ParentId;
    std::uint32_t Type;
    float MatLocal[16];
    RenderCtrlCreateInfo Rcci;
    std::uint8_t StateCtrl[kObjectStateNum];
    std::uint32_t MtlSize;
    std::uint32_t MeshSize;
    std::uint32_t HelperSize;
    std::uint32_t AnimSize;
};

// Элемент вершинной декларации D3D9 (D3DVERTEXELEMENT9) — 8 байт.
struct VertexElement {
    std::uint16_t Stream;
    std::uint16_t Offset;
    std::uint8_t Type;
    std::uint8_t Method;
    std::uint8_t Usage;
    std::uint8_t UsageIndex;
};

#pragma pack(pop)

inline constexpr std::size_t kGeomObjHeaderSize = sizeof(GeomObjHeader);
inline constexpr std::size_t kMtlTexInfoSize = sizeof(MtlTexInfo);
inline constexpr std::size_t kMeshInfoHeaderSize = sizeof(MeshInfoHeader);

// Раскладка на диске зафиксирована файлами, записанными десятилетия назад.
// Любое расхождение — ошибка компиляции, а не тихо испорченные данные.
static_assert(sizeof(ColorValue4f) == 16, "ColorValue4f: раскладка на диске 16 байт");
static_assert(sizeof(Material) == 68, "Material: раскладка на диске 68 байт");
static_assert(sizeof(RenderStateAtom) == 12, "RenderStateAtom: раскладка на диске 12 байт");
static_assert(sizeof(TexInfo) == 208, "TexInfo: раскладка на диске 208 байт");
static_assert(sizeof(MtlTexInfo) == 1004, "MtlTexInfo: раскладка на диске 1004 байта");
static_assert(sizeof(MeshInfoHeader) == 128, "MeshInfoHeader: раскладка на диске 128 байт");
static_assert(sizeof(SubsetInfo) == 16, "SubsetInfo: раскладка на диске 16 байт");
static_assert(sizeof(BlendInfo) == 20, "BlendInfo: раскладка на диске 20 байт");
static_assert(sizeof(GeomObjHeader) == 116, "GeomObjHeader: раскладка на диске 116 байт");
static_assert(sizeof(VertexElement) == 8, "VertexElement: раскладка на диске 8 байт");
static_assert(sizeof(Vector2) == 8, "Vector2: раскладка на диске 8 байт");
static_assert(sizeof(Vector3) == 12, "Vector3: раскладка на диске 12 байт");

} // namespace Corsairs::Tools::AssetConverter
