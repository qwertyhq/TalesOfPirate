#pragma once

#include "Corsairs/Tools/AssetConverter/TerrainPage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

enum class MapStatus : std::uint32_t {
    OK = 0,
    HEADER_TRUNCATED,
    BAD_MAGIC,
    INCONSISTENT_DIMENSIONS,
    OFFSET_TABLE_TRUNCATED,
    BODY_TRUNCATED,
};

[[nodiscard]] std::string_view ToString(MapStatus status);

struct MapDiagnostics {
    MapStatus Status{MapStatus::OK};
    std::string Detail;
    std::int32_t MapFlag{0};
};

// Магические значения версии формата. Источник — Corsairs::Util в
// sources/Libraries/Util/src/Map/MPMapDef.h.
inline constexpr std::int32_t kMapFlagLegacy = 780626;
inline constexpr std::int32_t kMapFlagCurrent = 780627;

#pragma pack(push, 1)

struct MapFileHeader {
    std::int32_t MapFlag;
    std::int32_t Width;
    std::int32_t Height;
    std::int32_t SectionWidth;
    std::int32_t SectionHeight;
};

#pragma pack(pop)

static_assert(sizeof(MapFileHeader) == 20, "MapFileHeader: раскладка на диске 20 байт");

// Разобранный террейн. Tiles покрывает всю сетку Width x Height; секции, для
// которых в файле нет данных, заполнены нулями.
struct MapTerrain {
    MapFileHeader Header{};
    std::vector<MapTile> Tiles;
    // Сколько секций реально присутствовало в файле — остальные пустые.
    std::size_t PresentSections{0};
    std::size_t TotalSections{0};
    // Размер сетки тайлов. Может быть меньше Width x Height из заголовка:
    // движок делит на размер секции нацело, остаток не покрывается секциями.
    std::size_t GridWidth{0};
    std::size_t GridHeight{0};

    [[nodiscard]] std::size_t SectionsX() const {
        return static_cast<std::size_t>(Header.Width / Header.SectionWidth);
    }

    [[nodiscard]] std::size_t SectionsY() const {
        return static_cast<std::size_t>(Header.Height / Header.SectionHeight);
    }
};

[[nodiscard]] std::optional<MapTerrain> ParseMap(std::span<const std::uint8_t> bytes,
                                                 MapDiagnostics& diag);

} // namespace Corsairs::Tools::AssetConverter
