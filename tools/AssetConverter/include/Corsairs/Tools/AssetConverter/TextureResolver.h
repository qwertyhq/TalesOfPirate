#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Corsairs::Tools::AssetConverter {

// Разрешает имя текстуры из модели в реальный файл на диске.
//
// Модели ссылаются на текстуры с расширением `.BMP` (например,
// `0066000000.BMP`), тогда как на диске лежит `0066000000.png`: игра перешла с
// BMP на PNG, не переписывая модели, и движок ищет файл, игнорируя расширение.
// Поэтому сопоставление идёт по имени без расширения, с перебором фактически
// существующих вариантов.
//
// Текстуры разложены по тем же категориям, что и модели:
// `Client/model/character/X.lgo` -> `Client/texture/character/X.png`.
class TextureResolver {
public:
    // `textureRoot` — каталог вида `Client/texture`. Пустой путь отключает
    // резолвинг: Resolve всегда вернёт std::nullopt.
    explicit TextureResolver(std::filesystem::path textureRoot)
        : _textureRoot(std::move(textureRoot)) {
    }

    [[nodiscard]] bool Enabled() const {
        return !_textureRoot.empty();
    }

    // `category` — подкаталог модели относительно корня моделей (`character`,
    // `item`, `scene`, ...). `name` — имя текстуры как записано в модели.
    // Возвращает путь к существующему файлу либо std::nullopt.
    [[nodiscard]] std::optional<std::filesystem::path> Resolve(
        const std::filesystem::path& category, std::string_view name) const;

private:
    std::filesystem::path _textureRoot;
};

} // namespace Corsairs::Tools::AssetConverter
