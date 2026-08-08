#include "Corsairs/Tools/AssetConverter/TerrainPageMeshWriter.h"

#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/TerrainPageBaker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::uint32_t kCellsPerPage = 128u;
constexpr std::uint32_t kStoredSamples = 129u;
constexpr std::size_t kSampleCount = 16641u;
constexpr std::uint32_t kInvalidVertex =
    std::numeric_limits<std::uint32_t>::max();

struct SourceVertex {
    std::uint32_t X{0};
    std::uint32_t Y{0};
    double HeightCm{0};
};

struct SourceTriangle {
    std::uint32_t A{0};
    std::uint32_t B{0};
    std::uint32_t C{0};
};

struct CellTriangleRange {
    std::size_t First{0};
    std::size_t Count{0};
};

struct BuiltPageMesh {
    std::uint32_t Step{0};
    std::vector<SourceVertex> Vertices;
    std::vector<SourceTriangle> Triangles;
    std::vector<CellTriangleRange> CellTriangles;
    std::vector<std::uint32_t> VertexAtSample;
};

TerrainMeshError InvalidError() {
    const double infinity = std::numeric_limits<double>::infinity();
    return TerrainMeshError{infinity, infinity, infinity, 0u};
}

bool IsSupportedStep(std::uint32_t step) {
    return step == 4u || step == 2u || step == 1u;
}

bool ValidatePage(const MapPageTiles& page, std::string& detail) {
    if (page.Cells.Width != kCellsPerPage ||
        page.Cells.Height != kCellsPerPage) {
        detail = "terrain page Cells должен быть ровно 128x128";
        return false;
    }
    if (page.StoredWidth != kStoredSamples ||
        page.StoredHeight != kStoredSamples) {
        detail = "terrain page stored dimensions должны быть ровно 129x129";
        return false;
    }
    if (page.Tiles.size() != kSampleCount ||
        page.TilePresent.size() != kSampleCount) {
        detail = "terrain page должен содержать ровно 16641 tile/presence samples";
        return false;
    }
    for (std::uint32_t y = 0u; y < kCellsPerPage; ++y) {
        for (std::uint32_t x = 0u; x < kCellsPerPage; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * kStoredSamples + x;
            if (page.TilePresent[index] == 0u) {
                detail = std::format(
                    "отсутствует обязательный owned terrain sample ({},{})", x, y);
                return false;
            }
        }
    }
    detail.clear();
    return true;
}

double SampleHeightCm(const MapPageTiles& page,
                      std::uint32_t x,
                      std::uint32_t y) {
    const std::size_t index =
        static_cast<std::size_t>(y) * kStoredSamples + x;
    return ResolveLegacyTerrainCornerSample(
        page.Tiles[index], page.TilePresent[index] != 0u).HeightCm;
}

void AddTriangle(BuiltPageMesh& mesh,
                 std::uint32_t a,
                 std::uint32_t b,
                 std::uint32_t c) {
    mesh.Triangles.push_back({a, b, c});
}

void AddFan(BuiltPageMesh& mesh,
            std::uint32_t anchor,
            const std::vector<std::uint32_t>& chain) {
    for (std::size_t index = 0u; index + 1u < chain.size(); ++index) {
        AddTriangle(mesh, anchor, chain[index], chain[index + 1u]);
    }
}

