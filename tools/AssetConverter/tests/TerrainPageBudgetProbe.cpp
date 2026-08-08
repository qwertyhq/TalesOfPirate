#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/TerrainCatalog.h"
#include "Corsairs/Tools/AssetConverter/TerrainPageBaker.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

int Fail(const std::string& detail) {
    std::cerr << "TerrainPageBudget: " << detail << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string{argv[1]} != "--repo-root" ||
        std::string{argv[2]}.empty()) {
        return Fail("ожидался ровно один аргумент --repo-root <path>");
    }

    const std::filesystem::path repoRoot =
        std::filesystem::absolute(std::filesystem::path{argv[2]}).lexically_normal();
    const std::filesystem::path mapPath =
        repoRoot / "Client" / "map" / "garner.map";
    const std::filesystem::path databasePath =
        repoRoot / "databases" / "gamedata.sqlite";
    const std::filesystem::path clientRoot = repoRoot / "Client";
    const std::filesystem::path alphaAtlas =
        clientRoot / "texture" / "terrain" / "alpha" / "total.png";

    AC::MapDiagnostics diagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, diagnostics);
    if (!reader.has_value()) {
        return Fail(diagnostics.Detail);
    }

    std::string detail;
    const auto catalog = AC::TerrainCatalog::Load(databasePath, clientRoot, detail);
    if (!catalog.has_value()) {
        return Fail(detail);
    }

    const std::filesystem::path outputDirectory =
        std::filesystem::temp_directory_path() / "corsairs-terrain-page-budget-probe";
    std::error_code error;
    std::filesystem::remove_all(outputDirectory, error);
    error.clear();
    std::filesystem::create_directories(outputDirectory, error);
    if (error) {
        return Fail("не удалось создать private output: " + error.message());
    }

    const AC::TerrainBakeOptions options;
    const AC::TerrainBakeResult result = AC::BakeTerrainPage(
        *reader,
        *catalog,
        AC::TerrainPageId{17u, 21u},
        alphaAtlas,
        outputDirectory,
        options,
        detail);
    if (!result.Ok) {
        std::filesystem::remove_all(outputDirectory, error);
        return Fail(detail);
    }
    std::cout << "TerrainPageBudget: rss=" << result.PeakRssBytes
              << " png=" << result.OutputBytes
              << " cache=" << result.PeakTextureCacheBytes
              << " row=" << result.PeakRgbaRowBytes
              << " sha256=" << result.PngSha256 << '\n';
    std::filesystem::remove_all(outputDirectory, error);
    return 0;
}
