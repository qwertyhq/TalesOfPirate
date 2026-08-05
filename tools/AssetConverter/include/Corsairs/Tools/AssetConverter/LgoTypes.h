#pragma once

#include <cstddef>
#include <cstdint>

namespace Corsairs::Tools::AssetConverter {

inline constexpr std::size_t kMaxName = 64;
inline constexpr std::size_t kCharName32 = 32;
inline constexpr std::size_t kMaxTextureStageNum = 4;
inline constexpr std::size_t kTexTssNum = 8;
inline constexpr std::size_t kMtlRsNum = 8;
inline constexpr std::size_t kMeshRsNum = 8;
inline constexpr std::size_t kObjectStateNum = 8;

// Флаги FVF DirectX 9. Значения фиксированы спецификацией D3D9 и продублированы
// здесь, чтобы не тянуть Windows SDK.
enum class FvfFlag : std::uint32_t {
    NORMAL           = 0x0010,
    DIFFUSE          = 0x0040,
    TEX1             = 0x0100,
    TEX2             = 0x0200,
    TEX3             = 0x0300,
    TEX4             = 0x0400,
    // В легаси-формате (version 0x0000) наличие скиннинга определяется этим
    // флагом, а не полем BoneIndexNum, как в 0x1004+.
    LASTBETA_UBYTE4  = 0x1000,
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

// Минимальная версия современной раскладки блоков.
inline constexpr std::uint32_t kMinSupportedVersion = 0x1004u;

// Легаси-раскладка: вложенная версия в начале каждого блока, другой порядок
// массивов геометрии, однобайтовые индексы костей.
inline constexpr std::uint32_t kLegacyVersion = 0x0000u;

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

// --- Helper-блок -------------------------------------------------------------
// Секции присутствуют в блоке по битам поля Type и всегда в этом порядке:
// DUMMY, BOX, MESH, BOUNDINGBOX, BOUNDINGSPHERE. Каждая начинается со своего
// счётчика. Dummy — точки крепления оружия и эффектов к модели.

enum class HelperType : std::uint32_t {
    DUMMY           = 0x0001,
    BOX             = 0x0002,
    MESH            = 0x0004,
    BOUNDING_BOX    = 0x0010,
    BOUNDING_SPHERE = 0x0020,
};

[[nodiscard]] inline bool HasHelper(std::uint32_t type, HelperType flag) {
    const std::uint32_t bits = static_cast<std::uint32_t>(flag);
    return (type & bits) != 0;
}

struct Box {
    Vector3 Center;
    Vector3 Radius;
};

struct Sphere {
    Vector3 Center;
    float Radius;
};

// Точка крепления. `Mat` — мировая матрица относительно объекта, `MatLocal` —
// локальная; ParentType: 0 обычный, 1 родитель-кость, 2 родитель-dummy кости.
struct HelperDummyInfo {
    std::uint32_t Id;
    float Mat[16];
    float MatLocal[16];
    std::uint32_t ParentType;
    std::uint32_t ParentId;
};

// Раскладка dummy в версиях <= 0x1000: только Id и матрица.
struct HelperDummyInfoV1000 {
    std::uint32_t Id;
    float Mat[16];
};

struct BoundingBoxInfo {
    std::uint32_t Id;
    Box BoundBox;
    float Mat[16];
};

// Плоскость (a, b, c, d).
struct Plane {
    float A;
    float B;
    float C;
    float D;
};

// Грань helper-меша: индексы вершин, индексы смежных граней, плоскость, центр.
struct HelperMeshFaceInfo {
    std::uint32_t Vertex[3];
    std::uint32_t AdjFace[3];
    Plane FacePlane;
    Vector3 Center;
};

// Заголовок helper-меша. Порядок полей — порядок чтения в
// LgoLoader::LoadHelperMeshSection, а не порядок объявления HelperMeshInfo
// в движке: они различаются, и на диске лежит именно этот порядок.
struct HelperMeshHeader {
    std::uint32_t Id;
    std::uint32_t Type;
    std::uint32_t SubType;
    char Name[kCharName32];
    std::uint32_t State;
    float Mat[16];
    Box BoundBox;
    std::uint32_t VertexNum;
    std::uint32_t FaceNum;
};

// Helper-бокс: объём с матрицей и именем.
struct HelperBoxInfo {
    std::uint32_t Id;
    std::uint32_t Type;
    std::uint32_t State;
    Box BoundBox;
    float Mat[16];
    char Name[kCharName32];
};

struct BoundingSphereInfo {
    std::uint32_t Id;
    Sphere BoundSphere;
    float Mat[16];
};

// --- Легаси-формат version = 0x0000 -----------------------------------------
// Ранняя раскладка, встречающаяся у 91 файла в датасете. Отличается от 0x1004+
// набором полей и порядком массивов. Источник — lwExpObj.h (lwTexInfo_0000,
// lwMtlTexInfo_0000, lwMeshInfo_0000) и ветки version==*_VERSION0000 в
// LgoLoader::LoadMtlTexInfoSingle / LoadMeshInfo.

// Пара «состояние-значение» в старых render-state наборах.
struct RenderStateValue {
    std::uint32_t State;
    std::uint32_t Value;
};

// lwRenderStateSetTemplate<2, 8> — двумерный массив 2x8 пар.
struct RenderStateSet2x8 {
    RenderStateValue Rsv[2][8];
};

struct TexInfoV0 {
    std::uint32_t Stage;
    std::uint32_t ColorkeyType;
    std::uint32_t Colorkey;
    std::uint32_t Format;
    char FileName[kMaxName];
    RenderStateSet2x8 TssSet;
};

struct MtlTexInfoV0 {
    Material Mtl;
    RenderStateSet2x8 RsSet;
    TexInfoV0 TexSeq[kMaxTextureStageNum];
};

struct MeshInfoHeaderV0 {
    std::uint32_t Fvf;
    std::uint32_t PtType;
    std::uint32_t VertexNum;
    std::uint32_t IndexNum;
    std::uint32_t SubsetNum;
    std::uint32_t BoneIndexNum;
    RenderStateSet2x8 RsSet;
};

// Заголовок геометрии промежуточных версий (вложенная 0x0001 и внешние
// 0x1000..0x1003): те же поля, но render states уже в компактном формате
// RenderStateAtom, и ещё нет BoneInflFactor / VertexElementNum.
struct MeshInfoHeaderV3 {
    std::uint32_t Fvf;
    std::uint32_t PtType;
    std::uint32_t VertexNum;
    std::uint32_t IndexNum;
    std::uint32_t SubsetNum;
    std::uint32_t BoneIndexNum;
    RenderStateAtom RsSet[kMeshRsNum];
};

// Текстура промежуточной версии материалов (вложенная 0x0001).
struct TexInfoV1 {
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
    std::uint32_t ReservedData;
    RenderStateSet2x8 TssSet;
};

struct MtlTexInfoV1 {
    float Opacity;
    std::uint32_t TranspType;
    Material Mtl;
    RenderStateSet2x8 RsSet;
    TexInfoV1 TexSeq[kMaxTextureStageNum];
};

#pragma pack(pop)

inline constexpr std::size_t kGeomObjHeaderSize = sizeof(GeomObjHeader);
inline constexpr std::size_t kMtlTexInfoSize = sizeof(MtlTexInfo);
inline constexpr std::size_t kMeshInfoHeaderSize = sizeof(MeshInfoHeader);
inline constexpr std::size_t kMtlTexInfoV0Size = sizeof(MtlTexInfoV0);
inline constexpr std::size_t kMeshInfoHeaderV0Size = sizeof(MeshInfoHeaderV0);

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

// Легаси-раскладка. Размеры выведены из lwExpObj.h и проверены на реальных
// файлах: у character/2000000003.lgo MtlSize=1036 = 4 (вложенная версия)
// + 4 (MtlNum) + 1028, а MeshSize=476 сходится с MeshInfoHeaderV0 = 152.
static_assert(sizeof(RenderStateValue) == 8, "RenderStateValue: 8 байт");
static_assert(sizeof(RenderStateSet2x8) == 128, "RenderStateSet2x8: 2*8*8 = 128 байт");
static_assert(sizeof(TexInfoV0) == 208, "TexInfoV0: раскладка на диске 208 байт");
static_assert(sizeof(MtlTexInfoV0) == 1028, "MtlTexInfoV0: раскладка на диске 1028 байт");
static_assert(sizeof(MeshInfoHeaderV0) == 152, "MeshInfoHeaderV0: раскладка на диске 152 байта");
static_assert(sizeof(MeshInfoHeaderV3) == 120, "MeshInfoHeaderV3: раскладка на диске 120 байт");
static_assert(sizeof(TexInfoV1) == 240, "TexInfoV1: раскладка на диске 240 байт");
static_assert(sizeof(MtlTexInfoV1) == 1164, "MtlTexInfoV1: раскладка на диске 1164 байта");

// Helper-блок. Размеры сверены с фактическими helperSize в датасете: у
// character/0066000000.lgo helperSize=92 = 4 (type) + 4 (num) + 84 (сфера);
// у character/04090084.lgo helperSize=804 = 4 + 4 + 140*5 + 4 + 92*1.
static_assert(sizeof(Box) == 24, "Box: раскладка на диске 24 байта");
static_assert(sizeof(Sphere) == 16, "Sphere: раскладка на диске 16 байт");
static_assert(sizeof(HelperDummyInfo) == 140, "HelperDummyInfo: раскладка на диске 140 байт");
static_assert(sizeof(HelperDummyInfoV1000) == 68, "HelperDummyInfoV1000: раскладка на диске 68 байт");
static_assert(sizeof(BoundingBoxInfo) == 92, "BoundingBoxInfo: раскладка на диске 92 байта");
static_assert(sizeof(BoundingSphereInfo) == 84, "BoundingSphereInfo: раскладка на диске 84 байта");
static_assert(sizeof(Plane) == 16, "Plane: раскладка на диске 16 байт");
static_assert(sizeof(HelperMeshFaceInfo) == 52, "HelperMeshFaceInfo: раскладка на диске 52 байта");
static_assert(sizeof(HelperMeshHeader) == 144, "HelperMeshHeader: раскладка на диске 144 байта");
static_assert(sizeof(HelperBoxInfo) == 132, "HelperBoxInfo: раскладка на диске 132 байта");

} // namespace Corsairs::Tools::AssetConverter
