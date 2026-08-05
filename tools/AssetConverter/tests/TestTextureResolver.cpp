#include "Corsairs/Tools/AssetConverter/TextureResolver.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path TextureRoot() {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "texture";
}

CORSAIRS_TEST(TextureResolver_FindsPngForBmpReference) {
    const AC::TextureResolver resolver{TextureRoot()};
    REQUIRE(resolver.Enabled());

    // Модель ссылается на .BMP, на диске лежит .png — расширение игнорируется.
    const auto found = resolver.Resolve("character", "0066000000.BMP");
    REQUIRE(found.has_value());
    REQUIRE_EQ(found->filename().string(), std::string{"0066000000.png"});
    REQUIRE(std::filesystem::is_regular_file(*found));
}

CORSAIRS_TEST(TextureResolver_ReturnsNulloptForMissingTexture) {
    const AC::TextureResolver resolver{TextureRoot()};

    REQUIRE(!resolver.Resolve("character", "нет-такой-текстуры.bmp").has_value());
    REQUIRE(!resolver.Resolve("character", "").has_value());
    REQUIRE(!resolver.Resolve("такой-категории-нет", "0066000000.BMP").has_value());
}

CORSAIRS_TEST(TextureResolver_DisabledWithoutRoot) {
    const AC::TextureResolver resolver{std::filesystem::path{}};

    REQUIRE(!resolver.Enabled());
    REQUIRE(!resolver.Resolve("character", "0066000000.BMP").has_value());
}

} // namespace