std::optional<BuiltPageMesh> BuildPageMesh(const MapPageTiles& page,
                                           std::uint32_t step,
                                           std::string& detail) {
    if (!IsSupportedStep(step)) {
        detail = "terrain mesh step должен быть одним из 4, 2, 1";
        return std::nullopt;
    }

    BuiltPageMesh mesh;
    mesh.Step = step;
    mesh.VertexAtSample.assign(kSampleCount, kInvalidVertex);
    for (std::uint32_t y = 0u; y <= kCellsPerPage; ++y) {
        for (std::uint32_t x = 0u; x <= kCellsPerPage; ++x) {
            const bool outer = x == 0u || y == 0u ||
                x == kCellsPerPage || y == kCellsPerPage;
            if (!outer && (x % step != 0u || y % step != 0u)) {
                continue;
            }
            if (mesh.Vertices.size() >= kInvalidVertex) {
                detail = "terrain mesh vertex count не помещается в uint32_t";
                return std::nullopt;
            }
            const std::size_t sampleIndex =
                static_cast<std::size_t>(y) * kStoredSamples + x;
            mesh.VertexAtSample[sampleIndex] =
                static_cast<std::uint32_t>(mesh.Vertices.size());
            mesh.Vertices.push_back({x, y, SampleHeightCm(page, x, y)});
        }
    }

    const auto vertex = [&mesh](std::uint32_t x, std::uint32_t y) {
        return mesh.VertexAtSample[
            static_cast<std::size_t>(y) * kStoredSamples + x];
    };
    const std::uint32_t coarseCells = kCellsPerPage / step;
    mesh.CellTriangles.resize(
        static_cast<std::size_t>(coarseCells) * coarseCells);

    for (std::uint32_t cellY = 0u; cellY < coarseCells; ++cellY) {
        for (std::uint32_t cellX = 0u; cellX < coarseCells; ++cellX) {
            const std::uint32_t x0 = cellX * step;
            const std::uint32_t y0 = cellY * step;
            const std::uint32_t x1 = x0 + step;
            const std::uint32_t y1 = y0 + step;
            const std::uint32_t topLeft = vertex(x0, y0);
            const std::uint32_t topRight = vertex(x1, y0);
            const std::uint32_t bottomLeft = vertex(x0, y1);
            const std::uint32_t bottomRight = vertex(x1, y1);
            if (topLeft == kInvalidVertex || topRight == kInvalidVertex ||
                bottomLeft == kInvalidVertex || bottomRight == kInvalidVertex) {
                detail = "terrain mesh не содержит coarse corner vertex";
                return std::nullopt;
            }

            CellTriangleRange& range = mesh.CellTriangles[
                static_cast<std::size_t>(cellY) * coarseCells + cellX];
            range.First = mesh.Triangles.size();
            const bool top = y0 == 0u;
            const bool bottom = y1 == kCellsPerPage;
            const bool left = x0 == 0u;
            const bool right = x1 == kCellsPerPage;
            const bool corner = (top || bottom) && (left || right);

            if (step == 1u || (!top && !bottom && !left && !right)) {
                AddTriangle(mesh, topLeft, bottomLeft, topRight);
                AddTriangle(mesh, topRight, bottomLeft, bottomRight);
            }
            else if (corner) {
                std::vector<std::uint32_t> chain;
                chain.reserve(static_cast<std::size_t>(step) * 2u + 1u);
                if (top && left) {
                    chain.push_back(topRight);
                    for (std::uint32_t x = x1; x-- > x0;) {
                        chain.push_back(vertex(x, y0));
                    }
                    for (std::uint32_t y = y0 + 1u; y <= y1; ++y) {
                        chain.push_back(vertex(x0, y));
                    }
                    AddFan(mesh, bottomRight, chain);
                }
                else if (top && right) {
                    chain.push_back(bottomRight);
                    for (std::uint32_t y = y1; y-- > y0;) {
                        chain.push_back(vertex(x1, y));
                    }
                    for (std::uint32_t x = x1; x-- > x0;) {
                        chain.push_back(vertex(x, y0));
                    }
                    AddFan(mesh, bottomLeft, chain);
                }
                else if (bottom && left) {
                    chain.push_back(topLeft);
                    for (std::uint32_t y = y0 + 1u; y <= y1; ++y) {
                        chain.push_back(vertex(x0, y));
                    }
                    for (std::uint32_t x = x0 + 1u; x <= x1; ++x) {
                        chain.push_back(vertex(x, y1));
                    }
                    AddFan(mesh, topRight, chain);
                }
                else {
                    chain.push_back(bottomLeft);
                    for (std::uint32_t x = x0 + 1u; x <= x1; ++x) {
                        chain.push_back(vertex(x, y1));
                    }
                    for (std::uint32_t y = y1; y-- > y0;) {
                        chain.push_back(vertex(x1, y));
                    }
                    AddFan(mesh, topLeft, chain);
                }
            }
            else if (top) {
                for (std::uint32_t index = 0u; index < step; ++index) {
                    AddTriangle(mesh, bottomLeft,
                                vertex(x0 + index + 1u, y0),
                                vertex(x0 + index, y0));
                }
                AddTriangle(mesh, topRight, bottomLeft, bottomRight);
            }
            else if (bottom) {
                for (std::uint32_t index = 0u; index < step; ++index) {
                    AddTriangle(mesh, topRight,
                                vertex(x0 + index, y1),
                                vertex(x0 + index + 1u, y1));
                }
                AddTriangle(mesh, topLeft, bottomLeft, topRight);
            }
            else if (left) {
                for (std::uint32_t index = 0u; index < step; ++index) {
                    AddTriangle(mesh, topRight,
                                vertex(x0, y0 + index),
                                vertex(x0, y0 + index + 1u));
                }
                AddTriangle(mesh, topRight, bottomLeft, bottomRight);
            }
            else {
                for (std::uint32_t index = 0u; index < step; ++index) {
                    AddTriangle(mesh, bottomLeft,
                                vertex(x1, y0 + index + 1u),
                                vertex(x1, y0 + index));
                }
                AddTriangle(mesh, topLeft, bottomLeft, topRight);
            }
            range.Count = mesh.Triangles.size() - range.First;
        }
    }

    detail.clear();
    return mesh;
}

