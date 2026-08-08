#include "Corsairs/Tools/AssetConverter/TerrainPageBaker.h"

#include "Corsairs/Tools/AssetConverter/ImageCodec.h"
#include "Corsairs/Tools/AssetConverter/ProcessMetrics.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"
#include "Corsairs/Tools/AssetConverter/StreamingPngWriter.h"
#include "Corsairs/Tools/AssetConverter/TerrainLayers.h"
#include "Corsairs/Tools/AssetConverter/TerrainTextureCache.h"
#include "Corsairs/Tools/AssetConverter/TerrainTextureSampling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::size_t kHardTextureCacheBytes = 32u * 1024u * 1024u;

std::uint8_t RoundUnorm8(double value) {
    return static_cast<std::uint8_t>(
        std::clamp(std::floor(value + 0.5), 0.0, 255.0));
}

bool MultiplyFits(std::uint64_t left,
                  std::uint64_t right,
                  std::uint64_t limit,
                  std::uint64_t& product) {
    if (left != 0u && right > limit / left) {
        return false;
    }
    product = left * right;
    return true;
}

std::array<double, 3> InterpolateDiffuse(
    const std::array<LegacyTerrainCornerSample, 4>& corners,
    double localU,
    double localV) {
    std::array<double, 3> result{};
    if (localU + localV <= 1.0) {
        const double topLeftWeight = 1.0 - localU - localV;
        for (std::size_t channel = 0; channel < result.size(); ++channel) {
            result[channel] =
                corners[0].Diffuse[channel] * topLeftWeight +
                corners[1].Diffuse[channel] * localU +
                corners[2].Diffuse[channel] * localV;
        }
    }
    else {
        const double bottomRightWeight = localU + localV - 1.0;
        const double bottomLeftWeight = 1.0 - localU;
        const double topRightWeight = 1.0 - localV;
        for (std::size_t channel = 0; channel < result.size(); ++channel) {
            result[channel] =
                corners[3].Diffuse[channel] * bottomRightWeight +
                corners[2].Diffuse[channel] * bottomLeftWeight +
                corners[1].Diffuse[channel] * topRightWeight;
        }
    }
    return result;
}

} // namespace

LegacyTerrainCornerSample ResolveLegacyTerrainCornerSample(
    const MapTile& tile, bool present) noexcept {
    if (!present) {
        return LegacyTerrainCornerSample{
            {255u, 255u, 255u, 255u},
            -200.0,
        };
    }

    const std::uint16_t color = static_cast<std::uint16_t>(tile.Color);
    return LegacyTerrainCornerSample{
        {
            static_cast<std::uint8_t>((color & 0x001fu) << 3u),
            static_cast<std::uint8_t>((color & 0x07e0u) >> 3u),
            static_cast<std::uint8_t>((color & 0xf800u) >> 8u),
            255u,
        },
        static_cast<double>(tile.Height) * 10.0,
    };
}

