#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Corsairs::Tools::AssetConverter {

class TerrainCatalog {
public:
    [[nodiscard]] static std::optional<TerrainCatalog> Load(
        const std::filesystem::path& sqlitePath,
        const std::filesystem::path& clientRoot,
        std::string& detail);

    [[nodiscard]] std::optional<std::filesystem::path> Resolve(
        std::uint8_t textureId) const;

private:
    std::array<std::optional<std::filesystem::path>, 256> _paths{};
};

} // namespace Corsairs::Tools::AssetConverter
