#include "Corsairs/Tools/AssetConverter/MapParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <format>

namespace Corsairs::Tools::AssetConverter {

std::string_view ToString(MapStatus status) {
    switch (status) {
    case MapStatus::OK:                      return "OK";
    case MapStatus::HEADER_TRUNCATED:        return "HEADER_TRUNCATED";
    case MapStatus::BAD_MAGIC:               return "BAD_MAGIC";
    case MapStatus::INCONSISTENT_DIMENSIONS: return "INCONSISTENT_DIMENSIONS";
    case MapStatus::OFFSET_TABLE_TRUNCATED:  return "OFFSET_TABLE_TRUNCATED";
    case MapStatus::BODY_TRUNCATED:          return "BODY_TRUNCATED";
    }
    return "UNKNOWN";
}

std::optional<MapTerrain> ParseMap(std::span<const std::uint8_t> bytes,
                                   MapDiagnostics& diag) {
    diag = {};
    BinaryReader reader{bytes};

    MapTerrain terrain;

    if (!reader.Read(terrain.Header)) {
        diag.Status = MapStatus::HEADER_TRUNCATED;
        diag.Detail = std::format("файл {} байт, нужно минимум {}",
                                  bytes.size(), sizeof(MapFileHeader));
        return std::nullopt;
    }
    diag.MapFlag = terrain.Header.MapFlag;

    if (terrain.Header.MapFlag != kMapFlagCurrent &&
        terrain.Header.MapFlag != kMapFlagLegacy) {
        diag.Status = MapStatus::BAD_MAGIC;
        diag.Detail = std::format("MapFlag={} (ожидалось {} или {})",
                                  terrain.Header.MapFlag, kMapFlagCurrent, kMapFlagLegacy);
        return std::nullopt;
    }

    const std::int32_t width = terrain.Header.Width;
    const std::int32_t height = terrain.Header.Height;
    const std::int32_t sectionWidth = terrain.Header.SectionWidth;
    const std::int32_t sectionHeight = terrain.Header.SectionHeight;

    // Кратность НЕ требуется: движок делит нацело с усечением, поэтому у карт
    // вроде room.map (52x52 при секции 8x8) сетка покрывает 48x48 тайлов, а
    // остаток игнорируется. Повторяем это поведение — иначе такие карты
    // отвергаются, хотя игра их загружает.
    if (width <= 0 || height <= 0 || sectionWidth <= 0 || sectionHeight <= 0 ||
        sectionWidth > width || sectionHeight > height) {
        diag.Status = MapStatus::INCONSISTENT_DIMENSIONS;
        diag.Detail = std::format(
            "width={}, height={}, sectionWidth={}, sectionHeight={}",
            width, height, sectionWidth, sectionHeight);
        return std::nullopt;
    }

    const std::size_t sectionsX = static_cast<std::size_t>(width / sectionWidth);
    const std::size_t sectionsY = static_cast<std::size_t>(height / sectionHeight);
    // Сетка тайлов покрывает только целые секции.
    const std::size_t gridWidth = sectionsX * static_cast<std::size_t>(sectionWidth);
    const std::size_t gridHeight = sectionsY * static_cast<std::size_t>(sectionHeight);
    const std::size_t sectionCount = sectionsX * sectionsY;
    terrain.TotalSections = sectionCount;

    std::vector<std::uint32_t> offsets(sectionCount);
    if (sectionCount > 0 && !reader.ReadArray(offsets.data(), sectionCount)) {
        diag.Status = MapStatus::OFFSET_TABLE_TRUNCATED;
        diag.Detail = std::format("не прочитана таблица из {} смещений", sectionCount);
        return std::nullopt;
    }

    const std::size_t prefixBytes = sizeof(MapFileHeader) + sectionCount * sizeof(std::uint32_t);
    const std::size_t tilesPerSection =
        static_cast<std::size_t>(sectionWidth) * static_cast<std::size_t>(sectionHeight);
    const std::size_t sectionBytes = tilesPerSection * sizeof(MapTile);

    terrain.GridWidth = gridWidth;
    terrain.GridHeight = gridHeight;
    terrain.Tiles.assign(gridWidth * gridHeight, MapTile{});

    for (std::size_t s = 0; s < sectionCount; ++s) {
        // Смещение 0 означает «секция пустая»: в теле для неё ничего нет.
        if (offsets[s] == 0) {
            continue;
        }

        // Смещения в таблице абсолютные, от начала файла: у первой непустой
        // секции значение равно размеру префикса (заголовок + таблица).
        const std::size_t absolute = offsets[s];
        if (absolute < prefixBytes || absolute + sectionBytes > bytes.size()) {
            diag.Status = MapStatus::BODY_TRUNCATED;
            diag.Detail = std::format(
                "секция {}: смещение {} + {} байт выходит за пределы файла ({} байт)",
                s, absolute, sectionBytes, bytes.size());
            return std::nullopt;
        }

        std::vector<MapTile> section(tilesPerSection);
        BinaryReader sectionReader{bytes.subspan(absolute, sectionBytes)};
        if (!sectionReader.ReadArray(section.data(), tilesPerSection)) {
            diag.Status = MapStatus::BODY_TRUNCATED;
            diag.Detail = std::format("секция {}: не прочитаны тайлы", s);
            return std::nullopt;
        }

        // Раскладываем секцию в общую сетку: секции идут построчно, внутри
        // секции тайлы тоже построчно.
        const std::size_t sectionX = s % sectionsX;
        const std::size_t sectionY = s / sectionsX;
        for (std::size_t ty = 0; ty < static_cast<std::size_t>(sectionHeight); ++ty) {
            for (std::size_t tx = 0; tx < static_cast<std::size_t>(sectionWidth); ++tx) {
                const std::size_t globalX = sectionX * static_cast<std::size_t>(sectionWidth) + tx;
                const std::size_t globalY = sectionY * static_cast<std::size_t>(sectionHeight) + ty;
                terrain.Tiles[globalY * gridWidth + globalX] =
                    section[ty * static_cast<std::size_t>(sectionWidth) + tx];
            }
        }

        ++terrain.PresentSections;
    }

    diag.Status = MapStatus::OK;
    return terrain;
}

} // namespace Corsairs::Tools::AssetConverter