std::optional<double> HeightFromTriangles(const BuiltPageMesh& mesh,
                                          std::uint32_t sampleX,
                                          std::uint32_t sampleY) {
    const std::size_t sampleIndex =
        static_cast<std::size_t>(sampleY) * kStoredSamples + sampleX;
    const std::uint32_t directVertex = mesh.VertexAtSample[sampleIndex];
    if (directVertex != kInvalidVertex) {
        return mesh.Vertices[directVertex].HeightCm;
    }

    const std::uint32_t coarseCells = kCellsPerPage / mesh.Step;
    const std::uint32_t cellX = std::min(sampleX / mesh.Step, coarseCells - 1u);
    const std::uint32_t cellY = std::min(sampleY / mesh.Step, coarseCells - 1u);
    const CellTriangleRange& range = mesh.CellTriangles[
        static_cast<std::size_t>(cellY) * coarseCells + cellX];
    constexpr double epsilon = 1e-12;
    const double pointX = sampleX;
    const double pointY = sampleY;

    for (std::size_t triangleIndex = range.First;
         triangleIndex < range.First + range.Count;
         ++triangleIndex) {
        const SourceTriangle& triangle = mesh.Triangles[triangleIndex];
        const SourceVertex& a = mesh.Vertices[triangle.A];
        const SourceVertex& b = mesh.Vertices[triangle.B];
        const SourceVertex& c = mesh.Vertices[triangle.C];
        const double denominator =
            (static_cast<double>(b.Y) - c.Y) *
                (static_cast<double>(a.X) - c.X) +
            (static_cast<double>(c.X) - b.X) *
                (static_cast<double>(a.Y) - c.Y);
        if (denominator == 0.0) {
            continue;
        }
        const double weightA =
            ((static_cast<double>(b.Y) - c.Y) *
                 (pointX - static_cast<double>(c.X)) +
             (static_cast<double>(c.X) - b.X) *
                 (pointY - static_cast<double>(c.Y))) /
            denominator;
        const double weightB =
            ((static_cast<double>(c.Y) - a.Y) *
                 (pointX - static_cast<double>(c.X)) +
             (static_cast<double>(a.X) - c.X) *
                 (pointY - static_cast<double>(c.Y))) /
            denominator;
        const double weightC = 1.0 - weightA - weightB;
        if (weightA >= -epsilon && weightB >= -epsilon && weightC >= -epsilon &&
            weightA <= 1.0 + epsilon && weightB <= 1.0 + epsilon &&
            weightC <= 1.0 + epsilon) {
            return weightA * a.HeightCm +
                   weightB * b.HeightCm +
                   weightC * c.HeightCm;
        }
    }
    return std::nullopt;
}

std::optional<TerrainMeshError> EvaluateBuiltMesh(const MapPageTiles& page,
                                                  const BuiltPageMesh& mesh) {
    TerrainMeshError error;
    double squaredError = 0.0;
    for (std::uint32_t y = 0u; y <= kCellsPerPage; ++y) {
        for (std::uint32_t x = 0u; x <= kCellsPerPage; ++x) {
            const auto generated = HeightFromTriangles(mesh, x, y);
            if (!generated.has_value()) {
                return std::nullopt;
            }
            const double source = SampleHeightCm(page, x, y);
            const double absolute = std::abs(source - *generated);
            error.MaxAbsCm = std::max(error.MaxAbsCm, absolute);
            squaredError += absolute * absolute;
            if (x == 0u || y == 0u ||
                x == kCellsPerPage || y == kCellsPerPage) {
                error.SharedBoundaryMaxCm =
                    std::max(error.SharedBoundaryMaxCm, absolute);
            }
            ++error.Samples;
        }
    }
    error.RmsCm = std::sqrt(squaredError / static_cast<double>(error.Samples));
    return error;
}

