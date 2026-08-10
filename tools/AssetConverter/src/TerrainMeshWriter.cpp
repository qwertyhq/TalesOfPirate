#include "Corsairs/Tools/AssetConverter/TerrainMeshWriter.h"

#include "Corsairs/Tools/AssetConverter/GltfWriter.h"

#include <format>

namespace Corsairs::Tools::AssetConverter {

namespace {

// Одна клетка карты — 100 единиц координат сервера, и столько же сантиметров
// UE. В glTF единица равна метру, а Interchange умножает на сто, поэтому в
// glTF клетка занимает ровно единицу.
constexpr float kUnitsPerTile = 1.0f;

// Высота на диске — знаковый байт в единицах по 10 см, то есть 0.1 метра.
constexpr float kHeightUnit = 0.1f;

std::int8_t HeightAt(const MapTerrain& terrain, std::size_t col, std::size_t row) {
    const std::size_t clampedCol = std::min(col, terrain.GridWidth - 1);
    const std::size_t clampedRow = std::min(row, terrain.GridHeight - 1);
    return terrain.Tiles[clampedRow * terrain.GridWidth + clampedCol].Height;
}

} // namespace

TerrainMeshStats WriteTerrainMesh(const MapTerrain& terrain,
                                  const std::filesystem::path& basePath,
                                  const TerrainMeshOptions& options,
                                  std::string& detail) {
    TerrainMeshStats stats;

    if (terrain.GridWidth == 0 || terrain.GridHeight == 0 || terrain.Tiles.empty()) {
        detail = "террейн пуст";
        return stats;
    }
    if (options.Step == 0 || options.TileQuads == 0) {
        detail = "шаг и размер плитки обязаны быть положительными";
        return stats;
    }

    // Сетка после прореживания. Рельеф карты пологий — на garner перепад
    // около шестнадцати метров на четыре километра, — поэтому брать каждую
    // клетку незачем: полная сетка 4096x4096 дала бы шестнадцать миллионов
    // вершин, а различить её от прореженной вчетверо на глаз нельзя.
    const std::size_t sampledWidth = (terrain.GridWidth - 1) / options.Step + 1;
    const std::size_t sampledHeight = (terrain.GridHeight - 1) / options.Step + 1;

    const std::size_t tilesX = (sampledWidth - 1 + options.TileQuads - 1) / options.TileQuads;
    const std::size_t tilesY = (sampledHeight - 1 + options.TileQuads - 1) / options.TileQuads;

    for (std::size_t tileY = 0; tileY < tilesY; ++tileY) {
        for (std::size_t tileX = 0; tileX < tilesX; ++tileX) {
            const std::size_t startCol = tileX * options.TileQuads;
            const std::size_t startRow = tileY * options.TileQuads;

            // Плитки делят крайние ряды вершин: иначе на стыках появятся щели.
            const std::size_t cols =
                std::min(options.TileQuads + 1, sampledWidth - startCol);
            const std::size_t rows =
                std::min(options.TileQuads + 1, sampledHeight - startRow);
            if (cols < 2 || rows < 2) {
                continue;
            }

            LgoGeomObj object;
            object.Mesh.Positions.reserve(cols * rows);
            object.Mesh.Normals.reserve(cols * rows);
            object.Mesh.Texcoords[0].reserve(cols * rows);

            for (std::size_t row = 0; row < rows; ++row) {
                for (std::size_t col = 0; col < cols; ++col) {
                    const std::size_t mapCol = (startCol + col) * options.Step;
                    const std::size_t mapRow = (startRow + row) * options.Step;

                    Vector3 position;
                    // Координаты кладутся в исходных осях, как есть. Поворот
                    // плоскости карты делает общая запись glTF вместе с
                    // импортёром: перестановка Y и Z, а затем возврат рукости
                    // дают в сумме (x, y) -> (-y, x). Ровно тот же поворот
                    // получают позиции объектов (GetObjectLocation в модуле
                    // CorsairsImport), поэтому город и земля сходятся.
                    //
                    // Прежде здесь стоял минус у оси строк. Он подменял
                    // поворот отражением: земля вставала в тот же угол мира,
                    // что и город, но обе половины оказывались зеркальными
                    // относительно оригинала.
                    position.X = static_cast<float>(mapCol) * kUnitsPerTile;
                    position.Y = static_cast<float>(mapRow) * kUnitsPerTile;
                    position.Z = static_cast<float>(HeightAt(terrain, mapCol, mapRow)) *
                                 kHeightUnit;
                    object.Mesh.Positions.push_back(position);

                    // Нормали вверх по исходной оси высоты: перестановку осей
                    // сделает общая запись glTF, как и для любой другой модели.
                    object.Mesh.Normals.push_back(Vector3{0.0f, 0.0f, 1.0f});

                    // Развёртка повторяется каждую клетку: конкретная текстура
                    // рельефа задаётся слоями в TileInfo, а их разбор — задача
                    // отдельного этапа.
                    Vector2 uv;
                    uv.X = static_cast<float>(startCol + col);
                    uv.Y = static_cast<float>(startRow + row);
                    object.Mesh.Texcoords[0].push_back(uv);
                }
            }

            for (std::size_t row = 0; row + 1 < rows; ++row) {
                for (std::size_t col = 0; col + 1 < cols; ++col) {
                    const std::uint32_t topLeft =
                        static_cast<std::uint32_t>(row * cols + col);
                    const std::uint32_t topRight = topLeft + 1;
                    const std::uint32_t bottomLeft =
                        static_cast<std::uint32_t>((row + 1) * cols + col);
                    const std::uint32_t bottomRight = bottomLeft + 1;

                    // Обход считается по векторному произведению в исходных
                    // осях: рёбра topLeft → topRight и topLeft → bottomLeft
                    // идут по +X и +Y, их произведение смотрит по +Z, то есть
                    // вверх — как и положено земле.
                    //
                    // Прежний порядок был обратным: его подобрали под минус у
                    // оси строк, который отражал плоскость и переворачивал
                    // нормали. Минуса больше нет, и обход возвращается к
                    // прямому — иначе земля пропадёт при взгляде сверху и
                    // останется видимой только снизу.
                    object.Mesh.Indices.push_back(topLeft);
                    object.Mesh.Indices.push_back(topRight);
                    object.Mesh.Indices.push_back(bottomLeft);

                    object.Mesh.Indices.push_back(topRight);
                    object.Mesh.Indices.push_back(bottomRight);
                    object.Mesh.Indices.push_back(bottomLeft);
                }
            }

            object.Mesh.Header.VertexNum = static_cast<std::uint32_t>(cols * rows);
            SubsetInfo subset{};
            subset.PrimitiveNum = static_cast<std::uint32_t>(object.Mesh.Indices.size() / 3);
            subset.StartIndex = 0;
            subset.VertexNum = static_cast<std::uint32_t>(cols * rows);
            subset.MinIndex = 0;
            object.Mesh.Subsets.push_back(subset);

            std::filesystem::path tilePath = basePath;
            tilePath.replace_filename(
                std::format("{}.terrain_{:02}_{:02}.gltf",
                            basePath.filename().string(), tileX, tileY));

            std::string tileDetail;
            const GltfStatus status = WriteGltf(object, tilePath, tileDetail, {});
            if (status != GltfStatus::OK) {
                detail = std::format("плитка {},{}: {}", tileX, tileY, tileDetail);
                return stats;
            }

            ++stats.Tiles;
            stats.Vertices += cols * rows;
            stats.Triangles += (cols - 1) * (rows - 1) * 2;
        }
    }

    detail.clear();
    stats.Ok = true;
    return stats;
}

} // namespace Corsairs::Tools::AssetConverter
