#include "Corsairs/Tools/AssetConverter/TextureResolver.h"

#include <array>
#include <cctype>

namespace Corsairs::Tools::AssetConverter {

namespace {

// Порядок перебора: сначала то, чем игра пользуется фактически.
constexpr std::array<const char*, 6> kExtensions{
    ".png", ".dds", ".jpg", ".jpeg", ".bmp", ".tga",
};

std::string ToLower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

std::optional<std::filesystem::path> TextureResolver::Resolve(
    const std::filesystem::path& category, std::string_view name) const {
    if (!Enabled() || name.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path declared{name};
    const std::string stem = declared.stem().string();
    if (stem.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path dir = _textureRoot / category;

    for (const char* extension : kExtensions) {
        std::filesystem::path candidate = dir / (stem + extension);
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    // Регистр имён на диске может отличаться от записанного в модели, а на
    // чувствительной к регистру файловой системе точное совпадение не сработает.
    // Поэтому при неудаче обходим каталог и сравниваем имена без регистра.
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return std::nullopt;
    }

    const std::string wanted = ToLower(stem);
    for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (ToLower(entry.path().stem().string()) == wanted) {
            return entry.path();
        }
    }

    return std::nullopt;
}

} // namespace Corsairs::Tools::AssetConverter
