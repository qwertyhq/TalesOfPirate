#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Corsairs::Tools::AssetConverter {

[[nodiscard]] std::string Sha256Bytes(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<std::string> Sha256File(
    const std::filesystem::path& path, std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
