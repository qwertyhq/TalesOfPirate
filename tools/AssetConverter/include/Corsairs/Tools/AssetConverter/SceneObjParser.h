#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

enum class SceneObjStatus : std::uint32_t {
    OK = 0,
    HEADER_TRUNCATED,
    BAD_MAGIC,
    VERSION_UNSUPPORTED,
    SECTION_TABLE_TRUNCATED,
    BODY_TRUNCATED,
    INTEGER_OVERFLOW,
};

[[nodiscard]] std::string_view ToString(SceneObjStatus status);

struct SceneObjDiagnostics {
    SceneObjStatus Status{SceneObjStatus::OK};
    std::string Detail;
    std::int32_t Version{0};
};

inline constexpr std::int32_t kObjVersionCurrent = 600;
inline constexpr const char* kObjMagic = "HF Object File!";

#pragma pack(push, 1)

struct SceneFileHeader {
    char Title[16];
    std::int32_t Version;
    std::int32_t FileSize;
    std::int32_t SectionCntX;
    std::int32_t SectionCntY;
    std::int32_t SectionWidth;
    std::int32_t SectionHeight;
    std::int32_t SectionObjNum;
};

struct SectionIndex {
    std::int32_t ObjInfoPos;
    std::int32_t ObjNum;
};

#pragma pack(pop)

// Запись объекта на карте. Раскладка на диске выровнена естественным образом
// (20 байт с паддингом), поэтому pack здесь НЕ применяется — иначе структура
// сожмётся до 16 байт и разбор поедет.
struct SceneObjInfo {
    std::int16_t TypeId;
    std::int32_t X;
    std::int32_t Y;
    std::int16_t HeightOff;
    std::int16_t YawAngle;
    std::int16_t Scale;

    // Старшие два бита TypeId — тип объекта, остальное — идентификатор модели.
    [[nodiscard]] std::int16_t Type() const {
        return static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(TypeId) >> 14u) & 0x0003u);
    }

    [[nodiscard]] std::int16_t Id() const {
        return static_cast<std::int16_t>(TypeId & 0x3FFF);
    }
};

static_assert(sizeof(SceneFileHeader) == 44, "SceneFileHeader: раскладка на диске 44 байта");
static_assert(sizeof(SectionIndex) == 8, "SectionIndex: раскладка на диске 8 байт");
static_assert(sizeof(SceneObjInfo) == 20, "SceneObjInfo: раскладка на диске 20 байт");

// Множитель мировых координат: клиент разворачивает относительные координаты
// объекта в мировые как `sectionIndex * sectionSize * 100`. Источник —
// CSceneObjFile::ReadSectionObjInfo в sources/Client/src/Scene/Nodes/Object/.
inline constexpr std::int32_t kWorldUnitsPerTile = 100;

struct SceneSourceKey {
    std::uint32_t SectionIndex{0};
    std::uint32_t SlotIndex{0};
    std::uint64_t ByteOffset{0};

    auto operator<=>(const SceneSourceKey&) const = default;
};

// Размещённый объект вместе с координатами секции, в которой он найден.
//
// ВАЖНО: `Info.X` / `Info.Y` на диске — координаты ОТНОСИТЕЛЬНО начала своей
// секции. Мировые координаты дают `WorldX()` / `WorldY()`. Запись сырых
// значений собрала бы всю карту в кучу у начала координат.
struct PlacedObject {
    SceneObjInfo Info{};
    std::uint32_t SectionX{0};
    std::uint32_t SectionY{0};
    std::uint32_t SectionWidth{0};
    std::uint32_t SectionHeight{0};
    SceneSourceKey Source{};

    [[nodiscard]] std::optional<std::int32_t> TryWorldX() const;
    [[nodiscard]] std::optional<std::int32_t> TryWorldY() const;
    [[nodiscard]] std::int32_t WorldX() const;
    [[nodiscard]] std::int32_t WorldY() const;
};

struct SceneObjects {
    SceneFileHeader Header{};
    std::vector<PlacedObject> Objects;
    std::size_t NonEmptySections{0};
};

[[nodiscard]] std::optional<SceneObjects> ParseSceneObj(std::span<const std::uint8_t> bytes,
                                                        SceneObjDiagnostics& diag);

} // namespace Corsairs::Tools::AssetConverter
