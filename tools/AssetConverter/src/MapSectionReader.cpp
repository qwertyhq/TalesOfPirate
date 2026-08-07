#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"

#include <algorithm>
#include <format>
#include <limits>

namespace Corsairs::Tools::AssetConverter {

namespace {

void ResetDiagnostics(MapDiagnostics& diagnostics, std::int32_t mapFlag = 0) {
    diagnostics = {};
    diagnostics.MapFlag = mapFlag;
}

void RecordMetadataRead(MapReadStats& stats, std::size_t bytes) {
    stats.MetadataBytesRead += bytes;
    stats.LargestMetadataRead = std::max(stats.LargestMetadataRead, bytes);
}

void RecordBodyRead(MapReadStats& stats, std::size_t bytes) {
    stats.BodyBytesRead += bytes;
    stats.LargestBodyRead = std::max(stats.LargestBodyRead, bytes);
}

} // namespace

std::optional<MapSectionReader> MapSectionReader::Open(
    const std::filesystem::path& path,
    MapDiagnostics& diagnostics) {
    ResetDiagnostics(diagnostics);

    MapSectionReader reader;
    reader._input.open(path, std::ios::binary);
    if (!reader._input) {
        diagnostics.Status = MapStatus::HEADER_TRUNCATED;
        diagnostics.Detail = std::format("не удалось открыть {}", path.string());
        return std::nullopt;
    }

    reader._input.seekg(0, std::ios::end);
    const std::streampos endPosition = reader._input.tellg();
    if (endPosition < std::streampos{0}) {
        diagnostics.Status = MapStatus::HEADER_TRUNCATED;
        diagnostics.Detail = std::format("не удалось определить размер {}", path.string());
        return std::nullopt;
    }
    reader._fileBytes = static_cast<std::uint64_t>(
        static_cast<std::streamoff>(endPosition));

    if (reader._fileBytes < sizeof(MapFileHeader)) {
        diagnostics.Status = MapStatus::HEADER_TRUNCATED;
        diagnostics.Detail = std::format(
            "файл {} байт, нужно минимум {}",
            reader._fileBytes,
            sizeof(MapFileHeader));
        return std::nullopt;
    }

    reader._input.seekg(0, std::ios::beg);
    reader._input.read(
        reinterpret_cast<char*>(&reader._header),
        static_cast<std::streamsize>(sizeof(MapFileHeader)));
    if (reader._input.gcount() !=
        static_cast<std::streamsize>(sizeof(MapFileHeader))) {
        diagnostics.Status = MapStatus::HEADER_TRUNCATED;
        diagnostics.Detail = "не прочитан заголовок карты";
        return std::nullopt;
    }
    RecordMetadataRead(reader._stats, sizeof(MapFileHeader));
    diagnostics.MapFlag = reader._header.MapFlag;

    if (reader._header.MapFlag != kMapFlagCurrent &&
        reader._header.MapFlag != kMapFlagLegacy) {
        diagnostics.Status = MapStatus::BAD_MAGIC;
        diagnostics.Detail = std::format(
            "MapFlag={} (ожидалось {} или {})",
            reader._header.MapFlag,
            kMapFlagCurrent,
            kMapFlagLegacy);
        return std::nullopt;
    }

    const std::int32_t width = reader._header.Width;
    const std::int32_t height = reader._header.Height;
    const std::int32_t sectionWidth = reader._header.SectionWidth;
    const std::int32_t sectionHeight = reader._header.SectionHeight;
    if (width <= 0 || height <= 0 || sectionWidth <= 0 || sectionHeight <= 0 ||
        sectionWidth > width || sectionHeight > height) {
        diagnostics.Status = MapStatus::INCONSISTENT_DIMENSIONS;
        diagnostics.Detail = std::format(
            "width={}, height={}, sectionWidth={}, sectionHeight={}",
            width,
            height,
            sectionWidth,
            sectionHeight);
        return std::nullopt;
    }

    reader._sectionsX = static_cast<std::size_t>(width / sectionWidth);
    reader._sectionsY = static_cast<std::size_t>(height / sectionHeight);
    const std::size_t sectionCount = reader._sectionsX * reader._sectionsY;
    const std::uint64_t tableBytes =
        static_cast<std::uint64_t>(sectionCount) * sizeof(std::uint32_t);
    reader._prefixBytes = sizeof(MapFileHeader) + tableBytes;
    if (reader._fileBytes < reader._prefixBytes) {
        diagnostics.Status = MapStatus::OFFSET_TABLE_TRUNCATED;
        diagnostics.Detail = std::format(
            "не прочитана таблица из {} смещений",
            sectionCount);
        return std::nullopt;
    }

    const std::uint64_t tilesPerSection =
        static_cast<std::uint64_t>(sectionWidth) *
        static_cast<std::uint64_t>(sectionHeight);
    if (tilesPerSection >
        std::numeric_limits<std::size_t>::max() / sizeof(MapTile)) {
        diagnostics.Status = MapStatus::INCONSISTENT_DIMENSIONS;
        diagnostics.Detail = "размер секции не помещается в адресное пространство";
        return std::nullopt;
    }
    reader._tilesPerSection = static_cast<std::size_t>(tilesPerSection);
    reader._sectionBytes = reader._tilesPerSection * sizeof(MapTile);

    reader._offsets.resize(sectionCount);
    reader._input.read(
        reinterpret_cast<char*>(reader._offsets.data()),
        static_cast<std::streamsize>(tableBytes));
    if (reader._input.gcount() != static_cast<std::streamsize>(tableBytes)) {
        diagnostics.Status = MapStatus::OFFSET_TABLE_TRUNCATED;
        diagnostics.Detail = std::format(
            "не прочитана таблица из {} смещений",
            sectionCount);
        return std::nullopt;
    }
    RecordMetadataRead(reader._stats, static_cast<std::size_t>(tableBytes));

    for (std::size_t index = 0; index < reader._offsets.size(); ++index) {
        const std::uint32_t offset = reader._offsets[index];
        if (offset == 0) {
            continue;
        }

        const std::uint64_t end =
            static_cast<std::uint64_t>(offset) + reader._sectionBytes;
        if (offset < reader._prefixBytes || end > reader._fileBytes) {
            diagnostics.Status = MapStatus::BODY_TRUNCATED;
            diagnostics.Detail = std::format(
                "секция {}: смещение {} + {} байт выходит за пределы файла ({} байт)",
                index,
                offset,
                reader._sectionBytes,
                reader._fileBytes);
            return std::nullopt;
        }
    }

    diagnostics.Status = MapStatus::OK;
    return std::optional<MapSectionReader>{std::move(reader)};
}

const MapFileHeader& MapSectionReader::Header() const noexcept {
    return _header;
}

std::optional<MapSection> MapSectionReader::ReadSection(
    std::uint32_t x,
    std::uint32_t y,
    MapDiagnostics& diagnostics) {
    ResetDiagnostics(diagnostics, _header.MapFlag);
    if (x >= _sectionsX || y >= _sectionsY) {
        diagnostics.Status = MapStatus::INCONSISTENT_DIMENSIONS;
        diagnostics.Detail = std::format(
            "секция ({},{}) вне сетки {}x{}",
            x,
            y,
            _sectionsX,
            _sectionsY);
        return std::nullopt;
    }

    MapSection section;
    section.X = x;
    section.Y = y;
    const std::size_t index =
        static_cast<std::size_t>(y) * _sectionsX + x;
    const std::uint32_t offset = _offsets[index];
    if (offset == 0) {
        diagnostics.Status = MapStatus::OK;
        return section;
    }

    section.Tiles.resize(_tilesPerSection);
    _input.clear();
    _input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!_input) {
        diagnostics.Status = MapStatus::BODY_TRUNCATED;
        diagnostics.Detail = std::format(
            "секция {}: не выполнен seek к смещению {}",
            index,
            offset);
        return std::nullopt;
    }

