#pragma once

#include <cstdint>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

#pragma pack(push, 1)

// Один тайл террейна, ровно 15 байт.
//   TileInfo — три верхних слоя текстур, по 10 бит (6 текстура + 4 альфа);
//   BaseTex  — базовый слой, всегда непрозрачный;
//   Color    — RGB565 тонировки;
//   Height   — высота в единицах по 10 см;
//   Region   — идентификатор региона;
//   Island   — индекс плавающей платформы;
//   Block    — флаги проходимости по четвертям тайла.
struct MapTile {
    std::uint32_t TileInfo;
    std::uint8_t BaseTex;
    std::int16_t Color;
    std::int8_t Height;
    std::int16_t Region;
    std::uint8_t Island;
    std::uint8_t Block[4];
};

#pragma pack(pop)

static_assert(sizeof(MapTile) == 15, "MapTile: раскладка на диске 15 байт");

struct MapCellRect {
    std::uint32_t X{0};
    std::uint32_t Y{0};
    std::uint32_t Width{0};
    std::uint32_t Height{0};
};

struct TerrainPageId {
    std::uint32_t X{0};
    std::uint32_t Y{0};
};

struct MapPageTiles {
    MapCellRect Cells;
    std::uint32_t StoredWidth{0};
    std::uint32_t StoredHeight{0};
    std::vector<MapTile> Tiles;
    std::vector<std::uint8_t> TilePresent;
    std::vector<std::uint8_t> SectionPresent;
};

} // namespace Corsairs::Tools::AssetConverter
