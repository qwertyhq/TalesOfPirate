#include "Corsairs/Tools/AssetConverter/MapWriter.h"

#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <fstream>
#include <limits>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

bool WriteBinary(const std::filesystem::path& path, const void* data, std::size_t bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    if (bytes > 0) {
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    }
    return static_cast<bool>(stream);
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream{path, std::ios::trunc};
    if (!stream) {
        return false;
    }
    stream << text;
    return static_cast<bool>(stream);
}

std::filesystem::path WithSuffix(const std::filesystem::path& base, const char* suffix) {
    std::filesystem::path out = base;
    out += suffix;
    return out;
}

} // namespace

MapWriteStatus WriteTerrain(const MapTerrain& terrain,
                            const std::filesystem::path& basePath,
                            std::string& detail) {
    const std::size_t tileCount = terrain.Tiles.size();

    std::vector<std::uint16_t> heights(tileCount);
    std::vector<std::uint8_t> blocks(tileCount * 4);
    std::vector<std::uint16_t> regions(tileCount);

    std::int32_t minHeight = std::numeric_limits<std::int32_t>::max();
    std::int32_t maxHeight = std::numeric_limits<std::int32_t>::lowest();
    std::size_t blockedTiles = 0;

    for (std::size_t i = 0; i < tileCount; ++i) {
        const MapTile& tile = terrain.Tiles[i];

        const std::int32_t raw = tile.Height;
        minHeight = raw < minHeight ? raw : minHeight;
        maxHeight = raw > maxHeight ? raw : maxHeight;

        // Знаковый байт -> беззнаковые 16 бит без потери диапазона.
        heights[i] = static_cast<std::uint16_t>((raw + 128) * 256);

        bool anyBlocked = false;
        for (std::size_t q = 0; q < 4; ++q) {
            blocks[i * 4 + q] = tile.Block[q];
            anyBlocked = anyBlocked || tile.Block[q] != 0;
        }
        if (anyBlocked) {
            ++blockedTiles;
        }

        regions[i] = static_cast<std::uint16_t>(tile.Region);
    }

    if (!WriteBinary(WithSuffix(basePath, ".height.r16"),
                     heights.data(), heights.size() * sizeof(std::uint16_t))) {
        detail = "не удалось записать .height.r16";
        return MapWriteStatus::WRITE_FAILED;
    }

    if (!WriteBinary(WithSuffix(basePath, ".block.raw"), blocks.data(), blocks.size())) {
        detail = "не удалось записать .block.raw";
        return MapWriteStatus::WRITE_FAILED;
    }

    if (!WriteBinary(WithSuffix(basePath, ".region.raw"),
                     regions.data(), regions.size() * sizeof(std::uint16_t))) {
        detail = "не удалось записать .region.raw";
        return MapWriteStatus::WRITE_FAILED;
    }

    JsonWriter json;
    json.BeginObject();
    json.Key("mapFlag");
    json.Value(static_cast<std::int64_t>(terrain.Header.MapFlag));
    json.Key("width");
    json.Value(static_cast<std::int64_t>(terrain.Header.Width));
    json.Key("height");
    json.Value(static_cast<std::int64_t>(terrain.Header.Height));
    // Размер растров ниже — именно gridWidth x gridHeight, а не width x height.
    json.Key("gridWidth");
    json.Value(static_cast<std::int64_t>(terrain.GridWidth));
    json.Key("gridHeight");
    json.Value(static_cast<std::int64_t>(terrain.GridHeight));
    json.Key("sectionWidth");
    json.Value(static_cast<std::int64_t>(terrain.Header.SectionWidth));
    json.Key("sectionHeight");
    json.Value(static_cast<std::int64_t>(terrain.Header.SectionHeight));
    json.Key("sectionsPresent");
    json.Value(static_cast<std::int64_t>(terrain.PresentSections));
    json.Key("sectionsTotal");
    json.Value(static_cast<std::int64_t>(terrain.TotalSections));
    json.Key("blockedTiles");
    json.Value(static_cast<std::int64_t>(blockedTiles));

    json.Key("heightRangeRaw");
    json.BeginArray();
    json.Value(static_cast<std::int64_t>(tileCount > 0 ? minHeight : 0));
    json.Value(static_cast<std::int64_t>(tileCount > 0 ? maxHeight : 0));
    json.EndArray();

    // Единица высоты в исходных данных — 10 см.
    json.Key("heightUnitMeters");
    json.Value(0.1);
    json.Key("heightEncoding");
    json.Value("uint16 = (rawHeight + 128) * 256");
    json.EndObject();

    if (!WriteText(WithSuffix(basePath, ".terrain.json"), json.Str())) {
        detail = "не удалось записать .terrain.json";
        return MapWriteStatus::WRITE_FAILED;
    }

    detail.clear();
    return MapWriteStatus::OK;
}

MapWriteStatus WriteSceneManifest(const SceneObjects& scene,
                                  const std::filesystem::path& basePath,
                                  std::string& detail) {
    JsonWriter json;
    json.BeginObject();

    json.Key("version");
    json.Value(static_cast<std::int64_t>(scene.Header.Version));
    json.Key("sectionCntX");
    json.Value(static_cast<std::int64_t>(scene.Header.SectionCntX));
    json.Key("sectionCntY");
    json.Value(static_cast<std::int64_t>(scene.Header.SectionCntY));
    json.Key("sectionWidth");
    json.Value(static_cast<std::int64_t>(scene.Header.SectionWidth));
    json.Key("sectionHeight");
    json.Value(static_cast<std::int64_t>(scene.Header.SectionHeight));
    json.Key("nonEmptySections");
    json.Value(static_cast<std::int64_t>(scene.NonEmptySections));
    json.Key("objectCount");
    json.Value(static_cast<std::int64_t>(scene.Objects.size()));

    json.Key("objects");
    json.BeginArray();
    for (const PlacedObject& placed : scene.Objects) {
        json.BeginObject();
        json.Key("modelId");
        json.Value(static_cast<std::int64_t>(placed.Info.Id()));
        json.Key("type");
        json.Value(static_cast<std::int64_t>(placed.Info.Type()));
        json.Key("x");
        json.Value(static_cast<std::int64_t>(placed.Info.X));
        json.Key("y");
        json.Value(static_cast<std::int64_t>(placed.Info.Y));
        json.Key("heightOff");
        json.Value(static_cast<std::int64_t>(placed.Info.HeightOff));
        json.Key("yaw");
        json.Value(static_cast<std::int64_t>(placed.Info.YawAngle));
        json.Key("scale");
        json.Value(static_cast<std::int64_t>(placed.Info.Scale));
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();

    if (!WriteText(WithSuffix(basePath, ".objects.json"), json.Str())) {
        detail = "не удалось записать .objects.json";
        return MapWriteStatus::WRITE_FAILED;
    }

    detail.clear();
    return MapWriteStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
