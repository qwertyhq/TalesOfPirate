#include "Corsairs/Tools/AssetConverter/TerrainCatalog.h"

#include "sqlite3.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace Corsairs::Tools::AssetConverter {

namespace {

struct SqliteCloser {
    void operator()(sqlite3* database) const noexcept {
        sqlite3_close(database);
    }
};

struct StatementFinalizer {
    void operator()(sqlite3_stmt* statement) const noexcept {
        sqlite3_finalize(statement);
    }
};

using DatabasePtr = std::unique_ptr<sqlite3, SqliteCloser>;
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::filesystem::path PathFromUtf8(std::string_view utf8) {
    std::u8string converted;
    converted.reserve(utf8.size());
    for (const unsigned char byte : utf8) {
        converted.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path{converted};
}

bool IsUnderRoot(const std::filesystem::path& path,
                 const std::filesystem::path& root) {
    auto pathPart = path.begin();
    for (auto rootPart = root.begin(); rootPart != root.end(); ++rootPart, ++pathPart) {
        if (pathPart == path.end() || *pathPart != *rootPart) {
            return false;
        }
    }
    return true;
}

bool NormalizeSource(const std::filesystem::path& clientRoot,
                     std::string_view source,
                     std::filesystem::path& result,
                     std::string& detail) {
    const std::string sourceText{source};
    const std::filesystem::path relative = PathFromUtf8(source);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory()) {
        detail = "путь текстуры должен быть относительным: " + sourceText;
        return false;
    }
    for (const auto& component : relative) {
        if (component == "..") {
            detail = "путь текстуры содержит переход к родителю: " + sourceText;
            return false;
        }
    }

    std::string extension = relative.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (extension != ".bmp") {
        detail = "ожидалось конечное расширение .bmp: " + sourceText;
        return false;
    }

    std::filesystem::path converted = relative;
    converted.replace_extension(".png");
    const std::filesystem::path candidate = (clientRoot / converted).lexically_normal();

    std::error_code error;
    const std::filesystem::path canonicalRoot = std::filesystem::canonical(clientRoot, error);
    if (error) {
        detail = "недоступен корень клиента: " + PathToUtf8(clientRoot);
        return false;
    }
    const std::filesystem::path canonicalCandidate = std::filesystem::canonical(candidate, error);
    if (error || !IsUnderRoot(canonicalCandidate, canonicalRoot)) {
        detail = "PNG текстуры отсутствует или выходит за корень клиента: " +
                 PathToUtf8(candidate);
        return false;
    }

    if (!std::filesystem::is_regular_file(canonicalCandidate, error) || error) {
        detail = "PNG текстуры не является обычным файлом: " + PathToUtf8(candidate);
        return false;
    }

    std::ifstream readable{canonicalCandidate, std::ios::binary};
    if (!readable) {
        detail = "PNG текстуры недоступен для чтения: " + PathToUtf8(candidate);
        return false;
    }

    result = canonicalCandidate;
    return true;
}

} // namespace

std::optional<TerrainCatalog> TerrainCatalog::Load(
    const std::filesystem::path& sqlitePath,
    const std::filesystem::path& clientRoot,
    std::string& detail) {
    detail.clear();
    sqlite3* rawDatabase = nullptr;
    const std::string sqlitePathUtf8 = PathToUtf8(sqlitePath);
    const int openResult = sqlite3_open_v2(sqlitePathUtf8.c_str(), &rawDatabase,
                                           SQLITE_OPEN_READONLY, nullptr);
    DatabasePtr database{rawDatabase};
    if (openResult != SQLITE_OK || database == nullptr) {
        detail = "не удалось открыть каталог terrain: " + sqlitePathUtf8;
        return std::nullopt;
    }

    sqlite3_stmt* rawStatement = nullptr;
    const char* query = "SELECT id, name FROM terrains";
    if (sqlite3_prepare_v2(database.get(), query, -1, &rawStatement, nullptr) != SQLITE_OK) {
        detail = "не удалось прочитать таблицу terrains: " +
                 std::string{sqlite3_errmsg(database.get())};
        return std::nullopt;
    }
    StatementPtr statement{rawStatement};

    TerrainCatalog catalog;
    int stepResult = SQLITE_ROW;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const sqlite3_int64 id = sqlite3_column_int64(statement.get(), 0);
        const unsigned char* text = sqlite3_column_text(statement.get(), 1);
        const int textBytes = sqlite3_column_bytes(statement.get(), 1);
        if (id < 0 || id > 255 || text == nullptr || textBytes < 0) {
            detail = "некорректная запись в таблице terrains";
            return std::nullopt;
        }

        std::filesystem::path resolved;
        const std::string_view source{reinterpret_cast<const char*>(text),
                                      static_cast<std::size_t>(textBytes)};
        if (!NormalizeSource(clientRoot, source,
                             resolved, detail)) {
            return std::nullopt;
        }
        catalog._paths[static_cast<std::size_t>(id)] = std::move(resolved);
    }

    if (stepResult != SQLITE_DONE) {
        detail = "ошибка чтения таблицы terrains: " +
                 std::string{sqlite3_errmsg(database.get())};
        return std::nullopt;
    }
    return catalog;
}

std::optional<std::filesystem::path> TerrainCatalog::Resolve(
    std::uint8_t textureId) const {
    return _paths[textureId];
}

} // namespace Corsairs::Tools::AssetConverter
