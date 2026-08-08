#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/TerrainCatalog.h"
#include "Corsairs/Tools/AssetConverter/TerrainPageBaker.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

int Fail(const std::string& detail) {
    std::cerr << "TerrainPageBudget: " << detail << '\n';
    return 1;
}

std::filesystem::file_status CleanupSymlinkStatus(
    const std::filesystem::path& path, std::error_code& error) {
    std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found &&
        error == std::errc::no_such_file_or_directory) {
        error.clear();
    }
    return status;
}

bool IsContainedBy(const std::filesystem::path& root,
                   const std::filesystem::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() || *candidatePart != *rootPart) {
            return false;
        }
    }
    return true;
}

std::optional<std::filesystem::path> CanonicalInput(
    const std::filesystem::path& repoRoot,
    const std::filesystem::path& relative,
    std::string_view label,
    std::string& detail) {
    std::error_code error;
    const std::filesystem::path resolved =
        std::filesystem::canonical(repoRoot / relative, error);
    if (error) {
        detail = std::format("не удалось canonicalize вход {}: {}", label,
                             error.message());
        return std::nullopt;
    }
    if (!IsContainedBy(repoRoot, resolved)) {
        detail = std::format("вход {} выходит за canonical repo root", label);
        return std::nullopt;
    }
    return resolved;
}

class PrivateOutputDirectory {
public:
    PrivateOutputDirectory(const PrivateOutputDirectory&) = delete;
    PrivateOutputDirectory& operator=(const PrivateOutputDirectory&) = delete;

    PrivateOutputDirectory(PrivateOutputDirectory&& other) noexcept
        : _path(std::move(other._path)), _owned(other._owned) {
        other._owned = false;
    }

    ~PrivateOutputDirectory() {
        if (_owned) {
            std::error_code ignored;
            std::filesystem::remove_all(_path, ignored);
        }
    }

    static std::optional<PrivateOutputDirectory> Create(std::string& detail) {
        std::error_code error;
        const std::filesystem::path temporaryRoot =
            std::filesystem::temp_directory_path(error);
        if (error) {
            detail = "не удалось определить temporary root: " + error.message();
            return std::nullopt;
        }

        std::uint64_t nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        try {
            std::random_device random;
            nonce ^= static_cast<std::uint64_t>(random()) << 32u;
            nonce ^= static_cast<std::uint64_t>(random());
        }
        catch (...) {
            // Atomic create_directory plus retry remains collision-safe.
        }

        for (std::uint32_t attempt = 0; attempt < 128u; ++attempt) {
            const std::filesystem::path candidate = temporaryRoot / std::format(
                "corsairs-terrain-page-budget-probe-{:016x}-{}", nonce, attempt);
            error.clear();
            if (std::filesystem::create_directory(candidate, error)) {
                detail.clear();
                return PrivateOutputDirectory{candidate};
            }
            if (error && error != std::errc::file_exists) {
                detail = "не удалось создать уникальный private output: " +
                    error.message();
                return std::nullopt;
            }
        }
        detail = "не удалось создать уникальный private output после 128 попыток";
        return std::nullopt;
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return _path;
    }

    [[nodiscard]] bool Cleanup(std::string& detail) {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
        if (error) {
            detail = "не удалось очистить private output " +
                _path.generic_string() + ": " + error.message();
            return false;
        }
        error.clear();
        const std::filesystem::file_status outputStatus =
            CleanupSymlinkStatus(_path, error);
        const bool verifiedGone = !error &&
            outputStatus.type() == std::filesystem::file_type::not_found;
        if (!verifiedGone) {
            detail = "не удалось очистить private output " +
                _path.generic_string() +
                (error ? ": " + error.message() : ": каталог сохранён");
            return false;
        }
        _owned = false;
        detail.clear();
        return true;
    }

private:
    explicit PrivateOutputDirectory(std::filesystem::path path)
        : _path(std::move(path)), _owned(true) {
    }

    std::filesystem::path _path;
    bool _owned{false};
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string{argv[1]} != "--repo-root" ||
        std::string{argv[2]}.empty()) {
        return Fail("ожидался ровно один аргумент --repo-root <path>");
    }

    std::error_code error;
    const std::filesystem::path repoRoot =
        std::filesystem::canonical(std::filesystem::path{argv[2]}, error);
    if (error || !std::filesystem::is_directory(repoRoot, error) || error) {
        return Fail("не удалось canonicalize repo root");
    }

    std::string detail;
    const auto mapPath = CanonicalInput(
        repoRoot, std::filesystem::path{"Client"} / "map" / "garner.map",
        "Client/map/garner.map", detail);
    if (!mapPath.has_value()) {
        return Fail(detail);
    }
    const auto databasePath = CanonicalInput(
        repoRoot, std::filesystem::path{"databases"} / "gamedata.sqlite",
        "databases/gamedata.sqlite", detail);
    if (!databasePath.has_value()) {
        return Fail(detail);
    }
    const auto clientRoot = CanonicalInput(
        repoRoot, "Client", "Client", detail);
    if (!clientRoot.has_value()) {
        return Fail(detail);
    }
    const auto alphaAtlas = CanonicalInput(
        repoRoot,
        std::filesystem::path{"Client"} / "texture" / "terrain" / "alpha" /
            "total.png",
        "Client/texture/terrain/alpha/total.png", detail);
    if (!alphaAtlas.has_value()) {
        return Fail(detail);
    }

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(*mapPath, diagnostics);
    if (!reader.has_value()) {
        return Fail(diagnostics.Detail);
    }

    const auto catalog = AC::TerrainCatalog::Load(
        *databasePath, *clientRoot, detail);
    if (!catalog.has_value()) {
        return Fail(detail);
    }

    auto outputDirectory = PrivateOutputDirectory::Create(detail);
    if (!outputDirectory.has_value()) {
        return Fail(detail);
    }

    const AC::TerrainBakeOptions options;
    const AC::TerrainBakeResult result = AC::BakeTerrainPage(
        *reader,
        *catalog,
        AC::TerrainPageId{17u, 21u},
        *alphaAtlas,
        outputDirectory->Path(),
        options,
        detail);
    if (!result.Ok) {
        const std::string bakeDetail = detail;
        if (!outputDirectory->Cleanup(detail)) {
            return Fail(detail + "; bake=" + bakeDetail);
        }
        return Fail(bakeDetail);
    }
    if (!outputDirectory->Cleanup(detail)) {
        return Fail(detail);
    }
    std::cout << "TerrainPageBudget: rss=" << result.PeakRssBytes
              << " png=" << result.OutputBytes
              << " cache=" << result.PeakTextureCacheBytes
              << " row=" << result.PeakRgbaRowBytes
              << " sha256=" << result.PngSha256 << '\n';
    return 0;
}