    _input.read(
        reinterpret_cast<char*>(section.Tiles.data()),
        static_cast<std::streamsize>(_sectionBytes));
    const std::streamsize bytesRead = _input.gcount();
    if (bytesRead > 0) {
        RecordBodyRead(_stats, static_cast<std::size_t>(bytesRead));
    }
    if (bytesRead != static_cast<std::streamsize>(_sectionBytes)) {
        diagnostics.Status = MapStatus::BODY_TRUNCATED;
        diagnostics.Detail = std::format(
            "секция {}: прочитано {} из {} байт",
            index,
            bytesRead,
            _sectionBytes);
        return std::nullopt;
    }

    section.Present = true;
    _stats.PeakResidentTiles =
        std::max(_stats.PeakResidentTiles, section.Tiles.size());
    diagnostics.Status = MapStatus::OK;
    return section;
}

std::optional<MapPageTiles> MapSectionReader::ReadWindow(
    MapCellRect cells,
    std::uint32_t rightHalo,
    std::uint32_t bottomHalo,
    MapDiagnostics& diagnostics) {
    ResetDiagnostics(diagnostics, _header.MapFlag);

    const std::uint64_t storedWidth =
        static_cast<std::uint64_t>(cells.Width) + rightHalo;
    const std::uint64_t storedHeight =
        static_cast<std::uint64_t>(cells.Height) + bottomHalo;
    const std::uint64_t endX = static_cast<std::uint64_t>(cells.X) + storedWidth;
    const std::uint64_t endY = static_cast<std::uint64_t>(cells.Y) + storedHeight;
    const std::uint64_t gridWidth =
        static_cast<std::uint64_t>(_sectionsX) * _header.SectionWidth;
    const std::uint64_t gridHeight =
        static_cast<std::uint64_t>(_sectionsY) * _header.SectionHeight;
    if (cells.Width == 0 || cells.Height == 0 || storedWidth == 0 ||
        storedHeight == 0 || endX > gridWidth || endY > gridHeight) {
        diagnostics.Status = MapStatus::INCONSISTENT_DIMENSIONS;
        diagnostics.Detail = std::format(
            "окно ({},{}) {}x{} + halo {}x{} вне сетки {}x{}",
            cells.X,
            cells.Y,
            cells.Width,
            cells.Height,
            rightHalo,
            bottomHalo,
            gridWidth,
            gridHeight);
        return std::nullopt;
    }

    const std::uint64_t tileCount = storedWidth * storedHeight;
    if (tileCount > std::numeric_limits<std::size_t>::max()) {
        diagnostics.Status = MapStatus::INCONSISTENT_DIMENSIONS;
        diagnostics.Detail = "размер окна не помещается в адресное пространство";
        return std::nullopt;
    }

    MapPageTiles page;
    page.Cells = cells;
    page.StoredWidth = static_cast<std::uint32_t>(storedWidth);
    page.StoredHeight = static_cast<std::uint32_t>(storedHeight);
    page.Tiles.resize(static_cast<std::size_t>(tileCount));
    page.TilePresent.resize(static_cast<std::size_t>(tileCount));
    _stats.PeakResidentTiles =
        std::max(_stats.PeakResidentTiles, page.Tiles.size());

    const std::uint32_t sectionWidth =
        static_cast<std::uint32_t>(_header.SectionWidth);
    const std::uint32_t sectionHeight =
        static_cast<std::uint32_t>(_header.SectionHeight);
    const std::uint32_t firstSectionX = cells.X / sectionWidth;
    const std::uint32_t firstSectionY = cells.Y / sectionHeight;
    const std::uint32_t lastSectionX =
        static_cast<std::uint32_t>((endX - 1u) / sectionWidth);
    const std::uint32_t lastSectionY =
        static_cast<std::uint32_t>((endY - 1u) / sectionHeight);
    const std::size_t sectionColumns = lastSectionX - firstSectionX + 1u;
    const std::size_t sectionRows = lastSectionY - firstSectionY + 1u;
    page.SectionPresent.resize(sectionColumns * sectionRows);

    for (std::uint32_t sectionY = firstSectionY;
         sectionY <= lastSectionY;
         ++sectionY) {
        for (std::uint32_t sectionX = firstSectionX;
             sectionX <= lastSectionX;
             ++sectionX) {
            auto section = ReadSection(sectionX, sectionY, diagnostics);
            if (!section.has_value()) {
                return std::nullopt;
            }

            const std::size_t presenceIndex =
                static_cast<std::size_t>(sectionY - firstSectionY) *
                    sectionColumns +
                (sectionX - firstSectionX);
            page.SectionPresent[presenceIndex] = section->Present ? 1u : 0u;
            if (!section->Present) {
                continue;
            }

            for (std::uint32_t tileY = 0; tileY < sectionHeight; ++tileY) {
                const std::uint64_t globalY =
                    static_cast<std::uint64_t>(sectionY) * sectionHeight + tileY;
                if (globalY < cells.Y || globalY >= endY) {
                    continue;
                }
                for (std::uint32_t tileX = 0; tileX < sectionWidth; ++tileX) {
                    const std::uint64_t globalX =
                        static_cast<std::uint64_t>(sectionX) * sectionWidth + tileX;
                    if (globalX < cells.X || globalX >= endX) {
                        continue;
                    }

                    const std::size_t sourceIndex =
                        static_cast<std::size_t>(tileY) * sectionWidth + tileX;
                    const std::size_t pageIndex =
                        static_cast<std::size_t>(globalY - cells.Y) *
                            page.StoredWidth +
                        static_cast<std::size_t>(globalX - cells.X);
                    page.Tiles[pageIndex] = section->Tiles[sourceIndex];
                    page.TilePresent[pageIndex] = 1u;
                }
            }
        }
    }

    diagnostics.Status = MapStatus::OK;
    return page;
}

const MapReadStats& MapSectionReader::Stats() const noexcept {
    return _stats;
}

} // namespace Corsairs::Tools::AssetConverter
