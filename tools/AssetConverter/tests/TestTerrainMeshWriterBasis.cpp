#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/MapParser.h"
#include "Corsairs/Tools/AssetConverter/TerrainMeshWriter.h"

#include "TestHarness.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

// Сетка нарочно крошечная: при шаге по умолчанию (каждая четвёртая клетка) из
// неё выходит ровно одна плитка 3x3 вершины, и все девять координат можно
// выписать руками.
constexpr std::size_t kGrid = 9u;
constexpr std::size_t kSampled = 3u;
constexpr std::size_t kVertices = kSampled * kSampled;
constexpr std::size_t kIndices = (kSampled - 1u) * (kSampled - 1u) * 2u * 3u;

class ScopedDirectory {
public:
    ScopedDirectory() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        _path = std::filesystem::temp_directory_path() /
            std::format("corsairs-terrain-mesh-basis-{}", stamp);
        std::error_code error;
        _ready = std::filesystem::create_directory(_path, error) && !error;
    }

    ~ScopedDirectory() {
        if (_ready) {
            std::error_code ignored;
            std::filesystem::remove_all(_path, ignored);
        }
    }

    [[nodiscard]] bool Ready() const { return _ready; }
    [[nodiscard]] const std::filesystem::path& Path() const { return _path; }

private:
    std::filesystem::path _path;
    bool _ready{false};
};

// Высота клетки — col + 10*row. Такая раскладка ловит перепутанные строку и
// столбец: у зеркала и у поворота набор координат один и тот же, различить их
// можно только по тому, какая высота оказалась в какой вершине.
AC::MapTerrain MakeStepTerrain() {
    AC::MapTerrain terrain;
    terrain.GridWidth = kGrid;
    terrain.GridHeight = kGrid;
    terrain.Tiles.resize(kGrid * kGrid);
    for (std::size_t row = 0; row < kGrid; ++row) {
        for (std::size_t col = 0; col < kGrid; ++col) {
            AC::MapTile tile{};
            tile.Color = static_cast<std::int16_t>(-1);
            tile.Height = static_cast<std::int8_t>(col + 10u * row);
            terrain.Tiles[row * kGrid + col] = tile;
        }
    }
    return terrain;
}

struct DecodedMesh {
    std::vector<AC::Vector3> Positions;
    std::vector<std::uint32_t> Indices;
};

// Читает .bin рядом с glTF. Раскладка буфера известна из WriteGltf: позиции,
// нормали, развёртка, индексы — цветов вершин и скиннинга у рельефа нет, так
// что промежуточных блоков в буфере не появляется.
std::optional<DecodedMesh> DecodeTerrainTile(const std::filesystem::path& gltfPath) {
    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");
    const auto bytes = AC::ReadWholeFile(binPath);
    if (!bytes.has_value()) {
        return std::nullopt;
    }

    const std::size_t positionBytes = kVertices * sizeof(AC::Vector3);
    const std::size_t normalBytes = kVertices * sizeof(AC::Vector3);
    const std::size_t uvBytes = kVertices * sizeof(AC::Vector2);
    const std::size_t indexBytes = kIndices * sizeof(std::uint32_t);
    const std::size_t indexOffset = positionBytes + normalBytes + uvBytes;
    if (bytes->size() < indexOffset + indexBytes) {
        return std::nullopt;
    }

    DecodedMesh mesh;
    mesh.Positions.resize(kVertices);
    mesh.Indices.resize(kIndices);
    std::memcpy(mesh.Positions.data(), bytes->data(), positionBytes);
    std::memcpy(mesh.Indices.data(), bytes->data() + indexOffset, indexBytes);
    return mesh;
}

// Треугольник смотрит вверх, если Y-компонента произведения (b-a) x (c-a)
// положительна: в glTF вверх — это +Y, а лицевая сторона обходится против
// часовой стрелки.
bool HasOnlyTopFacingTriangles(const DecodedMesh& mesh) {
    if (mesh.Indices.empty() || mesh.Indices.size() % 3u != 0u) {
        return false;
    }
    for (std::size_t offset = 0; offset < mesh.Indices.size(); offset += 3u) {
        const std::uint32_t ia = mesh.Indices[offset];
        const std::uint32_t ib = mesh.Indices[offset + 1u];
        const std::uint32_t ic = mesh.Indices[offset + 2u];
        if (ia >= mesh.Positions.size() || ib >= mesh.Positions.size() ||
            ic >= mesh.Positions.size()) {
            return false;
        }
        const AC::Vector3& a = mesh.Positions[ia];
        const AC::Vector3& b = mesh.Positions[ib];
        const AC::Vector3& c = mesh.Positions[ic];
        const double abX = static_cast<double>(b.X) - a.X;
        const double abZ = static_cast<double>(b.Z) - a.Z;
        const double acX = static_cast<double>(c.X) - a.X;
        const double acZ = static_cast<double>(c.Z) - a.Z;
        if (!(abZ * acX - abX * acZ > 0.0)) {
            return false;
        }
    }
    return true;
}

