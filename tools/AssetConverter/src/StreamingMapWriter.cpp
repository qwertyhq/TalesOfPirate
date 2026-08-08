#include "Corsairs/Tools/AssetConverter/StreamingMapWriter.h"

#include "Corsairs/Tools/AssetConverter/JsonWriter.h"
#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/MapWriter.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

std::filesystem::path WithSuffix(const std::filesystem::path& base,
                                 std::string_view suffix) {
    std::filesystem::path path = base;
    path += suffix;
    return path;
}

bool CheckedMultiply(std::uint64_t left,
                     std::uint64_t right,
                     std::uint64_t& result) {
    if (left != 0u && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool CheckedAdd(std::uint64_t left,
                std::uint64_t right,
                std::uint64_t& result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool FitsStreamOffset(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamoff>::max());
}

bool OpenSizedOutput(const std::filesystem::path& path,
                     std::uint64_t bytes,
                     std::ofstream& output,
                     std::string_view label,
                     std::string& detail) {
    if (!FitsStreamOffset(bytes)) {
        detail = std::format("{}: размер {} не помещается в streamoff",
                             label,
                             bytes);
        return false;
    }

    output.open(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        detail = std::format("не удалось открыть {}", label);
        return false;
    }

    if (bytes > 0u) {
        output.seekp(static_cast<std::streamoff>(bytes - 1u), std::ios::beg);
        if (!output) {
            detail = std::format("не удалось pre-size {}", label);
            return false;
        }
        output.put('\0');
        if (!output) {
            detail = std::format("не удалось записать размер {}", label);
            return false;
        }
    }
    return true;
}

bool WriteRowAt(std::ofstream& output,
                std::uint64_t offset,
                std::span<const std::uint8_t> row,
                std::string_view label,
                std::string& detail) {
    if (!FitsStreamOffset(offset) ||
        row.size() > static_cast<std::size_t>(
                         std::numeric_limits<std::streamsize>::max())) {
        detail = std::format("{}: смещение или строка не помещается в поток",
                             label);
        return false;
    }

    output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!output) {
        detail = std::format("не удалось выполнить seek в {}", label);
        return false;
    }
    output.write(reinterpret_cast<const char*>(row.data()),
                 static_cast<std::streamsize>(row.size()));
    if (!output) {
        detail = std::format("не удалось записать строку {}", label);
        return false;
    }
    return true;
}

bool FlushOutput(std::ofstream& output,
                 std::string_view label,
                 std::string& detail) {
    output.flush();
    if (!output) {
        detail = std::format("не удалось завершить {}", label);
        return false;
    }
    return true;
}

bool WriteMetadata(const std::filesystem::path& path,
                   const MapFileHeader& header,
                   std::uint64_t gridWidth,
                   std::uint64_t gridHeight,
                   std::size_t sectionsTotal,
                   const MapRasterStats& stats,
                   std::string& detail) {
    JsonWriter json;
    json.BeginObject();
    json.Key("mapFlag");
    json.Value(static_cast<std::int64_t>(header.MapFlag));
    json.Key("width");
    json.Value(static_cast<std::int64_t>(header.Width));
    json.Key("height");
    json.Value(static_cast<std::int64_t>(header.Height));
    json.Key("gridWidth");
    json.Value(static_cast<std::int64_t>(gridWidth));
    json.Key("gridHeight");
    json.Value(static_cast<std::int64_t>(gridHeight));
    json.Key("sectionWidth");
    json.Value(static_cast<std::int64_t>(header.SectionWidth));
    json.Key("sectionHeight");
    json.Value(static_cast<std::int64_t>(header.SectionHeight));
    json.Key("sectionsPresent");
    json.Value(static_cast<std::int64_t>(stats.PresentSections));
    json.Key("sectionsTotal");
    json.Value(static_cast<std::int64_t>(sectionsTotal));
    json.Key("blockedTiles");
    json.Value(static_cast<std::int64_t>(stats.BlockedTiles));
    json.Key("heightRangeRaw");
    json.BeginArray();
    json.Value(static_cast<std::int64_t>(stats.MinHeightRaw));
    json.Value(static_cast<std::int64_t>(stats.MaxHeightRaw));
    json.EndArray();
    json.Key("heightUnitMeters");
    json.Value(0.1);
    json.Key("heightEncoding");
    json.Value("uint16 = (rawHeight + 128) * 256");
    json.EndObject();

    std::ofstream output{path, std::ios::trunc};
    if (!output) {
        detail = "не удалось открыть .terrain.json";
        return false;
    }
    output << json.Str();
    output.flush();
    if (!output) {
        detail = "не удалось записать .terrain.json";
        return false;
    }
    return true;
}

} // namespace

