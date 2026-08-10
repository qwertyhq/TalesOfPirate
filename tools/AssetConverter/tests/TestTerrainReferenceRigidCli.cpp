#include "Corsairs/Tools/AssetConverter/TerrainReferenceCommand.h"

#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

constexpr std::array<std::string_view, 28> kRequiredArguments{
    "--map", "Client/map/garner.map",
    "--database", "databases/gamedata.sqlite",
    "--client-root", "Client",
    "--alpha", "Client/texture/terrain/alpha/total.png",
    "--output", "artifacts/maps",
    "--page", "17", "21",
    "--require-present-rect", "2193", "2756", "80", "47",
    "--max-rss-mib", "128",
    "--max-cache-mib", "32",
    "--max-png-mib", "96",
    "--max-height-error-cm", "5",
    "--max-rms-error-cm", "2",
};

CORSAIRS_TEST(TerrainReferenceRigidCli_DefaultRemainsTask8Legacy) {
    std::string detail;
    const auto options = AC::ParseTerrainReferenceArguments(
        kRequiredArguments, detail);
    REQUIRE(options.has_value());
    REQUIRE(detail.empty());
    REQUIRE_EQ(static_cast<std::uint32_t>(options->Mesh.CoordinateProfile),
               static_cast<std::uint32_t>(
                   AC::TerrainPageCoordinateProfile::Task8Legacy));
}

CORSAIRS_TEST(TerrainReferenceRigidCli_ParsesExactOptIn) {
    std::array<std::string_view, 30> arguments{};
    std::copy(kRequiredArguments.begin(), kRequiredArguments.end(),
              arguments.begin());
    arguments[28] = "--coordinate-profile";
    arguments[29] = "rigid-q";

    std::string detail;
    const auto options = AC::ParseTerrainReferenceArguments(arguments, detail);
    REQUIRE(options.has_value());
    REQUIRE(detail.empty());
    REQUIRE_EQ(static_cast<std::uint32_t>(options->Mesh.CoordinateProfile),
               static_cast<std::uint32_t>(
                   AC::TerrainPageCoordinateProfile::RigidQ));
}

CORSAIRS_TEST(TerrainReferenceRigidCli_RejectsOtherOrDuplicateProfiles) {
    std::array<std::string_view, 30> wrong{};
    std::copy(kRequiredArguments.begin(), kRequiredArguments.end(),
              wrong.begin());
    wrong[28] = "--coordinate-profile";
    wrong[29] = "RigidQ";
    std::string detail;
    REQUIRE(!AC::ParseTerrainReferenceArguments(wrong, detail).has_value());
    REQUIRE(detail.find("rigid-q") != std::string::npos);

    std::array<std::string_view, 32> duplicate{};
    std::copy(kRequiredArguments.begin(), kRequiredArguments.end(),
              duplicate.begin());
    duplicate[28] = "--coordinate-profile";
    duplicate[29] = "rigid-q";
    duplicate[30] = "--coordinate-profile";
    duplicate[31] = "rigid-q";
    REQUIRE(!AC::ParseTerrainReferenceArguments(duplicate, detail).has_value());
    REQUIRE(detail.find("duplicate") != std::string::npos);
}

CORSAIRS_TEST(TerrainReferenceRigidCli_HelpShowsExactOptInWithoutDependencies) {
    std::size_t recoveryCalls = 0u;
    AC::TerrainReferenceDependencies dependencies;
    dependencies.RecoverBeforeBuild =
        [&](const AC::TerrainReferenceOptions&) {
            ++recoveryCalls;
            return AC::TerrainPublicationResult{};
        };
    const std::array<std::string_view, 3> arguments{
        "AssetConverter", "terrain-reference", "--help"};
    std::ostringstream output;
    std::ostringstream error;
    const auto exitCode = AC::TryRunTerrainReferenceSubcommand(
        arguments, dependencies, output, error);
    REQUIRE(exitCode.has_value());
    REQUIRE_EQ(*exitCode, 0);
    REQUIRE_EQ(recoveryCalls, 0u);
    REQUIRE(error.str().empty());
    REQUIRE(output.str().find("[--coordinate-profile rigid-q]") !=
            std::string::npos);
}

} // namespace
