#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/TerrainPageMeshWriter.h"

#include "TestHarness.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::uint32_t kCellsPerPage = 128u;
constexpr std::uint32_t kStoredSamples = 129u;
constexpr std::size_t kSampleCount = 16641u;

class ScopedDirectory {
public:
    ScopedDirectory() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        _path = std::filesystem::temp_directory_path() /
            std::format("corsairs-rigid-terrain-basis-{}", stamp);
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

AC::MapPageTiles FlatPage(std::uint32_t originX, std::uint32_t originY) {
    AC::MapTile tile{};
    tile.Color = static_cast<std::int16_t>(-1);
    AC::MapPageTiles page;
    page.Cells = {originX, originY, kCellsPerPage, kCellsPerPage};
    page.StoredWidth = kStoredSamples;
    page.StoredHeight = kStoredSamples;
    page.Tiles.assign(kSampleCount, tile);
    page.TilePresent.assign(kSampleCount, 1u);
    return page;
}

struct DecodedMesh {
    std::vector<AC::Vector3> Positions;
    std::vector<std::uint32_t> Indices;
};

std::optional<DecodedMesh> DecodeKnownTerrainBuffer(
    const AC::LgoGeomObj& source,
    const std::filesystem::path& binPath) {
    const auto bytes = AC::ReadWholeFile(binPath);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    const std::size_t positionBytes =
        source.Mesh.Positions.size() * sizeof(AC::Vector3);
    const std::size_t normalBytes =
        source.Mesh.Normals.size() * sizeof(AC::Vector3);
    const std::size_t uvBytes =
        source.Mesh.Texcoords[0].size() * sizeof(AC::Vector2);
    const std::size_t indexBytes =
        source.Mesh.Indices.size() * sizeof(std::uint32_t);
    const std::size_t indexOffset = positionBytes + normalBytes + uvBytes;
    if (indexOffset > bytes->size() ||
        indexBytes > bytes->size() - indexOffset) {
        return std::nullopt;
    }

    DecodedMesh mesh;
    mesh.Positions.resize(source.Mesh.Positions.size());
    mesh.Indices.resize(source.Mesh.Indices.size());
    std::memcpy(mesh.Positions.data(), bytes->data(), positionBytes);
    std::memcpy(mesh.Indices.data(), bytes->data() + indexOffset, indexBytes);
    return mesh;
}

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

struct WrittenTerrain {
    AC::TerrainPageMeshResult Result;
    AC::LgoGeomObj Source;
    std::optional<DecodedMesh> Mesh;
    std::string GltfText;
};

WrittenTerrain WriteWithCapturedSource(
    const AC::MapPageTiles& page,
    AC::TerrainPageId pageId,
    const std::filesystem::path& output,
    AC::TerrainPageCoordinateProfile coordinateProfile,
    std::string& detail) {
    WrittenTerrain written;
    AC::TerrainPageMeshOptions options;
    options.CoordinateProfile = coordinateProfile;
    options.TestOnlyWriteGltf =
        [&](const AC::LgoGeomObj& object,
            const std::filesystem::path& gltfPath,
            std::string& writeDetail,
            const AC::GltfAssetMetadata& assetMetadata) {
            written.Source = object;
            return AC::WriteGltf(
                object, gltfPath, writeDetail, {}, nullptr,
                AC::GltfCoordinateProfile::Generic,
                AC::GltfSkinPolicy::Preserve, assetMetadata);
        };
    written.Result = AC::WriteTerrainPageMesh(
        page, pageId, output, options, detail);
    if (written.Result.Ok) {
        written.Mesh = DecodeKnownTerrainBuffer(
            written.Source, written.Result.BinPath);
        const auto text = AC::ReadWholeFile(written.Result.GltfPath);
        if (text.has_value()) {
            written.GltfText.assign(
                reinterpret_cast<const char*>(text->data()), text->size());
        }
    }
    return written;
}

CORSAIRS_TEST(TerrainPageMeshWriter_RigidQIsOptInAndKeepsTask8Default) {
    ScopedDirectory temporary;
    REQUIRE(temporary.Ready());
    const AC::MapPageTiles page = FlatPage(2176u, 2688u);
    std::string detail;

    AC::TerrainPageMeshOptions defaults;
    REQUIRE_EQ(static_cast<std::uint32_t>(defaults.CoordinateProfile),
               static_cast<std::uint32_t>(
                   AC::TerrainPageCoordinateProfile::Task8Legacy));
    const WrittenTerrain legacy = WriteWithCapturedSource(
        page, {17u, 21u}, temporary.Path() / "legacy",
        AC::TerrainPageCoordinateProfile::Task8Legacy, detail);
    REQUIRE(legacy.Result.Ok);
    REQUIRE(legacy.Mesh.has_value());
    REQUIRE_EQ(legacy.Result.ActorWorldXcm, 217600.0);
    REQUIRE_EQ(legacy.Result.ActorWorldYcm, -268800.0);
    REQUIRE_EQ(legacy.Mesh->Positions[1].X, 1.0f);
    REQUIRE_EQ(legacy.Mesh->Positions[1].Z, 0.0f);
    REQUIRE_EQ(legacy.Mesh->Positions[129].X, 0.0f);
    REQUIRE_EQ(legacy.Mesh->Positions[129].Z, -1.0f);
    REQUIRE(HasOnlyTopFacingTriangles(*legacy.Mesh));
    REQUIRE(legacy.GltfText.find("corsairsTerrainCoordinateProfile") ==
            std::string::npos);

    const WrittenTerrain rigid = WriteWithCapturedSource(
        page, {17u, 21u}, temporary.Path() / "rigid-q",
        AC::TerrainPageCoordinateProfile::RigidQ, detail);
    REQUIRE(rigid.Result.Ok);
    REQUIRE(rigid.Mesh.has_value());
    REQUIRE_EQ(rigid.Result.ActorWorldXcm, -268800.0);
    REQUIRE_EQ(rigid.Result.ActorWorldYcm, 217600.0);
    REQUIRE_EQ(rigid.Mesh->Positions[1].X, 0.0f);
    REQUIRE_EQ(rigid.Mesh->Positions[1].Z, 1.0f);
    REQUIRE_EQ(rigid.Mesh->Positions[129].X, -1.0f);
    REQUIRE_EQ(rigid.Mesh->Positions[129].Z, 0.0f);
    REQUIRE(HasOnlyTopFacingTriangles(*rigid.Mesh));
    REQUIRE(rigid.GltfText.find(
        R"("extras":{"corsairsTerrainCoordinateProfile":"RigidQ"})") !=
            std::string::npos);

    REQUIRE_EQ(rigid.Mesh->Indices[0], legacy.Mesh->Indices[0]);
    REQUIRE_EQ(rigid.Mesh->Indices[1], legacy.Mesh->Indices[2]);
    REQUIRE_EQ(rigid.Mesh->Indices[2], legacy.Mesh->Indices[1]);
}

} // namespace
