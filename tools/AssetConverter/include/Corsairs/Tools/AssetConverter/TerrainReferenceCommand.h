#pragma once

#include "Corsairs/Tools/AssetConverter/TerrainPageBaker.h"
#include "Corsairs/Tools/AssetConverter/TerrainPageMeshWriter.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

struct TerrainReferenceOptions {
    std::filesystem::path Map;
    std::filesystem::path Database;
    std::filesystem::path ClientRoot;
    std::filesystem::path AlphaAtlas;
    std::filesystem::path Output;
    TerrainPageId Page{17u, 21u};
    MapCellRect RequiredPresent{2193u, 2756u, 80u, 47u};
    TerrainBakeOptions Bake{};
    TerrainPageMeshOptions Mesh{};
};

enum class TerrainManifestIssueCode : std::uint32_t {
    INVALID_SCHEMA,
    INVALID_ALGORITHM,
    INVALID_PROVENANCE,
    INVALID_BOUNDS,
    INVALID_TEXTURE_IDS,
    INVALID_SECTION_MASK,
    INVALID_FILE,
    INVALID_HASH,
    INVALID_METRIC,
    BUDGET_EXCEEDED,
};

struct TerrainOutputFile {
    std::filesystem::path ManifestRelativePath;
    std::string Sha256;
    std::uint64_t SizeBytes{0};
};

struct TerrainHashedInputDto {
    std::filesystem::path Path;
    std::string Sha256;
};

struct TerrainTextureSourceDto {
    std::uint8_t TextureId{0};
    std::filesystem::path Path;
    std::string Sha256;
};

struct TerrainManifestSourceDto {
    TerrainHashedInputDto Map;
    TerrainHashedInputDto Database;
    std::filesystem::path ClientRoot;
    TerrainHashedInputDto AlphaAtlas;
    std::vector<TerrainTextureSourceDto> UsedTextures;
};

struct TerrainManifestPageDto {
    TerrainPageId Id;
    MapCellRect SourceCellBounds;
    std::uint32_t PixelsPerCell{0};
    std::uint32_t PixelWidth{0};
    std::uint32_t PixelHeight{0};
    std::array<double, 3> Ambient{};
    std::uint32_t DwTColor{0};
};

struct TerrainManifestSectionPresenceDto {
    std::uint32_t OriginX{0};
    std::uint32_t OriginY{0};
    std::uint32_t Width{0};
    std::uint32_t Height{0};
    std::vector<std::uint8_t> RowMajorMask;
};

struct TerrainManifestFilesDto {
    TerrainOutputFile Height;
    TerrainOutputFile Block;
    TerrainOutputFile Region;
    TerrainOutputFile TerrainMetadata;
    TerrainOutputFile Albedo;
    TerrainOutputFile MeshGltf;
    TerrainOutputFile MeshBin;
};

struct TerrainManifestMetricsDto {
    std::uint64_t PeakRssBytes{0};
    std::uint64_t PeakTextureCacheBytes{0};
    std::uint64_t PeakRgbaRowBytes{0};
    std::uint64_t PngBytes{0};
    std::uint64_t TotalOutputBytes{0};
    double MaxHeightErrorCm{0};
    double RmsHeightErrorCm{0};
    double SharedBoundaryMaxCm{0};
    std::uint64_t AbsentSectionCount{0};
    std::uint64_t UnresolvedLayerCount{0};
};

struct TerrainReferenceManifestDto {
    std::uint32_t SchemaVersion{0};
    std::string AlgorithmVersion;
    TerrainManifestSourceDto Source;
    TerrainManifestPageDto Page;
    MapCellRect RequiredPresentRect;
    std::vector<std::uint8_t> UsedTextureIds;
    TerrainManifestSectionPresenceDto SectionPresence;
    TerrainManifestFilesDto Files;
    TerrainManifestMetricsDto Metrics;
};

struct TerrainManifestIssue {
    TerrainManifestIssueCode Code;
    std::string Field;
    std::string Detail;
};

[[nodiscard]] std::string SerializeTerrainReferenceManifest(
    const TerrainReferenceManifestDto& manifest);

[[nodiscard]] std::optional<TerrainReferenceManifestDto>
ParseTerrainReferenceManifest(
    std::string_view json,
    std::vector<TerrainManifestIssue>& issues);