bool NearlyEq(float actual, float expected) {
    return std::fabs(actual - expected) <= 1.0e-5f;
}

CORSAIRS_TEST(TerrainMeshWriter_WritesRigidQBasis) {
    ScopedDirectory temporary;
    REQUIRE(temporary.Ready());

    const AC::MapTerrain terrain = MakeStepTerrain();
    std::string detail;
    const AC::TerrainMeshStats stats = AC::WriteTerrainMesh(
        terrain, temporary.Path() / "garner", {}, detail);
    REQUIRE(stats.Ok);
    REQUIRE_EQ(stats.Tiles, std::size_t{1});
    REQUIRE_EQ(stats.Vertices, kVertices);
    REQUIRE_EQ(stats.Triangles, std::size_t{8});

    const std::filesystem::path gltfPath =
        temporary.Path() / "garner.terrain_00_00.gltf";
    REQUIRE(std::filesystem::exists(gltfPath));
    const auto mesh = DecodeTerrainTile(gltfPath);
    REQUIRE(mesh.has_value());

    // Ожидаемые координаты в буфере glTF, посчитанные вручную.
    //
    // Вершина берётся из клетки (mapCol, mapRow) = (4*col, 4*row) — шаг по
    // умолчанию. Поворот Q даёт исходную тройку (-mapRow, mapCol, height), где
    // height — байт карты в единицах по 10 см, то есть (col+10*row)/10 метров.
    // Запись glTF переставляет Y и Z, и в буфер уходит
    // (-mapRow, height, mapCol). По X координаты уходят в минус, потому что
    // ось строк карты после поворота смотрит против X мира.
    struct ExpectedVertex {
        float X;
        float Y;
        float Z;
    };
    constexpr std::array<ExpectedVertex, kVertices> expected{{
        {0.0f, 0.0f, 0.0f},   // клетка (0, 0), высота 0
        {0.0f, 0.4f, 4.0f},   // клетка (4, 0), высота 4
        {0.0f, 0.8f, 8.0f},   // клетка (8, 0), высота 8
        {-4.0f, 4.0f, 0.0f},  // клетка (0, 4), высота 40
        {-4.0f, 4.4f, 4.0f},  // клетка (4, 4), высота 44
        {-4.0f, 4.8f, 8.0f},  // клетка (8, 4), высота 48
        {-8.0f, 8.0f, 0.0f},  // клетка (0, 8), высота 80
        {-8.0f, 8.4f, 4.0f},  // клетка (4, 8), высота 84
        {-8.0f, 8.8f, 8.0f},  // клетка (8, 8), высота 88
    }};
    for (std::size_t index = 0; index < kVertices; ++index) {
        REQUIRE(NearlyEq(mesh->Positions[index].X, expected[index].X));
        REQUIRE(NearlyEq(mesh->Positions[index].Y, expected[index].Y));
        REQUIRE(NearlyEq(mesh->Positions[index].Z, expected[index].Z));
    }

    // Обход в файле — уже после безусловной перестановки второго и третьего
    // индекса в WriteGltf. Writer кладёт (0, 1, 3) и (1, 4, 3), значит на
    // диске обязаны лежать (0, 3, 1) и (1, 3, 4). Записанный без разворота
    // обход дал бы здесь (0, 1, 3) и (1, 4, 3) — землю, видимую только снизу.
    REQUIRE_EQ(mesh->Indices[0], std::uint32_t{0});
    REQUIRE_EQ(mesh->Indices[1], std::uint32_t{3});
    REQUIRE_EQ(mesh->Indices[2], std::uint32_t{1});
    REQUIRE_EQ(mesh->Indices[3], std::uint32_t{1});
    REQUIRE_EQ(mesh->Indices[4], std::uint32_t{3});
    REQUIRE_EQ(mesh->Indices[5], std::uint32_t{4});
    REQUIRE(HasOnlyTopFacingTriangles(*mesh));
}

} // namespace