bool IsFiniteNonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

std::filesystem::file_status SymlinkStatus(
    const std::filesystem::path& path, std::error_code& error) {
    std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found &&
        error == std::errc::no_such_file_or_directory) {
        error.clear();
    }
    return status;
}

bool RemoveAttemptRegularFile(const std::filesystem::path& path,
                              std::string& detail) {
    std::error_code statusError;
    const std::filesystem::file_status status =
        SymlinkStatus(path, statusError);
    if (statusError) {
        detail = std::format("не удалось проверить {}: {}",
                             path.generic_string(), statusError.message());
        return false;
    }
    if (status.type() == std::filesystem::file_type::not_found ||
        status.type() != std::filesystem::file_type::regular) {
        return true;
    }

    std::error_code removeError;
    const bool removed = std::filesystem::remove(path, removeError);
    if (removeError || !removed) {
        detail = std::format("не удалось удалить {}: {}",
                             path.generic_string(),
                             removeError ? removeError.message()
                                         : "filesystem::remove вернул false");
        return false;
    }
    statusError.clear();
    const std::filesystem::file_status after =
        SymlinkStatus(path, statusError);
    if (statusError ||
        after.type() != std::filesystem::file_type::not_found) {
        detail = std::format("удаление {} не подтверждено: {}",
                             path.generic_string(),
                             statusError ? statusError.message()
                                         : "path всё ещё существует");
        return false;
    }
    return true;
}

std::filesystem::path OutputDirectoryLeafPath(
    const std::filesystem::path& path) {
    std::filesystem::path leaf = path;
    while (!leaf.empty() && leaf != leaf.root_path() &&
           (leaf.filename().empty() || leaf.filename() == ".")) {
        const std::filesystem::path parent = leaf.parent_path();
        if (parent.empty() || parent == leaf) {
            break;
        }
        leaf = parent;
    }
    return leaf;
}

bool PreparePhysicalOutputDirectory(const std::filesystem::path& path,
                                    std::string& detail) {
    const std::filesystem::path physicalLeaf = OutputDirectoryLeafPath(path);
    std::error_code error;
    std::filesystem::file_status status = SymlinkStatus(physicalLeaf, error);
    if (error) {
        detail = std::format("не удалось проверить outputDirectory {}: {}",
                             path.generic_string(), error.message());
        return false;
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        std::filesystem::create_directories(physicalLeaf, error);
        if (error) {
            detail = std::format("не удалось создать outputDirectory {}: {}",
                                 path.generic_string(), error.message());
            return false;
        }
        status = SymlinkStatus(physicalLeaf, error);
        if (error) {
            detail = std::format(
                "не удалось подтвердить созданный outputDirectory {}: {}",
                path.generic_string(), error.message());
            return false;
        }
    }

    std::error_code directoryError;
    const bool directory =
        std::filesystem::is_directory(physicalLeaf, directoryError);
    if (directoryError ||
        status.type() != std::filesystem::file_type::directory || !directory) {
        detail = std::format(
            "outputDirectory должен быть physical directory без symlink: {}{}{}",
            path.generic_string(),
            directoryError ? ": " : "",
            directoryError ? directoryError.message() : "");
        return false;
    }
    return true;
}

bool RequireMissingOutputLeaf(const std::filesystem::path& path,
                              std::string& detail) {
    std::error_code error;
    const std::filesystem::file_status status = SymlinkStatus(path, error);
    if (error) {
        detail = std::format("не удалось проверить terrain mesh target {}: {}",
                             path.generic_string(), error.message());
        return false;
    }
    if (status.type() != std::filesystem::file_type::not_found) {
        detail = std::format("terrain mesh target уже существует: {}",
                             path.generic_string());
        return false;
    }
    return true;
}

bool IsPhysicalRegularFile(const std::filesystem::path& path,
                           std::string& detail) {
    std::error_code error;
    const std::filesystem::file_status status = SymlinkStatus(path, error);
    if (error) {
        detail = std::format("не удалось проверить записанный target {}: {}",
                             path.generic_string(), error.message());
        return false;
    }
    if (status.type() != std::filesystem::file_type::regular) {
        detail = std::format("записанный target не является physical regular file: {}",
                             path.generic_string());
        return false;
    }
    return true;
}