[[nodiscard]] std::vector<TerrainManifestIssue>
ValidateTerrainReferenceManifest(
    const TerrainReferenceManifestDto& manifest,
    const std::filesystem::path& manifestDirectory,
    const TerrainReferenceOptions& limits);

[[nodiscard]] std::vector<TerrainManifestIssue>
CompareTerrainDeterministicManifests(
    const TerrainReferenceManifestDto& first,
    const std::filesystem::path& firstManifestDirectory,
    const TerrainReferenceManifestDto& second,
    const std::filesystem::path& secondManifestDirectory,
    const TerrainReferenceOptions& limits);

struct TerrainBuiltFile {
    std::filesystem::path RunLeafPath;
    std::string Sha256;
};

struct TerrainBuildFiles {
    TerrainBuiltFile Height;
    TerrainBuiltFile Block;
    TerrainBuiltFile Region;
    TerrainBuiltFile TerrainMetadata;
    TerrainBuiltFile Albedo;
    TerrainBuiltFile MeshGltf;
    TerrainBuiltFile MeshBin;
};

struct TerrainReferenceBuildProducts {
    TerrainBakeResult Bake;
    TerrainPageMeshResult Mesh;
    TerrainManifestSourceDto Source;
    TerrainBuildFiles Files;
};

enum class TerrainPublicationStatus : std::uint32_t {
    OK,
    WRITE_FAILED,
    RECOVERY_REQUIRED,
};

struct TerrainPublicationResult {
    TerrainPublicationStatus Status{TerrainPublicationStatus::WRITE_FAILED};
    std::string Detail;
    std::filesystem::path RecoveryBackup;
    std::string RecoveryCommand;
};

struct TerrainReferenceDependencies {
    std::function<TerrainPublicationResult(
        const TerrainReferenceOptions&)> RecoverBeforeBuild;
    std::function<std::optional<TerrainReferenceBuildProducts>(
        const TerrainReferenceOptions&,
        const std::filesystem::path& runDirectory,
        std::string& detail)> BuildProducts;
    std::function<bool(
        std::span<const std::filesystem::path> productPaths,
        std::string& detail)> DurabilizeRunProducts;
    std::function<TerrainPublicationResult(
        const std::filesystem::path& destination,
        std::string_view json)> AtomicPublish;
};

enum class TerrainReferenceFaultAction : std::uint32_t {
    NONE,
    FAIL,
    CRASH,
};

using TerrainReferenceFaultInjector =
    std::function<TerrainReferenceFaultAction(std::string_view point)>;

// Narrow test seam over the real durable publisher. Production binds the same
// implementation with an empty fault injector.
[[nodiscard]] TerrainPublicationResult
PublishTerrainReferenceManifestForTesting(
    const std::filesystem::path& destination,
    std::string_view json,
    const TerrainReferenceOptions& options,
    const TerrainReferenceFaultInjector& injectFault);

[[nodiscard]] TerrainReferenceDependencies
MakeProductionTerrainReferenceDependencies();

int RunTerrainReference(
    const TerrainReferenceOptions& options,
    const TerrainReferenceDependencies& dependencies,
    std::ostream& output,
    std::ostream& error);

int RunTerrainReference(
    const TerrainReferenceOptions& options,
    std::ostream& output,
    std::ostream& error);

// Аргументы идут после literal subcommand `terrain-reference`.
[[nodiscard]] std::optional<TerrainReferenceOptions>
ParseTerrainReferenceArguments(
    std::span<const std::string_view> arguments,
    std::string& detail);

// Возвращает nullopt только для legacy invocation. Main вызывает эту функцию
// до любого legacy positional parsing или чтения файлов.
[[nodiscard]] std::optional<int> TryRunTerrainReferenceSubcommand(
    std::span<const std::string_view> arguments,
    const TerrainReferenceDependencies& dependencies,
    std::ostream& output,
    std::ostream& error);

[[nodiscard]] std::optional<int> TryRunTerrainReferenceSubcommand(
    std::span<const std::string_view> arguments,
    std::ostream& output,
    std::ostream& error);

} // namespace Corsairs::Tools::AssetConverter
