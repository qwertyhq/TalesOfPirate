#pragma once

#include "Corsairs/Tools/AssetConverter/MapParser.h"
#include "Corsairs/Tools/AssetConverter/TerrainPage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

struct MapSection {
    std::uint32_t X{0};
    std::uint32_t Y{0};
    bool Present{false};
    std::vector<MapTile> Tiles;
};

struct MapReadStats {
    std::uint64_t MetadataBytesRead{0};
    std::size_t LargestMetadataRead{0};
    std::uint64_t BodyBytesRead{0};
    std::size_t LargestBodyRead{0};
    // Максимум по всем вызовам: число одновременно материализованных MapTile.
    // ReadWindow учитывает output page и текущую секцию.
    std::size_t PeakResidentTiles{0};
};

class MapSectionReader {
public:
    MapSectionReader(const MapSectionReader&) = delete;
    MapSectionReader& operator=(const MapSectionReader&) = delete;
    MapSectionReader(MapSectionReader&&) = default;
    MapSectionReader& operator=(MapSectionReader&&) = default;

    [[nodiscard]] static std::optional<MapSectionReader> Open(
        const std::filesystem::path& path,
        MapDiagnostics& diagnostics);

    [[nodiscard]] const MapFileHeader& Header() const noexcept;

    [[nodiscard]] std::optional<MapSection> ReadSection(
        std::uint32_t x,
        std::uint32_t y,
        MapDiagnostics& diagnostics);

    [[nodiscard]] std::optional<MapPageTiles> ReadWindow(
        MapCellRect cells,
        std::uint32_t rightHalo,
        std::uint32_t bottomHalo,
        MapDiagnostics& diagnostics);

    [[nodiscard]] const MapReadStats& Stats() const noexcept;

private:
    MapSectionReader() = default;

    std::ifstream _input;
    MapFileHeader _header{};
    std::vector<std::uint32_t> _offsets;
    std::uint64_t _fileBytes{0};
    std::uint64_t _prefixBytes{0};
    std::size_t _sectionsX{0};
    std::size_t _sectionsY{0};
    std::size_t _tilesPerSection{0};
    std::size_t _sectionBytes{0};
    MapReadStats _stats;
};

} // namespace Corsairs::Tools::AssetConverter