LgoGeomObj MakeGltfMesh(const BuiltPageMesh& mesh) {
    LgoGeomObj object;
    object.Version = 0x1004u;
    object.Mesh.Positions.reserve(mesh.Vertices.size());
    object.Mesh.Normals.reserve(mesh.Vertices.size());
    object.Mesh.Texcoords[0].reserve(mesh.Vertices.size());
    for (const SourceVertex& vertex : mesh.Vertices) {
        object.Mesh.Positions.push_back(Vector3{
            static_cast<float>(vertex.X),
            -static_cast<float>(vertex.Y),
            static_cast<float>(vertex.HeightCm / 100.0),
        });
        object.Mesh.Normals.push_back(Vector3{0.0f, 0.0f, 1.0f});
        object.Mesh.Texcoords[0].push_back(Vector2{
            static_cast<float>(vertex.X) / static_cast<float>(kCellsPerPage),
            static_cast<float>(vertex.Y) / static_cast<float>(kCellsPerPage),
        });
    }
    object.Mesh.Indices.reserve(mesh.Triangles.size() * 3u);
    for (const SourceTriangle& triangle : mesh.Triangles) {
        object.Mesh.Indices.push_back(triangle.A);
        object.Mesh.Indices.push_back(triangle.B);
        object.Mesh.Indices.push_back(triangle.C);
    }
    object.Mesh.Header.Fvf =
        static_cast<std::uint32_t>(FvfFlag::NORMAL) |
        static_cast<std::uint32_t>(FvfFlag::TEX1);
    object.Mesh.Header.PtType = 4u;
    object.Mesh.Header.VertexNum =
        static_cast<std::uint32_t>(object.Mesh.Positions.size());
    object.Mesh.Header.IndexNum =
        static_cast<std::uint32_t>(object.Mesh.Indices.size());
    object.Mesh.Header.SubsetNum = 1u;
    object.Mesh.Subsets.push_back(SubsetInfo{
        static_cast<std::uint32_t>(mesh.Triangles.size()),
        0u,
        static_cast<std::uint32_t>(object.Mesh.Positions.size()),
        0u,
    });
    return object;
}

} // namespace

TerrainMeshError EvaluateTerrainPageStep(const MapPageTiles& page,
                                         std::uint32_t step) {
    std::string detail;
    if (!ValidatePage(page, detail) || !IsSupportedStep(step)) {
        return InvalidError();
    }
    auto mesh = BuildPageMesh(page, step, detail);
    if (!mesh.has_value()) {
        return InvalidError();
    }
    const auto error = EvaluateBuiltMesh(page, *mesh);
    return error.value_or(InvalidError());
}

