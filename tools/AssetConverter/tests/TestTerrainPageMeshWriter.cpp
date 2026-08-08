#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"
#include "Corsairs/Tools/AssetConverter/TerrainPageBaker.h"
#include "Corsairs/Tools/AssetConverter/TerrainPageMeshWriter.h"

#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::uint32_t kCellsPerPage = 128u;
constexpr std::uint32_t kStoredSamples = 129u;
constexpr std::size_t kSampleCount = 16641u;

class ScopedTestDirectory {
public:
    explicit ScopedTestDirectory(std::string_view label) {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::uint32_t attempt = 0; attempt < 32u; ++attempt) {
            const std::filesystem::path candidate =
                std::filesystem::temp_directory_path() /
                std::format("corsairs-terrain-mesh-{}-{}-{}", label, stamp, attempt);
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                _path = candidate;
                _owned = true;
                break;
            }
            if (error != std::errc::file_exists) {
                break;
            }
        }
    }

    ~ScopedTestDirectory() {
        if (_owned) {
            std::error_code ignored;
            std::filesystem::remove_all(_path, ignored);
        }
    }

    ScopedTestDirectory(const ScopedTestDirectory&) = delete;
    ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;

    [[nodiscard]] bool Ready() const noexcept {
        return _owned;
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return _path;
    }

private:
    std::filesystem::path _path;
    bool _owned{false};
};

AC::MapTile HeightTile(std::int8_t heightRaw = 0) {
    AC::MapTile tile{};
    tile.Color = static_cast<std::int16_t>(-1);
    tile.Height = heightRaw;
    return tile;
}

AC::MapPageTiles FlatPage(std::uint32_t originX = 0u,
                          std::uint32_t originY = 0u) {
    AC::MapPageTiles page;
    page.Cells = {originX, originY, kCellsPerPage, kCellsPerPage};
    page.StoredWidth = kStoredSamples;
    page.StoredHeight = kStoredSamples;
    page.Tiles.assign(kSampleCount, HeightTile());
    page.TilePresent.assign(kSampleCount, 1u);
    return page;
}

AC::MapPageTiles StepTwoWavePage() {
    AC::MapPageTiles page = FlatPage();
    constexpr std::array<std::int8_t, 4> wave{0, 1, 2, 1};
    for (std::uint32_t y = 0; y < kStoredSamples; ++y) {
        for (std::uint32_t x = 0; x < kStoredSamples; ++x) {
            page.Tiles[static_cast<std::size_t>(y) * kStoredSamples + x].Height =
                wave[x % wave.size()];
        }
    }
    return page;
}