MapWriteStatus WriteTerrainRasters(MapSectionReader& reader,
                                   const std::filesystem::path& basePath,
                                   MapRasterStats& stats,
                                   std::string& detail) {
    stats = {};
    detail.clear();

    const MapFileHeader& header = reader.Header();
    const std::uint64_t sectionWidth =
        static_cast<std::uint64_t>(header.SectionWidth);
    const std::uint64_t sectionHeight =
        static_cast<std::uint64_t>(header.SectionHeight);
    const std::uint64_t sectionsX =
        static_cast<std::uint64_t>(header.Width) / sectionWidth;
    const std::uint64_t sectionsY =
        static_cast<std::uint64_t>(header.Height) / sectionHeight;

    std::uint64_t gridWidth = 0;
    std::uint64_t gridHeight = 0;
    std::uint64_t tileCount = 0;
    std::uint64_t heightBytes = 0;
    std::uint64_t blockBytes = 0;
    std::uint64_t regionBytes = 0;
    std::uint64_t sectionsTotal64 = 0;
    if (!CheckedMultiply(sectionsX, sectionWidth, gridWidth) ||
        !CheckedMultiply(sectionsY, sectionHeight, gridHeight) ||
        !CheckedMultiply(gridWidth, gridHeight, tileCount) ||
        !CheckedMultiply(tileCount, 2u, heightBytes) ||
        !CheckedMultiply(tileCount, 4u, blockBytes) ||
        !CheckedMultiply(tileCount, 2u, regionBytes) ||
        !CheckedMultiply(sectionsX, sectionsY, sectionsTotal64) ||
        sectionsTotal64 > std::numeric_limits<std::size_t>::max()) {
        detail = "размеры растров переполняют адресное пространство";
        return MapWriteStatus::WRITE_FAILED;
    }

    const std::uint64_t heightRowBytes64 = sectionWidth * 2u;
    const std::uint64_t blockRowBytes64 = sectionWidth * 4u;
    const std::uint64_t regionRowBytes64 = sectionWidth * 2u;
    if (heightRowBytes64 > std::numeric_limits<std::size_t>::max() ||
        blockRowBytes64 > std::numeric_limits<std::size_t>::max() ||
        regionRowBytes64 > std::numeric_limits<std::size_t>::max()) {
        detail = "строка секции не помещается в адресное пространство";
        return MapWriteStatus::WRITE_FAILED;
    }

    std::ofstream heightOutput;
    std::ofstream blockOutput;
    std::ofstream regionOutput;
    if (!OpenSizedOutput(WithSuffix(basePath, ".height.r16"),
                         heightBytes,
                         heightOutput,
                         ".height.r16",
                         detail) ||
        !OpenSizedOutput(WithSuffix(basePath, ".block.raw"),
                         blockBytes,
                         blockOutput,
                         ".block.raw",
                         detail) ||
        !OpenSizedOutput(WithSuffix(basePath, ".region.raw"),
                         regionBytes,
                         regionOutput,
                         ".region.raw",
                         detail)) {
        return MapWriteStatus::WRITE_FAILED;
    }

    std::vector<std::uint8_t> heightRow(
        static_cast<std::size_t>(heightRowBytes64));
    std::vector<std::uint8_t> blockRow(
        static_cast<std::size_t>(blockRowBytes64));
    std::vector<std::uint8_t> regionRow(
        static_cast<std::size_t>(regionRowBytes64));

    std::int32_t minHeight = std::numeric_limits<std::int8_t>::max();
    std::int32_t maxHeight = std::numeric_limits<std::int8_t>::min();
    bool hasHeightSample = false;

    for (std::uint64_t sectionY = 0; sectionY < sectionsY; ++sectionY) {
        for (std::uint64_t sectionX = 0; sectionX < sectionsX; ++sectionX) {
            MapDiagnostics diagnostics;
            auto section = reader.ReadSection(
                static_cast<std::uint32_t>(sectionX),
                static_cast<std::uint32_t>(sectionY),
                diagnostics);
            if (!section.has_value()) {
                detail = std::format("не удалось прочитать секцию ({},{}): {}",
                                     sectionX,
                                     sectionY,
                                     diagnostics.Detail);
                return MapWriteStatus::WRITE_FAILED;
            }

            if (!section->Present) {
                ++stats.AbsentSections;
                minHeight = std::min(minHeight, -128);
                maxHeight = std::max(maxHeight, -128);
                hasHeightSample = true;
                continue;
            }

            std::uint64_t expectedTiles64 = 0;
            if (!CheckedMultiply(sectionWidth, sectionHeight, expectedTiles64) ||
                expectedTiles64 != section->Tiles.size()) {
                detail = std::format(
                    "секция ({},{}) содержит {} тайлов вместо {}",
                    sectionX,
                    sectionY,
                    section->Tiles.size(),
                    expectedTiles64);
                return MapWriteStatus::WRITE_FAILED;
            }
            ++stats.PresentSections;

            for (std::uint64_t localY = 0; localY < sectionHeight; ++localY) {
                for (std::uint64_t localX = 0; localX < sectionWidth; ++localX) {
                    const std::size_t tileIndex = static_cast<std::size_t>(
                        localY * sectionWidth + localX);
                    const MapTile& tile = section->Tiles[tileIndex];

                    const std::int32_t rawHeight = tile.Height;
                    minHeight = std::min(minHeight, rawHeight);
                    maxHeight = std::max(maxHeight, rawHeight);
                    hasHeightSample = true;
                    const std::uint16_t encodedHeight =
                        static_cast<std::uint16_t>((rawHeight + 128) * 256);
                    const std::size_t heightIndex =
                        static_cast<std::size_t>(localX * 2u);
                    heightRow[heightIndex] =
                        static_cast<std::uint8_t>(encodedHeight);
                    heightRow[heightIndex + 1u] =
                        static_cast<std::uint8_t>(encodedHeight >> 8u);

                    const std::uint16_t region =
                        static_cast<std::uint16_t>(tile.Region);
                    const std::size_t regionIndex =
                        static_cast<std::size_t>(localX * 2u);
                    regionRow[regionIndex] = static_cast<std::uint8_t>(region);
                    regionRow[regionIndex + 1u] =
                        static_cast<std::uint8_t>(region >> 8u);

                    bool blocked = false;
                    const std::size_t blockIndex =
                        static_cast<std::size_t>(localX * 4u);
                    for (std::size_t quarter = 0; quarter < 4u; ++quarter) {
                        const std::uint8_t value = tile.Block[quarter];
                        blockRow[blockIndex + quarter] = value;
                        blocked = blocked || (value & 0x80u) != 0u;
                    }
                    if (blocked) {
                        ++stats.BlockedTiles;
                    }
                }

                std::uint64_t globalY = 0;
                std::uint64_t rowStartTile = 0;
                std::uint64_t sectionStartTile = 0;
                if (!CheckedMultiply(sectionY, sectionHeight, globalY) ||
                    !CheckedAdd(globalY, localY, globalY) ||
                    !CheckedMultiply(globalY, gridWidth, rowStartTile) ||
                    !CheckedMultiply(sectionX, sectionWidth, sectionStartTile) ||
                    !CheckedAdd(rowStartTile, sectionStartTile, rowStartTile)) {
                    detail = "смещение строки растра переполняет адресное пространство";
                    return MapWriteStatus::WRITE_FAILED;
                }

                std::uint64_t heightOffset = 0;
                std::uint64_t blockOffset = 0;
                std::uint64_t regionOffset = 0;
                if (!CheckedMultiply(rowStartTile, 2u, heightOffset) ||
                    !CheckedMultiply(rowStartTile, 4u, blockOffset) ||
                    !CheckedMultiply(rowStartTile, 2u, regionOffset) ||
                    !WriteRowAt(heightOutput,
                                heightOffset,
                                heightRow,
                                ".height.r16",
                                detail) ||
                    !WriteRowAt(blockOutput,
                                blockOffset,
                                blockRow,
                                ".block.raw",
                                detail) ||
                    !WriteRowAt(regionOutput,
                                regionOffset,
                                regionRow,
                                ".region.raw",
                                detail)) {
                    if (detail.empty()) {
                        detail = "смещение строки растра переполняет адресное пространство";
                    }
                    return MapWriteStatus::WRITE_FAILED;
                }
            }
        }
    }

    if (!hasHeightSample) {
        minHeight = 0;
        maxHeight = 0;
    }
    stats.MinHeightRaw = static_cast<std::int8_t>(minHeight);
    stats.MaxHeightRaw = static_cast<std::int8_t>(maxHeight);

    if (!FlushOutput(heightOutput, ".height.r16", detail) ||
        !FlushOutput(blockOutput, ".block.raw", detail) ||
        !FlushOutput(regionOutput, ".region.raw", detail) ||
        !WriteMetadata(WithSuffix(basePath, ".terrain.json"),
                       header,
                       gridWidth,
                       gridHeight,
                       static_cast<std::size_t>(sectionsTotal64),
                       stats,
                       detail)) {
        return MapWriteStatus::WRITE_FAILED;
    }

    detail.clear();
    return MapWriteStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