TerrainPageMeshResult WriteTerrainPageMesh(
    const MapPageTiles& page,
    TerrainPageId pageId,
    const std::filesystem::path& outputDirectory,
    const TerrainPageMeshOptions& options,
    std::string& detail) {
    detail.clear();
    TerrainPageMeshResult result;
    std::filesystem::path gltfPath;
    std::filesystem::path binPath;
    bool writeAttemptStarted = false;

    auto fail = [&](std::string message) {
        const std::string cause = detail.empty() ? std::move(message) : detail;
        std::string cleanupDetail;
        if (writeAttemptStarted) {
            const bool gltfRemoved =
                RemoveAttemptRegularFile(gltfPath, cleanupDetail);
            std::string binCleanupDetail;
            const bool binRemoved =
                RemoveAttemptRegularFile(binPath, binCleanupDetail);
            if (!gltfRemoved || !binRemoved) {
                detail = std::format(
                    "RECOVERY_REQUIRED: не удалось очистить terrain mesh pair; "
                    "cause={}; gltf={}; bin={}; cleanup={}{}{}",
                    cause,
                    gltfPath.generic_string(),
                    binPath.generic_string(),
                    cleanupDetail,
                    cleanupDetail.empty() || binCleanupDetail.empty() ? "" : "; ",
                    binCleanupDetail);
            }
            else {
                detail = cause;
            }
        }
        else {
            detail = cause;
        }
        result = {};
        return result;
    };

    if (!ValidatePage(page, detail)) {
        return fail("некорректная terrain page");
    }
    if (!IsFiniteNonnegative(options.MaxAbsCm) ||
        !IsFiniteNonnegative(options.MaxRmsCm) ||
        !IsFiniteNonnegative(options.MaxSharedBoundaryCm)) {
        return fail("terrain mesh error limits должны быть finite и nonnegative");
    }

    constexpr std::uint64_t cells = kCellsPerPage;
    const std::uint64_t expectedX =
        static_cast<std::uint64_t>(pageId.X) * cells;
    const std::uint64_t expectedY =
        static_cast<std::uint64_t>(pageId.Y) * cells;
    if (expectedX > std::numeric_limits<std::uint32_t>::max() ||
        expectedY > std::numeric_limits<std::uint32_t>::max() ||
        page.Cells.X != expectedX || page.Cells.Y != expectedY) {
        return fail("terrain page origin не соответствует pageId*128");
    }

    std::optional<BuiltPageMesh> selectedMesh;
    TerrainMeshError selectedError;
    for (const std::uint32_t step : std::array<std::uint32_t, 3>{4u, 2u, 1u}) {
        auto candidate = BuildPageMesh(page, step, detail);
        if (!candidate.has_value()) {
            return fail("не удалось построить terrain mesh candidate");
        }
        const auto candidateError = EvaluateBuiltMesh(page, *candidate);
        if (!candidateError.has_value()) {
            return fail("не удалось оценить terrain mesh candidate");
        }
        if (candidateError->MaxAbsCm <= options.MaxAbsCm &&
            candidateError->RmsCm <= options.MaxRmsCm &&
            candidateError->SharedBoundaryMaxCm <=
                options.MaxSharedBoundaryCm) {
            selectedError = *candidateError;
            selectedMesh = std::move(candidate);
            break;
        }
    }
    if (!selectedMesh.has_value()) {
        return fail("ни один terrain mesh step не прошёл error gates");
    }

    if (outputDirectory.empty()) {
        return fail("outputDirectory для terrain mesh пуст");
    }
    if (!PreparePhysicalOutputDirectory(outputDirectory, detail)) {
        return fail("не удалось подготовить physical outputDirectory");
    }

    gltfPath = outputDirectory /
        std::format("garner.terrain_{:02}_{:02}.gltf", pageId.X, pageId.Y);
    binPath = outputDirectory /
        std::format("garner.terrain_{:02}_{:02}.bin", pageId.X, pageId.Y);
    std::string gltfValidationDetail;
    std::string binValidationDetail;
    const bool gltfMissing =
        RequireMissingOutputLeaf(gltfPath, gltfValidationDetail);
    const bool binMissing =
        RequireMissingOutputLeaf(binPath, binValidationDetail);
    if (!gltfMissing || !binMissing) {
        detail = std::format(
            "terrain mesh pair должен быть полностью свободен: {}{}{}",
            gltfValidationDetail,
            gltfValidationDetail.empty() || binValidationDetail.empty() ? "" : "; ",
            binValidationDetail);
        return fail("terrain mesh target validation failed");
    }

    writeAttemptStarted = true;
    const LgoGeomObj object = MakeGltfMesh(*selectedMesh);
    const GltfStatus status = options.TestOnlyWriteGltf
        ? options.TestOnlyWriteGltf(object, gltfPath, detail)
        : WriteGltf(object, gltfPath, detail);
    if (status != GltfStatus::OK) {
        return fail("WriteGltf не записал terrain mesh pair");
    }
    std::string gltfCheckDetail;
    std::string binCheckDetail;
    const bool gltfRegular = IsPhysicalRegularFile(gltfPath, gltfCheckDetail);
    const bool binRegular = IsPhysicalRegularFile(binPath, binCheckDetail);
    if (!gltfRegular || !binRegular) {
        detail = std::format(
            "WriteGltf не оставил полный physical regular terrain mesh pair: "
            "{}{}{}",
            gltfCheckDetail,
            gltfCheckDetail.empty() || binCheckDetail.empty() ? "" : "; ",
            binCheckDetail);
        return fail("WriteGltf не оставил полный regular terrain mesh pair");
    }

    result.Ok = true;
    result.Step = selectedMesh->Step;
    result.Error = selectedError;
    result.GltfPath = gltfPath;
    result.BinPath = binPath;
    result.ActorWorldXcm = static_cast<double>(page.Cells.X) * 100.0;
    result.ActorWorldYcm = -static_cast<double>(page.Cells.Y) * 100.0;
    detail.clear();
    return result;
}

} // namespace Corsairs::Tools::AssetConverter