std::optional<std::string> ReadText(const std::filesystem::path& path) {
    const auto bytes = AC::ReadWholeFile(path);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

std::optional<std::size_t> MatchingDelimiter(std::string_view text,
                                             std::size_t opening,
                                             char open,
                                             char close) {
    std::size_t depth = 0u;
    bool inString = false;
    bool escaped = false;
    for (std::size_t index = opening; index < text.size(); ++index) {
        const char value = text[index];
        if (inString) {
            if (escaped) {
                escaped = false;
            }
            else if (value == '\\') {
                escaped = true;
            }
            else if (value == '"') {
                inString = false;
            }
            continue;
        }
        if (value == '"') {
            inString = true;
        }
        else if (value == open) {
            ++depth;
        }
        else if (value == close) {
            if (depth == 0u) {
                return std::nullopt;
            }
            --depth;
            if (depth == 0u) {
                return index;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> JsonArray(std::string_view text,
                                          std::string_view key) {
    const std::string token = std::format("\"{}\":[", key);
    const std::size_t tokenPosition = text.find(token);
    if (tokenPosition == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t opening = tokenPosition + token.size() - 1u;
    const auto closing = MatchingDelimiter(text, opening, '[', ']');
    if (!closing.has_value()) {
        return std::nullopt;
    }
    return text.substr(opening + 1u, *closing - opening - 1u);
}

std::vector<std::string_view> JsonObjectElements(std::string_view array) {
    std::vector<std::string_view> result;
    std::size_t cursor = 0u;
    while (cursor < array.size()) {
        const std::size_t opening = array.find('{', cursor);
        if (opening == std::string_view::npos) {
            break;
        }
        const auto closing = MatchingDelimiter(array, opening, '{', '}');
        if (!closing.has_value()) {
            return {};
        }
        result.push_back(array.substr(opening, *closing - opening + 1u));
        cursor = *closing + 1u;
    }
    return result;
}

std::optional<std::uint64_t> JsonUnsigned(std::string_view object,
                                          std::string_view key) {
    const std::string token = std::format("\"{}\":", key);
    const std::size_t position = object.find(token);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    const char* begin = object.data() + position + token.size();
    const char* end = object.data() + object.size();
    std::uint64_t value = 0u;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{}) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string_view> JsonString(std::string_view object,
                                           std::string_view key) {
    const std::string token = std::format("\"{}\":\"", key);
    const std::size_t position = object.find(token);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t begin = position + token.size();
    const std::size_t end = object.find('"', begin);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return object.substr(begin, end - begin);
}

struct BufferViewInfo {
    std::size_t Offset{0};
    std::size_t Length{0};
};

struct AccessorInfo {
    std::size_t View{0};
    std::size_t Offset{0};
    std::size_t Count{0};
    std::uint32_t ComponentType{0};
    std::string Type;
};

struct DecodedTerrainGltf {
    std::vector<std::array<float, 3>> Positions;
    std::vector<std::array<float, 3>> Normals;
    std::vector<std::array<float, 2>> Uvs;
    std::vector<std::uint32_t> Indices;
};

std::optional<DecodedTerrainGltf> DecodeTerrainGltf(
    const std::filesystem::path& gltfPath) {
    const auto textStorage = ReadText(gltfPath);
    if (!textStorage.has_value()) {
        return std::nullopt;
    }
    const std::string_view text{*textStorage};
    const auto viewArray = JsonArray(text, "bufferViews");
    const auto accessorArray = JsonArray(text, "accessors");
    if (!viewArray.has_value() || !accessorArray.has_value()) {
        return std::nullopt;
    }

    std::vector<BufferViewInfo> views;
    for (const std::string_view object : JsonObjectElements(*viewArray)) {
        const auto offset = JsonUnsigned(object, "byteOffset");
        const auto length = JsonUnsigned(object, "byteLength");
        if (!offset.has_value() || !length.has_value() ||
            *offset > std::numeric_limits<std::size_t>::max() ||
            *length > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        views.push_back({static_cast<std::size_t>(*offset),
                         static_cast<std::size_t>(*length)});
    }

    std::vector<AccessorInfo> accessors;
    for (const std::string_view object : JsonObjectElements(*accessorArray)) {
        const auto view = JsonUnsigned(object, "bufferView");
        const auto offset = JsonUnsigned(object, "byteOffset");
        const auto count = JsonUnsigned(object, "count");
        const auto component = JsonUnsigned(object, "componentType");
        const auto type = JsonString(object, "type");
        if (!view.has_value() || !count.has_value() ||
            !component.has_value() || !type.has_value() ||
            *view > std::numeric_limits<std::size_t>::max() ||
            *count > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        accessors.push_back({
            static_cast<std::size_t>(*view),
            static_cast<std::size_t>(offset.value_or(0u)),
            static_cast<std::size_t>(*count),
            static_cast<std::uint32_t>(*component),
            std::string{*type},
        });
    }

    const auto positionAccessor = JsonUnsigned(text, "POSITION");
    const auto normalAccessor = JsonUnsigned(text, "NORMAL");
    const auto uvAccessor = JsonUnsigned(text, "TEXCOORD_0");
    const auto indexAccessor = JsonUnsigned(text, "indices");
    const auto uri = JsonString(text, "uri");
    if (!positionAccessor.has_value() || !normalAccessor.has_value() ||
        !uvAccessor.has_value() || !indexAccessor.has_value() ||
        !uri.has_value() || *uri != gltfPath.stem().string() + ".bin") {
        return std::nullopt;
    }

    const std::filesystem::path binPath = gltfPath.parent_path() / *uri;
    const auto bytes = AC::ReadWholeFile(binPath);
    if (!bytes.has_value()) {
        return std::nullopt;
    }

    auto decodeFloatAccessor = [&]<std::size_t Components>(
                                   std::uint64_t accessorIndex)
        -> std::optional<std::vector<std::array<float, Components>>> {
        if (accessorIndex >= accessors.size()) {
            return std::nullopt;
        }
        const AccessorInfo& accessor = accessors[static_cast<std::size_t>(accessorIndex)];
        if (accessor.View >= views.size() || accessor.ComponentType != 5126u ||
            accessor.Type != std::format("VEC{}", Components)) {
            return std::nullopt;
        }
        const BufferViewInfo& view = views[accessor.View];
        const std::size_t elementBytes = Components * sizeof(float);
        if (accessor.Count > std::numeric_limits<std::size_t>::max() / elementBytes) {
            return std::nullopt;
        }
        const std::size_t totalBytes = accessor.Count * elementBytes;
        if (accessor.Offset > view.Length || totalBytes > view.Length - accessor.Offset ||
            view.Offset > bytes->size() ||
            accessor.Offset > bytes->size() - view.Offset ||
            totalBytes > bytes->size() - view.Offset - accessor.Offset) {
            return std::nullopt;
        }
        std::vector<std::array<float, Components>> values(accessor.Count);
        std::memcpy(values.data(),
                    bytes->data() + view.Offset + accessor.Offset,
                    totalBytes);
        return values;
    };

    auto decodeIndices = [&](std::uint64_t accessorIndex)
        -> std::optional<std::vector<std::uint32_t>> {
        if (accessorIndex >= accessors.size()) {
            return std::nullopt;
        }
        const AccessorInfo& accessor = accessors[static_cast<std::size_t>(accessorIndex)];
        if (accessor.View >= views.size() || accessor.ComponentType != 5125u ||
            accessor.Type != "SCALAR") {
            return std::nullopt;
        }
        const BufferViewInfo& view = views[accessor.View];
        if (accessor.Count >
            std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
            return std::nullopt;
        }
        const std::size_t totalBytes = accessor.Count * sizeof(std::uint32_t);
        if (accessor.Offset > view.Length || totalBytes > view.Length - accessor.Offset ||
            view.Offset > bytes->size() ||
            accessor.Offset > bytes->size() - view.Offset ||
            totalBytes > bytes->size() - view.Offset - accessor.Offset) {
            return std::nullopt;
        }
        std::vector<std::uint32_t> values(accessor.Count);
        std::memcpy(values.data(),
                    bytes->data() + view.Offset + accessor.Offset,
                    totalBytes);
        return values;
    };

    auto positions = decodeFloatAccessor.template operator()<3>(*positionAccessor);
    auto normals = decodeFloatAccessor.template operator()<3>(*normalAccessor);
    auto uvs = decodeFloatAccessor.template operator()<2>(*uvAccessor);
    auto indices = decodeIndices(*indexAccessor);
    if (!positions.has_value() || !normals.has_value() || !uvs.has_value() ||
        !indices.has_value()) {
        return std::nullopt;
    }
    return DecodedTerrainGltf{
        std::move(*positions), std::move(*normals),
        std::move(*uvs), std::move(*indices),
    };
}

std::optional<std::size_t> FindVertex(const DecodedTerrainGltf& mesh,
                                      std::uint32_t localX,
                                      std::uint32_t localY) {
    const float expectedX = static_cast<float>(localX);
    const float expectedZ = -static_cast<float>(localY);
    for (std::size_t index = 0; index < mesh.Positions.size(); ++index) {
        if (mesh.Positions[index][0] == expectedX &&
            mesh.Positions[index][2] == expectedZ) {
            return index;
        }
    }
    return std::nullopt;
}

bool HasOnlyTopFacingTriangles(const DecodedTerrainGltf& mesh) {
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
        const auto& a = mesh.Positions[ia];
        const auto& b = mesh.Positions[ib];
        const auto& c = mesh.Positions[ic];
        const double abX = static_cast<double>(b[0]) - a[0];
        const double abZ = static_cast<double>(b[2]) - a[2];
        const double acX = static_cast<double>(c[0]) - a[0];
        const double acZ = static_cast<double>(c[2]) - a[2];
        const double crossY = abZ * acX - abX * acZ;
        if (!(crossY > 0.0)) {
            return false;
        }
    }
    return true;
}

bool HasCanonicalVertexOrder(const DecodedTerrainGltf& mesh,
                             std::uint32_t step) {
    std::size_t expectedIndex = 0u;
    for (std::uint32_t y = 0; y <= kCellsPerPage; ++y) {
        for (std::uint32_t x = 0; x <= kCellsPerPage; ++x) {
            const bool included = x == 0u || y == 0u ||
                x == kCellsPerPage || y == kCellsPerPage ||
                (x % step == 0u && y % step == 0u);
            if (!included) {
                continue;
            }
            if (expectedIndex >= mesh.Positions.size() ||
                mesh.Positions[expectedIndex][0] != static_cast<float>(x) ||
                mesh.Positions[expectedIndex][2] != -static_cast<float>(y) ||
                expectedIndex >= mesh.Uvs.size() ||
                mesh.Uvs[expectedIndex][0] !=
                    static_cast<float>(x) / static_cast<float>(kCellsPerPage) ||
                mesh.Uvs[expectedIndex][1] !=
                    static_cast<float>(y) / static_cast<float>(kCellsPerPage)) {
                return false;
            }
            ++expectedIndex;
        }
    }
    return expectedIndex == mesh.Positions.size();
}

bool HasGlobalRowMajorCellBlocks(const DecodedTerrainGltf& mesh,
                                 std::uint32_t step) {
    if (step == 0u || kCellsPerPage % step != 0u ||
        mesh.Indices.empty() || mesh.Indices.size() % 3u != 0u) {
        return false;
    }
    const std::uint32_t coarseCells = kCellsPerPage / step;
    std::vector<std::size_t> trianglesPerCell(
        static_cast<std::size_t>(coarseCells) * coarseCells, 0u);
    std::size_t previousCell = 0u;
    bool firstTriangle = true;
    for (std::size_t offset = 0u; offset < mesh.Indices.size(); offset += 3u) {
        float minX = static_cast<float>(kCellsPerPage);
        float minY = static_cast<float>(kCellsPerPage);
        float maxX = 0.0f;
        float maxY = 0.0f;
        for (std::size_t corner = 0u; corner < 3u; ++corner) {
            const std::uint32_t vertex = mesh.Indices[offset + corner];
            if (vertex >= mesh.Positions.size()) {
                return false;
            }
            const float x = mesh.Positions[vertex][0];
            const float y = -mesh.Positions[vertex][2];
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
        const std::uint32_t cellX = std::min(
            static_cast<std::uint32_t>(minX) / step, coarseCells - 1u);
        const std::uint32_t cellY = std::min(
            static_cast<std::uint32_t>(minY) / step, coarseCells - 1u);
        if (minX < static_cast<float>(cellX * step) ||
            minY < static_cast<float>(cellY * step) ||
            maxX > static_cast<float>((cellX + 1u) * step) ||
            maxY > static_cast<float>((cellY + 1u) * step)) {
            return false;
        }
        const std::size_t cell =
            static_cast<std::size_t>(cellY) * coarseCells + cellX;
        if (!firstTriangle && cell < previousCell) {
            return false;
        }
        firstTriangle = false;
        previousCell = cell;
        ++trianglesPerCell[cell];
    }

    for (std::uint32_t cellY = 0u; cellY < coarseCells; ++cellY) {
        for (std::uint32_t cellX = 0u; cellX < coarseCells; ++cellX) {
            const bool top = cellY == 0u;
            const bool bottom = cellY + 1u == coarseCells;
            const bool left = cellX == 0u;
            const bool right = cellX + 1u == coarseCells;
            const bool corner = (top || bottom) && (left || right);
            const std::size_t expected = step == 1u ||
                    (!top && !bottom && !left && !right)
                ? 2u
                : corner ? static_cast<std::size_t>(step) * 2u
                         : static_cast<std::size_t>(step) + 1u;
            const std::size_t cell =
                static_cast<std::size_t>(cellY) * coarseCells + cellX;
            if (trianglesPerCell[cell] != expected) {
                return false;
            }
        }
    }
    return true;
}

std::vector<std::uint32_t> CellIndices(const DecodedTerrainGltf& mesh,
                                       std::uint32_t cellX,
                                       std::uint32_t cellY,
                                       std::uint32_t step) {
    std::vector<std::uint32_t> result;
    const float minX = static_cast<float>(cellX);
    const float maxX = static_cast<float>(cellX + step);
    const float minY = static_cast<float>(cellY);
    const float maxY = static_cast<float>(cellY + step);
    for (std::size_t offset = 0; offset < mesh.Indices.size(); offset += 3u) {
        bool belongs = true;
        for (std::size_t corner = 0; corner < 3u; ++corner) {
            const auto& position = mesh.Positions[mesh.Indices[offset + corner]];
            const float x = position[0];
            const float y = -position[2];
            belongs = belongs && x >= minX && x <= maxX &&
                y >= minY && y <= maxY;
        }
        if (belongs) {
            result.insert(result.end(),
                          mesh.Indices.begin() + static_cast<std::ptrdiff_t>(offset),
                          mesh.Indices.begin() + static_cast<std::ptrdiff_t>(offset + 3u));
        }
    }
    return result;
}

bool CellIndicesEqual(const DecodedTerrainGltf& mesh,
                      std::uint32_t cellX,
                      std::uint32_t cellY,
                      std::uint32_t step,
                      std::initializer_list<std::uint32_t> expected) {
    return CellIndices(mesh, cellX, cellY, step) ==
        std::vector<std::uint32_t>{expected};
}

bool FilesEqual(const std::filesystem::path& first,
                const std::filesystem::path& second) {
    const auto firstBytes = AC::ReadWholeFile(first);
    const auto secondBytes = AC::ReadWholeFile(second);
    return firstBytes.has_value() && secondBytes.has_value() &&
        *firstBytes == *secondBytes;
}

std::optional<std::uint32_t> ReadU32LeAt(
    const std::filesystem::path& path, std::uint64_t offset) {
    std::ifstream input{path, std::ios::binary};
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<std::uint8_t, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

bool IsNoFile(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    return (!error && status.type() == std::filesystem::file_type::not_found) ||
        error == std::errc::no_such_file_or_directory;
}

bool IsWriterFailure(const AC::TerrainPageMeshResult& result,
                     const std::string& detail) {
    return !result.Ok && result.Step == 0u && result.GltfPath.empty() &&
        result.BinPath.empty() && !detail.empty();
}

bool WriteTextFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

struct FileSnapshot {
    std::filesystem::path Path;
    std::string Contents;
    std::string Sha256;
};

std::optional<FileSnapshot> SnapshotFile(const std::filesystem::path& path) {
    const auto contents = ReadText(path);
    std::string detail;
    const auto sha256 = AC::Sha256File(path, detail);
    if (!contents.has_value() || !sha256.has_value() || !detail.empty()) {
        return std::nullopt;
    }
    return FileSnapshot{path, *contents, *sha256};
}

bool SnapshotUnchanged(const FileSnapshot& snapshot) {
    const auto contents = ReadText(snapshot.Path);
    std::string detail;
    const auto sha256 = AC::Sha256File(snapshot.Path, detail);
    return contents.has_value() && sha256.has_value() && detail.empty() &&
        *contents == snapshot.Contents && *sha256 == snapshot.Sha256;
}

std::optional<std::filesystem::file_type> PhysicalType(
    const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found &&
        error == std::errc::no_such_file_or_directory) {
        error.clear();
    }
    if (error) {
        return std::nullopt;
    }
    return status.type();
}

bool IsWindowsSymlinkPermissionError(const std::error_code& error) {
#if defined(_WIN32)
    return error == std::errc::permission_denied ||
        error.value() == ERROR_ACCESS_DENIED ||
        error.value() == ERROR_PRIVILEGE_NOT_HELD;
#else
    static_cast<void>(error);
    return false;
#endif
}

CORSAIRS_TEST(TerrainPageMeshWriter_SelectsLargestPassingStepInclusively) {
    ScopedTestDirectory temporary{"adaptive"};
    REQUIRE(temporary.Ready());

    const AC::MapPageTiles flat = FlatPage();
    const AC::TerrainMeshError flatError = AC::EvaluateTerrainPageStep(flat, 4u);
    REQUIRE_EQ(flatError.MaxAbsCm, 0.0);
    REQUIRE_EQ(flatError.RmsCm, 0.0);
    REQUIRE_EQ(flatError.SharedBoundaryMaxCm, 0.0);
    REQUIRE_EQ(flatError.Samples, kSampleCount);

    std::string detail;
    const auto flatResult = AC::WriteTerrainPageMesh(
        flat, {0u, 0u}, temporary.Path() / "flat", {}, detail);
    REQUIRE(flatResult.Ok);
    REQUIRE_EQ(flatResult.Step, 4u);
    REQUIRE_EQ(flatResult.Error.MaxAbsCm, 0.0);
    REQUIRE_EQ(flatResult.Error.RmsCm, 0.0);
    REQUIRE_EQ(flatResult.Error.SharedBoundaryMaxCm, 0.0);
    REQUIRE_EQ(flatResult.Error.Samples, kSampleCount);
    REQUIRE(detail.empty());

    AC::MapPageTiles spike = FlatPage();
    spike.Tiles[65u * kStoredSamples + 65u].Height = 10;
    const AC::TerrainMeshError stepFour =
        AC::EvaluateTerrainPageStep(spike, 4u);
    const AC::TerrainMeshError stepTwo =
        AC::EvaluateTerrainPageStep(spike, 2u);
    REQUIRE_EQ(stepFour.MaxAbsCm, 100.0);
    REQUIRE_EQ(stepTwo.MaxAbsCm, 100.0);
    REQUIRE(stepFour.RmsCm < 2.0);
    REQUIRE(stepTwo.RmsCm < 2.0);

    const auto spikeResult = AC::WriteTerrainPageMesh(
        spike, {0u, 0u}, temporary.Path() / "spike", {}, detail);
    REQUIRE(spikeResult.Ok);
    REQUIRE_EQ(spikeResult.Step, 1u);
    REQUIRE_EQ(spikeResult.Error.MaxAbsCm, 0.0);
    REQUIRE_EQ(spikeResult.Error.RmsCm, 0.0);
    REQUIRE_EQ(spikeResult.Error.SharedBoundaryMaxCm, 0.0);
    REQUIRE_EQ(spikeResult.Error.Samples, kSampleCount);
    const auto spikeMesh = DecodeTerrainGltf(spikeResult.GltfPath);
    REQUIRE(spikeMesh.has_value());
    REQUIRE_EQ(spikeMesh->Positions.size(), kSampleCount);
    REQUIRE(HasCanonicalVertexOrder(*spikeMesh, 1u));
    REQUIRE(HasOnlyTopFacingTriangles(*spikeMesh));
    REQUIRE(HasGlobalRowMajorCellBlocks(*spikeMesh, 1u));
    REQUIRE(CellIndicesEqual(*spikeMesh, 65u, 65u, 1u,
        {8450u, 8451u, 8579u, 8451u, 8580u, 8579u}));

    AC::TerrainPageMeshOptions inclusive;
    inclusive.MaxAbsCm = 100.0;
    inclusive.MaxRmsCm = stepFour.RmsCm;
    inclusive.MaxSharedBoundaryCm = 0.0;
    const auto inclusiveResult = AC::WriteTerrainPageMesh(
        spike, {0u, 0u}, temporary.Path() / "inclusive", inclusive, detail);
    REQUIRE(inclusiveResult.Ok);
    REQUIRE_EQ(inclusiveResult.Step, 4u);
}

CORSAIRS_TEST(TerrainPageMeshWriter_DecodesCanonicalTopologyAndDeterministicPair) {
    ScopedTestDirectory temporary{"topology"};
    REQUIRE(temporary.Ready());
    std::string detail;

    const auto stepFourResult = AC::WriteTerrainPageMesh(
        FlatPage(2176u, 2688u), {17u, 21u},
        temporary.Path() / "step-four", {}, detail);
    REQUIRE(stepFourResult.Ok);
    REQUIRE_EQ(stepFourResult.Step, 4u);
    REQUIRE(stepFourResult.GltfPath == temporary.Path() / "step-four" /
        "garner.terrain_17_21.gltf");
    REQUIRE(stepFourResult.BinPath == temporary.Path() / "step-four" /
        "garner.terrain_17_21.bin");
    REQUIRE_EQ(stepFourResult.ActorWorldXcm, 217600.0);
    REQUIRE_EQ(stepFourResult.ActorWorldYcm, -268800.0);

    const auto stepTwoResult = AC::WriteTerrainPageMesh(
        StepTwoWavePage(), {0u, 0u}, temporary.Path() / "step-two", {}, detail);
    REQUIRE(stepTwoResult.Ok);
    REQUIRE_EQ(stepTwoResult.Step, 2u);

    const auto stepFour = DecodeTerrainGltf(stepFourResult.GltfPath);
    const auto stepTwo = DecodeTerrainGltf(stepTwoResult.GltfPath);
    REQUIRE(stepFour.has_value());
    REQUIRE(stepTwo.has_value());
    REQUIRE_EQ(stepFour->Positions.size(), 1473u);
    REQUIRE_EQ(stepTwo->Positions.size(), 4481u);
    REQUIRE(stepFour->Normals.size() == stepFour->Positions.size());
    REQUIRE(stepTwo->Normals.size() == stepTwo->Positions.size());
    REQUIRE(HasCanonicalVertexOrder(*stepFour, 4u));
    REQUIRE(HasCanonicalVertexOrder(*stepTwo, 2u));
    REQUIRE(HasOnlyTopFacingTriangles(*stepFour));
    REQUIRE(HasOnlyTopFacingTriangles(*stepTwo));
    REQUIRE(HasGlobalRowMajorCellBlocks(*stepFour, 4u));
    REQUIRE(HasGlobalRowMajorCellBlocks(*stepTwo, 2u));
    REQUIRE(std::all_of(stepFour->Normals.begin(), stepFour->Normals.end(),
                        [](const auto& normal) {
                            return normal == std::array<float, 3>{0.0f, 1.0f, 0.0f};
                        }));

    const auto stepFourMinMaxX = std::minmax_element(
        stepFour->Positions.begin(), stepFour->Positions.end(),
        [](const auto& left, const auto& right) { return left[0] < right[0]; });
    const auto stepFourMinMaxZ = std::minmax_element(
        stepFour->Positions.begin(), stepFour->Positions.end(),
        [](const auto& left, const auto& right) { return left[2] < right[2]; });
    REQUIRE_EQ((*stepFourMinMaxX.first)[0], 0.0f);
    REQUIRE_EQ((*stepFourMinMaxX.second)[0], 128.0f);
    REQUIRE_EQ((*stepFourMinMaxZ.first)[2], -128.0f);
    REQUIRE_EQ((*stepFourMinMaxZ.second)[2], 0.0f);
    REQUIRE(std::all_of(stepFour->Positions.begin(), stepFour->Positions.end(),
                        [](const auto& position) { return position[1] == 0.0f; }));
    REQUIRE_EQ(stepFourResult.ActorWorldXcm +
                   static_cast<double>((*stepFourMinMaxX.first)[0]) * 100.0,
               217600.0);
    REQUIRE_EQ(stepFourResult.ActorWorldXcm +
                   static_cast<double>((*stepFourMinMaxX.second)[0]) * 100.0,
               230400.0);
    REQUIRE_EQ(stepFourResult.ActorWorldYcm +
                   static_cast<double>((*stepFourMinMaxZ.first)[2]) * 100.0,
               -281600.0);
    REQUIRE_EQ(stepFourResult.ActorWorldYcm +
                   static_cast<double>((*stepFourMinMaxZ.second)[2]) * 100.0,
               -268800.0);
    const auto topLeft = FindVertex(*stepFour, 0u, 0u);
    const auto bottomRight = FindVertex(*stepFour, 128u, 128u);
    REQUIRE(topLeft.has_value());
    REQUIRE(bottomRight.has_value());
    REQUIRE((stepFour->Uvs[*topLeft] == std::array<float, 2>{0.0f, 0.0f}));
    REQUIRE((stepFour->Uvs[*bottomRight] == std::array<float, 2>{1.0f, 1.0f}));

    REQUIRE(CellIndicesEqual(*stepFour, 4u, 4u, 4u,
        {136u, 137u, 175u, 137u, 176u, 175u}));
    REQUIRE(CellIndicesEqual(*stepFour, 4u, 0u, 4u,
        {136u, 4u, 5u, 136u, 5u, 6u, 136u, 6u, 7u,
         136u, 7u, 8u, 8u, 137u, 136u}));
    REQUIRE(CellIndicesEqual(*stepFour, 4u, 124u, 4u,
        {1307u, 1349u, 1348u, 1307u, 1350u, 1349u,
         1307u, 1351u, 1350u, 1307u, 1352u, 1351u,
         1306u, 1307u, 1348u}));
    REQUIRE(CellIndicesEqual(*stepFour, 0u, 4u, 4u,
        {136u, 168u, 135u, 136u, 170u, 168u, 136u, 172u, 170u,
         136u, 174u, 172u, 136u, 175u, 174u}));
    REQUIRE(CellIndicesEqual(*stepFour, 124u, 4u, 4u,
        {205u, 167u, 169u, 205u, 169u, 171u, 205u, 171u, 173u,
         205u, 173u, 206u, 166u, 167u, 205u}));
    REQUIRE(CellIndicesEqual(*stepFour, 0u, 0u, 4u,
        {136u, 3u, 4u, 136u, 2u, 3u, 136u, 1u, 2u, 136u, 0u, 1u,
         136u, 129u, 0u, 136u, 131u, 129u, 136u, 133u, 131u,
         136u, 135u, 133u}));
    REQUIRE(CellIndicesEqual(*stepFour, 124u, 0u, 4u,
        {166u, 134u, 167u, 166u, 132u, 134u, 166u, 130u, 132u,
         166u, 128u, 130u, 166u, 127u, 128u, 166u, 126u, 127u,
         166u, 125u, 126u, 166u, 124u, 125u}));
    REQUIRE(CellIndicesEqual(*stepFour, 0u, 124u, 4u,
        {1306u, 1338u, 1305u, 1306u, 1340u, 1338u,
         1306u, 1342u, 1340u, 1306u, 1344u, 1342u,
         1306u, 1345u, 1344u, 1306u, 1346u, 1345u,
         1306u, 1347u, 1346u, 1306u, 1348u, 1347u}));
    REQUIRE(CellIndicesEqual(*stepFour, 124u, 124u, 4u,
        {1336u, 1469u, 1468u, 1336u, 1470u, 1469u,
         1336u, 1471u, 1470u, 1336u, 1472u, 1471u,
         1336u, 1343u, 1472u, 1336u, 1341u, 1343u,
         1336u, 1339u, 1341u, 1336u, 1337u, 1339u}));

    REQUIRE(CellIndicesEqual(*stepTwo, 2u, 2u, 2u,
        {132u, 133u, 199u, 133u, 200u, 199u}));
    REQUIRE(CellIndicesEqual(*stepTwo, 2u, 0u, 2u,
        {132u, 2u, 3u, 132u, 3u, 4u, 4u, 133u, 132u}));
    REQUIRE(CellIndicesEqual(*stepTwo, 2u, 126u, 2u,
        {4287u, 4355u, 4354u, 4287u, 4356u, 4355u,
         4286u, 4287u, 4354u}));
    REQUIRE(CellIndicesEqual(*stepTwo, 0u, 2u, 2u,
        {132u, 196u, 131u, 132u, 198u, 196u, 132u, 199u, 198u}));
    REQUIRE(CellIndicesEqual(*stepTwo, 126u, 2u, 2u,
        {261u, 195u, 197u, 261u, 197u, 262u, 194u, 195u, 261u}));
    REQUIRE(CellIndicesEqual(*stepTwo, 0u, 0u, 2u,
        {132u, 1u, 2u, 132u, 0u, 1u, 132u, 129u, 0u,
         132u, 131u, 129u}));
    REQUIRE(CellIndicesEqual(*stepTwo, 126u, 0u, 2u,
        {194u, 130u, 195u, 194u, 128u, 130u,
         194u, 127u, 128u, 194u, 126u, 127u}));
    REQUIRE(CellIndicesEqual(*stepTwo, 0u, 126u, 2u,
        {4286u, 4350u, 4285u, 4286u, 4352u, 4350u,
         4286u, 4353u, 4352u, 4286u, 4354u, 4353u}));
    REQUIRE(CellIndicesEqual(*stepTwo, 126u, 126u, 2u,
        {4348u, 4479u, 4478u, 4348u, 4480u, 4479u,
         4348u, 4351u, 4480u, 4348u, 4349u, 4351u}));

    const auto repeated = AC::WriteTerrainPageMesh(
        FlatPage(2176u, 2688u), {17u, 21u},
        temporary.Path() / "step-four-repeat", {}, detail);
    REQUIRE(repeated.Ok);
    REQUIRE(FilesEqual(stepFourResult.GltfPath, repeated.GltfPath));
    REQUIRE(FilesEqual(stepFourResult.BinPath, repeated.BinPath));
}

CORSAIRS_TEST(TerrainPageMeshWriter_PreservesSharedWorldBoundaryAcrossSteps) {
    ScopedTestDirectory temporary{"shared-boundary"};
    REQUIRE(temporary.Ready());
    AC::MapPageTiles left = FlatPage(0u, 0u);
    AC::MapPageTiles right = FlatPage(128u, 0u);
    right.Tiles[65u * kStoredSamples + 65u].Height = 10;

    std::string detail;
    const auto leftResult = AC::WriteTerrainPageMesh(
        left, {0u, 0u}, temporary.Path() / "left", {}, detail);
    const auto rightResult = AC::WriteTerrainPageMesh(
        right, {1u, 0u}, temporary.Path() / "right", {}, detail);
    REQUIRE(leftResult.Ok);
    REQUIRE(rightResult.Ok);
    REQUIRE_EQ(leftResult.Step, 4u);
    REQUIRE_EQ(rightResult.Step, 1u);
    REQUIRE_EQ(leftResult.Error.SharedBoundaryMaxCm, 0.0);
    REQUIRE_EQ(rightResult.Error.SharedBoundaryMaxCm, 0.0);

    const auto leftMesh = DecodeTerrainGltf(leftResult.GltfPath);
    const auto rightMesh = DecodeTerrainGltf(rightResult.GltfPath);
    REQUIRE(leftMesh.has_value());
    REQUIRE(rightMesh.has_value());

    using WorldPoint = std::tuple<double, double, double>;
    auto boundary = [](const DecodedTerrainGltf& mesh,
                       float localX,
                       double actorX,
                       double actorY) {
        std::vector<WorldPoint> points;
        for (const auto& position : mesh.Positions) {
            if (position[0] == localX) {
                points.emplace_back(
                    actorX + static_cast<double>(position[0]) * 100.0,
                    actorY + static_cast<double>(position[2]) * 100.0,
                    static_cast<double>(position[1]) * 100.0);
            }
        }
        std::sort(points.begin(), points.end());
        return points;
    };
    const auto leftBoundary = boundary(
        *leftMesh, 128.0f, leftResult.ActorWorldXcm, leftResult.ActorWorldYcm);
    const auto rightBoundary = boundary(
        *rightMesh, 0.0f, rightResult.ActorWorldXcm, rightResult.ActorWorldYcm);
    REQUIRE_EQ(leftBoundary.size(), 129u);
    REQUIRE_EQ(rightBoundary.size(), 129u);
    REQUIRE(leftBoundary == rightBoundary);
}

CORSAIRS_TEST(TerrainPageMeshWriter_UsesGarnerSparseHaloDefaultDeterministically) {
    const std::filesystem::path repoRoot{CORSAIRS_REPO_ROOT};
    const std::filesystem::path mapPath = repoRoot / "Client" / "map" / "garner.map";
    const auto sparseOffset = ReadU32LeAt(mapPath, 717972u);
    REQUIRE(sparseOffset.has_value());
    REQUIRE_EQ(*sparseOffset, 0u);

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, diagnostics);
    REQUIRE(reader.has_value());
    auto page = reader->ReadWindow({2176u, 2688u, 128u, 128u},
                                   1u, 1u, diagnostics);
    REQUIRE(page.has_value());
    REQUIRE_EQ(page->SectionPresent.size(), 17u * 17u);
    REQUIRE_EQ(std::count(page->SectionPresent.begin(),
                          page->SectionPresent.end(), 0u), 1);
    REQUIRE_EQ(page->SectionPresent[254u], 0u);
    REQUIRE_EQ(page->Cells.X /
                   static_cast<std::uint32_t>(reader->Header().SectionWidth) + 16u,
               288u);
    REQUIRE_EQ(page->Cells.Y /
                   static_cast<std::uint32_t>(reader->Header().SectionHeight) + 14u,
               350u);
    REQUIRE_EQ(std::count(page->TilePresent.begin(),
                          page->TilePresent.end(), 0u), 8);
    for (std::uint32_t y = 0u; y < 128u; ++y) {
        for (std::uint32_t x = 0u; x < 128u; ++x) {
            REQUIRE_EQ(page->TilePresent[
                static_cast<std::size_t>(y) * kStoredSamples + x], 1u);
        }
    }
    for (std::uint32_t y = 112u; y <= 119u; ++y) {
        const std::size_t left = static_cast<std::size_t>(y) * kStoredSamples + 127u;
        const std::size_t right = left + 1u;
        REQUIRE_EQ(page->TilePresent[left], 1u);
        REQUIRE_EQ(page->TilePresent[right], 0u);
        REQUIRE_EQ(AC::ResolveLegacyTerrainCornerSample(
                       page->Tiles[left], true).HeightCm,
                   -60.0);
        REQUIRE_EQ(AC::ResolveLegacyTerrainCornerSample(
                       page->Tiles[right], false).HeightCm,
                   -200.0);
    }
    REQUIRE_EQ(AC::ResolveLegacyTerrainCornerSample(
                   page->Tiles[120u * kStoredSamples + 127u], true).HeightCm,
               -100.0);
    REQUIRE_EQ(AC::ResolveLegacyTerrainCornerSample(
                   page->Tiles[120u * kStoredSamples + 128u], true).HeightCm,
               -100.0);

    ScopedTestDirectory temporary{"garner"};
    REQUIRE(temporary.Ready());
    std::string detail;
    const auto first = AC::WriteTerrainPageMesh(
        *page, {17u, 21u}, temporary.Path() / "first", {}, detail);
    REQUIRE(first.Ok);
    REQUIRE(first.Step == 4u || first.Step == 2u || first.Step == 1u);
    REQUIRE(first.Error.MaxAbsCm <= 5.0);
    REQUIRE(first.Error.RmsCm <= 2.0);
    REQUIRE_EQ(first.Error.SharedBoundaryMaxCm, 0.0);
    REQUIRE_EQ(first.Error.Samples, kSampleCount);

    const auto firstMesh = DecodeTerrainGltf(first.GltfPath);
    REQUIRE(firstMesh.has_value());
    for (std::uint32_t y = 112u; y <= 119u; ++y) {
        const auto top = FindVertex(*firstMesh, 128u, y);
        const auto bottom = FindVertex(*firstMesh, 128u, y + 1u);
        REQUIRE(top.has_value());
        REQUIRE(bottom.has_value());
        REQUIRE_EQ(firstMesh->Positions[*top][1], -2.0f);
        REQUIRE_EQ(firstMesh->Positions[*bottom][1],
                   y < 119u ? -2.0f : -1.0f);
        REQUIRE(std::find(firstMesh->Indices.begin(), firstMesh->Indices.end(),
                          static_cast<std::uint32_t>(*top)) != firstMesh->Indices.end());
        REQUIRE(std::find(firstMesh->Indices.begin(), firstMesh->Indices.end(),
                          static_cast<std::uint32_t>(*bottom)) != firstMesh->Indices.end());
    }

    AC::MapPageTiles mutated = *page;
    for (std::uint32_t y = 112u; y <= 119u; ++y) {
        AC::MapTile& tile =
            mutated.Tiles[static_cast<std::size_t>(y) * kStoredSamples + 128u];
        tile.TileInfo = 0xffffffffu;
        tile.BaseTex = 255u;
        tile.Color = static_cast<std::int16_t>(0x1234);
        tile.Height = 127;
        tile.Region = std::numeric_limits<std::int16_t>::min();
        tile.Island = 255u;
        std::fill(std::begin(tile.Block), std::end(tile.Block), 255u);
    }
    const auto second = AC::WriteTerrainPageMesh(
        mutated, {17u, 21u}, temporary.Path() / "second", {}, detail);
    REQUIRE(second.Ok);
    REQUIRE_EQ(first.Step, second.Step);
    REQUIRE(FilesEqual(first.GltfPath, second.GltfPath));
    REQUIRE(FilesEqual(first.BinPath, second.BinPath));

    AC::MapPageTiles missingOwned = *page;
    missingOwned.TilePresent[0u] = 0u;
    const auto failed = AC::WriteTerrainPageMesh(
        missingOwned, {17u, 21u}, temporary.Path() / "missing", {}, detail);
    REQUIRE(IsWriterFailure(failed, detail));
    REQUIRE(IsNoFile(temporary.Path() / "missing" /
                     "garner.terrain_17_21.gltf"));
    REQUIRE(IsNoFile(temporary.Path() / "missing" /
                     "garner.terrain_17_21.bin"));
}

CORSAIRS_TEST(TerrainPageMeshWriter_PreservesEveryOccupiedOutputLeaf) {
    ScopedTestDirectory temporary{"occupied-leaves"};
    REQUIRE(temporary.Ready());

    enum class LeafKind { Missing, Regular, Directory, Symlink };
    struct OccupiedLeafCase {
        std::string_view Name;
        LeafKind Gltf;
        LeafKind Bin;
    };
    std::vector<OccupiedLeafCase> cases{
        {"gltf-regular-bin-missing", LeafKind::Regular, LeafKind::Missing},
        {"gltf-missing-bin-regular", LeafKind::Missing, LeafKind::Regular},
        {"gltf-directory-bin-regular", LeafKind::Directory, LeafKind::Regular},
        {"gltf-regular-bin-directory", LeafKind::Regular, LeafKind::Directory},
        {"both-regular", LeafKind::Regular, LeafKind::Regular},
    };
    bool leafSymlinkSupported = true;
#if defined(_WIN32)
    const std::filesystem::path probeTarget =
        temporary.Path() / "windows-symlink-probe-target.txt";
    const std::filesystem::path probeLink =
        temporary.Path() / "windows-symlink-probe-link.txt";
    REQUIRE(WriteTextFile(probeTarget, "probe"));
    std::error_code probeError;
    std::filesystem::create_symlink(probeTarget, probeLink, probeError);
    if (probeError && IsWindowsSymlinkPermissionError(probeError)) {
        std::cout << std::format(
            "        SKIP Windows occupied-leaf symlink case: {}\n",
            probeError.message());
        leafSymlinkSupported = false;
    }
    else {
        REQUIRE(!probeError);
        probeError.clear();
        REQUIRE(std::filesystem::remove(probeLink, probeError));
        REQUIRE(!probeError);
        REQUIRE(IsNoFile(probeLink));
    }
#endif
    if (leafSymlinkSupported) {
        cases.push_back(
            {"gltf-symlink-bin-regular", LeafKind::Symlink, LeafKind::Regular});
    }

    bool allCasesPassed = true;
    for (const OccupiedLeafCase& testCase : cases) {
        const std::filesystem::path outputDirectory =
            temporary.Path() / testCase.Name;
        std::error_code error;
        const bool directoryCreated =
            std::filesystem::create_directory(outputDirectory, error);
        if (!directoryCreated || error) {
            std::cout << std::format(
                "        case {}: output directory setup failed: {}\n",
                testCase.Name, error.message());
            allCasesPassed = false;
            continue;
        }
        const std::filesystem::path gltfPath =
            outputDirectory / "garner.terrain_00_00.gltf";
        const std::filesystem::path binPath =
            outputDirectory / "garner.terrain_00_00.bin";
        std::vector<FileSnapshot> snapshots;

        const auto prepareLeaf = [&](const std::filesystem::path& path,
                                     LeafKind kind,
                                     std::string_view label) {
            if (kind == LeafKind::Missing) {
                return true;
            }
            std::filesystem::path protectedFile = path;
            if (kind == LeafKind::Directory) {
                std::error_code createError;
                if (!std::filesystem::create_directory(path, createError) ||
                    createError) {
                    return false;
                }
                protectedFile = path / "inside-sentinel.txt";
            }
            else if (kind == LeafKind::Symlink) {
                protectedFile = path.parent_path() /
                    std::format("{}-target.txt", label);
            }
            if (!WriteTextFile(
                    protectedFile,
                    std::format("preserve-{}-{}", testCase.Name, label))) {
                return false;
            }
            if (kind == LeafKind::Symlink) {
                std::error_code linkError;
                std::filesystem::create_symlink(protectedFile, path, linkError);
                if (linkError) {
                    return false;
                }
            }
            const auto snapshot = SnapshotFile(protectedFile);
            if (!snapshot.has_value()) {
                return false;
            }
            snapshots.push_back(*snapshot);
            return true;
        };

        const bool setupOk =
            prepareLeaf(gltfPath, testCase.Gltf, "gltf") &&
            prepareLeaf(binPath, testCase.Bin, "bin");
        const auto gltfTypeBefore = PhysicalType(gltfPath);
        const auto binTypeBefore = PhysicalType(binPath);
        if (!setupOk || !gltfTypeBefore.has_value() ||
            !binTypeBefore.has_value()) {
            std::cout << std::format(
                "        case {}: leaf setup/snapshot failed\n", testCase.Name);
            allCasesPassed = false;
            continue;
        }

        std::string detail;
        const auto result = AC::WriteTerrainPageMesh(
            FlatPage(), {0u, 0u}, outputDirectory, {}, detail);
        const auto gltfTypeAfter = PhysicalType(gltfPath);
        const auto binTypeAfter = PhysicalType(binPath);
        const auto symlinkTargetUnchanged = [](
            const std::filesystem::path& path,
            LeafKind kind,
            std::string_view label) {
            if (kind != LeafKind::Symlink) {
                return true;
            }
            std::error_code linkError;
            const std::filesystem::path target =
                std::filesystem::read_symlink(path, linkError);
            return !linkError && target == path.parent_path() /
                std::format("{}-target.txt", label);
        };
        const bool snapshotsUnchanged = std::all_of(
            snapshots.begin(), snapshots.end(), SnapshotUnchanged);
        const bool casePassed = IsWriterFailure(result, detail) &&
            gltfTypeAfter == gltfTypeBefore && binTypeAfter == binTypeBefore &&
            snapshotsUnchanged &&
            symlinkTargetUnchanged(gltfPath, testCase.Gltf, "gltf") &&
            symlinkTargetUnchanged(binPath, testCase.Bin, "bin");
        if (!casePassed) {
            std::cout << std::format(
                "        case {}: result/presence/content/hash changed\n",
                testCase.Name);
            allCasesPassed = false;
        }
    }
    REQUIRE(allCasesPassed);
}

CORSAIRS_TEST(TerrainPageMeshWriter_RejectsFifoLeafWithoutBlocking) {
#if defined(_WIN32)
    std::cout << "        SKIP POSIX-only FIFO regression\n";
    static_cast<void>(corsairsTestOk);
    return;
#else
    ScopedTestDirectory temporary{"fifo-leaf"};
    REQUIRE(temporary.Ready());
    const std::filesystem::path outputDirectory = temporary.Path() / "output";
    REQUIRE(std::filesystem::create_directory(outputDirectory));
    const std::filesystem::path gltfPath =
        outputDirectory / "garner.terrain_00_00.gltf";
    const std::filesystem::path binPath =
        outputDirectory / "garner.terrain_00_00.bin";
    REQUIRE(::mkfifo(binPath.c_str(), 0600) == 0);
    const AC::MapPageTiles page = FlatPage();

    constexpr auto timeout = std::chrono::milliseconds{1500};
    const auto started = std::chrono::steady_clock::now();
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::string detail;
        const auto result = AC::WriteTerrainPageMesh(
            page, {0u, 0u}, outputDirectory, {}, detail);
        ::_exit(IsWriterFailure(result, detail) ? 0 : 2);
    }

    int childStatus = 0;
    bool completed = false;
    bool waitError = false;
    while (std::chrono::steady_clock::now() - started < timeout) {
        const pid_t waited = ::waitpid(child, &childStatus, WNOHANG);
        if (waited == child) {
            completed = true;
            break;
        }
        if (waited < 0) {
            waitError = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    bool childReaped = completed;
    if (!completed) {
        const int killResult = ::kill(child, SIGKILL);
        const pid_t reaped = ::waitpid(child, &childStatus, 0);
        childReaped = killResult == 0 && reaped == child;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const auto retainedFifoType = PhysicalType(binPath);
    std::error_code cleanupError;
    const bool fifoRemoved = std::filesystem::remove(binPath, cleanupError);
    const bool cleanupChecked =
        retainedFifoType == std::filesystem::file_type::fifo && fifoRemoved &&
        !cleanupError && IsNoFile(binPath);
    REQUIRE(cleanupChecked);
    REQUIRE(childReaped);
    REQUIRE(!waitError);
    REQUIRE(completed);
    REQUIRE(WIFEXITED(childStatus));
    REQUIRE_EQ(WEXITSTATUS(childStatus), 0);
    REQUIRE(elapsed < timeout);
    REQUIRE(IsNoFile(gltfPath));
#endif
}

CORSAIRS_TEST(TerrainPageMeshWriter_RejectsSymlinkOutputDirectoryWithoutEscape) {
    ScopedTestDirectory temporary{"symlink-output-directory"};
    REQUIRE(temporary.Ready());
    const std::filesystem::path outside = temporary.Path() / "outside";
    REQUIRE(std::filesystem::create_directory(outside));
    const std::filesystem::path sentinel = outside / "outside-sentinel.txt";
    REQUIRE(WriteTextFile(sentinel, "outside-must-stay-byte-identical"));
    const auto sentinelBefore = SnapshotFile(sentinel);
    REQUIRE(sentinelBefore.has_value());

    const std::filesystem::path linkedOutput = temporary.Path() / "linked-output";
    std::error_code linkError;
    std::filesystem::create_directory_symlink(outside, linkedOutput, linkError);
    bool symlinkCasePassed = true;
    if (linkError) {
        if (IsWindowsSymlinkPermissionError(linkError)) {
            std::cout << std::format(
                "        SKIP Windows directory symlink regression: {}\n",
                linkError.message());
        }
        else {
            symlinkCasePassed = false;
        }
    }
    else {
        std::string detail;
        const auto result = AC::WriteTerrainPageMesh(
            FlatPage(), {0u, 0u}, linkedOutput, {}, detail);
        const std::filesystem::path trailingLinkedOutput{
            linkedOutput.generic_string() + "/"};
        const auto trailingResult = AC::WriteTerrainPageMesh(
            FlatPage(), {0u, 0u}, trailingLinkedOutput, {}, detail);
        symlinkCasePassed = IsWriterFailure(result, detail) &&
            IsWriterFailure(trailingResult, detail) &&
            PhysicalType(linkedOutput) == std::filesystem::file_type::symlink &&
            SnapshotUnchanged(*sentinelBefore) &&
            IsNoFile(outside / "garner.terrain_00_00.gltf") &&
            IsNoFile(outside / "garner.terrain_00_00.bin");
    }

    std::string detail;
    const std::filesystem::path missingReal = temporary.Path() / "missing-real";
    const auto missingResult = AC::WriteTerrainPageMesh(
        FlatPage(), {0u, 0u}, missingReal, {}, detail);
    const std::filesystem::path existingReal = temporary.Path() / "existing-real";
    REQUIRE(std::filesystem::create_directory(existingReal));
    const auto existingResult = AC::WriteTerrainPageMesh(
        FlatPage(), {0u, 0u}, existingReal, {}, detail);
    REQUIRE(symlinkCasePassed);
    REQUIRE(missingResult.Ok);
    REQUIRE(existingResult.Ok);
}

CORSAIRS_TEST(TerrainPageMeshWriter_RejectsMalformedInputsAndCleansPartialPair) {
    ScopedTestDirectory temporary{"failures"};
    REQUIRE(temporary.Ready());
    std::string detail;

    const AC::TerrainMeshError zeroStep =
        AC::EvaluateTerrainPageStep(FlatPage(), 0u);
    const AC::TerrainMeshError unsupportedStep =
        AC::EvaluateTerrainPageStep(FlatPage(), 3u);
    REQUIRE_EQ(zeroStep.Samples, 0u);
    REQUIRE_EQ(unsupportedStep.Samples, 0u);
    REQUIRE(std::isinf(zeroStep.MaxAbsCm));
    REQUIRE(std::isinf(zeroStep.RmsCm));
    REQUIRE(std::isinf(zeroStep.SharedBoundaryMaxCm));
    REQUIRE(std::isinf(unsupportedStep.MaxAbsCm));

    std::vector<AC::MapPageTiles> malformed;
    malformed.push_back(FlatPage());
    malformed.back().Cells.Width = 127u;
    malformed.push_back(FlatPage());
    malformed.back().StoredHeight = 128u;
    malformed.push_back(FlatPage());
    malformed.back().Tiles.pop_back();
    malformed.push_back(FlatPage());
    malformed.back().TilePresent.pop_back();
    malformed.push_back(FlatPage());
    malformed.back().TilePresent[64u * kStoredSamples + 64u] = 0u;

    for (std::size_t index = 0; index < malformed.size(); ++index) {
        const auto result = AC::WriteTerrainPageMesh(
            malformed[index], {0u, 0u},
            temporary.Path() / std::format("malformed-{}", index), {}, detail);
        REQUIRE(IsWriterFailure(result, detail));
        const AC::TerrainMeshError evaluated =
            AC::EvaluateTerrainPageStep(malformed[index], 4u);
        REQUIRE_EQ(evaluated.Samples, 0u);
        REQUIRE(std::isinf(evaluated.MaxAbsCm));
    }

    const auto wrongOrigin = AC::WriteTerrainPageMesh(
        FlatPage(1u, 0u), {0u, 0u}, temporary.Path() / "wrong-origin", {}, detail);
    REQUIRE(IsWriterFailure(wrongOrigin, detail));
    const auto overflowOrigin = AC::WriteTerrainPageMesh(
        FlatPage(), {std::numeric_limits<std::uint32_t>::max(), 0u},
        temporary.Path() / "overflow-origin", {}, detail);
    REQUIRE(IsWriterFailure(overflowOrigin, detail));

    std::array<AC::TerrainPageMeshOptions, 3> invalidOptions{};
    invalidOptions[0].MaxAbsCm = std::numeric_limits<double>::quiet_NaN();
    invalidOptions[1].MaxRmsCm = std::numeric_limits<double>::infinity();
    invalidOptions[2].MaxSharedBoundaryCm = -1.0;
    for (std::size_t index = 0; index < invalidOptions.size(); ++index) {
        const auto result = AC::WriteTerrainPageMesh(
            FlatPage(), {0u, 0u},
            temporary.Path() / std::format("invalid-limit-{}", index),
            invalidOptions[index], detail);
        REQUIRE(IsWriterFailure(result, detail));
    }

    AC::MapPageTiles absentHaloA = FlatPage();
    AC::MapPageTiles absentHaloB = absentHaloA;
    const std::array<std::size_t, 2> absentIndices{
        64u * kStoredSamples + 128u,
        128u * kStoredSamples + 64u,
    };
    for (const std::size_t index : absentIndices) {
        absentHaloA.TilePresent[index] = 0u;
        absentHaloB.TilePresent[index] = 0u;
        absentHaloA.Tiles[index] = HeightTile(127);
        absentHaloA.Tiles[index].TileInfo = 0xffffffffu;
        absentHaloB.Tiles[index] = HeightTile(-127);
        absentHaloB.Tiles[index].BaseTex = 63u;
        absentHaloB.Tiles[index].Block[3] = 255u;
    }
    const auto absentA = AC::WriteTerrainPageMesh(
        absentHaloA, {0u, 0u}, temporary.Path() / "absent-a", {}, detail);
    const auto absentB = AC::WriteTerrainPageMesh(
        absentHaloB, {0u, 0u}, temporary.Path() / "absent-b", {}, detail);
    REQUIRE(absentA.Ok);
    REQUIRE(absentB.Ok);
    REQUIRE(FilesEqual(absentA.GltfPath, absentB.GltfPath));
    REQUIRE(FilesEqual(absentA.BinPath, absentB.BinPath));
    const auto absentMesh = DecodeTerrainGltf(absentA.GltfPath);
    REQUIRE(absentMesh.has_value());
    const auto rightAbsent = FindVertex(*absentMesh, 128u, 64u);
    const auto bottomAbsent = FindVertex(*absentMesh, 64u, 128u);
    REQUIRE(rightAbsent.has_value());
    REQUIRE(bottomAbsent.has_value());
    REQUIRE_EQ(absentMesh->Positions[*rightAbsent][1], -2.0f);
    REQUIRE_EQ(absentMesh->Positions[*bottomAbsent][1], -2.0f);

    const std::filesystem::path regularInsteadOfDirectory =
        temporary.Path() / "not-a-directory";
    {
        std::ofstream output{regularInsteadOfDirectory, std::ios::binary};
        output << "occupied";
    }
    const auto unwritable = AC::WriteTerrainPageMesh(
        FlatPage(), {0u, 0u}, regularInsteadOfDirectory, {}, detail);
    REQUIRE(IsWriterFailure(unwritable, detail));

    const std::filesystem::path partialDirectory = temporary.Path() / "partial";
    REQUIRE(std::filesystem::create_directory(partialDirectory));
    const std::filesystem::path blockedGltf =
        partialDirectory / "garner.terrain_00_00.gltf";
    REQUIRE(std::filesystem::create_directory(blockedGltf));
    const auto partial = AC::WriteTerrainPageMesh(
        FlatPage(), {0u, 0u}, partialDirectory, {}, detail);
    REQUIRE(IsWriterFailure(partial, detail));
    REQUIRE(std::filesystem::is_directory(blockedGltf));
    REQUIRE(IsNoFile(partialDirectory / "garner.terrain_00_00.bin"));
}

} // namespace