TerrainBakeResult BakeTerrainPage(
    MapSectionReader& reader,
    const TerrainCatalog& catalog,
    TerrainPageId page,
    const std::filesystem::path& alphaAtlas,
    const std::filesystem::path& outputDirectory,
    const TerrainBakeOptions& options,
    std::string& detail) {
    detail.clear();
    TerrainBakeResult result;
    std::filesystem::path outputPath;
    bool completedOutputOwned = false;
    auto fail = [&](std::string message) {
        if (detail.empty()) {
            detail = std::move(message);
        }
        if (completedOutputOwned) {
            std::error_code cleanupError;
            std::filesystem::remove(outputPath, cleanupError);
            completedOutputOwned = false;
        }
        result.Ok = false;
        result.PngPath.clear();
        result.PngSha256.clear();
        return result;
    };

    if (options.CellsPerPage == 0u || options.PixelsPerCell == 0u) {
        return fail("CellsPerPage и PixelsPerCell должны быть ненулевыми");
    }

    const std::uint64_t cellsPerPage = options.CellsPerPage;
    const std::uint64_t pixelsPerCell = options.PixelsPerCell;
    std::uint64_t sourceX = 0;
    std::uint64_t sourceY = 0;
    std::uint64_t pixelWidth = 0;
    std::uint64_t pixelHeight = 0;
    if (!MultiplyFits(page.X, cellsPerPage,
                      std::numeric_limits<std::uint64_t>::max(), sourceX) ||
        !MultiplyFits(page.Y, cellsPerPage,
                      std::numeric_limits<std::uint64_t>::max(), sourceY) ||
        !MultiplyFits(cellsPerPage, pixelsPerCell,
                      std::numeric_limits<std::uint64_t>::max(), pixelWidth) ||
        !MultiplyFits(cellsPerPage, pixelsPerCell,
                      std::numeric_limits<std::uint64_t>::max(), pixelHeight)) {
        return fail("переполнение 64-битной арифметики страницы");
    }
    if (sourceX > std::numeric_limits<std::uint32_t>::max() ||
        sourceY > std::numeric_limits<std::uint32_t>::max()) {
        return fail("координаты страницы не помещаются в uint32_t");
    }
    if (pixelWidth == 0u || pixelHeight == 0u ||
        pixelWidth > std::numeric_limits<std::uint32_t>::max() ||
        pixelHeight > std::numeric_limits<std::uint32_t>::max()) {
        return fail("размер изображения не помещается в uint32_t");
    }

    std::uint64_t rgbaRowBytes = 0;
    if (!MultiplyFits(pixelWidth, 4u,
                      std::numeric_limits<std::uint64_t>::max(), rgbaRowBytes) ||
        rgbaRowBytes > std::numeric_limits<std::size_t>::max() ||
        rgbaRowBytes > static_cast<std::uint64_t>(
                           std::numeric_limits<std::streamsize>::max())) {
        return fail("размер RGBA-строки переполнен");
    }
    std::uint64_t rgbaBytes = 0;
    std::uint64_t filteredBytes = 0;
    if (!MultiplyFits(rgbaRowBytes, pixelHeight,
                      std::numeric_limits<std::uint64_t>::max(), rgbaBytes) ||
        rgbaRowBytes == std::numeric_limits<std::uint64_t>::max() ||
        !MultiplyFits(rgbaRowBytes + 1u, pixelHeight,
                      std::numeric_limits<std::uint64_t>::max(), filteredBytes) ||
        rgbaBytes > std::numeric_limits<std::size_t>::max() ||
        filteredBytes > std::numeric_limits<std::size_t>::max() ||
        rgbaBytes > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamsize>::max()) ||
        filteredBytes > static_cast<std::uint64_t>(
                            std::numeric_limits<std::streamsize>::max())) {
        return fail("полный byte count изображения переполнен");
    }

    const MapFileHeader& header = reader.Header();
    const std::uint64_t sectionWidth = static_cast<std::uint32_t>(header.SectionWidth);
    const std::uint64_t sectionHeight = static_cast<std::uint32_t>(header.SectionHeight);
    const std::uint64_t gridWidth =
        static_cast<std::uint64_t>(header.Width / header.SectionWidth) * sectionWidth;
    const std::uint64_t gridHeight =
        static_cast<std::uint64_t>(header.Height / header.SectionHeight) * sectionHeight;
    if (sourceX > gridWidth || sourceY > gridHeight ||
        cellsPerPage + 1u > gridWidth - sourceX ||
        cellsPerPage + 1u > gridHeight - sourceY) {
        return fail("страница с right/bottom halo выходит за усечённую сетку карты");
    }

    result.SourceCellBounds = MapCellRect{
        static_cast<std::uint32_t>(sourceX),
        static_cast<std::uint32_t>(sourceY),
        options.CellsPerPage,
        options.CellsPerPage,
    };
    MapDiagnostics diagnostics;
    auto pageTiles = reader.ReadWindow(result.SourceCellBounds, 1u, 1u, diagnostics);
    if (!pageTiles.has_value()) {
        detail = diagnostics.Detail;
        return fail("не удалось прочитать страницу с halo");
    }

    const std::uint64_t pageEndX = sourceX + cellsPerPage;
    const std::uint64_t pageEndY = sourceY + cellsPerPage;
    const std::uint32_t firstSectionX = static_cast<std::uint32_t>(sourceX / sectionWidth);
    const std::uint32_t firstSectionY = static_cast<std::uint32_t>(sourceY / sectionHeight);
    const std::uint32_t lastPageSectionX =
        static_cast<std::uint32_t>((pageEndX - 1u) / sectionWidth);
    const std::uint32_t lastPageSectionY =
        static_cast<std::uint32_t>((pageEndY - 1u) / sectionHeight);
    const std::uint32_t lastStoredSectionX =
        static_cast<std::uint32_t>(pageEndX / sectionWidth);
    const std::uint32_t lastStoredSectionY =
        static_cast<std::uint32_t>(pageEndY / sectionHeight);
    const std::uint32_t storedSectionColumns =
        lastStoredSectionX - firstSectionX + 1u;
    const std::uint32_t storedSectionRows =
        lastStoredSectionY - firstSectionY + 1u;
    if (pageTiles->SectionPresent.size() !=
        static_cast<std::size_t>(storedSectionColumns) * storedSectionRows) {
        return fail("reader вернул некорректную section presence mask");
    }

    result.SectionOriginX = firstSectionX;
    result.SectionOriginY = firstSectionY;
    result.SectionGridWidth = lastPageSectionX - firstSectionX + 1u;
    result.SectionGridHeight = lastPageSectionY - firstSectionY + 1u;
    result.SectionPresenceMask.reserve(
        static_cast<std::size_t>(result.SectionGridWidth) * result.SectionGridHeight);
    for (std::uint32_t sectionY = 0; sectionY < result.SectionGridHeight; ++sectionY) {
        for (std::uint32_t sectionX = 0; sectionX < result.SectionGridWidth; ++sectionX) {
            const std::size_t sourceIndex =
                static_cast<std::size_t>(sectionY) * storedSectionColumns + sectionX;
            const std::uint8_t present = pageTiles->SectionPresent[sourceIndex];
            result.SectionPresenceMask.push_back(present);
            if (present == 0u) {
                ++result.AbsentSections;
            }
        }
    }
    if (result.AbsentSections != 0u) {
        return fail("owned page содержит отсутствующую секцию");
    }

    for (std::uint32_t y = 0; y < options.CellsPerPage; ++y) {
        for (std::uint32_t x = 0; x < options.CellsPerPage; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * pageTiles->StoredWidth + x;
            if (pageTiles->TilePresent[index] == 0u) {
                return fail(std::format(
                    "отсутствует обязательный owned sample ({},{})", x, y));
            }
        }
    }

    std::array<bool, 256> used{};
    std::array<std::optional<std::filesystem::path>, 256> texturePaths;
    std::optional<std::uint8_t> firstUnresolved;
    for (std::uint32_t y = 0; y < options.CellsPerPage; ++y) {
        for (std::uint32_t x = 0; x < options.CellsPerPage; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * pageTiles->StoredWidth + x;
            const ResolvedTerrainLayers layers = ResolveTerrainLayers(pageTiles->Tiles[index]);
            for (std::size_t layerIndex = 0; layerIndex < layers.Count; ++layerIndex) {
                const std::uint8_t textureId = layers.Values[layerIndex].TextureId;
                used[textureId] = true;
                if (!texturePaths[textureId].has_value()) {
                    texturePaths[textureId] = catalog.Resolve(textureId);
                }
                if (!texturePaths[textureId].has_value()) {
                    ++result.UnresolvedLayers;
                    if (!firstUnresolved.has_value()) {
                        firstUnresolved = textureId;
                    }
                }
            }
        }
    }
    for (std::size_t textureId = 0; textureId < used.size(); ++textureId) {
        if (used[textureId]) {
            result.UsedTextureIds.push_back(static_cast<std::uint8_t>(textureId));
        }
    }
    if (result.UnresolvedLayers != 0u) {
        return fail(std::format("не разрешён используемый terrain texture ID {}",
                                *firstUnresolved));
    }

    auto atlas = DecodeImageFile(alphaAtlas, detail);
    if (!atlas.has_value()) {
        return fail("не удалось декодировать alpha atlas");
    }

    TerrainTextureCache textureCache{kHardTextureCacheBytes};
    for (const std::uint8_t textureId : result.UsedTextureIds) {
        const auto image = textureCache.Load(*texturePaths[textureId], detail);
        if (!image) {
            return fail(std::format("не удалось декодировать terrain texture ID {}",
                                    textureId));
        }
    }
    result.PeakTextureCacheBytes = textureCache.PeakDecodedBytes();
    if (result.PeakTextureCacheBytes > options.MaxTextureCacheBytes) {
        return fail("превышен budget декодированного texture cache");
    }
    if (static_cast<std::size_t>(rgbaRowBytes) > options.MaxRgbaRowBytes) {
        return fail("превышен budget RGBA-строки");
    }

    outputPath = outputDirectory /
        std::format("garner.albedo_{}_{}.png", page.X, page.Y);
    auto writer = StreamingPngWriter::Open(
        outputPath,
        static_cast<std::uint32_t>(pixelWidth),
        static_cast<std::uint32_t>(pixelHeight),
        detail);
    if (!writer) {
        return fail("не удалось открыть streaming PNG");
    }

    std::vector<std::uint8_t> row(static_cast<std::size_t>(rgbaRowBytes));
    for (std::uint32_t pixelY = 0;
         pixelY < static_cast<std::uint32_t>(pixelHeight);
         ++pixelY) {
        const std::uint32_t cellY = pixelY / options.PixelsPerCell;
        const std::uint32_t pixelInCellY = pixelY % options.PixelsPerCell;
        const double localV =
            (static_cast<double>(pixelInCellY) + 0.5) / options.PixelsPerCell;
        const std::uint32_t globalCellY =
            static_cast<std::uint32_t>(sourceY) + cellY;

        for (std::uint32_t pixelX = 0;
             pixelX < static_cast<std::uint32_t>(pixelWidth);
             ++pixelX) {
            const std::uint32_t cellX = pixelX / options.PixelsPerCell;
            const std::uint32_t pixelInCellX = pixelX % options.PixelsPerCell;
            const double localU =
                (static_cast<double>(pixelInCellX) + 0.5) / options.PixelsPerCell;
            const std::uint32_t globalCellX =
                static_cast<std::uint32_t>(sourceX) + cellX;
            const std::size_t tileIndex =
                static_cast<std::size_t>(cellY) * pageTiles->StoredWidth + cellX;
            const MapTile& tile = pageTiles->Tiles[tileIndex];
            const ResolvedTerrainLayers layers = ResolveTerrainLayers(tile);
            const std::size_t outputOffset = static_cast<std::size_t>(pixelX) * 4u;
            if (layers.Count == 0u) {
                std::fill_n(row.begin() + static_cast<std::ptrdiff_t>(outputOffset),
                            4u, 0u);
                continue;
            }

            const double textureU =
                (static_cast<double>(globalCellX % 4u) + localU) / 4.0;
            const double textureV =
                (static_cast<double>(globalCellY % 4u) + localV) / 4.0;
            const auto baseImage = textureCache.Load(
                *texturePaths[layers.Values[0].TextureId], detail);
            if (!baseImage) {
                writer.reset();
                return fail("terrain texture стал недоступен во время bake");
            }
            std::array<std::uint8_t, 4> composite = SampleTerrainImageLinear(
                *baseImage, textureU, textureV,
                TerrainAddressMode::WRAP, TerrainAddressMode::WRAP);

            for (std::size_t layerIndex = 1; layerIndex < layers.Count; ++layerIndex) {
                const TerrainLayer& layer = layers.Values[layerIndex];
                const auto upperImage = textureCache.Load(
                    *texturePaths[layer.TextureId], detail);
                if (!upperImage) {
                    writer.reset();
                    return fail("upper terrain texture стал недоступен во время bake");
                }
                const auto upper = SampleTerrainImageLinear(
                    *upperImage, textureU, textureV,
                    TerrainAddressMode::WRAP, TerrainAddressMode::WRAP);
                const std::optional<AtlasRect> rect = ResolveAlphaAtlasRect(layer.AlphaMask);
                if (!rect.has_value()) {
                    writer.reset();
                    return fail("не разрешён alpha mask используемого слоя");
                }
                const double atlasU = static_cast<double>(rect->U0) + 0.01 + localU * 0.23;
                const double atlasV = static_cast<double>(rect->V0) + 0.01 + localV * 0.23;
                const auto alpha = SampleTerrainImageLinear(
                    *atlas, atlasU, atlasV,
                    TerrainAddressMode::MIRROR, TerrainAddressMode::MIRROR);
                for (std::size_t channel = 0; channel < 3u; ++channel) {
                    composite[channel] = RoundUnorm8(
                        (static_cast<double>(upper[channel]) * alpha[3] +
                         static_cast<double>(composite[channel]) * (255u - alpha[3])) /
                        255.0);
                }
            }

            const std::array<std::size_t, 4> cornerIndices{
                tileIndex,
                tileIndex + 1u,
                tileIndex + pageTiles->StoredWidth,
                tileIndex + pageTiles->StoredWidth + 1u,
            };
            std::array<LegacyTerrainCornerSample, 4> corners{};
            for (std::size_t corner = 0; corner < corners.size(); ++corner) {
                corners[corner] = ResolveLegacyTerrainCornerSample(
                    pageTiles->Tiles[cornerIndices[corner]],
                    pageTiles->TilePresent[cornerIndices[corner]] != 0u);
            }
            const std::array<double, 3> diffuse =
                InterpolateDiffuse(corners, localU, localV);
            for (std::size_t channel = 0; channel < 3u; ++channel) {
                row[outputOffset + channel] = RoundUnorm8(
                    static_cast<double>(composite[channel]) * diffuse[channel] / 255.0);
            }
            row[outputOffset + 3u] = 255u;
        }

        if (!writer->WriteRgbaRow(row, detail)) {
            writer.reset();
            return fail("не удалось записать RGBA-строку PNG");
        }
    }

    result.PeakRgbaRowBytes = writer->PeakRgbaRowBytes();
    if (!writer->Finish(detail)) {
        writer.reset();
        return fail("не удалось завершить streaming PNG");
    }
    completedOutputOwned = true;
    writer.reset();

    std::error_code fileError;
    const std::uintmax_t outputBytes = std::filesystem::file_size(outputPath, fileError);
    if (fileError || outputBytes > std::numeric_limits<std::size_t>::max()) {
        return fail("не удалось получить размер готового PNG");
    }
    result.OutputBytes = static_cast<std::size_t>(outputBytes);
    const auto hash = Sha256File(outputPath, detail);
    if (!hash.has_value()) {
        return fail("не удалось вычислить SHA-256 готового PNG");
    }
    result.PngSha256 = *hash;

    const auto peakRss = QueryPeakProcessRssBytes(detail);
    if (!peakRss.has_value()) {
        return fail("не удалось получить peak RSS после bake");
    }
    result.PeakRssBytes = *peakRss;

    if (result.OutputBytes > options.MaxPngBytes) {
        return fail("превышен budget размера PNG");
    }
    if (result.PeakTextureCacheBytes > options.MaxTextureCacheBytes) {
        return fail("превышен budget декодированного texture cache");
    }
    if (result.PeakRgbaRowBytes > options.MaxRgbaRowBytes) {
        return fail("превышен budget RGBA-строки");
    }
    if (result.PeakRssBytes > options.MaxRssBytes) {
        return fail("превышен budget lifetime peak RSS");
    }

    result.Ok = true;
    result.PngPath = outputPath;
    completedOutputOwned = false;
    detail.clear();
    return result;
}

} // namespace Corsairs::Tools::AssetConverter
