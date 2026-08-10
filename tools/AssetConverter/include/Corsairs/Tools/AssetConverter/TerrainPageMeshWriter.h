#pragma once

#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/TerrainPage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Corsairs::Tools::AssetConverter {

struct TerrainMeshError {
    double MaxAbsCm{0};
    double RmsCm{0};
    double SharedBoundaryMaxCm{0};
    std::size_t Samples{0};
};

[[nodiscard]] TerrainMeshError EvaluateTerrainPageStep(
    const MapPageTiles& page, std::uint32_t step);

enum class TerrainPageCoordinateProfile : std::uint8_t {
    // Task8 production contract: local=(x,-y), actor=(pageX,-pageY).
    Task8Legacy,
    // Rigid scene basis Q: local=(-y,x), actor=(-pageY,pageX).
    RigidQ,
};

struct TerrainPageMeshOptions {
    double MaxAbsCm{5.0};
    double MaxRmsCm{2.0};
    double MaxSharedBoundaryCm{0.0};
    // По умолчанию — историческое зеркало, поворот выбирается ключом.
    // Так задумано: контракт закреплён тестами RigidQIsOptInAndKeepsTask8Default
    // и DefaultRemainsTask8Legacy.
    //
    // Ловушка тут всё же есть: сцена целиком живёт в повороте Q, и страница,
    // испечённая без ключа, встанет зеркально относительно города. Пока у
    // зеркала остаются потребители, снимать opt-in нельзя; когда их не
    // останется — умолчание стоит перевернуть вместе с обоими тестами.
    TerrainPageCoordinateProfile CoordinateProfile{
        TerrainPageCoordinateProfile::Task8Legacy};
    // Test-only paired writer; empty in production. Its status is advisory:
    // physical pair validation and cleanup remain authoritative.
    std::function<GltfStatus(
        const LgoGeomObj&, const std::filesystem::path&, std::string&,
        const GltfAssetMetadata&)>
        TestOnlyWriteGltf;
};

struct TerrainPageMeshResult {
    bool Ok{false};
    std::uint32_t Step{0};
    TerrainMeshError Error;
    std::filesystem::path GltfPath;
    std::filesystem::path BinPath;
    double ActorWorldXcm{0};
    double ActorWorldYcm{0};
};

[[nodiscard]] TerrainPageMeshResult WriteTerrainPageMesh(
    const MapPageTiles& page,
    TerrainPageId pageId,
    const std::filesystem::path& outputDirectory,
    const TerrainPageMeshOptions& options,
    std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
