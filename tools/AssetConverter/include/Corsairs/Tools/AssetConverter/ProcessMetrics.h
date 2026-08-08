#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace Corsairs::Tools::AssetConverter {

[[nodiscard]] std::optional<std::size_t> QueryPeakProcessRssBytes(
    std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
