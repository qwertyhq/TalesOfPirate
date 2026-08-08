#include "Corsairs/Tools/AssetConverter/TerrainReferenceCommand.h"

#include "Corsairs/Tools/AssetConverter/MapWriter.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"
#include "Corsairs/Tools/AssetConverter/StreamingMapWriter.h"
#include "Corsairs/Tools/AssetConverter/TerrainCatalog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::string_view kUsage =
    "Использование: AssetConverter terrain-reference "
    "--map <path> --database <path> --client-root <path> --alpha <path> "
    "--output <path> --page X Y --require-present-rect X Y Width Height "
    "--max-rss-mib N --max-cache-mib N --max-png-mib N "
    "--max-height-error-cm N --max-rms-error-cm N";

enum class JsonType : std::uint32_t {
    OBJECT,
    ARRAY,
    STRING,
    NUMBER,
    BOOLEAN,
    NIL,
};

struct JsonValue {
    JsonType Type{JsonType::NIL};
    std::vector<std::pair<std::string, JsonValue>> Object;
    std::vector<JsonValue> Array;
    std::string Text;
    bool Boolean{false};
};

std::string EscapePointer(std::string_view value) {
    std::string escaped;
    for (const char character : value) {
        if (character == '~') {
            escaped += "~0";
        }
        else if (character == '/') {
            escaped += "~1";
        }
        else {
            escaped += character;
        }
    }
    return escaped;
}

std::string ChildPointer(std::string_view parent, std::string_view child) {
    return std::format("{}/{}", parent, EscapePointer(child));
}

void AppendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7fu) {
        output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
    else if (codePoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
    else {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

std::size_t Utf8SequenceLength(std::string_view text, std::size_t position) {
    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    const auto continuation = [&](std::size_t index,
                                  unsigned char minimum,
                                  unsigned char maximum) {
        return index < text.size() && byte(index) >= minimum &&
            byte(index) <= maximum;
    };
    const unsigned char first = byte(position);
    if (first < 0x80u) {
        return 1u;
    }
    if (first >= 0xc2u && first <= 0xdfu) {
        return continuation(position + 1u, 0x80u, 0xbfu) ? 2u : 0u;
    }
    if (first == 0xe0u) {
        return continuation(position + 1u, 0xa0u, 0xbfu) &&
            continuation(position + 2u, 0x80u, 0xbfu) ? 3u : 0u;
    }
    if (first >= 0xe1u && first <= 0xecu) {
        return continuation(position + 1u, 0x80u, 0xbfu) &&
            continuation(position + 2u, 0x80u, 0xbfu) ? 3u : 0u;
    }
    if (first == 0xedu) {
        return continuation(position + 1u, 0x80u, 0x9fu) &&
            continuation(position + 2u, 0x80u, 0xbfu) ? 3u : 0u;
    }
    if (first >= 0xeeu && first <= 0xefu) {
        return continuation(position + 1u, 0x80u, 0xbfu) &&
            continuation(position + 2u, 0x80u, 0xbfu) ? 3u : 0u;
    }
    if (first == 0xf0u) {
        return continuation(position + 1u, 0x90u, 0xbfu) &&
            continuation(position + 2u, 0x80u, 0xbfu) &&
            continuation(position + 3u, 0x80u, 0xbfu) ? 4u : 0u;
    }
    if (first >= 0xf1u && first <= 0xf3u) {
        return continuation(position + 1u, 0x80u, 0xbfu) &&
            continuation(position + 2u, 0x80u, 0xbfu) &&
            continuation(position + 3u, 0x80u, 0xbfu) ? 4u : 0u;
    }
    if (first == 0xf4u) {
        return continuation(position + 1u, 0x80u, 0x8fu) &&
            continuation(position + 2u, 0x80u, 0xbfu) &&
            continuation(position + 3u, 0x80u, 0xbfu) ? 4u : 0u;
    }
    return 0u;
}

class StrictJsonParser {
public:
    explicit StrictJsonParser(std::string_view input) : _input(input) {
    }

    std::optional<JsonValue> Parse(std::vector<TerrainManifestIssue>& issues) {
        SkipWhitespace();
        auto value = ParseValue("", issues);
        if (!value.has_value()) {
            return std::nullopt;
        }
        SkipWhitespace();
        if (_position != _input.size()) {
            AddIssue(issues, "", "trailing input");
            return std::nullopt;
        }
        return value;
    }

private:
    void SkipWhitespace() {
        while (_position < _input.size() &&
               (_input[_position] == ' ' || _input[_position] == '\n' ||
                _input[_position] == '\r' || _input[_position] == '\t')) {
            ++_position;
        }
    }

    void AddIssue(std::vector<TerrainManifestIssue>& issues,
                  std::string path,
                  std::string detail) const {
        if (issues.empty()) {
            issues.push_back({TerrainManifestIssueCode::INVALID_SCHEMA,
                              std::move(path), std::move(detail)});
        }
    }

    std::optional<JsonValue> ParseValue(
        std::string_view path,
        std::vector<TerrainManifestIssue>& issues) {
        SkipWhitespace();
        if (_position >= _input.size()) {
            AddIssue(issues, std::string{path}, "unexpected end of JSON");
            return std::nullopt;
        }
        switch (_input[_position]) {
        case '{': return ParseObject(path, issues);
        case '[': return ParseArray(path, issues);
        case '"': {
            auto text = ParseString(path, issues);
            if (!text.has_value()) {
                return std::nullopt;
            }
            JsonValue value;
            value.Type = JsonType::STRING;
            value.Text = std::move(*text);
            return value;
        }
        case 't': return ParseLiteral(path, "true", JsonType::BOOLEAN, true, issues);
        case 'f': return ParseLiteral(path, "false", JsonType::BOOLEAN, false, issues);
        case 'n': return ParseLiteral(path, "null", JsonType::NIL, false, issues);
        default: return ParseNumber(path, issues);
        }
    }

    std::optional<JsonValue> ParseObject(
        std::string_view path,
        std::vector<TerrainManifestIssue>& issues) {
        ++_position;
        JsonValue result;
        result.Type = JsonType::OBJECT;
        SkipWhitespace();
        if (_position < _input.size() && _input[_position] == '}') {
            ++_position;
            return result;
        }
        while (true) {
            SkipWhitespace();
            if (_position >= _input.size() || _input[_position] != '"') {
                AddIssue(issues, std::string{path}, "object key must be a string");
                return std::nullopt;
            }
            auto key = ParseString(path, issues);
            if (!key.has_value()) {
                return std::nullopt;
            }
            if (std::ranges::any_of(result.Object, [&](const auto& entry) {
                    return entry.first == *key;
                })) {
                AddIssue(issues, ChildPointer(path, *key), "duplicate key");
                return std::nullopt;
            }
            SkipWhitespace();
            if (_position >= _input.size() || _input[_position] != ':') {
                AddIssue(issues, ChildPointer(path, *key), "expected ':'");
                return std::nullopt;
            }
            ++_position;
            auto value = ParseValue(ChildPointer(path, *key), issues);
            if (!value.has_value()) {
                return std::nullopt;
            }
            result.Object.emplace_back(std::move(*key), std::move(*value));
            SkipWhitespace();
            if (_position >= _input.size()) {
                AddIssue(issues, std::string{path}, "unterminated object");
                return std::nullopt;
            }
            const char delimiter = _input[_position++];
            if (delimiter == '}') {
                return result;
            }
            if (delimiter != ',') {
                AddIssue(issues, std::string{path}, "expected ',' or '}'");
                return std::nullopt;
            }
        }
    }

    std::optional<JsonValue> ParseArray(
        std::string_view path,
        std::vector<TerrainManifestIssue>& issues) {
        ++_position;
        JsonValue result;
        result.Type = JsonType::ARRAY;
        SkipWhitespace();
        if (_position < _input.size() && _input[_position] == ']') {
            ++_position;
            return result;
        }
        std::size_t index = 0u;
        while (true) {
            auto value = ParseValue(std::format("{}/{}", path, index), issues);
            if (!value.has_value()) {
                return std::nullopt;
            }
            result.Array.push_back(std::move(*value));
            ++index;
            SkipWhitespace();
            if (_position >= _input.size()) {
                AddIssue(issues, std::string{path}, "unterminated array");
                return std::nullopt;
            }
            const char delimiter = _input[_position++];
            if (delimiter == ']') {
                return result;
            }
            if (delimiter != ',') {
                AddIssue(issues, std::string{path}, "expected ',' or ']'");
                return std::nullopt;
            }
        }
    }

    std::optional<std::string> ParseString(
        std::string_view path,
        std::vector<TerrainManifestIssue>& issues) {
        ++_position;
        std::string result;
        while (_position < _input.size()) {
            const unsigned char character =
                static_cast<unsigned char>(_input[_position++]);
            if (character == '"') {
                return result;
            }
            if (character < 0x20u) {
                AddIssue(issues, std::string{path}, "unescaped control character");
                return std::nullopt;
            }
            if (character >= 0x80u) {
                const std::size_t start = _position - 1u;
                const std::size_t length = Utf8SequenceLength(_input, start);
                if (length == 0u) {
                    AddIssue(issues, std::string{path}, "invalid UTF-8 string");
                    return std::nullopt;
                }
                result.append(_input, start, length);
                _position = start + length;
                continue;
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (_position >= _input.size()) {
                AddIssue(issues, std::string{path}, "unterminated escape");
                return std::nullopt;
            }
            const char escape = _input[_position++];
            switch (escape) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                const auto first = ParseHexCodePoint(path, issues);
                if (!first.has_value()) {
                    return std::nullopt;
                }
                std::uint32_t codePoint = *first;
                if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                    if (_position + 2u > _input.size() ||
                        _input.substr(_position, 2u) != "\\u") {
                        AddIssue(issues, std::string{path}, "missing low surrogate");
                        return std::nullopt;
                    }
                    _position += 2u;
                    const auto second = ParseHexCodePoint(path, issues);
                    if (!second.has_value() || *second < 0xdc00u || *second > 0xdfffu) {
                        AddIssue(issues, std::string{path}, "invalid low surrogate");
                        return std::nullopt;
                    }
                    codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) +
                        (*second - 0xdc00u);
                }
                else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                    AddIssue(issues, std::string{path}, "unexpected low surrogate");
                    return std::nullopt;
                }
                AppendUtf8(result, codePoint);
                break;
            }
            default:
                AddIssue(issues, std::string{path}, "invalid string escape");
                return std::nullopt;
            }
        }
        AddIssue(issues, std::string{path}, "unterminated string");
        return std::nullopt;
    }

    std::optional<std::uint32_t> ParseHexCodePoint(
        std::string_view path,
        std::vector<TerrainManifestIssue>& issues) {
        if (_position + 4u > _input.size()) {
            AddIssue(issues, std::string{path}, "truncated unicode escape");
            return std::nullopt;
        }
        std::uint32_t value = 0u;
        for (std::size_t index = 0u; index < 4u; ++index) {
            const char character = _input[_position++];
            value <<= 4u;
            if (character >= '0' && character <= '9') {
                value += static_cast<std::uint32_t>(character - '0');
            }
            else if (character >= 'a' && character <= 'f') {
                value += static_cast<std::uint32_t>(character - 'a' + 10);
            }
            else if (character >= 'A' && character <= 'F') {
                value += static_cast<std::uint32_t>(character - 'A' + 10);
            }
            else {
                AddIssue(issues, std::string{path}, "invalid unicode escape");
                return std::nullopt;
            }
        }
        return value;
    }

    std::optional<JsonValue> ParseNumber(
        std::string_view path,
        std::vector<TerrainManifestIssue>& issues) {
        const std::size_t begin = _position;
        if (_input[_position] == '-') {
            ++_position;
        }
        if (_position >= _input.size()) {
            AddIssue(issues, std::string{path}, "invalid number");
            return std::nullopt;
        }
        if (_input[_position] == '0') {
            ++_position;
            if (_position < _input.size() && _input[_position] >= '0' &&
                _input[_position] <= '9') {
                AddIssue(issues, std::string{path}, "leading zero in number");
                return std::nullopt;
            }
        }
        else if (_input[_position] >= '1' && _input[_position] <= '9') {
            while (_position < _input.size() && _input[_position] >= '0' &&
                   _input[_position] <= '9') {
                ++_position;
            }
        }
        else {
            AddIssue(issues, std::string{path}, "invalid number");
            return std::nullopt;
        }
        if (_position < _input.size() && _input[_position] == '.') {
            ++_position;
            const std::size_t digits = _position;
            while (_position < _input.size() && _input[_position] >= '0' &&
                   _input[_position] <= '9') {
                ++_position;
            }
            if (_position == digits) {
                AddIssue(issues, std::string{path}, "fraction has no digits");
                return std::nullopt;
            }
        }
        if (_position < _input.size() &&
            (_input[_position] == 'e' || _input[_position] == 'E')) {
            ++_position;
            if (_position < _input.size() &&
                (_input[_position] == '+' || _input[_position] == '-')) {
                ++_position;
            }
            const std::size_t digits = _position;
            while (_position < _input.size() && _input[_position] >= '0' &&
                   _input[_position] <= '9') {
                ++_position;
            }
            if (_position == digits) {
                AddIssue(issues, std::string{path}, "exponent has no digits");
                return std::nullopt;
            }
        }
        JsonValue result;
        result.Type = JsonType::NUMBER;
        result.Text = std::string{_input.substr(begin, _position - begin)};
        return result;
    }

    std::optional<JsonValue> ParseLiteral(
        std::string_view path,
        std::string_view literal,
        JsonType type,
        bool boolean,
        std::vector<TerrainManifestIssue>& issues) {
        if (_input.substr(_position, literal.size()) != literal) {
            AddIssue(issues, std::string{path}, "invalid JSON token");
            return std::nullopt;
        }
        _position += literal.size();
        JsonValue result;
        result.Type = type;
        result.Boolean = boolean;
        return result;
    }

    std::string_view _input;
    std::size_t _position{0};
};

void AddSchemaIssue(std::vector<TerrainManifestIssue>& issues,
                    std::string path,
                    std::string detail) {
    if (issues.empty()) {
        issues.push_back({TerrainManifestIssueCode::INVALID_SCHEMA,
                          std::move(path), std::move(detail)});
    }
}

bool RequireType(const JsonValue& value,
                 JsonType type,
                 std::string_view path,
                 std::vector<TerrainManifestIssue>& issues) {
    if (value.Type != type) {
        AddSchemaIssue(issues, std::string{path}, "wrong JSON type");
        return false;
    }
    return true;
}

const JsonValue* RequireMember(
    const JsonValue& object,
    std::string_view key,
    JsonType type,
    std::string_view path,
    std::vector<TerrainManifestIssue>& issues) {
    const std::string child = ChildPointer(path, key);
    const auto found = std::ranges::find_if(object.Object, [&](const auto& entry) {
        return entry.first == key;
    });
    if (found == object.Object.end()) {
        AddSchemaIssue(issues, child, "required field is missing");
        return nullptr;
    }
    return RequireType(found->second, type, child, issues) ? &found->second : nullptr;
}

bool RejectUnknownMembers(
    const JsonValue& object,
    std::span<const std::string_view> expected,
    std::string_view path,
    std::vector<TerrainManifestIssue>& issues) {
    for (const auto& [key, value] : object.Object) {
        static_cast<void>(value);
        if (std::ranges::find(expected, key) == expected.end()) {
            AddSchemaIssue(issues, ChildPointer(path, key), "unknown key");
            return false;
        }
    }
    return true;
}

bool ReadUint(const JsonValue& value,
              std::uint64_t maximum,
              std::string_view path,
              std::uint64_t& result,
              std::vector<TerrainManifestIssue>& issues) {
    if (!RequireType(value, JsonType::NUMBER, path, issues) ||
        value.Text.empty() || value.Text.front() == '-' ||
        value.Text.find_first_of(".eE") != std::string::npos) {
        if (issues.empty()) {
            AddSchemaIssue(issues, std::string{path}, "expected unsigned integer");
        }
        return false;
    }
    const auto converted = std::from_chars(
        value.Text.data(), value.Text.data() + value.Text.size(), result);
    if (converted.ec != std::errc{} ||
        converted.ptr != value.Text.data() + value.Text.size() ||
        result > maximum) {
        AddSchemaIssue(issues, std::string{path}, "unsigned integer out of range");
        return false;
    }
    return true;
}

bool ReadDouble(const JsonValue& value,
                std::string_view path,
                double& result,
                std::vector<TerrainManifestIssue>& issues) {
    if (!RequireType(value, JsonType::NUMBER, path, issues)) {
        return false;
    }
    const auto converted = std::from_chars(
        value.Text.data(), value.Text.data() + value.Text.size(), result);
    if (converted.ec != std::errc{} ||
        converted.ptr != value.Text.data() + value.Text.size() ||
        !std::isfinite(result)) {
        AddSchemaIssue(issues, std::string{path}, "number is not finite");
        return false;
    }
    return true;
}

bool ReadString(const JsonValue& value,
                std::string_view path,
                std::string& result,
                std::vector<TerrainManifestIssue>& issues) {
    if (!RequireType(value, JsonType::STRING, path, issues)) {
        return false;
    }
    result = value.Text;
    return true;
}

bool ParseRect(const JsonValue& value,
               std::string_view path,
               MapCellRect& result,
               std::vector<TerrainManifestIssue>& issues) {
    constexpr std::array<std::string_view, 4> keys{"x", "y", "width", "height"};
    if (!RequireType(value, JsonType::OBJECT, path, issues) ||
        !RejectUnknownMembers(value, keys, path, issues)) {
        return false;
    }
    const JsonValue* x = RequireMember(value, "x", JsonType::NUMBER, path, issues);
    const JsonValue* y = RequireMember(value, "y", JsonType::NUMBER, path, issues);
    const JsonValue* width = RequireMember(value, "width", JsonType::NUMBER, path, issues);
    const JsonValue* height = RequireMember(value, "height", JsonType::NUMBER, path, issues);
    std::uint64_t converted = 0u;
    if (x == nullptr || !ReadUint(*x, UINT32_MAX, ChildPointer(path, "x"), converted, issues)) {
        return false;
    }
    result.X = static_cast<std::uint32_t>(converted);
    if (y == nullptr || !ReadUint(*y, UINT32_MAX, ChildPointer(path, "y"), converted, issues)) {
        return false;
    }
    result.Y = static_cast<std::uint32_t>(converted);
    if (width == nullptr || !ReadUint(*width, UINT32_MAX,
                                      ChildPointer(path, "width"), converted, issues)) {
        return false;
    }
    result.Width = static_cast<std::uint32_t>(converted);
    if (height == nullptr || !ReadUint(*height, UINT32_MAX,
                                       ChildPointer(path, "height"), converted, issues)) {
        return false;
    }
    result.Height = static_cast<std::uint32_t>(converted);
    return true;
}

bool ParseHashedInput(const JsonValue& value,
                      std::string_view path,
                      TerrainHashedInputDto& result,
                      std::vector<TerrainManifestIssue>& issues) {
    constexpr std::array<std::string_view, 2> keys{"path", "sha256"};
    if (!RequireType(value, JsonType::OBJECT, path, issues) ||
        !RejectUnknownMembers(value, keys, path, issues)) {
        return false;
    }
    const JsonValue* pathValue = RequireMember(value, "path", JsonType::STRING, path, issues);
    const JsonValue* hashValue = RequireMember(value, "sha256", JsonType::STRING, path, issues);
    std::string text;
    if (pathValue == nullptr ||
        !ReadString(*pathValue, ChildPointer(path, "path"), text, issues)) {
        return false;
    }
    result.Path = std::filesystem::path{text};
    return hashValue != nullptr &&
        ReadString(*hashValue, ChildPointer(path, "sha256"), result.Sha256, issues);
}

bool ParseOutputFile(const JsonValue& value,
                     std::string_view path,
                     TerrainOutputFile& result,
                     std::vector<TerrainManifestIssue>& issues) {
    constexpr std::array<std::string_view, 3> keys{"path", "sha256", "sizeBytes"};
    if (!RequireType(value, JsonType::OBJECT, path, issues) ||
        !RejectUnknownMembers(value, keys, path, issues)) {
        return false;
    }
    const JsonValue* pathValue = RequireMember(value, "path", JsonType::STRING, path, issues);
    const JsonValue* hashValue = RequireMember(value, "sha256", JsonType::STRING, path, issues);
    const JsonValue* sizeValue = RequireMember(value, "sizeBytes", JsonType::NUMBER, path, issues);
    std::string text;
    if (pathValue == nullptr ||
        !ReadString(*pathValue, ChildPointer(path, "path"), text, issues)) {
        return false;
    }
    result.ManifestRelativePath = std::filesystem::path{text};
    if (hashValue == nullptr ||
        !ReadString(*hashValue, ChildPointer(path, "sha256"), result.Sha256, issues)) {
        return false;
    }
    return sizeValue != nullptr && ReadUint(
        *sizeValue, UINT64_MAX, ChildPointer(path, "sizeBytes"),
        result.SizeBytes, issues);
}

class JsonEmitter {
public:
    void ObjectBegin() { BeforeValue(); _output.push_back('{'); _frames.push_back({true, true}); }
    void ObjectEnd() { _output.push_back('}'); _frames.pop_back(); }
    void ArrayBegin() { BeforeValue(); _output.push_back('['); _frames.push_back({false, true}); }
    void ArrayEnd() { _output.push_back(']'); _frames.pop_back(); }
    void Key(std::string_view key) {
        Frame& frame = _frames.back();
        if (!frame.First) { _output.push_back(','); }
        frame.First = false;
        AppendString(key);
        _output.push_back(':');
        _afterKey = true;
    }
    void String(std::string_view value) { BeforeValue(); AppendString(value); }
    void Uint(std::uint64_t value) {
        BeforeValue();
        std::array<char, 32> buffer{};
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        _output.append(buffer.data(), converted.ptr);
    }
    void Double(double value) {
        BeforeValue();
        _output += std::format("{}", value);
    }
    [[nodiscard]] std::string Take() && { return std::move(_output); }

private:
    struct Frame { bool Object; bool First; };
    void BeforeValue() {
        if (_afterKey) { _afterKey = false; return; }
        if (!_frames.empty() && !_frames.back().Object) {
            if (!_frames.back().First) { _output.push_back(','); }
            _frames.back().First = false;
        }
    }
    void AppendString(std::string_view value) {
        _output.push_back('"');
        for (const unsigned char character : value) {
            switch (character) {
            case '"': _output += "\\\""; break;
            case '\\': _output += "\\\\"; break;
            case '\b': _output += "\\b"; break;
            case '\f': _output += "\\f"; break;
            case '\n': _output += "\\n"; break;
            case '\r': _output += "\\r"; break;
            case '\t': _output += "\\t"; break;
            default:
                if (character < 0x20u) {
                    _output += std::format("\\u{:04x}", character);
                }
                else {
                    _output.push_back(static_cast<char>(character));
                }
                break;
            }
        }
        _output.push_back('"');
    }
    std::string _output;
    std::vector<Frame> _frames;
    bool _afterKey{false};
};

void EmitRect(JsonEmitter& json, const MapCellRect& rect) {
    json.ObjectBegin();
    json.Key("x"); json.Uint(rect.X);
    json.Key("y"); json.Uint(rect.Y);
    json.Key("width"); json.Uint(rect.Width);
    json.Key("height"); json.Uint(rect.Height);
    json.ObjectEnd();
}

void EmitHashedInput(JsonEmitter& json, const TerrainHashedInputDto& input) {
    json.ObjectBegin();
    json.Key("path"); json.String(input.Path.generic_string());
    json.Key("sha256"); json.String(input.Sha256);
    json.ObjectEnd();
}

void EmitOutputFile(JsonEmitter& json, const TerrainOutputFile& file) {
    json.ObjectBegin();
    json.Key("path"); json.String(file.ManifestRelativePath.generic_string());
    json.Key("sha256"); json.String(file.Sha256);
    json.Key("sizeBytes"); json.Uint(file.SizeBytes);
    json.ObjectEnd();
}

bool ParseUnsignedText(std::string_view text,
                       std::uint64_t maximum,
                       std::uint64_t& result) {
    if (text.empty() || text.front() == '-' || text.front() == '+') {
        return false;
    }
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), result);
    return converted.ec == std::errc{} &&
        converted.ptr == text.data() + text.size() && result <= maximum;
}

bool ParseDoubleText(std::string_view text, double& result) {
    if (text.empty()) {
        return false;
    }
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), result);
    return converted.ec == std::errc{} &&
        converted.ptr == text.data() + text.size() && std::isfinite(result);
}

bool IsLowerHexSha256(std::string_view hash) {
    return hash.size() == 64u &&
        std::ranges::all_of(hash, [](unsigned char value) {
            return (value >= '0' && value <= '9') ||
                (value >= 'a' && value <= 'f');
        });
}

bool IsContainedBy(const std::filesystem::path& root,
                   const std::filesystem::path& candidate) {
    auto candidatePart = candidate.begin();
    for (auto rootPart = root.begin(); rootPart != root.end();
         ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() || *candidatePart != *rootPart) {
            return false;
        }
    }
    return true;
}

bool IsNormalizedRelativePath(const std::filesystem::path& path) {
    const std::string text = path.generic_string();
    if (text.empty() || text == "." || text.contains('\\') ||
        path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        path.lexically_normal().generic_string() != text) {
        return false;
    }
    for (const std::filesystem::path& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

std::string LowerAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return static_cast<char>(character);
    });
    return value;
}

struct PhysicalFileIdentity {
    std::uint64_t Volume{0u};
    std::uint64_t FileHigh{0u};
    std::uint64_t FileLow{0u};

    bool operator==(const PhysicalFileIdentity&) const = default;
    bool operator<(const PhysicalFileIdentity& other) const {
        return std::array{Volume, FileHigh, FileLow} <
            std::array{other.Volume, other.FileHigh, other.FileLow};
    }
};

std::string SerializePhysicalIdentity(const PhysicalFileIdentity& identity) {
    return std::format("{:016x}-{:016x}-{:016x}", identity.Volume,
                       identity.FileHigh, identity.FileLow);
}

std::string DurableNativeError(
    std::string_view operation,
    const std::filesystem::path& path,
    int nativeError);

#if defined(_WIN32)
std::optional<PhysicalFileIdentity> PhysicalIdentityFromHandle(
    HANDLE handle,
    const std::filesystem::path& path,
    std::string_view operation,
    std::string& detail) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        const DWORD nativeError = GetLastError();
        detail = DurableNativeError(
            operation, path, static_cast<int>(nativeError));
        return std::nullopt;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
        information.nNumberOfLinks != 1u) {
        detail = DurableNativeError(operation, path, ERROR_INVALID_DATA);
        return std::nullopt;
    }
    return PhysicalFileIdentity{
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow};
}
#endif

std::optional<PhysicalFileIdentity> PhysicalIdentityNoFollow(
    const std::filesystem::path& path,
    std::string& detail) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        detail = std::format("cannot open physical identity: {}",
                             path.generic_string());
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool valid = GetFileInformationByHandle(handle, &information) &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u &&
        information.nNumberOfLinks == 1u;
    CloseHandle(handle);
    if (!valid) {
        detail = std::format("physical identity is not a no-link regular file: {}",
                             path.generic_string());
        return std::nullopt;
    }
    return PhysicalFileIdentity{
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow};
#else
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1) {
        detail = std::format("physical identity is not a no-link regular file: {}",
                             path.generic_string());
        return std::nullopt;
    }
    return PhysicalFileIdentity{
        static_cast<std::uint64_t>(status.st_dev), 0u,
        static_cast<std::uint64_t>(status.st_ino)};
#endif
}

bool IsLexicallyBeneath(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    if (!IsNormalizedRelativePath(root) ||
        !IsNormalizedRelativePath(candidate) || root == candidate) {
        return false;
    }
    auto candidatePart = candidate.begin();
    for (auto rootPart = root.begin(); rootPart != root.end();
         ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() || *candidatePart != *rootPart) {
            return false;
        }
    }
    return candidatePart != candidate.end();
}

} // namespace

std::string SerializeTerrainReferenceManifest(
    const TerrainReferenceManifestDto& manifest) {
    JsonEmitter json;
    json.ObjectBegin();
    json.Key("schemaVersion"); json.Uint(manifest.SchemaVersion);
    json.Key("algorithmVersion"); json.String(manifest.AlgorithmVersion);
    json.Key("source");
    json.ObjectBegin();
    json.Key("map"); EmitHashedInput(json, manifest.Source.Map);
    json.Key("database"); EmitHashedInput(json, manifest.Source.Database);
    json.Key("clientRoot"); json.String(manifest.Source.ClientRoot.generic_string());
    json.Key("alphaAtlas"); EmitHashedInput(json, manifest.Source.AlphaAtlas);
    json.Key("usedTextures");
    json.ArrayBegin();
    for (const TerrainTextureSourceDto& texture : manifest.Source.UsedTextures) {
        json.ObjectBegin();
        json.Key("textureId"); json.Uint(texture.TextureId);
        json.Key("path"); json.String(texture.Path.generic_string());
        json.Key("sha256"); json.String(texture.Sha256);
        json.ObjectEnd();
    }
    json.ArrayEnd();
    json.ObjectEnd();
    json.Key("page");
    json.ObjectBegin();
    json.Key("x"); json.Uint(manifest.Page.Id.X);
    json.Key("y"); json.Uint(manifest.Page.Id.Y);
    json.Key("sourceCellBounds"); EmitRect(json, manifest.Page.SourceCellBounds);
    json.Key("pixelsPerCell"); json.Uint(manifest.Page.PixelsPerCell);
    json.Key("pixelWidth"); json.Uint(manifest.Page.PixelWidth);
    json.Key("pixelHeight"); json.Uint(manifest.Page.PixelHeight);
    json.Key("ambient");
    json.ArrayBegin();
    for (const double value : manifest.Page.Ambient) { json.Double(value); }
    json.ArrayEnd();
    json.Key("dwTColor"); json.Uint(manifest.Page.DwTColor);
    json.ObjectEnd();
    json.Key("requiredPresentRect"); EmitRect(json, manifest.RequiredPresentRect);
    json.Key("usedTextureIds");
    json.ArrayBegin();
    for (const std::uint8_t textureId : manifest.UsedTextureIds) {
        json.Uint(textureId);
    }
    json.ArrayEnd();
    json.Key("sectionPresence");
    json.ObjectBegin();
    json.Key("originX"); json.Uint(manifest.SectionPresence.OriginX);
    json.Key("originY"); json.Uint(manifest.SectionPresence.OriginY);
    json.Key("width"); json.Uint(manifest.SectionPresence.Width);
    json.Key("height"); json.Uint(manifest.SectionPresence.Height);
    json.Key("rowMajorMask");
    json.ArrayBegin();
    for (const std::uint8_t value : manifest.SectionPresence.RowMajorMask) {
        json.Uint(value);
    }
    json.ArrayEnd();
    json.ObjectEnd();
    json.Key("files");
    json.ObjectBegin();
    json.Key("height"); EmitOutputFile(json, manifest.Files.Height);
    json.Key("block"); EmitOutputFile(json, manifest.Files.Block);
    json.Key("region"); EmitOutputFile(json, manifest.Files.Region);
    json.Key("terrainMetadata"); EmitOutputFile(json, manifest.Files.TerrainMetadata);
    json.Key("albedo"); EmitOutputFile(json, manifest.Files.Albedo);
    json.Key("meshGltf"); EmitOutputFile(json, manifest.Files.MeshGltf);
    json.Key("meshBin"); EmitOutputFile(json, manifest.Files.MeshBin);
    json.ObjectEnd();
    json.Key("metrics");
    json.ObjectBegin();
    json.Key("peakRssBytes"); json.Uint(manifest.Metrics.PeakRssBytes);
    json.Key("peakTextureCacheBytes"); json.Uint(manifest.Metrics.PeakTextureCacheBytes);
    json.Key("peakRgbaRowBytes"); json.Uint(manifest.Metrics.PeakRgbaRowBytes);
    json.Key("pngBytes"); json.Uint(manifest.Metrics.PngBytes);
    json.Key("totalOutputBytes"); json.Uint(manifest.Metrics.TotalOutputBytes);
    json.Key("maxHeightErrorCm"); json.Double(manifest.Metrics.MaxHeightErrorCm);
    json.Key("rmsHeightErrorCm"); json.Double(manifest.Metrics.RmsHeightErrorCm);
    json.Key("sharedBoundaryMaxCm"); json.Double(manifest.Metrics.SharedBoundaryMaxCm);
    json.Key("absentSectionCount"); json.Uint(manifest.Metrics.AbsentSectionCount);
    json.Key("unresolvedLayerCount"); json.Uint(manifest.Metrics.UnresolvedLayerCount);
    json.ObjectEnd();
    json.ObjectEnd();
    return std::move(json).Take();
}

std::optional<TerrainReferenceManifestDto> ParseTerrainReferenceManifest(
    std::string_view input,
    std::vector<TerrainManifestIssue>& issues) {
    issues.clear();
    StrictJsonParser parser{input};
    auto root = parser.Parse(issues);
    if (!root.has_value() || !RequireType(*root, JsonType::OBJECT, "", issues)) {
        return std::nullopt;
    }
    constexpr std::array<std::string_view, 9> rootKeys{
        "schemaVersion", "algorithmVersion", "source", "page",
        "requiredPresentRect", "usedTextureIds", "sectionPresence", "files",
        "metrics"};
    if (!RejectUnknownMembers(*root, rootKeys, "", issues)) {
        return std::nullopt;
    }

    TerrainReferenceManifestDto result;
    const JsonValue* schema = RequireMember(*root, "schemaVersion", JsonType::NUMBER, "", issues);
    const JsonValue* algorithm = RequireMember(*root, "algorithmVersion", JsonType::STRING, "", issues);
    const JsonValue* source = RequireMember(*root, "source", JsonType::OBJECT, "", issues);
    const JsonValue* page = RequireMember(*root, "page", JsonType::OBJECT, "", issues);
    const JsonValue* required = RequireMember(*root, "requiredPresentRect", JsonType::OBJECT, "", issues);
    const JsonValue* ids = RequireMember(*root, "usedTextureIds", JsonType::ARRAY, "", issues);
    const JsonValue* presence = RequireMember(*root, "sectionPresence", JsonType::OBJECT, "", issues);
    const JsonValue* files = RequireMember(*root, "files", JsonType::OBJECT, "", issues);
    const JsonValue* metrics = RequireMember(*root, "metrics", JsonType::OBJECT, "", issues);
    std::uint64_t integer = 0u;
    if (schema == nullptr || !ReadUint(*schema, UINT32_MAX, "/schemaVersion", integer, issues)) {
        return std::nullopt;
    }
    result.SchemaVersion = static_cast<std::uint32_t>(integer);
    if (algorithm == nullptr ||
        !ReadString(*algorithm, "/algorithmVersion", result.AlgorithmVersion, issues) ||
        source == nullptr || page == nullptr || required == nullptr || ids == nullptr ||
        presence == nullptr || files == nullptr || metrics == nullptr) {
        return std::nullopt;
    }

    constexpr std::array<std::string_view, 5> sourceKeys{
        "map", "database", "clientRoot", "alphaAtlas", "usedTextures"};
    if (!RejectUnknownMembers(*source, sourceKeys, "/source", issues)) {
        return std::nullopt;
    }
    const JsonValue* map = RequireMember(*source, "map", JsonType::OBJECT, "/source", issues);
    const JsonValue* database = RequireMember(*source, "database", JsonType::OBJECT, "/source", issues);
    const JsonValue* clientRoot = RequireMember(*source, "clientRoot", JsonType::STRING, "/source", issues);
    const JsonValue* alpha = RequireMember(*source, "alphaAtlas", JsonType::OBJECT, "/source", issues);
    const JsonValue* textures = RequireMember(*source, "usedTextures", JsonType::ARRAY, "/source", issues);
    std::string pathText;
    if (map == nullptr || !ParseHashedInput(*map, "/source/map", result.Source.Map, issues) ||
        database == nullptr || !ParseHashedInput(*database, "/source/database", result.Source.Database, issues) ||
        clientRoot == nullptr || !ReadString(*clientRoot, "/source/clientRoot", pathText, issues) ||
        alpha == nullptr || !ParseHashedInput(*alpha, "/source/alphaAtlas", result.Source.AlphaAtlas, issues) ||
        textures == nullptr) {
        return std::nullopt;
    }
    result.Source.ClientRoot = std::filesystem::path{pathText};
    constexpr std::array<std::string_view, 3> textureKeys{"textureId", "path", "sha256"};
    for (std::size_t index = 0u; index < textures->Array.size(); ++index) {
        const JsonValue& texture = textures->Array[index];
        const std::string path = std::format("/source/usedTextures/{}", index);
        if (!RequireType(texture, JsonType::OBJECT, path, issues) ||
            !RejectUnknownMembers(texture, textureKeys, path, issues)) {
            return std::nullopt;
        }
        const JsonValue* id = RequireMember(texture, "textureId", JsonType::NUMBER, path, issues);
        const JsonValue* texturePath = RequireMember(texture, "path", JsonType::STRING, path, issues);
        const JsonValue* hash = RequireMember(texture, "sha256", JsonType::STRING, path, issues);
        TerrainTextureSourceDto dto;
        if (id == nullptr || !ReadUint(*id, UINT8_MAX, ChildPointer(path, "textureId"), integer, issues) ||
            texturePath == nullptr || !ReadString(*texturePath, ChildPointer(path, "path"), pathText, issues) ||
            hash == nullptr || !ReadString(*hash, ChildPointer(path, "sha256"), dto.Sha256, issues)) {
            return std::nullopt;
        }
        dto.TextureId = static_cast<std::uint8_t>(integer);
        dto.Path = std::filesystem::path{pathText};
        result.Source.UsedTextures.push_back(std::move(dto));
    }

    constexpr std::array<std::string_view, 8> pageKeys{
        "x", "y", "sourceCellBounds", "pixelsPerCell", "pixelWidth",
        "pixelHeight", "ambient", "dwTColor"};
    if (!RejectUnknownMembers(*page, pageKeys, "/page", issues)) {
        return std::nullopt;
    }
    const JsonValue* pageX = RequireMember(*page, "x", JsonType::NUMBER, "/page", issues);
    const JsonValue* pageY = RequireMember(*page, "y", JsonType::NUMBER, "/page", issues);
    const JsonValue* bounds = RequireMember(*page, "sourceCellBounds", JsonType::OBJECT, "/page", issues);
    const JsonValue* pixelsPerCell = RequireMember(*page, "pixelsPerCell", JsonType::NUMBER, "/page", issues);
    const JsonValue* pixelWidth = RequireMember(*page, "pixelWidth", JsonType::NUMBER, "/page", issues);
    const JsonValue* pixelHeight = RequireMember(*page, "pixelHeight", JsonType::NUMBER, "/page", issues);
    const JsonValue* ambient = RequireMember(*page, "ambient", JsonType::ARRAY, "/page", issues);
    const JsonValue* tint = RequireMember(*page, "dwTColor", JsonType::NUMBER, "/page", issues);
    if (pageX == nullptr || !ReadUint(*pageX, UINT32_MAX, "/page/x", integer, issues)) { return std::nullopt; }
    result.Page.Id.X = static_cast<std::uint32_t>(integer);
    if (pageY == nullptr || !ReadUint(*pageY, UINT32_MAX, "/page/y", integer, issues)) { return std::nullopt; }
    result.Page.Id.Y = static_cast<std::uint32_t>(integer);
    if (bounds == nullptr || !ParseRect(*bounds, "/page/sourceCellBounds", result.Page.SourceCellBounds, issues)) { return std::nullopt; }
    if (pixelsPerCell == nullptr || !ReadUint(*pixelsPerCell, UINT32_MAX, "/page/pixelsPerCell", integer, issues)) { return std::nullopt; }
    result.Page.PixelsPerCell = static_cast<std::uint32_t>(integer);
    if (pixelWidth == nullptr || !ReadUint(*pixelWidth, UINT32_MAX, "/page/pixelWidth", integer, issues)) { return std::nullopt; }
    result.Page.PixelWidth = static_cast<std::uint32_t>(integer);
    if (pixelHeight == nullptr || !ReadUint(*pixelHeight, UINT32_MAX, "/page/pixelHeight", integer, issues)) { return std::nullopt; }
    result.Page.PixelHeight = static_cast<std::uint32_t>(integer);
    if (ambient == nullptr || ambient->Array.size() != 3u) {
        AddSchemaIssue(issues, "/page/ambient", "ambient must contain exactly three numbers");
        return std::nullopt;
    }
    for (std::size_t index = 0u; index < 3u; ++index) {
        if (!ReadDouble(ambient->Array[index], std::format("/page/ambient/{}", index),
                        result.Page.Ambient[index], issues)) { return std::nullopt; }
    }
    if (tint == nullptr || !ReadUint(*tint, UINT32_MAX, "/page/dwTColor", integer, issues)) { return std::nullopt; }
    result.Page.DwTColor = static_cast<std::uint32_t>(integer);
    if (!ParseRect(*required, "/requiredPresentRect", result.RequiredPresentRect, issues)) { return std::nullopt; }

    for (std::size_t index = 0u; index < ids->Array.size(); ++index) {
        if (!ReadUint(ids->Array[index], UINT8_MAX,
                      std::format("/usedTextureIds/{}", index), integer, issues)) { return std::nullopt; }
        result.UsedTextureIds.push_back(static_cast<std::uint8_t>(integer));
    }

    constexpr std::array<std::string_view, 5> presenceKeys{
        "originX", "originY", "width", "height", "rowMajorMask"};
    if (!RejectUnknownMembers(*presence, presenceKeys, "/sectionPresence", issues)) { return std::nullopt; }
    const JsonValue* originX = RequireMember(*presence, "originX", JsonType::NUMBER, "/sectionPresence", issues);
    const JsonValue* originY = RequireMember(*presence, "originY", JsonType::NUMBER, "/sectionPresence", issues);
    const JsonValue* presenceWidth = RequireMember(*presence, "width", JsonType::NUMBER, "/sectionPresence", issues);
    const JsonValue* presenceHeight = RequireMember(*presence, "height", JsonType::NUMBER, "/sectionPresence", issues);
    const JsonValue* mask = RequireMember(*presence, "rowMajorMask", JsonType::ARRAY, "/sectionPresence", issues);
    if (originX == nullptr || !ReadUint(*originX, UINT32_MAX, "/sectionPresence/originX", integer, issues)) { return std::nullopt; }
    result.SectionPresence.OriginX = static_cast<std::uint32_t>(integer);
    if (originY == nullptr || !ReadUint(*originY, UINT32_MAX, "/sectionPresence/originY", integer, issues)) { return std::nullopt; }
    result.SectionPresence.OriginY = static_cast<std::uint32_t>(integer);
    if (presenceWidth == nullptr || !ReadUint(*presenceWidth, UINT32_MAX, "/sectionPresence/width", integer, issues)) { return std::nullopt; }
    result.SectionPresence.Width = static_cast<std::uint32_t>(integer);
    if (presenceHeight == nullptr || !ReadUint(*presenceHeight, UINT32_MAX, "/sectionPresence/height", integer, issues)) { return std::nullopt; }
    result.SectionPresence.Height = static_cast<std::uint32_t>(integer);
    if (mask == nullptr) { return std::nullopt; }
    for (std::size_t index = 0u; index < mask->Array.size(); ++index) {
        if (!ReadUint(mask->Array[index], UINT8_MAX,
                      std::format("/sectionPresence/rowMajorMask/{}", index), integer, issues)) { return std::nullopt; }
        result.SectionPresence.RowMajorMask.push_back(static_cast<std::uint8_t>(integer));
    }

    constexpr std::array<std::string_view, 7> fileKeys{
        "height", "block", "region", "terrainMetadata", "albedo", "meshGltf", "meshBin"};
    if (!RejectUnknownMembers(*files, fileKeys, "/files", issues)) { return std::nullopt; }
    std::array<TerrainOutputFile*, 7> outputDtos{
        &result.Files.Height, &result.Files.Block, &result.Files.Region,
        &result.Files.TerrainMetadata, &result.Files.Albedo,
        &result.Files.MeshGltf, &result.Files.MeshBin};
    for (std::size_t index = 0u; index < fileKeys.size(); ++index) {
        const JsonValue* file = RequireMember(*files, fileKeys[index], JsonType::OBJECT, "/files", issues);
        if (file == nullptr || !ParseOutputFile(*file, ChildPointer("/files", fileKeys[index]),
                                                *outputDtos[index], issues)) { return std::nullopt; }
    }

    constexpr std::array<std::string_view, 10> metricKeys{
        "peakRssBytes", "peakTextureCacheBytes", "peakRgbaRowBytes", "pngBytes",
        "totalOutputBytes", "maxHeightErrorCm", "rmsHeightErrorCm",
        "sharedBoundaryMaxCm", "absentSectionCount", "unresolvedLayerCount"};
    if (!RejectUnknownMembers(*metrics, metricKeys, "/metrics", issues)) { return std::nullopt; }
    const auto metric = [&](std::string_view key, JsonType type) {
        return RequireMember(*metrics, key, type, "/metrics", issues);
    };
    std::array<std::pair<std::string_view, std::uint64_t*>, 7> integers{
        std::pair{"peakRssBytes", &result.Metrics.PeakRssBytes},
        std::pair{"peakTextureCacheBytes", &result.Metrics.PeakTextureCacheBytes},
        std::pair{"peakRgbaRowBytes", &result.Metrics.PeakRgbaRowBytes},
        std::pair{"pngBytes", &result.Metrics.PngBytes},
        std::pair{"totalOutputBytes", &result.Metrics.TotalOutputBytes},
        std::pair{"absentSectionCount", &result.Metrics.AbsentSectionCount},
        std::pair{"unresolvedLayerCount", &result.Metrics.UnresolvedLayerCount},
    };
    for (const auto& [key, target] : integers) {
        const JsonValue* value = metric(key, JsonType::NUMBER);
        if (value == nullptr || !ReadUint(*value, UINT64_MAX, ChildPointer("/metrics", key), *target, issues)) { return std::nullopt; }
    }
    std::array<std::pair<std::string_view, double*>, 3> doubles{
        std::pair{"maxHeightErrorCm", &result.Metrics.MaxHeightErrorCm},
        std::pair{"rmsHeightErrorCm", &result.Metrics.RmsHeightErrorCm},
        std::pair{"sharedBoundaryMaxCm", &result.Metrics.SharedBoundaryMaxCm},
    };
    for (const auto& [key, target] : doubles) {
        const JsonValue* value = metric(key, JsonType::NUMBER);
        if (value == nullptr || !ReadDouble(*value, ChildPointer("/metrics", key), *target, issues)) { return std::nullopt; }
    }
    return result;
}

std::vector<TerrainManifestIssue> ValidateTerrainReferenceManifest(
    const TerrainReferenceManifestDto& manifest,
    const std::filesystem::path& manifestDirectory,
    const TerrainReferenceOptions& limits) {
    const auto issue = [](TerrainManifestIssueCode code,
                          std::string field,
                          std::string detail) {
        return std::vector<TerrainManifestIssue>{{
            code, std::move(field), std::move(detail)}};
    };
    if (manifest.SchemaVersion != 1u) {
        return issue(TerrainManifestIssueCode::INVALID_SCHEMA,
                     "/schemaVersion", "schemaVersion must be 1");
    }
    if (manifest.AlgorithmVersion != "legacy-fixed-pipeline-v1") {
        return issue(TerrainManifestIssueCode::INVALID_ALGORITHM,
                     "/algorithmVersion",
                     "algorithmVersion must be legacy-fixed-pipeline-v1");
    }

    const std::uint64_t expectedX =
        static_cast<std::uint64_t>(limits.Page.X) * limits.Bake.CellsPerPage;
    const std::uint64_t expectedY =
        static_cast<std::uint64_t>(limits.Page.Y) * limits.Bake.CellsPerPage;
    const std::uint64_t expectedPixels =
        static_cast<std::uint64_t>(limits.Bake.CellsPerPage) *
        limits.Bake.PixelsPerCell;
    if (expectedX > UINT32_MAX || expectedY > UINT32_MAX ||
        expectedPixels > UINT32_MAX ||
        manifest.Page.Id.X != limits.Page.X ||
        manifest.Page.Id.Y != limits.Page.Y ||
        manifest.Page.SourceCellBounds.X != expectedX ||
        manifest.Page.SourceCellBounds.Y != expectedY ||
        manifest.Page.SourceCellBounds.Width != limits.Bake.CellsPerPage ||
        manifest.Page.SourceCellBounds.Height != limits.Bake.CellsPerPage ||
        manifest.Page.PixelsPerCell != limits.Bake.PixelsPerCell ||
        manifest.Page.PixelWidth != expectedPixels ||
        manifest.Page.PixelHeight != expectedPixels ||
        manifest.Page.Ambient != std::array<double, 3>{1.0, 1.0, 1.0} ||
        manifest.Page.DwTColor != 0u ||
        manifest.RequiredPresentRect.X != limits.RequiredPresent.X ||
        manifest.RequiredPresentRect.Y != limits.RequiredPresent.Y ||
        manifest.RequiredPresentRect.Width != limits.RequiredPresent.Width ||
        manifest.RequiredPresentRect.Height != limits.RequiredPresent.Height) {
        return issue(TerrainManifestIssueCode::INVALID_BOUNDS,
                     "/page", "page geometry does not match configured bounds");
    }
    const std::uint64_t pageEndX = expectedX + limits.Bake.CellsPerPage;
    const std::uint64_t pageEndY = expectedY + limits.Bake.CellsPerPage;
    const std::uint64_t requiredEndX =
        static_cast<std::uint64_t>(limits.RequiredPresent.X) +
        limits.RequiredPresent.Width;
    const std::uint64_t requiredEndY =
        static_cast<std::uint64_t>(limits.RequiredPresent.Y) +
        limits.RequiredPresent.Height;
    if (limits.RequiredPresent.Width == 0u ||
        limits.RequiredPresent.Height == 0u ||
        limits.RequiredPresent.X < expectedX ||
        limits.RequiredPresent.Y < expectedY ||
        requiredEndX > pageEndX || requiredEndY > pageEndY) {
        return issue(TerrainManifestIssueCode::INVALID_BOUNDS,
                     "/requiredPresentRect",
                     "required-present rectangle is outside the page");
    }

    if (manifest.UsedTextureIds.empty() ||
        !std::ranges::is_sorted(manifest.UsedTextureIds) ||
        std::ranges::adjacent_find(manifest.UsedTextureIds) !=
            manifest.UsedTextureIds.end() ||
        std::ranges::find(manifest.UsedTextureIds, std::uint8_t{0}) !=
            manifest.UsedTextureIds.end()) {
        return issue(TerrainManifestIssueCode::INVALID_TEXTURE_IDS,
                     "/usedTextureIds",
                     "texture IDs must be nonzero, sorted, and unique");
    }
    if (manifest.Source.UsedTextures.size() != manifest.UsedTextureIds.size()) {
        return issue(TerrainManifestIssueCode::INVALID_TEXTURE_IDS,
                     "/source/usedTextures",
                     "source texture mapping must equal usedTextureIds");
    }
    for (std::size_t index = 0u; index < manifest.UsedTextureIds.size(); ++index) {
        if (manifest.Source.UsedTextures[index].TextureId !=
            manifest.UsedTextureIds[index]) {
            return issue(TerrainManifestIssueCode::INVALID_TEXTURE_IDS,
                         "/source/usedTextures",
                         "source texture mapping must equal usedTextureIds");
        }
    }

    constexpr std::uint32_t sectionSize = 8u;
    const std::uint32_t expectedSectionWidth =
        limits.Bake.CellsPerPage / sectionSize;
    if (limits.Bake.CellsPerPage % sectionSize != 0u ||
        manifest.SectionPresence.OriginX != expectedX / sectionSize ||
        manifest.SectionPresence.OriginY != expectedY / sectionSize ||
        manifest.SectionPresence.Width != expectedSectionWidth ||
        manifest.SectionPresence.Height != expectedSectionWidth ||
        manifest.SectionPresence.RowMajorMask.size() !=
            static_cast<std::size_t>(expectedSectionWidth) * expectedSectionWidth ||
        !std::ranges::all_of(manifest.SectionPresence.RowMajorMask,
                             [](std::uint8_t value) { return value == 1u; })) {
        return issue(TerrainManifestIssueCode::INVALID_SECTION_MASK,
                     "/sectionPresence",
                     "section presence must be the complete owned-page mask");
    }

    const auto& metrics = manifest.Metrics;
    if (metrics.PeakRssBytes == 0u) {
        return issue(TerrainManifestIssueCode::INVALID_METRIC,
                     "/metrics/peakRssBytes", "peak RSS must be nonzero");
    }
    if (metrics.PeakRssBytes > limits.Bake.MaxRssBytes) {
        return issue(TerrainManifestIssueCode::BUDGET_EXCEEDED,
                     "/metrics/peakRssBytes",
                     "peak RSS exceeds configured budget");
    }
    if (metrics.PeakTextureCacheBytes > limits.Bake.MaxTextureCacheBytes) {
        return issue(TerrainManifestIssueCode::BUDGET_EXCEEDED,
                     "/metrics/peakTextureCacheBytes",
                     "texture cache exceeds configured budget");
    }
    if (metrics.PeakRgbaRowBytes > limits.Bake.MaxRgbaRowBytes) {
        return issue(TerrainManifestIssueCode::BUDGET_EXCEEDED,
                     "/metrics/peakRgbaRowBytes",
                     "RGBA row exceeds configured budget");
    }
    if (metrics.PngBytes > limits.Bake.MaxPngBytes) {
        return issue(TerrainManifestIssueCode::BUDGET_EXCEEDED,
                     "/metrics/pngBytes", "PNG exceeds configured budget");
    }
    if (!std::isfinite(metrics.MaxHeightErrorCm) ||
        !std::isfinite(metrics.RmsHeightErrorCm) ||
        !std::isfinite(metrics.SharedBoundaryMaxCm) ||
        metrics.MaxHeightErrorCm < 0.0 || metrics.RmsHeightErrorCm < 0.0 ||
        metrics.SharedBoundaryMaxCm < 0.0) {
        return issue(TerrainManifestIssueCode::INVALID_METRIC,
                     "/metrics", "geometry metrics must be finite and nonnegative");
    }
    if (metrics.MaxHeightErrorCm > limits.Mesh.MaxAbsCm ||
        metrics.RmsHeightErrorCm > limits.Mesh.MaxRmsCm ||
        metrics.SharedBoundaryMaxCm > limits.Mesh.MaxSharedBoundaryCm) {
        return issue(TerrainManifestIssueCode::BUDGET_EXCEEDED,
                     "/metrics", "geometry metrics exceed configured limits");
    }
    if (metrics.AbsentSectionCount != 0u ||
        metrics.UnresolvedLayerCount != 0u) {
        return issue(TerrainManifestIssueCode::INVALID_METRIC,
                     "/metrics", "absent and unresolved counts must be zero");
    }
    const std::array<const TerrainOutputFile*, 7> files{
        &manifest.Files.Height, &manifest.Files.Block, &manifest.Files.Region,
        &manifest.Files.TerrainMetadata, &manifest.Files.Albedo,
        &manifest.Files.MeshGltf, &manifest.Files.MeshBin};
    std::uint64_t total = 0u;
    for (const TerrainOutputFile* file : files) {
        if (file->SizeBytes > UINT64_MAX - total) {
            return issue(TerrainManifestIssueCode::INVALID_METRIC,
                         "/metrics/totalOutputBytes", "file size sum overflows uint64");
        }
        total += file->SizeBytes;
    }
    if (metrics.TotalOutputBytes != total) {
        return issue(TerrainManifestIssueCode::INVALID_METRIC,
                     "/metrics/totalOutputBytes",
                     "totalOutputBytes must equal the seven file sizes");
    }
    if (metrics.PngBytes != manifest.Files.Albedo.SizeBytes) {
        return issue(TerrainManifestIssueCode::INVALID_METRIC,
                     "/metrics/pngBytes", "pngBytes must equal albedo sizeBytes");
    }
    if (manifestDirectory.empty()) {
        return issue(TerrainManifestIssueCode::INVALID_FILE,
                     "/files", "manifest directory is empty");
    }

    std::error_code pathError;
    const std::filesystem::path canonicalManifestDirectory =
        std::filesystem::canonical(manifestDirectory, pathError);
    if (pathError || !std::filesystem::is_directory(
            canonicalManifestDirectory, pathError) || pathError) {
        return issue(TerrainManifestIssueCode::INVALID_FILE,
                     "/files", "manifest directory is not a readable directory");
    }
    pathError.clear();
    const std::filesystem::path canonicalOutput =
        std::filesystem::canonical(limits.Output, pathError);
    if (pathError || canonicalOutput != canonicalManifestDirectory) {
        return issue(TerrainManifestIssueCode::INVALID_FILE,
                     "/files", "manifest validation base differs from output root");
    }

    struct FileCheck {
        std::string_view Key;
        std::string_view Leaf;
        const TerrainOutputFile* File;
    };
    const std::array<FileCheck, 7> fileChecks{{
        {"height", "garner.height.r16", &manifest.Files.Height},
        {"block", "garner.block.raw", &manifest.Files.Block},
        {"region", "garner.region.raw", &manifest.Files.Region},
        {"terrainMetadata", "garner.terrain.json", &manifest.Files.TerrainMetadata},
        {"albedo", "garner.albedo_17_21.png", &manifest.Files.Albedo},
        {"meshGltf", "garner.terrain_17_21.gltf", &manifest.Files.MeshGltf},
        {"meshBin", "garner.terrain_17_21.bin", &manifest.Files.MeshBin},
    }};
    std::optional<std::string> runId;
    std::set<std::string> normalizedFileAliases;
    std::set<PhysicalFileIdentity> physicalFileIdentities;
    std::filesystem::path canonicalRunDirectory;
    for (const FileCheck& check : fileChecks) {
        const std::string field = std::format("/files/{}", check.Key);
        const std::filesystem::path& relative = check.File->ManifestRelativePath;
        std::vector<std::string> components;
        for (const auto& component : relative) {
            components.push_back(component.generic_string());
        }
        if (!IsNormalizedRelativePath(relative) || components.size() != 3u ||
            components[0] != "runs" || components[1].empty() ||
            components[1] == "." || components[1] == ".." ||
            components[2] != check.Leaf) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         field + "/path", "invalid one-run manifest path");
        }
        if (!runId.has_value()) {
            runId = components[1];
        }
        else if (*runId != components[1]) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         field + "/path", "all files must share one run ID");
        }
        const std::string alias = LowerAscii(relative.generic_string());
        if (!normalizedFileAliases.insert(alias).second) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         field + "/path", "duplicate or case-alias file path");
        }
        if (!IsLowerHexSha256(check.File->Sha256)) {
            return issue(TerrainManifestIssueCode::INVALID_HASH,
                         field + "/sha256", "SHA-256 must be lowercase hex");
        }
        if (check.File->SizeBytes == 0u) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         field + "/sizeBytes", "output size must be nonzero");
        }

        const std::filesystem::path candidate = manifestDirectory / relative;
        pathError.clear();
        const std::filesystem::file_status physical =
            std::filesystem::symlink_status(candidate, pathError);
        if (pathError || physical.type() != std::filesystem::file_type::regular) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         field + "/path",
                         "output must be a non-symlink regular file");
        }
        std::string identityDetail;
        const auto identity = PhysicalIdentityNoFollow(candidate, identityDetail);
        if (!identity.has_value() ||
            !physicalFileIdentities.insert(*identity).second) {
            return issue(
                TerrainManifestIssueCode::INVALID_FILE, field + "/path",
                "outputs must have seven distinct physical identities");
        }
        const std::filesystem::path canonicalCandidate =
            std::filesystem::canonical(candidate, pathError);
        if (pathError || !IsContainedBy(canonicalManifestDirectory, canonicalCandidate)) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         field + "/path", "output escapes manifest directory");
        }
        const std::filesystem::path currentRunDirectory =
            canonicalCandidate.parent_path();
        if (canonicalRunDirectory.empty()) {
            canonicalRunDirectory = currentRunDirectory;
        }
        else if (canonicalRunDirectory != currentRunDirectory) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         field + "/path", "outputs resolve to mixed run directories");
        }
        pathError.clear();
        const std::uintmax_t actualSize =
            std::filesystem::file_size(canonicalCandidate, pathError);
        if (pathError || actualSize != check.File->SizeBytes) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         field + "/sizeBytes", "output size does not match disk");
        }
        std::string hashDetail;
        const auto actualHash = Sha256File(canonicalCandidate, hashDetail);
        if (!actualHash.has_value() || *actualHash != check.File->Sha256) {
            return issue(TerrainManifestIssueCode::INVALID_HASH,
                         field + "/sha256", "output SHA-256 does not match disk");
        }
    }
    pathError.clear();
    std::size_t runEntries = 0u;
    for (std::filesystem::directory_iterator iterator{
             canonicalRunDirectory, pathError};
         !pathError && iterator != std::filesystem::directory_iterator{};
         iterator.increment(pathError)) {
        ++runEntries;
    }
    if (pathError || runEntries != fileChecks.size()) {
        return issue(TerrainManifestIssueCode::INVALID_FILE,
                     "/files", "run directory must contain exactly seven products");
    }

    const std::filesystem::path workingDirectory =
        std::filesystem::canonical(std::filesystem::current_path(), pathError);
    if (pathError) {
        return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                     "/source", "cannot resolve repository working directory");
    }
    struct SourceCheck {
        std::string_view Field;
        const TerrainHashedInputDto* Dto;
        const std::filesystem::path* Option;
    };
    const std::array<SourceCheck, 3> sourceChecks{{
        {"/source/map", &manifest.Source.Map, &limits.Map},
        {"/source/database", &manifest.Source.Database, &limits.Database},
        {"/source/alphaAtlas", &manifest.Source.AlphaAtlas, &limits.AlphaAtlas},
    }};
    std::set<std::string> sourceAliases;
    for (const SourceCheck& check : sourceChecks) {
        if (!IsNormalizedRelativePath(check.Dto->Path) ||
            check.Dto->Path.generic_string() != check.Option->generic_string()) {
            return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                         std::string{check.Field} + "/path",
                         "source path differs from normalized CLI path");
        }
        const std::string alias = LowerAscii(check.Dto->Path.generic_string());
        if (!sourceAliases.insert(alias).second) {
            return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                         std::string{check.Field} + "/path",
                         "duplicate or case-alias source path");
        }
        if (!IsLowerHexSha256(check.Dto->Sha256)) {
            return issue(TerrainManifestIssueCode::INVALID_HASH,
                         std::string{check.Field} + "/sha256",
                         "SHA-256 must be lowercase hex");
        }
        pathError.clear();
        const std::filesystem::path canonicalSource =
            std::filesystem::canonical(*check.Option, pathError);
        if (pathError || !IsContainedBy(workingDirectory, canonicalSource) ||
            !std::filesystem::is_regular_file(canonicalSource, pathError) || pathError) {
            return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                         std::string{check.Field} + "/path",
                         "source is not a contained readable regular file");
        }
        std::string hashDetail;
        const auto actualHash = Sha256File(canonicalSource, hashDetail);
        if (!actualHash.has_value() || *actualHash != check.Dto->Sha256) {
            return issue(TerrainManifestIssueCode::INVALID_HASH,
                         std::string{check.Field} + "/sha256",
                         "source SHA-256 does not match disk");
        }
    }
    if (!IsNormalizedRelativePath(manifest.Source.ClientRoot) ||
        manifest.Source.ClientRoot.generic_string() !=
            limits.ClientRoot.generic_string()) {
        return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                     "/source/clientRoot",
                     "client root differs from normalized CLI path");
    }
    pathError.clear();
    const std::filesystem::path canonicalClientRoot =
        std::filesystem::canonical(limits.ClientRoot, pathError);
    if (pathError || !IsContainedBy(workingDirectory, canonicalClientRoot) ||
        !std::filesystem::is_directory(canonicalClientRoot, pathError) || pathError) {
        return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                     "/source/clientRoot", "client root is not a contained directory");
    }
    sourceAliases.insert(LowerAscii(manifest.Source.ClientRoot.generic_string()));
    for (std::size_t index = 0u;
         index < manifest.Source.UsedTextures.size(); ++index) {
        const TerrainTextureSourceDto& texture = manifest.Source.UsedTextures[index];
        const std::string field = std::format("/source/usedTextures/{}", index);
        if (!IsNormalizedRelativePath(texture.Path) ||
            !IsLowerHexSha256(texture.Sha256)) {
            return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                         field + "/path", "invalid texture provenance path/hash");
        }
        if (!IsLexicallyBeneath(manifest.Source.ClientRoot, texture.Path)) {
            return issue(
                TerrainManifestIssueCode::INVALID_PROVENANCE,
                field + "/path",
                "texture path is not lexically beneath client root");
        }
        const std::string alias = LowerAscii(texture.Path.generic_string());
        if (!sourceAliases.insert(alias).second) {
            return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                         field + "/path", "duplicate or case-alias source path");
        }
        pathError.clear();
        const std::filesystem::path canonicalTexture =
            std::filesystem::canonical(texture.Path, pathError);
        if (pathError || !IsContainedBy(canonicalClientRoot, canonicalTexture) ||
            !std::filesystem::is_regular_file(canonicalTexture, pathError) || pathError) {
            return issue(TerrainManifestIssueCode::INVALID_PROVENANCE,
                         field + "/path", "texture escapes canonical client root");
        }
        std::string hashDetail;
        const auto actualHash = Sha256File(canonicalTexture, hashDetail);
        if (!actualHash.has_value() || *actualHash != texture.Sha256) {
            return issue(TerrainManifestIssueCode::INVALID_HASH,
                         field + "/sha256", "texture SHA-256 does not match disk");
        }
    }
    return {};
}

std::vector<TerrainManifestIssue> CompareTerrainDeterministicManifests(
    const TerrainReferenceManifestDto& first,
    const std::filesystem::path& firstManifestDirectory,
    const TerrainReferenceManifestDto& second,
    const std::filesystem::path& secondManifestDirectory,
    const TerrainReferenceOptions& limits) {
    auto issues = ValidateTerrainReferenceManifest(
        first, firstManifestDirectory, limits);
    if (!issues.empty()) {
        return issues;
    }
    issues = ValidateTerrainReferenceManifest(
        second, secondManifestDirectory, limits);
    if (!issues.empty()) {
        return issues;
    }
    const auto issue = [](TerrainManifestIssueCode code,
                          std::string field,
                          std::string detail) {
        return std::vector<TerrainManifestIssue>{{
            code, std::move(field), std::move(detail)}};
    };
    const auto runId = [](const TerrainOutputFile& file) {
        auto part = file.ManifestRelativePath.begin();
        ++part;
        return part->generic_string();
    };
    const std::string firstRunId = runId(first.Files.Height);
    const std::string secondRunId = runId(second.Files.Height);
    if (firstRunId == secondRunId) {
        return issue(TerrainManifestIssueCode::INVALID_FILE,
                     "/files", "determinism comparison requires distinct run IDs");
    }

    struct Pair {
        const TerrainOutputFile* First;
        const TerrainOutputFile* Second;
    };
    const std::array<Pair, 7> pairs{{
        {&first.Files.Height, &second.Files.Height},
        {&first.Files.Block, &second.Files.Block},
        {&first.Files.Region, &second.Files.Region},
        {&first.Files.TerrainMetadata, &second.Files.TerrainMetadata},
        {&first.Files.Albedo, &second.Files.Albedo},
        {&first.Files.MeshGltf, &second.Files.MeshGltf},
        {&first.Files.MeshBin, &second.Files.MeshBin},
    }};
    const auto equalFiles = [](const std::filesystem::path& left,
                               const std::filesystem::path& right) {
        std::ifstream firstInput{left, std::ios::binary};
        std::ifstream secondInput{right, std::ios::binary};
        if (!firstInput || !secondInput) {
            return false;
        }
        std::array<char, 64u * 1024u> firstBytes{};
        std::array<char, 64u * 1024u> secondBytes{};
        while (firstInput || secondInput) {
            firstInput.read(firstBytes.data(),
                            static_cast<std::streamsize>(firstBytes.size()));
            secondInput.read(secondBytes.data(),
                             static_cast<std::streamsize>(secondBytes.size()));
            const std::streamsize firstCount = firstInput.gcount();
            const std::streamsize secondCount = secondInput.gcount();
            if (firstCount != secondCount ||
                !std::equal(firstBytes.begin(),
                            firstBytes.begin() + firstCount,
                            secondBytes.begin())) {
                return false;
            }
        }
        return firstInput.eof() && secondInput.eof();
    };
    for (const Pair& pair : pairs) {
        if (pair.First->Sha256 != pair.Second->Sha256 ||
            pair.First->SizeBytes != pair.Second->SizeBytes ||
            !equalFiles(firstManifestDirectory / pair.First->ManifestRelativePath,
                        secondManifestDirectory / pair.Second->ManifestRelativePath)) {
            return issue(TerrainManifestIssueCode::INVALID_FILE,
                         "/files", "corresponding run products differ");
        }
    }

    TerrainReferenceManifestDto firstProjection = first;
    TerrainReferenceManifestDto secondProjection = second;
    const auto normalize = [](TerrainReferenceManifestDto& manifest) {
        const auto normalizeFile = [](TerrainOutputFile& file) {
            file.ManifestRelativePath =
                std::filesystem::path{"runs"} / "<run-id>" /
                file.ManifestRelativePath.filename();
        };
        normalizeFile(manifest.Files.Height);
        normalizeFile(manifest.Files.Block);
        normalizeFile(manifest.Files.Region);
        normalizeFile(manifest.Files.TerrainMetadata);
        normalizeFile(manifest.Files.Albedo);
        normalizeFile(manifest.Files.MeshGltf);
        normalizeFile(manifest.Files.MeshBin);
        manifest.Metrics.PeakRssBytes = std::uint64_t{0};
    };
    normalize(firstProjection);
    normalize(secondProjection);
    if (SerializeTerrainReferenceManifest(firstProjection) !=
        SerializeTerrainReferenceManifest(secondProjection)) {
        return issue(TerrainManifestIssueCode::INVALID_METRIC,
                     "/deterministicProjection",
                     "manifests differ outside run ID and peak RSS");
    }
    return {};
}

std::optional<TerrainReferenceOptions> ParseTerrainReferenceArguments(
    std::span<const std::string_view> arguments,
    std::string& detail) {
    detail.clear();
    TerrainReferenceOptions options;
    enum Field : std::size_t { MAP, DATABASE, CLIENT, ALPHA, OUTPUT, PAGE, RECT,
                              RSS, CACHE, PNG, MAX_HEIGHT, RMS, COUNT };
    std::array<bool, COUNT> seen{};
    auto fail = [&](std::string message) -> std::optional<TerrainReferenceOptions> {
        detail = std::move(message);
        return std::nullopt;
    };
    const auto mark = [&](Field field, std::string_view option) {
        if (seen[field]) {
            detail = std::format("duplicate switch {}", option);
            return false;
        }
        seen[field] = true;
        return true;
    };
    std::size_t index = 0u;
    while (index < arguments.size()) {
        const std::string_view option = arguments[index++];
        auto require = [&](std::size_t count) {
            if (count > arguments.size() - index) {
                detail = std::format("switch {} has missing value", option);
                return false;
            }
            return true;
        };
        auto path = [&](Field field, std::filesystem::path& target) {
            if (!mark(field, option) || !require(1u)) { return false; }
            if (arguments[index].empty()) {
                detail = std::format("switch {} has empty path", option);
                return false;
            }
            target = std::filesystem::path{arguments[index++]};
            return true;
        };
        if (option == "--map") { if (!path(MAP, options.Map)) return std::nullopt; }
        else if (option == "--database") { if (!path(DATABASE, options.Database)) return std::nullopt; }
        else if (option == "--client-root") { if (!path(CLIENT, options.ClientRoot)) return std::nullopt; }
        else if (option == "--alpha") { if (!path(ALPHA, options.AlphaAtlas)) return std::nullopt; }
        else if (option == "--output") { if (!path(OUTPUT, options.Output)) return std::nullopt; }
        else if (option == "--page") {
            if (!mark(PAGE, option) || !require(2u)) return std::nullopt;
            std::uint64_t x = 0u, y = 0u;
            if (!ParseUnsignedText(arguments[index], UINT32_MAX, x) ||
                !ParseUnsignedText(arguments[index + 1u], UINT32_MAX, y)) {
                return fail("--page expects two uint32 values");
            }
            options.Page = {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)};
            index += 2u;
        }
        else if (option == "--require-present-rect") {
            if (!mark(RECT, option) || !require(4u)) return std::nullopt;
            std::array<std::uint64_t, 4> values{};
            for (std::size_t value = 0u; value < values.size(); ++value) {
                if (!ParseUnsignedText(arguments[index + value], UINT32_MAX, values[value])) {
                    return fail("--require-present-rect expects four uint32 values");
                }
            }
            if (values[2] == 0u || values[3] == 0u) {
                return fail("--require-present-rect width/height must be nonzero");
            }
            options.RequiredPresent = {
                static_cast<std::uint32_t>(values[0]), static_cast<std::uint32_t>(values[1]),
                static_cast<std::uint32_t>(values[2]), static_cast<std::uint32_t>(values[3])};
            index += 4u;
        }
        else if (option == "--max-rss-mib" || option == "--max-cache-mib" ||
                 option == "--max-png-mib") {
            const Field field = option == "--max-rss-mib" ? RSS :
                (option == "--max-cache-mib" ? CACHE : PNG);
            if (!mark(field, option) || !require(1u)) return std::nullopt;
            std::uint64_t mib = 0u;
            constexpr std::uint64_t unit = 1024u * 1024u;
            if (!ParseUnsignedText(arguments[index++],
                                   std::numeric_limits<std::size_t>::max() / unit, mib)) {
                return fail(std::format("{} expects bounded MiB integer", option));
            }
            const std::size_t bytes = static_cast<std::size_t>(mib * unit);
            if (field == RSS) options.Bake.MaxRssBytes = bytes;
            else if (field == CACHE) options.Bake.MaxTextureCacheBytes = bytes;
            else options.Bake.MaxPngBytes = bytes;
        }
        else if (option == "--max-height-error-cm" || option == "--max-rms-error-cm") {
            const Field field = option == "--max-height-error-cm" ? MAX_HEIGHT : RMS;
            if (!mark(field, option) || !require(1u)) return std::nullopt;
            double value = 0.0;
            if (!ParseDoubleText(arguments[index++], value) || value < 0.0) {
                return fail(std::format("{} expects finite nonnegative number", option));
            }
            if (field == MAX_HEIGHT) options.Mesh.MaxAbsCm = value;
            else options.Mesh.MaxRmsCm = value;
        }
        else {
            return fail(std::format("unknown argument {}", option));
        }
    }
    if (!std::ranges::all_of(seen, [](bool value) { return value; })) {
        return fail("all terrain-reference switches are required exactly once");
    }
    return options;
}

std::optional<std::vector<TerrainWindowsDurableStep>>
PlanTerrainWindowsEntryFinalization(
    std::span<const TerrainWindowsDurableEntryState> states,
    std::string& detail) {
    detail.clear();
    std::vector<TerrainWindowsDurableStep> steps;
    steps.reserve(states.size() * 7u);
    for (std::size_t index = 0u; index < states.size(); ++index) {
        const TerrainWindowsDurableEntryState& state = states[index];
        if (state.SourcePresent && !state.SourceMatchesRecordedArtifact) {
            detail = std::format(
                "typed finalization entry {} source identity/mode changed",
                index);
            return std::nullopt;
        }
        if (state.SourcePresent && state.ReservationPresent) {
            if (state.ReservationIsExactRecord &&
                !state.ReservationRecordMatchesOwnedArtifact) {
                detail = "Windows finalization reservation record changed";
                return std::nullopt;
            }
            if (!state.ReservationIsExactRecord ||
                state.ReservationIsRecordedPayload) {
                detail = std::format(
                    "typed finalization entry {} has foreign reservation",
                    index);
                return std::nullopt;
            }
            steps.push_back({
                index, TerrainWindowsDurableAction::REMOVE_RESERVATION});
        }
        else if (!state.SourcePresent && state.ReservationPresent) {
            if (state.ReservationIsExactRecord ||
                !state.ReservationIsRecordedPayload ||
                !state.ReservationMatchesRecordedArtifact) {
                detail = std::format(
                    "typed finalization entry {} has ambiguous moved state",
                    index);
                return std::nullopt;
            }
            steps.push_back({
                index, TerrainWindowsDurableAction::RESTORE_SOURCE});
        }
        else if (!state.SourcePresent) {
            detail = std::format(
                "typed finalization entry {} has no recoverable payload",
                index);
            return std::nullopt;
        }
    }
    for (std::size_t index = 0u; index < states.size(); ++index) {
        steps.push_back({index, TerrainWindowsDurableAction::FLUSH_SOURCE});
    }
    for (std::size_t index = 0u; index < states.size(); ++index) {
        steps.push_back({index, TerrainWindowsDurableAction::RESERVE_TARGET});
        steps.push_back({index,
                         TerrainWindowsDurableAction::MOVE_TO_RESERVATION});
        steps.push_back({index, TerrainWindowsDurableAction::MOVE_TO_SOURCE});
        steps.push_back({index, TerrainWindowsDurableAction::VERIFY_SOURCE});
    }
    return steps;
}

std::vector<TerrainRunDurabilityStep>
PlanTerrainRunDurability(const std::size_t productCount) {
    std::vector<TerrainRunDurabilityStep> steps;
    steps.reserve(productCount + 1u);
    for (std::size_t index = 0u; index < productCount; ++index) {
        steps.push_back({index, TerrainRunDurabilityAction::FLUSH_FILE});
    }
    steps.push_back({productCount,
                     TerrainRunDurabilityAction::SYNC_DIRECTORY_OR_EQUIVALENT});
    return steps;
}

bool ExecuteTerrainRunDurabilitySteps(
    const std::span<const TerrainRunDurabilityStep> steps,
    const TerrainRunDurabilityStepExecutor& execute,
    std::string& detail) {
    detail.clear();
    if (!execute) {
        detail = "run durability step executor is missing";
        return false;
    }
    for (const TerrainRunDurabilityStep& step : steps) {
        if (!execute(step, detail)) {
            if (detail.empty()) {
                detail = std::format(
                    "run durability action {} failed for product {}",
                    static_cast<std::uint32_t>(step.Action),
                    step.ProductIndex);
            }
            return false;
        }
    }
    return true;
}

std::optional<std::string>
ReconcileTerrainWindowsFinalizationIdentity(
    std::string_view recordedIdentity,
    std::string_view observedIdentity,
    std::string& detail) {
    detail.clear();
    if (observedIdentity.empty()) {
        detail = "Windows finalization identity is missing";
        return std::nullopt;
    }
    if (!recordedIdentity.empty() && recordedIdentity != observedIdentity) {
        detail = "Windows finalization identity changed";
        return std::nullopt;
    }
    return std::string{observedIdentity};
}

std::optional<std::vector<TerrainWindowsDurableStep>>
PlanTerrainWindowsOwnedRemoval(
    const TerrainWindowsRemoveOwnedState& state,
    std::string& detail) {
    detail.clear();
    std::vector<TerrainWindowsDurableStep> steps;
    if (!state.SourcePresent && !state.TombstonePresent) {
        return steps;
    }
    if (state.SourcePresent && !state.SourceMatchesRecordedArtifact) {
        detail = "remove-owned source identity/hash/mode changed";
        return std::nullopt;
    }
    if (state.SourcePresent && state.TombstonePresent) {
        if (!state.TombstoneIsExactReservation) {
            detail = "remove-owned foreign pre-move tombstone";
            return std::nullopt;
        }
        steps.push_back({0u, TerrainWindowsDurableAction::REMOVE_RESERVATION});
    }
    else if (!state.SourcePresent) {
        if (state.TombstoneIsExactReservation ||
            !state.TombstoneMatchesRecordedArtifact) {
            detail = "remove-owned ambiguous post-move tombstone";
            return std::nullopt;
        }
        steps.push_back({
            0u, TerrainWindowsDurableAction::VERIFY_RESERVATION_PAYLOAD});
        steps.push_back({0u, TerrainWindowsDurableAction::CLEAR_READONLY});
        steps.push_back({0u, TerrainWindowsDurableAction::DELETE_RESERVATION});
        steps.push_back({0u, TerrainWindowsDurableAction::VERIFY_ABSENCE});
        return steps;
    }
    steps.push_back({0u, TerrainWindowsDurableAction::VERIFY_SOURCE});
    steps.push_back({0u, TerrainWindowsDurableAction::RESERVE_TARGET});
    steps.push_back({0u, TerrainWindowsDurableAction::MOVE_TO_RESERVATION});
    steps.push_back({
        0u, TerrainWindowsDurableAction::VERIFY_RESERVATION_PAYLOAD});
    steps.push_back({0u, TerrainWindowsDurableAction::CLEAR_READONLY});
    steps.push_back({0u, TerrainWindowsDurableAction::DELETE_RESERVATION});
    steps.push_back({0u, TerrainWindowsDurableAction::VERIFY_ABSENCE});
    return steps;
}

std::vector<TerrainWindowsDurableStep>
PlanTerrainWindowsLockRetirement() {
    return {
        {0u, TerrainWindowsDurableAction::VERIFY_LOCK_IDENTITIES},
        {0u, TerrainWindowsDurableAction::RESERVE_TARGET},
        {0u, TerrainWindowsDurableAction::MOVE_TO_RESERVATION},
        {0u, TerrainWindowsDurableAction::VERIFY_RESERVATION_PAYLOAD},
        {0u, TerrainWindowsDurableAction::ACCEPT_DELETE_PENDING},
        {0u, TerrainWindowsDurableAction::CLOSE_LOCK_HANDLE},
    };
}

bool ExecuteTerrainWindowsDurableSteps(
    std::span<const TerrainWindowsDurableStep> steps,
    const TerrainWindowsDurableStepExecutor& execute,
    std::string& detail) {
    detail.clear();
    if (!execute) {
        detail = "Windows durable step executor is missing";
        return false;
    }
    for (const TerrainWindowsDurableStep& step : steps) {
        if (!execute(step, detail)) {
            if (detail.empty()) {
                detail = std::format(
                    "Windows durable action {} failed for entry {}",
                    static_cast<std::uint32_t>(step.Action), step.EntryIndex);
            }
            return false;
        }
    }
    return true;
}

namespace {

constexpr std::array<std::string_view, 7> kTerrainReferenceLeaves{
    "garner.height.r16",
    "garner.block.raw",
    "garner.region.raw",
    "garner.terrain.json",
    "garner.albedo_17_21.png",
    "garner.terrain_17_21.gltf",
    "garner.terrain_17_21.bin",
};

bool EnsurePhysicalDirectory(
    const std::filesystem::path& directory,
    std::string_view label,
    std::ostream& error) {
    std::error_code filesystemError;
    const std::filesystem::file_status before =
        std::filesystem::symlink_status(directory, filesystemError);
    if (before.type() == std::filesystem::file_type::not_found &&
        (!filesystemError ||
         filesystemError == std::errc::no_such_file_or_directory)) {
        filesystemError.clear();
        std::filesystem::create_directories(directory, filesystemError);
    }
    else if (filesystemError) {
        error << label << ": cannot inspect directory: "
              << filesystemError.message() << '\n';
        return false;
    }
    if (filesystemError) {
        error << label << ": cannot create directory: "
              << filesystemError.message() << '\n';
        return false;
    }
    filesystemError.clear();
    const std::filesystem::file_status after =
        std::filesystem::symlink_status(directory, filesystemError);
    if (filesystemError ||
        after.type() != std::filesystem::file_type::directory) {
        error << label << ": path is not a physical directory\n";
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> CreateUniqueRunDirectory(
    const std::filesystem::path& outputRoot,
    std::ostream& error) {
    if (!EnsurePhysicalDirectory(outputRoot, "output", error)) {
        return std::nullopt;
    }
    const std::filesystem::path runs = outputRoot / "runs";
    if (!EnsurePhysicalDirectory(runs, "runs", error)) {
        return std::nullopt;
    }

    std::error_code filesystemError;
    const std::filesystem::path canonicalOutput =
        std::filesystem::canonical(outputRoot, filesystemError);
    if (filesystemError) {
        error << "output: cannot resolve directory: "
              << filesystemError.message() << '\n';
        return std::nullopt;
    }
    static std::atomic<std::uint64_t> sequence{0u};
    const std::uint64_t stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::uint32_t attempt = 0u; attempt < 1024u; ++attempt) {
        const std::string runId = std::format(
            "run-{:016x}-{:016x}-{:04x}", stamp,
            sequence.fetch_add(1u, std::memory_order_relaxed), attempt);
        const std::filesystem::path candidate = runs / runId;
        filesystemError.clear();
        if (!std::filesystem::create_directory(candidate, filesystemError)) {
            if (!filesystemError) {
                continue;
            }
            error << "run directory: exclusive create failed: "
                  << filesystemError.message() << '\n';
            return std::nullopt;
        }
        filesystemError.clear();
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(candidate, filesystemError);
        const std::filesystem::path canonicalCandidate =
            std::filesystem::canonical(candidate, filesystemError);
        if (filesystemError ||
            status.type() != std::filesystem::file_type::directory ||
            !IsContainedBy(canonicalOutput, canonicalCandidate) ||
            canonicalCandidate.parent_path() != canonicalOutput / "runs") {
            error << "run directory: containment verification failed\n";
            return std::nullopt;
        }
        return candidate;
    }
    error << "run directory: exhausted exclusive run IDs\n";
    return std::nullopt;
}

std::string IssueSummary(const TerrainManifestIssue& issue) {
    return std::format("manifest gate failed field={} detail={}",
                       issue.Field, issue.Detail);
}

bool ValidateTerrainReferenceOptionsForCommand(
    const TerrainReferenceOptions& options,
    std::string& detail) {
    const std::array<const std::filesystem::path*, 5> paths{
        &options.Map, &options.Database, &options.ClientRoot,
        &options.AlphaAtlas, &options.Output};
    for (const std::filesystem::path* path : paths) {
        if (!IsNormalizedRelativePath(*path)) {
            detail = std::format(
                "terrain-reference path must be a normalized relative path: {}",
                path->generic_string());
            return false;
        }
    }
    if (options.Page.X != 17u || options.Page.Y != 21u ||
        options.RequiredPresent.X != 2193u ||
        options.RequiredPresent.Y != 2756u ||
        options.RequiredPresent.Width != 80u ||
        options.RequiredPresent.Height != 47u ||
        options.Bake.CellsPerPage != 128u ||
        options.Bake.PixelsPerCell != 32u) {
        detail = "terrain-reference options differ from the fixed Garner page";
        return false;
    }
    if (options.Bake.MaxRssBytes == 0u ||
        options.Bake.MaxTextureCacheBytes == 0u ||
        options.Bake.MaxPngBytes == 0u ||
        options.Bake.MaxRgbaRowBytes == 0u ||
        !std::isfinite(options.Mesh.MaxAbsCm) ||
        !std::isfinite(options.Mesh.MaxRmsCm) ||
        !std::isfinite(options.Mesh.MaxSharedBoundaryCm) ||
        options.Mesh.MaxAbsCm < 0.0 || options.Mesh.MaxRmsCm < 0.0 ||
        options.Mesh.MaxSharedBoundaryCm != 0.0 ||
        options.Bake.TestOnlyRemoveCompletedOutput ||
        options.Mesh.TestOnlyWriteGltf) {
        detail = "terrain-reference budget or test seam options are invalid";
        return false;
    }
    detail.clear();
    return true;
}

std::string ShellQuote(std::string_view value) {
#if defined(_WIN32)
    std::string quoted{"\""};
    for (const char character : value) {
        if (character == '\"') {
            quoted += "\\\"";
        }
        else {
            quoted += character;
        }
    }
    quoted += '\"';
    return quoted;
#else
    std::string quoted{"'"};
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        }
        else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
#endif
}

std::string TerrainReferenceRecoveryCommand(
    const TerrainReferenceOptions& options) {
    constexpr std::size_t mib = 1024u * 1024u;
    return std::format(
        "nice -n 10 ./tools/AssetConverter/build/AssetConverter "
        "terrain-reference --map {} --database {} --client-root {} "
        "--alpha {} --output {} --page {} {} "
        "--require-present-rect {} {} {} {} --max-rss-mib {} "
        "--max-cache-mib {} --max-png-mib {} "
        "--max-height-error-cm {} --max-rms-error-cm {}",
        ShellQuote(options.Map.generic_string()),
        ShellQuote(options.Database.generic_string()),
        ShellQuote(options.ClientRoot.generic_string()),
        ShellQuote(options.AlphaAtlas.generic_string()),
        ShellQuote(options.Output.generic_string()),
        options.Page.X, options.Page.Y,
        options.RequiredPresent.X, options.RequiredPresent.Y,
        options.RequiredPresent.Width, options.RequiredPresent.Height,
        options.Bake.MaxRssBytes / mib,
        options.Bake.MaxTextureCacheBytes / mib,
        options.Bake.MaxPngBytes / mib,
        options.Mesh.MaxAbsCm, options.Mesh.MaxRmsCm);
}

std::string JsonQuotedPath(const std::filesystem::path& path) {
    JsonEmitter emitter;
    emitter.String(path.lexically_normal().generic_string());
    return std::move(emitter).Take();
}

std::string DurableNativeError(
    std::string_view operation,
    const std::filesystem::path& path,
    int nativeError) {
#if defined(_WIN32)
    return std::format(
        "DURABLE_FS_ERROR op={} path={} native=win32:{}:",
        operation, JsonQuotedPath(path), nativeError);
#else
    return std::format(
        "DURABLE_FS_ERROR op={} path={} native=errno:{}:",
        operation, JsonQuotedPath(path), nativeError);
#endif
}

constexpr std::uint32_t PrivatePhysicalMode() noexcept {
#if defined(_WIN32)
    return FILE_ATTRIBUTE_NORMAL;
#else
    return 0600u;
#endif
}

struct PhysicalEntryToFinalize {
    std::filesystem::path Source;
    std::filesystem::path Reservation;
    std::string Kind;
    std::string TransactionId;
    std::string Sha256;
    std::string Identity;
    std::uint32_t Mode{0u};
};

std::optional<std::uint32_t> PhysicalFileMode(
    const std::filesystem::path& path,
    std::string& detail);

#if defined(_WIN32)
bool ReserveMoveTargetWindows(
    const std::filesystem::path& source,
    const std::filesystem::path& reserved,
    std::string_view kind,
    std::string_view transactionId,
    std::string_view sourceHash,
    std::string& detail);

bool FinalizeWindowsPhysicalEntries(
    std::span<const PhysicalEntryToFinalize> entries,
    std::string& detail);
#endif

bool FlushPhysicalFile(
    const std::filesystem::path& path,
    std::string& detail) {
#if defined(_WIN32)
    HANDLE metadata = CreateFileW(
        path.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (metadata == INVALID_HANDLE_VALUE) {
        const DWORD nativeError = GetLastError();
        detail = DurableNativeError("FlushFile", path,
                                    static_cast<int>(nativeError));
        return false;
    }
    BY_HANDLE_FILE_INFORMATION originalIdentity{};
    FILE_BASIC_INFO originalBasic{};
    DWORD nativeError = ERROR_SUCCESS;
    bool readonlyCleared = false;
    if (!GetFileInformationByHandle(metadata, &originalIdentity)) {
        nativeError = GetLastError();
    }
    else if ((originalIdentity.dwFileAttributes &
              (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0u ||
             originalIdentity.nNumberOfLinks != 1u) {
        nativeError = ERROR_INVALID_DATA;
    }
    else if (!GetFileInformationByHandleEx(
                 metadata, FileBasicInfo, &originalBasic,
                 sizeof(originalBasic))) {
        nativeError = GetLastError();
    }
    else if ((originalBasic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0u) {
        FILE_BASIC_INFO writableBasic = originalBasic;
        writableBasic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (writableBasic.FileAttributes == 0u) {
            writableBasic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        if (!SetFileInformationByHandle(
                metadata, FileBasicInfo, &writableBasic,
                sizeof(writableBasic))) {
            nativeError = GetLastError();
        }
        else {
            readonlyCleared = true;
            FILE_BASIC_INFO readback{};
            if (!GetFileInformationByHandleEx(
                    metadata, FileBasicInfo, &readback, sizeof(readback))) {
                nativeError = GetLastError();
            }
            else if (readback.FileAttributes != writableBasic.FileAttributes) {
                nativeError = ERROR_INVALID_DATA;
            }
        }
    }
    if (nativeError != ERROR_SUCCESS) {
        if (readonlyCleared) {
            SetFileInformationByHandle(
                metadata, FileBasicInfo, &originalBasic,
                sizeof(originalBasic));
        }
        CloseHandle(metadata);
        detail = DurableNativeError(
            "FlushFile", path, static_cast<int>(nativeError));
        return false;
    }

    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        nativeError = GetLastError();
        if (readonlyCleared) {
            SetFileInformationByHandle(
                metadata, FileBasicInfo, &originalBasic,
                sizeof(originalBasic));
        }
        CloseHandle(metadata);
        detail = DurableNativeError(
            "FlushFile", path, static_cast<int>(nativeError));
        return false;
    }

    BY_HANDLE_FILE_INFORMATION openedIdentity{};
    bool flushOk = true;
    if (!GetFileInformationByHandle(file, &openedIdentity)) {
        nativeError = GetLastError();
        flushOk = false;
    }
    else if (openedIdentity.dwVolumeSerialNumber !=
                 originalIdentity.dwVolumeSerialNumber ||
             openedIdentity.nFileIndexHigh != originalIdentity.nFileIndexHigh ||
             openedIdentity.nFileIndexLow != originalIdentity.nFileIndexLow ||
             openedIdentity.nNumberOfLinks != 1u ||
             (openedIdentity.dwFileAttributes &
              (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0u) {
        nativeError = ERROR_INVALID_DATA;
        flushOk = false;
    }
    else if (!FlushFileBuffers(file)) {
        nativeError = GetLastError();
        flushOk = false;
    }

    if (flushOk && readonlyCleared) {
        if (!SetFileInformationByHandle(
                file, FileBasicInfo, &originalBasic, sizeof(originalBasic))) {
            nativeError = GetLastError();
            flushOk = false;
        }
        else {
            FILE_BASIC_INFO readback{};
            if (!GetFileInformationByHandleEx(
                    file, FileBasicInfo, &readback, sizeof(readback))) {
                nativeError = GetLastError();
                flushOk = false;
            }
            else if (readback.FileAttributes != originalBasic.FileAttributes) {
                nativeError = ERROR_INVALID_DATA;
                flushOk = false;
            }
            else if (!FlushFileBuffers(file)) {
                nativeError = GetLastError();
                flushOk = false;
            }
        }
    }

    BY_HANDLE_FILE_INFORMATION finalIdentity{};
    if (flushOk && !GetFileInformationByHandle(file, &finalIdentity)) {
        nativeError = GetLastError();
        flushOk = false;
    }
    else if (flushOk &&
             (finalIdentity.dwVolumeSerialNumber !=
                  originalIdentity.dwVolumeSerialNumber ||
              finalIdentity.nFileIndexHigh != originalIdentity.nFileIndexHigh ||
              finalIdentity.nFileIndexLow != originalIdentity.nFileIndexLow ||
              finalIdentity.nNumberOfLinks != 1u)) {
        nativeError = ERROR_INVALID_DATA;
        flushOk = false;
    }

    if (!flushOk && readonlyCleared) {
        SetFileInformationByHandle(
            metadata, FileBasicInfo, &originalBasic, sizeof(originalBasic));
    }
    const bool fileCloseOk = CloseHandle(file) != 0;
    const bool metadataCloseOk = CloseHandle(metadata) != 0;
    if (!flushOk || !fileCloseOk || !metadataCloseOk) {
        if (nativeError == ERROR_SUCCESS) {
            nativeError = ERROR_WRITE_FAULT;
        }
        detail = DurableNativeError(
            "FlushFile", path, static_cast<int>(nativeError));
        return false;
    }
    return true;
#else
    const int descriptor =
        ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        const int nativeError = errno;
        detail = DurableNativeError("FlushFile", path, nativeError);
        return false;
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        const int nativeError = errno == 0 ? EINVAL : errno;
        ::close(descriptor);
        detail = DurableNativeError("FlushFile", path, nativeError);
        return false;
    }
    if (::fsync(descriptor) != 0) {
        const int nativeError = errno;
        ::close(descriptor);
        detail = DurableNativeError("FlushFile", path, nativeError);
        return false;
    }
    if (::close(descriptor) != 0) {
        const int nativeError = errno;
        detail = DurableNativeError("FlushFile", path, nativeError);
        return false;
    }
    return true;
#endif
}

bool SyncPhysicalDirectory(
    const std::filesystem::path& directory,
    std::string& detail,
    std::span<const PhysicalEntryToFinalize> entriesToFinalize = {}) {
#if defined(_WIN32)
    (void)directory;
    if (!FinalizeWindowsPhysicalEntries(entriesToFinalize, detail)) {
        return false;
    }
    detail.clear();
    return true;
#else
    (void)entriesToFinalize;
    const int descriptor =
        ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        const int nativeError = errno;
        detail = DurableNativeError(
            "SyncDirectoryOrEquivalent", directory, nativeError);
        return false;
    }
    if (::fsync(descriptor) != 0) {
        const int nativeError = errno;
        ::close(descriptor);
        detail = DurableNativeError(
            "SyncDirectoryOrEquivalent", directory, nativeError);
        return false;
    }
    if (::close(descriptor) != 0) {
        const int nativeError = errno;
        detail = DurableNativeError(
            "SyncDirectoryOrEquivalent", directory, nativeError);
        return false;
    }
    return true;
#endif
}

bool DurabilizeRunProductsPortable(
    std::span<const std::filesystem::path> paths,
    std::string& detail) {
    detail.clear();
    if (paths.size() != kTerrainReferenceLeaves.size()) {
        detail = "DurabilizeRunProducts requires exactly seven paths";
        return false;
    }
    std::vector<PhysicalEntryToFinalize> entries(paths.size());
    const std::string runId = paths.front().parent_path().filename().generic_string();
    const std::vector plan = PlanTerrainRunDurability(paths.size());
    return ExecuteTerrainRunDurabilitySteps(
        plan,
        [&](const TerrainRunDurabilityStep& step,
            std::string& stepDetail) {
            if (step.Action == TerrainRunDurabilityAction::FLUSH_FILE) {
                const std::filesystem::path& path = paths[step.ProductIndex];
                if (!FlushPhysicalFile(path, stepDetail)) {
                    return false;
                }
                std::string hashDetail;
                const auto hash = Sha256File(path, hashDetail);
                if (!hash.has_value()) {
                    stepDetail = hashDetail;
                    return false;
                }
                const auto identity = PhysicalIdentityNoFollow(path, stepDetail);
                const auto mode = PhysicalFileMode(path, stepDetail);
                if (!identity.has_value() || !mode.has_value()) {
                    return false;
                }
                entries[step.ProductIndex] = PhysicalEntryToFinalize{
                    path,
                    path.parent_path() /
                        std::format(
                            ".garner.reference-albedo.finalize.{}.{}",
                            runId, path.filename().generic_string()),
                    "finalize-run-entry", runId, *hash,
                    SerializePhysicalIdentity(*identity), *mode};
                return true;
            }
            if (step.Action ==
                TerrainRunDurabilityAction::SYNC_DIRECTORY_OR_EQUIVALENT) {
                return SyncPhysicalDirectory(
                    paths.front().parent_path(), stepDetail, entries);
            }
            stepDetail = "unexpected run durability action";
            return false;
        },
        detail);
}

bool ReplacePhysicalFile(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string& detail) {
#if defined(_WIN32)
    wchar_t sourceVolume[MAX_PATH]{};
    wchar_t destinationVolume[MAX_PATH]{};
    if (!GetVolumePathNameW(source.c_str(), sourceVolume, MAX_PATH) ||
        !GetVolumePathNameW(destination.parent_path().c_str(),
                            destinationVolume, MAX_PATH)) {
        const DWORD nativeError = GetLastError();
        detail = std::format(
            "DURABLE_FS_ERROR op=ReplaceSameVolume source={} destination={} "
            "native=win32:{}:",
            JsonQuotedPath(source), JsonQuotedPath(destination), nativeError);
        return false;
    }
    DWORD sourceSerial = 0u;
    DWORD destinationSerial = 0u;
    if (!GetVolumeInformationW(sourceVolume, nullptr, 0u, &sourceSerial,
                               nullptr, nullptr, nullptr, 0u) ||
        !GetVolumeInformationW(destinationVolume, nullptr, 0u,
                               &destinationSerial, nullptr, nullptr,
                               nullptr, 0u)) {
        const DWORD nativeError = GetLastError();
        detail = std::format(
            "DURABLE_FS_ERROR op=ReplaceSameVolume source={} destination={} "
            "native=win32:{}:",
            JsonQuotedPath(source), JsonQuotedPath(destination), nativeError);
        return false;
    }
    if (sourceSerial != destinationSerial) {
        detail = std::format(
            "DURABLE_FS_ERROR op=ReplaceSameVolume source={} destination={} "
            "native=contract:CROSS_VOLUME:",
            JsonQuotedPath(source), JsonQuotedPath(destination));
        return false;
    }
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD nativeError = GetLastError();
        detail = std::format(
            "DURABLE_FS_ERROR op=ReplaceSameVolume source={} destination={} "
            "native=win32:{}:",
            JsonQuotedPath(source), JsonQuotedPath(destination), nativeError);
        return false;
    }
    return true;
#else
    struct stat sourceStatus {};
    struct stat directoryStatus {};
    if (::lstat(source.c_str(), &sourceStatus) != 0 ||
        !S_ISREG(sourceStatus.st_mode) ||
        ::stat(destination.parent_path().c_str(), &directoryStatus) != 0) {
        const int nativeError = errno == 0 ? EINVAL : errno;
        detail = std::format(
            "DURABLE_FS_ERROR op=ReplaceSameVolume source={} destination={} "
            "native=errno:{}:",
            JsonQuotedPath(source), JsonQuotedPath(destination), nativeError);
        return false;
    }
    if (sourceStatus.st_dev != directoryStatus.st_dev) {
        detail = std::format(
            "DURABLE_FS_ERROR op=ReplaceSameVolume source={} destination={} "
            "native=contract:CROSS_VOLUME:",
            JsonQuotedPath(source), JsonQuotedPath(destination));
        return false;
    }
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        const int nativeError = errno;
        detail = std::format(
            "DURABLE_FS_ERROR op=ReplaceSameVolume source={} destination={} "
            "native=errno:{}:",
            JsonQuotedPath(source), JsonQuotedPath(destination), nativeError);
        return false;
    }
    return true;
#endif
}

bool RemoveOwnedPhysical(
    const std::filesystem::path& path,
    std::string& detail,
    std::string_view expectedIdentity = {},
    std::string_view expectedHash = {},
    std::optional<std::uint32_t> expectedMode = std::nullopt) {
    std::error_code statusError;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, statusError);
    if (status.type() == std::filesystem::file_type::not_found &&
        (!statusError || statusError == std::errc::no_such_file_or_directory)) {
        return true;
    }
    if (statusError || status.type() != std::filesystem::file_type::regular) {
        detail = DurableNativeError("RemoveOwned", path,
                                    statusError ? statusError.value() : EINVAL);
        return false;
    }
    if (expectedIdentity.empty() || expectedHash.empty() ||
        !expectedMode.has_value()) {
        detail = std::format(
            "owned removal lacks recorded identity/hash/mode: {}",
            path.generic_string());
        return false;
    }
    if (!expectedIdentity.empty()) {
        const auto identity = PhysicalIdentityNoFollow(path, detail);
        std::string hashDetail;
        const auto hash = Sha256File(path, hashDetail);
        const auto mode = PhysicalFileMode(path, detail);
        if (!identity.has_value() ||
            SerializePhysicalIdentity(*identity) != expectedIdentity ||
            !hash.has_value() || *hash != expectedHash ||
            (expectedMode.has_value() &&
             (!mode.has_value() || *mode != *expectedMode))) {
            detail = hash.has_value()
                ? std::format(
                      "owned removal identity/hash/mode changed: {}",
                      path.generic_string())
                : hashDetail;
            return false;
        }
    }
#if defined(_WIN32)
    if (!expectedIdentity.empty()) {
        HANDLE handle = CreateFileW(
            path.c_str(), DELETE | GENERIC_READ,
            FILE_SHARE_READ,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD nativeError = GetLastError();
            detail = DurableNativeError(
                "RemoveOwned", path, static_cast<int>(nativeError));
            return false;
        }
        const auto handleIdentity = PhysicalIdentityFromHandle(
            handle, path, "RemoveOwned", detail);
        FILE_BASIC_INFO basic{};
        LARGE_INTEGER pinnedSize{};
        DWORD nativeError = ERROR_SUCCESS;
        bool deletePending = false;
        if (!handleIdentity.has_value() ||
            SerializePhysicalIdentity(*handleIdentity) != expectedIdentity) {
            nativeError = ERROR_FILE_INVALID;
        }
        else if (!GetFileInformationByHandleEx(
                     handle, FileBasicInfo, &basic, sizeof(basic))) {
            nativeError = GetLastError();
        }
        else if (expectedMode.has_value() &&
                 basic.FileAttributes != *expectedMode) {
            nativeError = ERROR_INVALID_DATA;
        }
        else if (!GetFileSizeEx(handle, &pinnedSize)) {
            nativeError = GetLastError();
        }
        else if (pinnedSize.QuadPart < 0 ||
                 static_cast<unsigned long long>(pinnedSize.QuadPart) >
                     static_cast<unsigned long long>(
                         std::numeric_limits<std::size_t>::max())) {
            nativeError = ERROR_FILE_TOO_LARGE;
        }
        else {
            std::vector<std::uint8_t> pinnedBytes(
                static_cast<std::size_t>(pinnedSize.QuadPart));
            std::size_t offset = 0u;
            while (offset < pinnedBytes.size()) {
                const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                    pinnedBytes.size() - offset,
                    static_cast<std::size_t>(UINT32_MAX)));
                DWORD read = 0u;
                if (!ReadFile(handle, pinnedBytes.data() + offset, requested,
                              &read, nullptr)) {
                    nativeError = GetLastError();
                    break;
                }
                if (read != requested) {
                    nativeError = ERROR_HANDLE_EOF;
                    break;
                }
                offset += read;
            }
            if (nativeError == ERROR_SUCCESS &&
                Sha256Bytes(pinnedBytes) != expectedHash) {
                nativeError = ERROR_CRC;
            }
            if (nativeError == ERROR_SUCCESS) {
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = TRUE;
                if (!SetFileInformationByHandle(
                        handle, FileDispositionInfo, &disposition,
                        sizeof(disposition))) {
                    nativeError = GetLastError();
                }
                else {
                    deletePending = true;
                }
            }
        }
        const bool closeOk = CloseHandle(handle) != 0;
        if (!deletePending || !closeOk) {
            if (detail.empty()) {
                detail = DurableNativeError(
                    "RemoveOwned", path,
                    static_cast<int>(nativeError == ERROR_SUCCESS
                                         ? ERROR_WRITE_FAULT
                                         : nativeError));
            }
            return false;
        }
    }
    else if (!DeleteFileW(path.c_str())) {
        const DWORD nativeError = GetLastError();
        detail = DurableNativeError("RemoveOwned", path,
                                    static_cast<int>(nativeError));
        return false;
    }
#else
    if (::unlink(path.c_str()) != 0) {
        const int nativeError = errno;
        detail = DurableNativeError("RemoveOwned", path, nativeError);
        return false;
    }
#endif
    if (!SyncPhysicalDirectory(path.parent_path(), detail)) {
        return false;
    }
    statusError.clear();
    const std::filesystem::file_status after =
        std::filesystem::symlink_status(path, statusError);
    if (after.type() != std::filesystem::file_type::not_found ||
        (statusError && statusError != std::errc::no_such_file_or_directory)) {
        detail = DurableNativeError(
            "RemoveOwned", path, statusError ? statusError.value() : EIO);
        return false;
    }
    return true;
}

bool ReadPhysicalText(
    const std::filesystem::path& path,
    std::string& bytes,
    std::string& detail) {
    std::error_code filesystemError;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, filesystemError);
    if (filesystemError ||
        status.type() != std::filesystem::file_type::regular) {
        detail = std::format("not a physical regular file: {}",
                             path.generic_string());
        return false;
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        detail = std::format("cannot open physical file: {}",
                             path.generic_string());
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>{input},
                 std::istreambuf_iterator<char>{});
    if (input.bad()) {
        detail = std::format("cannot read physical file: {}",
                             path.generic_string());
        return false;
    }
    return true;
}

#if defined(_WIN32)
struct WindowsFileIdentity {
    DWORD VolumeSerial{0u};
    DWORD FileIndexHigh{0u};
    DWORD FileIndexLow{0u};

    bool operator==(const WindowsFileIdentity&) const = default;
};

std::optional<WindowsFileIdentity> WindowsIdentityFromHandle(
    HANDLE handle,
    const std::filesystem::path& path,
    std::string_view operation,
    std::string& detail) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        const DWORD nativeError = GetLastError();
        detail = DurableNativeError(operation, path,
                                    static_cast<int>(nativeError));
        return std::nullopt;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
        information.nNumberOfLinks != 1u) {
        detail = DurableNativeError(operation, path, ERROR_INVALID_DATA);
        return std::nullopt;
    }
    return WindowsFileIdentity{
        information.dwVolumeSerialNumber,
        information.nFileIndexHigh,
        information.nFileIndexLow};
}

std::string WindowsMoveReservationRecord(
    const std::filesystem::path& source,
    const std::filesystem::path& reserved,
    std::string_view kind,
    std::string_view transactionId,
    std::string_view sourceHash) {
    JsonEmitter json;
    json.ObjectBegin();
    json.Key("kind"); json.String(kind);
    json.Key("reserved");
    json.String(std::filesystem::absolute(reserved).lexically_normal().generic_string());
    json.Key("source");
    json.String(std::filesystem::absolute(source).lexically_normal().generic_string());
    json.Key("sourceHash"); json.String(sourceHash);
    json.Key("transactionId"); json.String(transactionId);
    json.Key("version"); json.Uint(1u);
    json.ObjectEnd();
    return std::move(json).Take();
}

bool ReserveMoveTargetWindows(
    const std::filesystem::path& source,
    const std::filesystem::path& reserved,
    std::string_view kind,
    std::string_view transactionId,
    std::string_view sourceHash,
    std::string& detail) {
    const std::string record = WindowsMoveReservationRecord(
        source, reserved, kind, transactionId, sourceHash);
    if (record.size() > static_cast<std::size_t>(UINT32_MAX)) {
        detail = DurableNativeError(
            "ReserveMoveTarget", reserved, ERROR_FILE_TOO_LARGE);
        return false;
    }
    HANDLE handle = CreateFileW(
        reserved.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD nativeError = GetLastError();
        detail = DurableNativeError(
            "ReserveMoveTarget", reserved, static_cast<int>(nativeError));
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        const DWORD nativeError = GetLastError();
        CloseHandle(handle);
        detail = DurableNativeError(
            "ReserveMoveTarget", reserved, static_cast<int>(nativeError));
        return false;
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        CloseHandle(handle);
        detail = DurableNativeError(
            "ReserveMoveTarget", reserved, ERROR_REPARSE_TAG_INVALID);
        return false;
    }
    const auto originalIdentity = WindowsIdentityFromHandle(
        handle, reserved, "ReserveMoveTarget", detail);
    if (!originalIdentity.has_value()) {
        CloseHandle(handle);
        return false;
    }
    DWORD written = 0u;
    if (!WriteFile(handle, record.data(), static_cast<DWORD>(record.size()),
                   &written, nullptr)) {
        const DWORD nativeError = GetLastError();
        CloseHandle(handle);
        detail = DurableNativeError(
            "ReserveMoveTarget", reserved, static_cast<int>(nativeError));
        return false;
    }
    if (written != static_cast<DWORD>(record.size())) {
        CloseHandle(handle);
        detail = DurableNativeError(
            "ReserveMoveTarget", reserved,
            static_cast<int>(ERROR_WRITE_FAULT));
        return false;
    }
    if (!FlushFileBuffers(handle)) {
        const DWORD nativeError = GetLastError();
        CloseHandle(handle);
        detail = DurableNativeError(
            "ReserveMoveTarget", reserved, static_cast<int>(nativeError));
        return false;
    }
    CloseHandle(handle);

    handle = CreateFileW(
        reserved.c_str(), GENERIC_READ, 0u, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD nativeError = GetLastError();
        detail = DurableNativeError(
            "ReserveMoveTarget", reserved, static_cast<int>(nativeError));
        return false;
    }
    const auto reopenedIdentity = WindowsIdentityFromHandle(
        handle, reserved, "ReserveMoveTarget", detail);
    std::string readback(record.size(), '\0');
    DWORD read = 0u;
    bool readOk = false;
    DWORD nativeError = ERROR_SUCCESS;
    if (!reopenedIdentity.has_value()) {
        nativeError = ERROR_INVALID_DATA;
    }
    else if (*reopenedIdentity != *originalIdentity) {
        nativeError = ERROR_FILE_INVALID;
    }
    else if (!ReadFile(
                 handle, readback.data(), static_cast<DWORD>(readback.size()),
                 &read, nullptr)) {
        nativeError = GetLastError();
    }
    else if (read != static_cast<DWORD>(readback.size()) ||
             readback != record) {
        nativeError = ERROR_INVALID_DATA;
    }
    else {
        readOk = true;
    }
    CloseHandle(handle);
    if (!readOk) {
        if (detail.empty()) {
            detail = DurableNativeError(
                "ReserveMoveTarget", reserved,
                static_cast<int>(nativeError));
        }
        return false;
    }
    return true;
}

bool VerifyWindowsFinalizationArtifact(
    const std::filesystem::path& path,
    const PhysicalEntryToFinalize& entry,
    std::string& detail) {
    const auto identity = PhysicalIdentityNoFollow(path, detail);
    if (!identity.has_value() || entry.Identity.empty() ||
        SerializePhysicalIdentity(*identity) != entry.Identity) {
        detail = std::format(
            "finalized Windows entry identity changed: {}",
            path.generic_string());
        return false;
    }
    const auto mode = PhysicalFileMode(path, detail);
    if (!mode.has_value() || *mode != entry.Mode) {
        detail = std::format(
            "finalized Windows entry mode changed: {}",
            path.generic_string());
        return false;
    }
    std::string hashDetail;
    const auto hash = Sha256File(path, hashDetail);
    if (!hash.has_value() || *hash != entry.Sha256) {
        detail = hash.has_value()
            ? std::format("finalized Windows entry hash changed: {}",
                          path.generic_string())
            : hashDetail;
        return false;
    }
    return true;
}

bool FinalizeWindowsPhysicalEntries(
    std::span<const PhysicalEntryToFinalize> entries,
    std::string& detail) {
    const auto absent = [](const std::filesystem::path& path) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        return status.type() == std::filesystem::file_type::not_found &&
            (!error || error == std::errc::no_such_file_or_directory);
    };
    std::vector<TerrainWindowsDurableEntryState> states;
    std::vector<std::string> reservationIdentities(entries.size());
    std::vector<std::string> reservationHashes(entries.size());
    std::vector<std::optional<std::uint32_t>> reservationModes(entries.size());
    states.reserve(entries.size());
    for (std::size_t entryIndex = 0u; entryIndex < entries.size(); ++entryIndex) {
        const PhysicalEntryToFinalize& entry = entries[entryIndex];
        TerrainWindowsDurableEntryState state;
        state.SourcePresent = !absent(entry.Source);
        state.ReservationPresent = !absent(entry.Reservation);
        if (state.SourcePresent) {
            state.SourceMatchesRecordedArtifact =
                VerifyWindowsFinalizationArtifact(
                    entry.Source, entry, detail);
            if (!state.SourceMatchesRecordedArtifact) {
                return false;
            }
        }
        if (state.ReservationPresent) {
            std::string bytes;
            if (!ReadPhysicalText(entry.Reservation, bytes, detail)) {
                return false;
            }
            state.ReservationIsExactRecord =
                bytes == WindowsMoveReservationRecord(
                    entry.Source, entry.Reservation, entry.Kind,
                    entry.TransactionId, entry.Sha256);
            std::string hashDetail;
            const auto hash = Sha256File(entry.Reservation, hashDetail);
            if (!hash.has_value()) {
                detail = hashDetail;
                return false;
            }
            const auto identity =
                PhysicalIdentityNoFollow(entry.Reservation, detail);
            const auto mode = PhysicalFileMode(entry.Reservation, detail);
            if (!identity.has_value() || !mode.has_value()) {
                return false;
            }
            reservationIdentities[entryIndex] =
                SerializePhysicalIdentity(*identity);
            reservationHashes[entryIndex] = *hash;
            reservationModes[entryIndex] = *mode;
            state.ReservationIsRecordedPayload = *hash == entry.Sha256;
            if (state.ReservationIsExactRecord) {
                state.ReservationRecordMatchesOwnedArtifact =
                    *mode == PrivatePhysicalMode() &&
                    *hash == Sha256Bytes(std::span{
                        reinterpret_cast<const std::uint8_t*>(bytes.data()),
                        bytes.size()});
            }
            if (state.ReservationIsRecordedPayload) {
                state.ReservationMatchesRecordedArtifact =
                    VerifyWindowsFinalizationArtifact(
                        entry.Reservation, entry, detail);
                if (!state.ReservationMatchesRecordedArtifact) {
                    return false;
                }
            }
        }
        states.push_back(state);
    }
    const auto plan = PlanTerrainWindowsEntryFinalization(states, detail);
    if (!plan.has_value()) {
        return false;
    }
    return ExecuteTerrainWindowsDurableSteps(
        *plan,
        [&](const TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
        const PhysicalEntryToFinalize& entry = entries[step.EntryIndex];
        switch (step.Action) {
        case TerrainWindowsDurableAction::REMOVE_RESERVATION:
            return RemoveOwnedPhysical(
                entry.Reservation, stepDetail,
                reservationIdentities[step.EntryIndex],
                reservationHashes[step.EntryIndex],
                reservationModes[step.EntryIndex]);
        case TerrainWindowsDurableAction::RESTORE_SOURCE:
            return ReplacePhysicalFile(
                       entry.Reservation, entry.Source, stepDetail) &&
                VerifyWindowsFinalizationArtifact(
                    entry.Source, entry, stepDetail);
        case TerrainWindowsDurableAction::FLUSH_SOURCE:
            return FlushPhysicalFile(entry.Source, stepDetail);
        case TerrainWindowsDurableAction::RESERVE_TARGET:
            return ReserveMoveTargetWindows(
                entry.Source, entry.Reservation, entry.Kind,
                entry.TransactionId, entry.Sha256, stepDetail);
        case TerrainWindowsDurableAction::MOVE_TO_RESERVATION:
            return ReplacePhysicalFile(
                       entry.Source, entry.Reservation, stepDetail) &&
                VerifyWindowsFinalizationArtifact(
                    entry.Reservation, entry, stepDetail);
        case TerrainWindowsDurableAction::MOVE_TO_SOURCE:
            return ReplacePhysicalFile(
                       entry.Reservation, entry.Source, stepDetail) &&
                VerifyWindowsFinalizationArtifact(
                    entry.Source, entry, stepDetail);
        case TerrainWindowsDurableAction::VERIFY_SOURCE:
            return VerifyWindowsFinalizationArtifact(
                entry.Source, entry, stepDetail);
        default:
            stepDetail = "unexpected Windows entry-finalization action";
            return false;
        }
        },
        detail);
}
#endif

bool WriteExclusiveText(
    const std::filesystem::path& path,
    std::string_view bytes,
    std::uint32_t mode,
    std::string& detail,
    const std::function<void()>& afterExclusiveCreate = {}) {
#if defined(_WIN32)
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD nativeError = GetLastError();
        detail = DurableNativeError(
            "OpenExclusiveTemp", path, static_cast<int>(nativeError));
        return false;
    }
    const auto originalIdentity = WindowsIdentityFromHandle(
        handle, path, "OpenExclusiveTemp", detail);
    if (!originalIdentity.has_value()) {
        CloseHandle(handle);
        return false;
    }
    if (afterExclusiveCreate) {
        afterExclusiveCreate();
    }
    DWORD written = 0u;
    FILE_BASIC_INFO basic{};
    bool modeOk = false;
    DWORD nativeError = ERROR_SUCCESS;
    if (bytes.size() > static_cast<std::size_t>(UINT32_MAX)) {
        nativeError = ERROR_FILE_TOO_LARGE;
    }
    else if (!WriteFile(
                 handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                 &written, nullptr)) {
        nativeError = GetLastError();
    }
    else if (written != static_cast<DWORD>(bytes.size())) {
        nativeError = ERROR_WRITE_FAULT;
    }
    else if (!GetFileInformationByHandleEx(
                 handle, FileBasicInfo, &basic, sizeof(basic))) {
        nativeError = GetLastError();
    }
    else {
        basic.FileAttributes = mode;
        if (!SetFileInformationByHandle(
                handle, FileBasicInfo, &basic, sizeof(basic))) {
            nativeError = GetLastError();
        }
        else {
            FILE_BASIC_INFO readback{};
            if (!GetFileInformationByHandleEx(
                    handle, FileBasicInfo, &readback, sizeof(readback))) {
                nativeError = GetLastError();
            }
            else if (readback.FileAttributes != mode) {
                nativeError = ERROR_INVALID_DATA;
            }
            else if (!FlushFileBuffers(handle)) {
                nativeError = GetLastError();
            }
            else {
                modeOk = true;
            }
        }
    }
    if (!modeOk) {
        CloseHandle(handle);
        detail = DurableNativeError(
            "OpenExclusiveTemp", path,
            static_cast<int>(nativeError == ERROR_SUCCESS
                                 ? ERROR_WRITE_FAULT
                                 : nativeError));
        return false;
    }
    const auto finalIdentity = WindowsIdentityFromHandle(
        handle, path, "OpenExclusiveTemp", detail);
    const bool identityOk = finalIdentity.has_value() &&
        *finalIdentity == *originalIdentity;
    const bool closeOk = CloseHandle(handle) != 0;
    if (!identityOk || !closeOk) {
        if (detail.empty()) {
            detail = "owned exclusive file identity changed before close";
        }
        return false;
    }
    return true;
#else
    const int descriptor = ::open(
        path.c_str(), O_CREAT | O_EXCL | O_NOFOLLOW | O_RDWR | O_CLOEXEC,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        detail = DurableNativeError("OpenExclusiveTemp", path, errno);
        return false;
    }
    struct stat openedStatus {};
    if (::fstat(descriptor, &openedStatus) != 0 ||
        !S_ISREG(openedStatus.st_mode) || openedStatus.st_nlink != 1) {
        const int nativeError = errno == 0 ? EINVAL : errno;
        ::close(descriptor);
        detail = DurableNativeError("OpenExclusiveTemp", path, nativeError);
        return false;
    }
    if (afterExclusiveCreate) {
        afterExclusiveCreate();
    }
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            const int nativeError = errno == 0 ? EIO : errno;
            ::close(descriptor);
            detail = DurableNativeError("OpenExclusiveTemp", path, nativeError);
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    struct stat finalStatus {};
    struct stat pathStatus {};
    const bool finished =
        ::fchmod(descriptor, static_cast<mode_t>(mode)) == 0 &&
        ::fsync(descriptor) == 0 &&
        ::fstat(descriptor, &finalStatus) == 0 &&
        ::lstat(path.c_str(), &pathStatus) == 0 &&
        S_ISREG(finalStatus.st_mode) && finalStatus.st_nlink == 1 &&
        S_ISREG(pathStatus.st_mode) && pathStatus.st_nlink == 1 &&
        openedStatus.st_dev == finalStatus.st_dev &&
        openedStatus.st_ino == finalStatus.st_ino &&
        finalStatus.st_dev == pathStatus.st_dev &&
        finalStatus.st_ino == pathStatus.st_ino;
    const int nativeError = errno;
    const bool closeOk = ::close(descriptor) == 0;
    if (!finished || !closeOk) {
        detail = !finished && nativeError == 0
            ? "owned exclusive file identity changed before close"
            : DurableNativeError(
                  "OpenExclusiveTemp", path,
                  nativeError == 0 ? EIO : nativeError);
        return false;
    }
    return true;
#endif
}

bool CopyExclusivePhysicalFile(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::uint32_t mode,
    std::string& detail) {
    std::string bytes;
    if (!ReadPhysicalText(source, bytes, detail)) {
        return false;
    }
    return WriteExclusiveText(destination, bytes, mode, detail);
}

std::optional<std::uint32_t> PhysicalFileMode(
    const std::filesystem::path& path,
    std::string& detail) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD nativeError = GetLastError();
        detail = DurableNativeError("ReadMode", path,
                                    static_cast<int>(nativeError));
        return std::nullopt;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        detail = DurableNativeError(
            "ReadMode", path, ERROR_REPARSE_TAG_INVALID);
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(attributes);
#else
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        const int nativeError = errno == 0 ? EINVAL : errno;
        detail = DurableNativeError("ReadMode", path, nativeError);
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(status.st_mode & 07777u);
#endif
}

struct PublicationLock {
    std::filesystem::path Canonical;
    std::filesystem::path Retired;
#if defined(_WIN32)
    HANDLE Handle{INVALID_HANDLE_VALUE};
    WindowsFileIdentity Identity{};
#else
    int Descriptor{-1};
#endif

    PublicationLock() = default;
    PublicationLock(const PublicationLock&) = delete;
    PublicationLock& operator=(const PublicationLock&) = delete;
    PublicationLock(PublicationLock&& other) noexcept
        : Canonical(std::move(other.Canonical)),
          Retired(std::move(other.Retired)) {
#if defined(_WIN32)
        Handle = std::exchange(other.Handle, INVALID_HANDLE_VALUE);
        Identity = other.Identity;
#else
        Descriptor = std::exchange(other.Descriptor, -1);
#endif
    }
    PublicationLock& operator=(PublicationLock&&) = delete;

    ~PublicationLock() {
#if defined(_WIN32)
        if (Handle != INVALID_HANDLE_VALUE) {
            CloseHandle(Handle);
        }
#else
        if (Descriptor >= 0) {
            ::close(Descriptor);
        }
#endif
    }

    void Abandon() noexcept {
#if defined(_WIN32)
        if (Handle != INVALID_HANDLE_VALUE) {
            CloseHandle(Handle);
            Handle = INVALID_HANDLE_VALUE;
        }
#else
        if (Descriptor >= 0) {
            ::close(Descriptor);
            Descriptor = -1;
        }
#endif
    }
};

std::string PublicationLockMarker(const std::filesystem::path& canonical) {
    return std::format("corsairs-durable-lock-v1\npath={}\n",
                       canonical.lexically_normal().generic_string());
}

#if defined(_WIN32)
std::string WindowsLockGeneration(const WindowsFileIdentity& identity) {
    return std::format("{:08x}-{:08x}-{:08x}", identity.VolumeSerial,
                       identity.FileIndexHigh, identity.FileIndexLow);
}
#endif

std::optional<PublicationLock> AcquirePublicationLock(
    const std::filesystem::path& canonical,
    const std::filesystem::path& retired,
    std::string& detail) {
    detail.clear();
    const std::string marker = PublicationLockMarker(canonical);
#if defined(_WIN32)
    // A share-delete handle plus LockFileEx gives process-lifetime ownership.
    for (std::uint32_t attempt = 0u; attempt < 32u; ++attempt) {
        bool created = true;
        HANDLE handle = CreateFileW(
            canonical.c_str(), DELETE | GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL |
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const DWORD createError =
            handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (handle == INVALID_HANDLE_VALUE &&
            (createError == ERROR_FILE_EXISTS ||
             createError == ERROR_ALREADY_EXISTS)) {
            created = false;
            handle = CreateFileW(
                canonical.c_str(), DELETE | GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        }
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD nativeError = GetLastError();
            detail = DurableNativeError("LockExclusive", canonical,
                                        static_cast<int>(nativeError));
            return std::nullopt;
        }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(
                handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
            const DWORD nativeError = GetLastError();
            CloseHandle(handle);
            detail = DurableNativeError(
                "LockExclusive", canonical, static_cast<int>(nativeError));
            return std::nullopt;
        }
        if ((tag.FileAttributes &
             (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0u) {
            CloseHandle(handle);
            detail = DurableNativeError(
                "LockExclusive", canonical, ERROR_REPARSE_TAG_INVALID);
            return std::nullopt;
        }
        OVERLAPPED overlap{};
        if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK |
                        LOCKFILE_FAIL_IMMEDIATELY, 0u, 1u, 0u, &overlap)) {
            const DWORD nativeError = GetLastError();
            CloseHandle(handle);
            detail = DurableNativeError("LockExclusive", canonical,
                                        static_cast<int>(nativeError));
            return std::nullopt;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle, &size)) {
            const DWORD nativeError = GetLastError();
            CloseHandle(handle);
            detail = DurableNativeError("LockExclusive", canonical,
                                        static_cast<int>(nativeError));
            return std::nullopt;
        }
        if (size.QuadPart == 0) {
            if (!created) {
                CloseHandle(handle);
                detail = DurableNativeError(
                    "LockExclusive", canonical, ERROR_FILE_EXISTS);
                return std::nullopt;
            }
            DWORD written = 0u;
            if (!WriteFile(handle, marker.data(),
                           static_cast<DWORD>(marker.size()), &written,
                           nullptr)) {
                const DWORD nativeError = GetLastError();
                CloseHandle(handle);
                detail = DurableNativeError("LockExclusive", canonical,
                                            static_cast<int>(nativeError));
                return std::nullopt;
            }
            if (written != static_cast<DWORD>(marker.size())) {
                CloseHandle(handle);
                detail = DurableNativeError(
                    "LockExclusive", canonical, ERROR_WRITE_FAULT);
                return std::nullopt;
            }
            if (!FlushFileBuffers(handle)) {
                const DWORD nativeError = GetLastError();
                CloseHandle(handle);
                detail = DurableNativeError("LockExclusive", canonical,
                                            static_cast<int>(nativeError));
                return std::nullopt;
            }
            if (!GetFileSizeEx(handle, &size)) {
                const DWORD nativeError = GetLastError();
                CloseHandle(handle);
                detail = DurableNativeError("LockExclusive", canonical,
                                            static_cast<int>(nativeError));
                return std::nullopt;
            }
        }
        LARGE_INTEGER beginning{};
        if (size.QuadPart != static_cast<LONGLONG>(marker.size())) {
            CloseHandle(handle);
            detail = DurableNativeError(
                "LockExclusive", canonical, ERROR_INVALID_DATA);
            return std::nullopt;
        }
        if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
            const DWORD nativeError = GetLastError();
            CloseHandle(handle);
            detail = DurableNativeError(
                "LockExclusive", canonical, static_cast<int>(nativeError));
            return std::nullopt;
        }
        std::string readback(marker.size(), '\0');
        DWORD read = 0u;
        if (!ReadFile(handle, readback.data(),
                      static_cast<DWORD>(readback.size()), &read, nullptr)) {
            const DWORD nativeError = GetLastError();
            CloseHandle(handle);
            detail = DurableNativeError(
                "LockExclusive", canonical, static_cast<int>(nativeError));
            return std::nullopt;
        }
        if (read != static_cast<DWORD>(readback.size()) ||
            readback != marker) {
            CloseHandle(handle);
            detail = DurableNativeError(
                "LockExclusive", canonical, ERROR_INVALID_DATA);
            return std::nullopt;
        }
        const auto handleIdentity = WindowsIdentityFromHandle(
            handle, canonical, "LockExclusive", detail);
        if (!handleIdentity.has_value()) {
            CloseHandle(handle);
            return std::nullopt;
        }
        HANDLE fresh = CreateFileW(
            canonical.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (fresh == INVALID_HANDLE_VALUE) {
            const DWORD nativeError = GetLastError();
            CloseHandle(handle);
            if (nativeError == ERROR_FILE_NOT_FOUND ||
                nativeError == ERROR_PATH_NOT_FOUND) {
                continue;
            }
            detail = DurableNativeError("LockExclusive", canonical,
                                        static_cast<int>(nativeError));
            return std::nullopt;
        }
        const auto pathIdentity = WindowsIdentityFromHandle(
            fresh, canonical, "LockExclusive", detail);
        CloseHandle(fresh);
        if (!pathIdentity.has_value()) {
            CloseHandle(handle);
            return std::nullopt;
        }
        if (*handleIdentity != *pathIdentity) {
            CloseHandle(handle);
            continue;
        }

        const DWORD retiredAttributes = GetFileAttributesW(retired.c_str());
        if (retiredAttributes != INVALID_FILE_ATTRIBUTES) {
            if ((retiredAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
                (retiredAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
                CloseHandle(handle);
                detail = DurableNativeError(
                    "LockExclusive", retired, ERROR_REPARSE_TAG_INVALID);
                return std::nullopt;
            }
            std::string retiredBytes;
            if (!ReadPhysicalText(retired, retiredBytes, detail)) {
                CloseHandle(handle);
                return std::nullopt;
            }
            const auto retiredPhysicalIdentity =
                PhysicalIdentityNoFollow(retired, detail);
            const auto retiredMode = PhysicalFileMode(retired, detail);
            const std::string retiredHash = Sha256Bytes(std::span{
                reinterpret_cast<const std::uint8_t*>(retiredBytes.data()),
                retiredBytes.size()});
            if (!retiredPhysicalIdentity.has_value() ||
                !retiredMode.has_value()) {
                CloseHandle(handle);
                return std::nullopt;
            }
            const std::string expectedReservation =
                WindowsMoveReservationRecord(
                    canonical, retired, "retire-lock",
                    WindowsLockGeneration(*handleIdentity),
                    Sha256Bytes(std::span{
                        reinterpret_cast<const std::uint8_t*>(marker.data()),
                        marker.size()}));
            if (retiredBytes == expectedReservation) {
                if (!RemoveOwnedPhysical(
                        retired, detail,
                        SerializePhysicalIdentity(*retiredPhysicalIdentity),
                        retiredHash, *retiredMode)) {
                    CloseHandle(handle);
                    return std::nullopt;
                }
            }
            else if (retiredBytes == marker) {
                HANDLE old = CreateFileW(
                    retired.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (old == INVALID_HANDLE_VALUE) {
                    const DWORD nativeError = GetLastError();
                    CloseHandle(handle);
                    detail = DurableNativeError(
                        "LockExclusive", retired,
                        static_cast<int>(nativeError));
                    return std::nullopt;
                }
                const auto oldIdentity = WindowsIdentityFromHandle(
                    old, retired, "LockExclusive", detail);
                CloseHandle(old);
                if (!oldIdentity.has_value() ||
                    *oldIdentity == *handleIdentity ||
                    !RemoveOwnedPhysical(
                        retired, detail,
                        SerializePhysicalIdentity(*retiredPhysicalIdentity),
                        retiredHash, *retiredMode)) {
                    CloseHandle(handle);
                    if (detail.empty()) {
                        detail = "foreign retired durable lock";
                    }
                    return std::nullopt;
                }
            }
            else {
                CloseHandle(handle);
                detail = "foreign retired durable lock";
                return std::nullopt;
            }
        }
        else {
            const DWORD nativeError = GetLastError();
            if (nativeError != ERROR_FILE_NOT_FOUND &&
                nativeError != ERROR_PATH_NOT_FOUND) {
                CloseHandle(handle);
                detail = DurableNativeError("LockExclusive", retired,
                                            static_cast<int>(nativeError));
                return std::nullopt;
            }
        }
        PublicationLock lock;
        lock.Canonical = canonical;
        lock.Retired = retired;
        lock.Handle = handle;
        lock.Identity = *handleIdentity;
        return lock;
    }
#else
    for (std::uint32_t attempt = 0u; attempt < 32u; ++attempt) {
        bool created = true;
        int descriptor = ::open(
            canonical.c_str(), O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC,
            S_IRUSR | S_IWUSR);
        if (descriptor < 0 && errno == EEXIST) {
            created = false;
            descriptor = ::open(
                canonical.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        }
        if (descriptor < 0) {
            const int nativeError = errno;
            detail = DurableNativeError("LockExclusive", canonical,
                                        nativeError);
            return std::nullopt;
        }
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const int nativeError = errno;
            ::close(descriptor);
            detail = DurableNativeError("LockExclusive", canonical,
                                        nativeError);
            return std::nullopt;
        }
        struct stat handleStatus {};
        struct stat pathStatus {};
        if (::fstat(descriptor, &handleStatus) != 0) {
            const int nativeError = errno;
            ::close(descriptor);
            detail = DurableNativeError("LockExclusive", canonical,
                                        nativeError);
            return std::nullopt;
        }
        if (!S_ISREG(handleStatus.st_mode) || handleStatus.st_nlink != 1) {
            ::close(descriptor);
            detail = DurableNativeError("LockExclusive", canonical, EMLINK);
            return std::nullopt;
        }
        if (handleStatus.st_size == 0) {
            if (!created) {
                ::close(descriptor);
                detail = DurableNativeError("LockExclusive", canonical, EEXIST);
                return std::nullopt;
            }
            if (::ftruncate(descriptor, 0) != 0 ||
                ::pwrite(descriptor, marker.data(), marker.size(), 0) !=
                    static_cast<ssize_t>(marker.size()) ||
                ::fsync(descriptor) != 0) {
                const int nativeError = errno;
                ::close(descriptor);
                detail = DurableNativeError("LockExclusive", canonical,
                                            nativeError);
                return std::nullopt;
            }
            if (::fstat(descriptor, &handleStatus) != 0) {
                const int nativeError = errno;
                ::close(descriptor);
                detail = DurableNativeError("LockExclusive", canonical,
                                            nativeError);
                return std::nullopt;
            }
        }
        std::string readback(marker.size(), '\0');
        if (handleStatus.st_size != static_cast<off_t>(marker.size()) ||
            ::pread(descriptor, readback.data(), readback.size(), 0) !=
                static_cast<ssize_t>(readback.size()) ||
            readback != marker) {
            ::close(descriptor);
            detail = "durable lock marker mismatch";
            return std::nullopt;
        }
        if (::lstat(canonical.c_str(), &pathStatus) != 0 ||
            !S_ISREG(pathStatus.st_mode) || pathStatus.st_nlink != 1 ||
            handleStatus.st_dev != pathStatus.st_dev ||
            handleStatus.st_ino != pathStatus.st_ino) {
            ::close(descriptor);
            continue;
        }

        std::error_code retiredError;
        const std::filesystem::file_status retiredStatus =
            std::filesystem::symlink_status(retired, retiredError);
        if (retiredStatus.type() != std::filesystem::file_type::not_found ||
            (retiredError &&
             retiredError != std::errc::no_such_file_or_directory)) {
            std::string retiredBytes;
            struct stat retiredInfo {};
            std::string retiredIdentity;
            std::optional<std::uint32_t> retiredMode;
            const auto physicalIdentity =
                PhysicalIdentityNoFollow(retired, detail);
            retiredMode = PhysicalFileMode(retired, detail);
            if (physicalIdentity.has_value()) {
                retiredIdentity = SerializePhysicalIdentity(*physicalIdentity);
            }
            if (retiredError ||
                retiredStatus.type() != std::filesystem::file_type::regular ||
                ::lstat(retired.c_str(), &retiredInfo) != 0 ||
                !S_ISREG(retiredInfo.st_mode) || retiredInfo.st_nlink != 1 ||
                (retiredInfo.st_dev == handleStatus.st_dev &&
                 retiredInfo.st_ino == handleStatus.st_ino) ||
                !ReadPhysicalText(retired, retiredBytes, detail) ||
                retiredBytes != marker ||
                physicalIdentity == std::nullopt || !retiredMode.has_value() ||
                !RemoveOwnedPhysical(
                    retired, detail, retiredIdentity,
                    Sha256Bytes(std::span{
                        reinterpret_cast<const std::uint8_t*>(
                            retiredBytes.data()),
                        retiredBytes.size()}),
                    *retiredMode)) {
                ::close(descriptor);
                if (detail.empty()) {
                    detail = "foreign retired durable lock";
                }
                return std::nullopt;
            }
        }
        PublicationLock lock;
        lock.Canonical = canonical;
        lock.Retired = retired;
        lock.Descriptor = descriptor;
        return lock;
    }
#endif
    detail = "durable lock identity retry exhausted";
    return std::nullopt;
}

bool RetirePublicationLock(
    PublicationLock& lock,
    const TerrainReferenceFaultInjector& injectFault,
    std::string& detail) {
    const auto fault = [&](std::string_view point) {
        return injectFault ? injectFault(point)
                           : TerrainReferenceFaultAction::NONE;
    };
    const std::string marker = PublicationLockMarker(lock.Canonical);
#if defined(_WIN32)
    const std::string markerHash = Sha256Bytes(std::span{
        reinterpret_cast<const std::uint8_t*>(marker.data()), marker.size()});
    const std::vector steps = PlanTerrainWindowsLockRetirement();
    bool handleClosed = false;
    const bool executed = ExecuteTerrainWindowsDurableSteps(
        steps,
        [&](const TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
            switch (step.Action) {
            case TerrainWindowsDurableAction::VERIFY_LOCK_IDENTITIES: {
                HANDLE fresh = CreateFileW(
                    lock.Canonical.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (fresh == INVALID_HANDLE_VALUE) {
                    const DWORD nativeError = GetLastError();
                    stepDetail = DurableNativeError(
                        "RetireLock", lock.Canonical,
                        static_cast<int>(nativeError));
                    return false;
                }
                const auto pathIdentity = WindowsIdentityFromHandle(
                    fresh, lock.Canonical, "RetireLock", stepDetail);
                const bool closeOk = CloseHandle(fresh) != 0;
                const auto handleIdentity = WindowsIdentityFromHandle(
                    lock.Handle, lock.Canonical, "RetireLock", stepDetail);
                if (!closeOk || !pathIdentity.has_value() ||
                    !handleIdentity.has_value() ||
                    *pathIdentity != lock.Identity ||
                    *handleIdentity != lock.Identity) {
                    if (stepDetail.empty()) {
                        stepDetail =
                            "durable lock identity changed before retirement";
                    }
                    return false;
                }
                return true;
            }
            case TerrainWindowsDurableAction::RESERVE_TARGET:
                if (!ReserveMoveTargetWindows(
                        lock.Canonical, lock.Retired, "retire-lock",
                        WindowsLockGeneration(lock.Identity), markerHash,
                        stepDetail)) {
                    return false;
                }
                if (fault("MANIFEST_AFTER_LOCK_RESERVATION_DURABLE") !=
                    TerrainReferenceFaultAction::NONE) {
                    stepDetail =
                        "injected fault MANIFEST_AFTER_LOCK_RESERVATION_DURABLE";
                    return false;
                }
                return true;
            case TerrainWindowsDurableAction::MOVE_TO_RESERVATION:
                return ReplacePhysicalFile(
                           lock.Canonical, lock.Retired, stepDetail) &&
                    SyncPhysicalDirectory(
                        lock.Canonical.parent_path(), stepDetail);
            case TerrainWindowsDurableAction::VERIFY_RESERVATION_PAYLOAD: {
                const DWORD canonicalAttributes =
                    GetFileAttributesW(lock.Canonical.c_str());
                if (canonicalAttributes != INVALID_FILE_ATTRIBUTES) {
                    stepDetail = DurableNativeError(
                        "RetireLock", lock.Canonical, ERROR_INVALID_DATA);
                    return false;
                }
                const DWORD absenceError = GetLastError();
                if (absenceError != ERROR_FILE_NOT_FOUND &&
                    absenceError != ERROR_PATH_NOT_FOUND) {
                    stepDetail = DurableNativeError(
                        "RetireLock", lock.Canonical,
                        static_cast<int>(absenceError));
                    return false;
                }
                const auto movedIdentity = WindowsIdentityFromHandle(
                    lock.Handle, lock.Retired, "RetireLock", stepDetail);
                if (!movedIdentity.has_value() ||
                    *movedIdentity != lock.Identity) {
                    if (stepDetail.empty()) {
                        stepDetail =
                            "durable lock identity changed after retirement move";
                    }
                    return false;
                }
                if (fault("MANIFEST_AFTER_LOCK_MOVE_TO_RETIRED") !=
                    TerrainReferenceFaultAction::NONE) {
                    stepDetail =
                        "injected fault MANIFEST_AFTER_LOCK_MOVE_TO_RETIRED";
                    return false;
                }
                return true;
            }
            case TerrainWindowsDurableAction::ACCEPT_DELETE_PENDING:
                {
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = TRUE;
                if (!SetFileInformationByHandle(
                        lock.Handle, FileDispositionInfo, &disposition,
                        sizeof(disposition))) {
                    const DWORD nativeError = GetLastError();
                    stepDetail = DurableNativeError(
                        "RetireLock", lock.Retired,
                        static_cast<int>(nativeError));
                    return false;
                }
                if (fault("MANIFEST_AFTER_LOCK_DELETE_PENDING") !=
                    TerrainReferenceFaultAction::NONE) {
                    stepDetail =
                        "injected fault MANIFEST_AFTER_LOCK_DELETE_PENDING";
                    return false;
                }
                return true;
                }
            case TerrainWindowsDurableAction::CLOSE_LOCK_HANDLE:
                lock.Abandon();
                handleClosed = true;
                return true;
            default:
                stepDetail = "unexpected Windows lock-retirement action";
                return false;
            }
        },
        detail);
    if (!executed && !handleClosed) {
        lock.Abandon();
    }
    return executed;
#else
    struct stat handleStatus {};
    struct stat pathStatus {};
    if (::fstat(lock.Descriptor, &handleStatus) != 0 ||
        ::lstat(lock.Canonical.c_str(), &pathStatus) != 0 ||
        !S_ISREG(handleStatus.st_mode) || handleStatus.st_nlink != 1 ||
        !S_ISREG(pathStatus.st_mode) || pathStatus.st_nlink != 1 ||
        handleStatus.st_dev != pathStatus.st_dev ||
        handleStatus.st_ino != pathStatus.st_ino) {
        detail = "durable lock identity changed before retirement";
        lock.Abandon();
        return false;
    }
    if (fault("MANIFEST_AFTER_LOCK_RESERVATION_DURABLE") !=
        TerrainReferenceFaultAction::NONE) {
        detail = "injected fault MANIFEST_AFTER_LOCK_RESERVATION_DURABLE";
        lock.Abandon();
        return false;
    }
    if (!ReplacePhysicalFile(lock.Canonical, lock.Retired, detail) ||
        !SyncPhysicalDirectory(lock.Canonical.parent_path(), detail)) {
        lock.Abandon();
        return false;
    }
    if (fault("MANIFEST_AFTER_LOCK_MOVE_TO_RETIRED") !=
        TerrainReferenceFaultAction::NONE) {
        detail = "injected fault MANIFEST_AFTER_LOCK_MOVE_TO_RETIRED";
        lock.Abandon();
        return false;
    }
    const PhysicalFileIdentity retainedIdentity{
        static_cast<std::uint64_t>(handleStatus.st_dev), 0u,
        static_cast<std::uint64_t>(handleStatus.st_ino)};
    if (!RemoveOwnedPhysical(
            lock.Retired, detail,
            SerializePhysicalIdentity(retainedIdentity),
            Sha256Bytes(std::span{
                reinterpret_cast<const std::uint8_t*>(marker.data()),
                marker.size()}),
            static_cast<std::uint32_t>(handleStatus.st_mode & 07777))) {
        lock.Abandon();
        return false;
    }
    if (fault("MANIFEST_AFTER_LOCK_DELETE_PENDING") !=
        TerrainReferenceFaultAction::NONE) {
        detail = "injected fault MANIFEST_AFTER_LOCK_DELETE_PENDING";
        lock.Abandon();
        return false;
    }
    lock.Abandon();
    return true;
#endif
}

struct ManifestPublicationJournal {
    std::string Phase;
    std::string TransactionId;
    std::filesystem::path Destination;
    std::filesystem::path Temp;
    std::filesystem::path Backup;
    std::filesystem::path TempReservation;
    std::filesystem::path BackupReservation;
    std::filesystem::path Rollback;
    std::filesystem::path RollbackReservation;
    std::filesystem::path TempTombstone;
    std::filesystem::path BackupTombstone;
    std::filesystem::path RollbackTombstone;
    std::filesystem::path DestinationTombstone;
    std::filesystem::path JournalRetired;
    bool PriorExists{false};
    std::string PriorSha256;
    std::uint32_t PriorMode{0u};
    std::string IntendedSha256;
    std::uint32_t IntendedMode{0u};
    std::string TempIdentity;
    std::string BackupIdentity;
    std::string RollbackIdentity;
    std::string RunId;
};

std::string SerializeManifestJournal(
    const ManifestPublicationJournal& journal) {
    JsonEmitter json;
    json.ObjectBegin();
    json.Key("version"); json.Uint(1u);
    json.Key("phase"); json.String(journal.Phase);
    json.Key("transactionId"); json.String(journal.TransactionId);
    json.Key("destination");
    json.String(journal.Destination.lexically_normal().generic_string());
    json.Key("temp");
    json.String(journal.Temp.lexically_normal().generic_string());
    json.Key("backup");
    json.String(journal.Backup.empty()
                    ? std::string_view{}
                    : journal.Backup.lexically_normal().generic_string());
    json.Key("tempReservation");
    json.String(journal.TempReservation.lexically_normal().generic_string());
    json.Key("backupReservation");
    json.String(journal.BackupReservation.lexically_normal().generic_string());
    json.Key("rollback");
    json.String(journal.Rollback.lexically_normal().generic_string());
    json.Key("rollbackReservation");
    json.String(journal.RollbackReservation.lexically_normal().generic_string());
    json.Key("tempTombstone");
    json.String(journal.TempTombstone.lexically_normal().generic_string());
    json.Key("backupTombstone");
    json.String(journal.BackupTombstone.lexically_normal().generic_string());
    json.Key("rollbackTombstone");
    json.String(journal.RollbackTombstone.lexically_normal().generic_string());
    json.Key("destinationTombstone");
    json.String(journal.DestinationTombstone.lexically_normal().generic_string());
    json.Key("journalRetired");
    json.String(journal.JournalRetired.lexically_normal().generic_string());
    json.Key("priorExists"); json.Uint(journal.PriorExists ? 1u : 0u);
    json.Key("priorSha256"); json.String(journal.PriorSha256);
    json.Key("priorMode"); json.Uint(journal.PriorMode);
    json.Key("intendedSha256"); json.String(journal.IntendedSha256);
    json.Key("intendedMode"); json.Uint(journal.IntendedMode);
    json.Key("tempIdentity"); json.String(journal.TempIdentity);
    json.Key("backupIdentity"); json.String(journal.BackupIdentity);
    json.Key("rollbackIdentity"); json.String(journal.RollbackIdentity);
    json.Key("runId"); json.String(journal.RunId);
    json.ObjectEnd();
    return std::move(json).Take();
}

std::optional<ManifestPublicationJournal> ParseManifestJournal(
    std::string_view bytes,
    std::string& detail) {
    detail.clear();
    std::vector<TerrainManifestIssue> issues;
    StrictJsonParser parser{bytes};
    const auto root = parser.Parse(issues);
    if (!root.has_value() ||
        !RequireType(*root, JsonType::OBJECT, "", issues)) {
        detail = "invalid publication journal JSON";
        return std::nullopt;
    }
    constexpr std::array<std::string_view, 24> keys{
        "version", "phase", "transactionId", "destination", "temp",
        "backup", "tempReservation", "backupReservation", "rollback",
        "rollbackReservation", "tempTombstone", "backupTombstone",
        "rollbackTombstone",
        "destinationTombstone", "journalRetired", "priorExists",
        "priorSha256", "priorMode", "intendedSha256", "intendedMode",
        "tempIdentity", "backupIdentity", "rollbackIdentity", "runId"};
    if (!RejectUnknownMembers(*root, keys, "", issues)) {
        detail = "invalid publication journal member set";
        return std::nullopt;
    }
    const auto member = [&](std::string_view key, JsonType type) {
        return RequireMember(*root, key, type, "", issues);
    };
    const JsonValue* version = member("version", JsonType::NUMBER);
    const JsonValue* phase = member("phase", JsonType::STRING);
    const JsonValue* transactionId = member("transactionId", JsonType::STRING);
    const JsonValue* destination = member("destination", JsonType::STRING);
    const JsonValue* temp = member("temp", JsonType::STRING);
    const JsonValue* backup = member("backup", JsonType::STRING);
    const JsonValue* tempReservation =
        member("tempReservation", JsonType::STRING);
    const JsonValue* backupReservation =
        member("backupReservation", JsonType::STRING);
    const JsonValue* rollback = member("rollback", JsonType::STRING);
    const JsonValue* rollbackReservation =
        member("rollbackReservation", JsonType::STRING);
    const JsonValue* tempTombstone =
        member("tempTombstone", JsonType::STRING);
    const JsonValue* backupTombstone =
        member("backupTombstone", JsonType::STRING);
    const JsonValue* rollbackTombstone =
        member("rollbackTombstone", JsonType::STRING);
    const JsonValue* destinationTombstone =
        member("destinationTombstone", JsonType::STRING);
    const JsonValue* journalRetired =
        member("journalRetired", JsonType::STRING);
    const JsonValue* priorExists = member("priorExists", JsonType::NUMBER);
    const JsonValue* priorSha256 = member("priorSha256", JsonType::STRING);
    const JsonValue* priorMode = member("priorMode", JsonType::NUMBER);
    const JsonValue* intendedSha256 =
        member("intendedSha256", JsonType::STRING);
    const JsonValue* intendedMode = member("intendedMode", JsonType::NUMBER);
    const JsonValue* tempIdentity = member("tempIdentity", JsonType::STRING);
    const JsonValue* backupIdentity = member("backupIdentity", JsonType::STRING);
    const JsonValue* rollbackIdentity =
        member("rollbackIdentity", JsonType::STRING);
    const JsonValue* runId = member("runId", JsonType::STRING);
    if (version == nullptr || phase == nullptr || transactionId == nullptr ||
        destination == nullptr || temp == nullptr || backup == nullptr ||
        tempReservation == nullptr || backupReservation == nullptr ||
        rollback == nullptr || rollbackReservation == nullptr ||
        tempTombstone == nullptr || backupTombstone == nullptr ||
        rollbackTombstone == nullptr ||
        destinationTombstone == nullptr || journalRetired == nullptr ||
        priorExists == nullptr || priorSha256 == nullptr ||
        priorMode == nullptr || intendedSha256 == nullptr ||
        intendedMode == nullptr || tempIdentity == nullptr ||
        backupIdentity == nullptr || rollbackIdentity == nullptr ||
        runId == nullptr) {
        detail = "publication journal has missing member";
        return std::nullopt;
    }
    std::uint64_t integer = 0u;
    ManifestPublicationJournal journal;
    std::string pathText;
    if (!ReadUint(*version, 1u, "/version", integer, issues) || integer != 1u ||
        !ReadString(*phase, "/phase", journal.Phase, issues) ||
        !ReadString(*transactionId, "/transactionId",
                    journal.TransactionId, issues) ||
        !ReadString(*destination, "/destination", pathText, issues)) {
        detail = "publication journal has invalid scalar";
        return std::nullopt;
    }
    journal.Destination = std::filesystem::path{pathText};
    if (!ReadString(*temp, "/temp", pathText, issues)) {
        detail = "publication journal has invalid temp";
        return std::nullopt;
    }
    journal.Temp = std::filesystem::path{pathText};
    if (!ReadString(*backup, "/backup", pathText, issues)) {
        detail = "publication journal has invalid backup";
        return std::nullopt;
    }
    journal.Backup = std::filesystem::path{pathText};
    if (!ReadString(*tempReservation, "/tempReservation", pathText, issues)) {
        detail = "publication journal has invalid temp reservation";
        return std::nullopt;
    }
    journal.TempReservation = std::filesystem::path{pathText};
    if (!ReadString(*backupReservation, "/backupReservation", pathText,
                    issues)) {
        detail = "publication journal has invalid backup reservation";
        return std::nullopt;
    }
    journal.BackupReservation = std::filesystem::path{pathText};
    if (!ReadString(*rollback, "/rollback", pathText, issues)) {
        detail = "publication journal has invalid rollback";
        return std::nullopt;
    }
    journal.Rollback = std::filesystem::path{pathText};
    if (!ReadString(*rollbackReservation, "/rollbackReservation", pathText,
                    issues)) {
        detail = "publication journal has invalid rollback reservation";
        return std::nullopt;
    }
    journal.RollbackReservation = std::filesystem::path{pathText};
    if (!ReadString(*tempTombstone, "/tempTombstone", pathText, issues)) {
        detail = "publication journal has invalid temp tombstone";
        return std::nullopt;
    }
    journal.TempTombstone = std::filesystem::path{pathText};
    if (!ReadString(*backupTombstone, "/backupTombstone", pathText, issues)) {
        detail = "publication journal has invalid backup tombstone";
        return std::nullopt;
    }
    journal.BackupTombstone = std::filesystem::path{pathText};
    if (!ReadString(*rollbackTombstone, "/rollbackTombstone", pathText,
                    issues)) {
        detail = "publication journal has invalid rollback tombstone";
        return std::nullopt;
    }
    journal.RollbackTombstone = std::filesystem::path{pathText};
    if (!ReadString(*destinationTombstone, "/destinationTombstone",
                    pathText, issues)) {
        detail = "publication journal has invalid destination tombstone";
        return std::nullopt;
    }
    journal.DestinationTombstone = std::filesystem::path{pathText};
    if (!ReadString(*journalRetired, "/journalRetired", pathText, issues)) {
        detail = "publication journal has invalid retired mapping";
        return std::nullopt;
    }
    journal.JournalRetired = std::filesystem::path{pathText};
    if (!ReadUint(*priorExists, 1u, "/priorExists", integer, issues)) {
        detail = "publication journal has invalid priorExists";
        return std::nullopt;
    }
    journal.PriorExists = integer == 1u;
    if (!ReadString(*priorSha256, "/priorSha256",
                    journal.PriorSha256, issues) ||
        !ReadUint(*priorMode, UINT32_MAX, "/priorMode", integer, issues)) {
        detail = "publication journal has invalid prior metadata";
        return std::nullopt;
    }
    journal.PriorMode = static_cast<std::uint32_t>(integer);
    if (!ReadString(*intendedSha256, "/intendedSha256",
                    journal.IntendedSha256, issues) ||
        !ReadUint(*intendedMode, UINT32_MAX, "/intendedMode",
                  integer, issues)) {
        detail = "publication journal has invalid intended metadata";
        return std::nullopt;
    }
    journal.IntendedMode = static_cast<std::uint32_t>(integer);
    if (!ReadString(*tempIdentity, "/tempIdentity",
                    journal.TempIdentity, issues) ||
        !ReadString(*backupIdentity, "/backupIdentity",
                    journal.BackupIdentity, issues) ||
        !ReadString(*rollbackIdentity, "/rollbackIdentity",
                    journal.RollbackIdentity, issues) ||
        !ReadString(*runId, "/runId", journal.RunId, issues) ||
        !issues.empty()) {
        detail = "publication journal has invalid intended metadata";
        return std::nullopt;
    }
    constexpr std::array<std::string_view, 4> phases{
        "SNAPSHOT", "PREPARED", "REPLACED", "COMMITTED"};
    if (std::ranges::find(phases, journal.Phase) == phases.end() ||
        journal.TransactionId.empty() || journal.RunId.empty() ||
        !IsLowerHexSha256(journal.IntendedSha256) ||
        journal.IntendedMode == 0u ||
        journal.TempReservation.empty() || journal.BackupReservation.empty() ||
        journal.Rollback.empty() || journal.RollbackReservation.empty() ||
        journal.TempTombstone.empty() || journal.BackupTombstone.empty() ||
        journal.RollbackTombstone.empty() ||
        journal.DestinationTombstone.empty() || journal.JournalRetired.empty() ||
        (journal.PriorExists && !IsLowerHexSha256(journal.PriorSha256)) ||
        (!journal.PriorExists &&
         (!journal.PriorSha256.empty() || journal.PriorMode != 0u)) ||
        (journal.Phase != "SNAPSHOT" && journal.TempIdentity.empty()) ||
        (journal.Phase != "SNAPSHOT" && journal.PriorExists &&
         journal.BackupIdentity.empty()) ||
        journal.Destination.empty() || journal.Temp.empty() ||
        (journal.PriorExists && journal.Backup.empty())) {
        detail = "publication journal violates invariant";
        return std::nullopt;
    }
    return journal;
}

std::string NextTransactionId() {
    static std::atomic<std::uint64_t> transactionSequence{0u};
    return std::format(
        "{:016x}-{:016x}",
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()),
        transactionSequence.fetch_add(1u, std::memory_order_relaxed));
}

bool IsDirectOwnedChild(
    const std::filesystem::path& parent,
    const std::filesystem::path& path) {
    return !path.empty() && path.is_relative() == parent.is_relative() &&
        path.lexically_normal().parent_path() == parent.lexically_normal() &&
        path.filename() != "." && path.filename() != "..";
}

bool WriteManifestJournal(
    const ManifestPublicationJournal& journal,
    const std::filesystem::path& canonicalJournal,
    std::string& detail) {
    static std::atomic<std::uint64_t> updateSequence{0u};
    const std::filesystem::path update =
        canonicalJournal.parent_path() /
        std::format(".garner.reference-albedo.publish.update.{}.{:016x}",
                    journal.TransactionId,
                    updateSequence.fetch_add(1u, std::memory_order_relaxed));
    const std::string bytes = SerializeManifestJournal(journal);
    const auto failWithCheckedUpdateCleanup = [&](std::string primaryDetail) {
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(update, statusError);
        if (status.type() == std::filesystem::file_type::not_found &&
            (!statusError ||
             statusError == std::errc::no_such_file_or_directory)) {
            detail = std::move(primaryDetail);
            return false;
        }
        std::string cleanupDetail;
        const auto identity = PhysicalIdentityNoFollow(update, cleanupDetail);
        const auto hash = Sha256File(update, cleanupDetail);
        const auto mode = PhysicalFileMode(update, cleanupDetail);
        if (!identity.has_value() || !hash.has_value() || !mode.has_value() ||
            !RemoveOwnedPhysical(
                update, cleanupDetail, SerializePhysicalIdentity(*identity),
                *hash, *mode)) {
            detail = std::format(
                "RECOVERY_REQUIRED journal update retained path={} cause={} "
                "cleanup={}",
                update.generic_string(), primaryDetail, cleanupDetail);
            return false;
        }
        detail = std::move(primaryDetail);
        return false;
    };
    if (!WriteExclusiveText(update, bytes, PrivatePhysicalMode(), detail)) {
        return failWithCheckedUpdateCleanup(detail);
    }
    std::string readback;
    if (!ReadPhysicalText(update, readback, detail)) {
        return failWithCheckedUpdateCleanup(detail);
    }
    const auto parsed = ParseManifestJournal(readback, detail);
    if (!parsed.has_value() ||
        SerializeManifestJournal(*parsed) != bytes || readback != bytes) {
        if (detail.empty()) {
            detail = "publication journal readback mismatch";
        }
        return failWithCheckedUpdateCleanup(detail);
    }
    if (!ReplacePhysicalFile(update, canonicalJournal, detail) ||
        !SyncPhysicalDirectory(canonicalJournal.parent_path(), detail)) {
        return failWithCheckedUpdateCleanup(detail);
    }
    return true;
}

bool RetireManifestJournal(
    const ManifestPublicationJournal& journal,
    const std::filesystem::path& canonicalJournal,
    const std::filesystem::path& retiredJournal,
    std::string_view expectedIdentity,
    const TerrainReferenceFaultInjector& injectFault,
    std::string& detail) {
    std::string journalBytes;
    const auto identity = PhysicalIdentityNoFollow(canonicalJournal, detail);
    const auto mode = PhysicalFileMode(canonicalJournal, detail);
    if (!ReadPhysicalText(canonicalJournal, journalBytes, detail) ||
        SerializeManifestJournal(journal) != journalBytes ||
        !identity.has_value() ||
        SerializePhysicalIdentity(*identity) != expectedIdentity ||
        !mode.has_value() || *mode != PrivatePhysicalMode()) {
        if (detail.empty()) {
            detail = "canonical publication journal changed before retirement";
        }
        return false;
    }
#if defined(_WIN32)
    if (!ReserveMoveTargetWindows(
            canonicalJournal, retiredJournal, "retire-journal",
            journal.TransactionId,
            Sha256Bytes(std::span{
                reinterpret_cast<const std::uint8_t*>(journalBytes.data()),
                journalBytes.size()}),
            detail)) {
        return false;
    }
#endif
    if (!ReplacePhysicalFile(canonicalJournal, retiredJournal, detail) ||
        !SyncPhysicalDirectory(canonicalJournal.parent_path(), detail)) {
        return false;
    }
    if (injectFault &&
        injectFault("MANIFEST_AFTER_JOURNAL_RETIRE") !=
            TerrainReferenceFaultAction::NONE) {
        detail = "injected fault MANIFEST_AFTER_JOURNAL_RETIRE";
        return false;
    }
    return true;
}

bool PhysicalPathAbsent(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    return status.type() == std::filesystem::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory);
}

bool VerifyPhysicalFile(
    const std::filesystem::path& path,
    std::string_view expectedBytes,
    std::string_view expectedHash,
    std::optional<std::uint32_t> expectedMode,
    std::string& detail) {
    std::string bytes;
    if (!ReadPhysicalText(path, bytes, detail) || bytes != expectedBytes) {
        if (detail.empty()) {
            detail = "physical file bytes differ";
        }
        return false;
    }
    std::string hashDetail;
    const auto hash = Sha256File(path, hashDetail);
    if (!hash.has_value() || *hash != expectedHash) {
        detail = hash.has_value() ? "physical file hash differs" : hashDetail;
        return false;
    }
    if (expectedMode.has_value()) {
        const auto mode = PhysicalFileMode(path, detail);
        if (!mode.has_value() || *mode != *expectedMode) {
            if (detail.empty()) {
                detail = "physical file mode differs";
            }
            return false;
        }
    }
    return true;
}

std::string Sha256Text(std::string_view text) {
    return Sha256Bytes(std::span{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

bool VerifyHashAndMode(
    const std::filesystem::path& path,
    std::string_view expectedHash,
    std::uint32_t expectedMode,
    std::string& detail) {
    std::string hashDetail;
    const auto hash = Sha256File(path, hashDetail);
    if (!hash.has_value() || *hash != expectedHash) {
        detail = hash.has_value() ? "physical file hash differs" : hashDetail;
        return false;
    }
    const auto mode = PhysicalFileMode(path, detail);
    return mode.has_value() && *mode == expectedMode;
}

bool VerifyRecordedOwnedArtifact(
    const std::filesystem::path& path,
    std::string_view expectedHash,
    std::uint32_t expectedMode,
    std::string_view expectedIdentity,
    std::string& detail) {
    const auto identity = PhysicalIdentityNoFollow(path, detail);
    if (!identity.has_value() || expectedIdentity.empty() ||
        SerializePhysicalIdentity(*identity) != expectedIdentity ||
        !VerifyHashAndMode(path, expectedHash, expectedMode, detail)) {
        detail = std::format(
            "owned artifact identity/hash/mode mismatch: {}",
            path.generic_string());
        return false;
    }
    return true;
}

bool RemoveRecordedOwnedArtifact(
    const std::filesystem::path& path,
    const std::filesystem::path& tombstone,
    std::string_view kind,
    std::string_view transactionId,
    std::string_view expectedHash,
    std::uint32_t expectedMode,
    std::string_view expectedIdentity,
    const TerrainReferenceFaultInjector& injectFault,
    std::string_view reservationFaultPoint,
    std::string& detail) {
    const bool pathAbsent = PhysicalPathAbsent(path);
    const bool tombstoneAbsent = PhysicalPathAbsent(tombstone);
#if defined(_WIN32)
    const std::string reservation = WindowsMoveReservationRecord(
        path, tombstone, kind, transactionId, expectedHash);
    TerrainWindowsRemoveOwnedState state;
    state.SourcePresent = !pathAbsent;
    state.TombstonePresent = !tombstoneAbsent;
    state.SourceMatchesRecordedArtifact = pathAbsent ||
        VerifyRecordedOwnedArtifact(
            path, expectedHash, expectedMode, expectedIdentity, detail);
    std::string reservationIdentity;
    const std::string reservationHash = Sha256Text(reservation);
    if (!tombstoneAbsent) {
        const auto identity = PhysicalIdentityNoFollow(tombstone, detail);
        std::string tombstoneBytes;
        if (!identity.has_value() ||
            !ReadPhysicalText(tombstone, tombstoneBytes, detail)) {
            return false;
        }
        reservationIdentity = SerializePhysicalIdentity(*identity);
        state.TombstoneIsExactReservation = tombstoneBytes == reservation;
        state.TombstoneMatchesRecordedArtifact =
            state.TombstoneIsExactReservation ||
            VerifyRecordedOwnedArtifact(
                tombstone, expectedHash, expectedMode, expectedIdentity,
                detail);
    }
    const auto plan = PlanTerrainWindowsOwnedRemoval(state, detail);
    if (!plan.has_value()) {
        return false;
    }
    DWORD originalAttributes = 0u;
    std::uint32_t deleteMode = expectedMode;
    bool attributesChanged = false;
    const auto restoreAttributes = [&] {
        if (!attributesChanged || PhysicalPathAbsent(tombstone)) {
            return true;
        }
        const auto identity = PhysicalIdentityNoFollow(tombstone, detail);
        if (!identity.has_value() ||
            SerializePhysicalIdentity(*identity) != expectedIdentity) {
            return true;
        }
        if (!SetFileAttributesW(tombstone.c_str(), originalAttributes)) {
            const DWORD nativeError = GetLastError();
            detail = DurableNativeError(
                "RemoveOwned", tombstone, static_cast<int>(nativeError));
            return false;
        }
        const DWORD readback = GetFileAttributesW(tombstone.c_str());
        if (readback == INVALID_FILE_ATTRIBUTES) {
            const DWORD nativeError = GetLastError();
            detail = DurableNativeError(
                "RemoveOwned", tombstone, static_cast<int>(nativeError));
            return false;
        }
        if (readback != originalAttributes) {
            detail = DurableNativeError(
                "RemoveOwned", tombstone, ERROR_INVALID_DATA);
            return false;
        }
        attributesChanged = false;
        return true;
    };
    const bool executed = ExecuteTerrainWindowsDurableSteps(
        *plan,
        [&](const TerrainWindowsDurableStep& step,
            std::string& stepDetail) {
            switch (step.Action) {
            case TerrainWindowsDurableAction::REMOVE_RESERVATION:
                return RemoveOwnedPhysical(
                    tombstone, stepDetail, reservationIdentity,
                    reservationHash, PrivatePhysicalMode());
            case TerrainWindowsDurableAction::VERIFY_SOURCE:
                return VerifyRecordedOwnedArtifact(
                    path, expectedHash, expectedMode, expectedIdentity,
                    stepDetail);
            case TerrainWindowsDurableAction::RESERVE_TARGET:
                if (!ReserveMoveTargetWindows(
                        path, tombstone, kind, transactionId, expectedHash,
                        stepDetail)) {
                    return false;
                }
                if (!reservationFaultPoint.empty() && injectFault &&
                    injectFault(reservationFaultPoint) !=
                        TerrainReferenceFaultAction::NONE) {
                    stepDetail = std::format(
                        "injected fault {}", reservationFaultPoint);
                    return false;
                }
                return VerifyRecordedOwnedArtifact(
                    path, expectedHash, expectedMode, expectedIdentity,
                    stepDetail);
            case TerrainWindowsDurableAction::MOVE_TO_RESERVATION:
                return ReplacePhysicalFile(path, tombstone, stepDetail);
            case TerrainWindowsDurableAction::VERIFY_RESERVATION_PAYLOAD:
                if (injectFault &&
                    injectFault("MANIFEST_BEFORE_TOMBSTONE_DELETE") !=
                        TerrainReferenceFaultAction::NONE) {
                    stepDetail =
                        "injected fault MANIFEST_BEFORE_TOMBSTONE_DELETE";
                    return false;
                }
                return VerifyRecordedOwnedArtifact(
                    tombstone, expectedHash, expectedMode, expectedIdentity,
                    stepDetail);
            case TerrainWindowsDurableAction::CLEAR_READONLY: {
                originalAttributes = GetFileAttributesW(tombstone.c_str());
                if (originalAttributes == INVALID_FILE_ATTRIBUTES) {
                    const DWORD nativeError = GetLastError();
                    stepDetail = DurableNativeError(
                        "RemoveOwned", tombstone,
                        static_cast<int>(nativeError));
                    return false;
                }
                if ((originalAttributes & FILE_ATTRIBUTE_READONLY) == 0u) {
                    return true;
                }
                DWORD updated =
                    originalAttributes & ~FILE_ATTRIBUTE_READONLY;
                if (updated == 0u) {
                    updated = FILE_ATTRIBUTE_NORMAL;
                }
                deleteMode = updated;
                if (!SetFileAttributesW(tombstone.c_str(), updated)) {
                    const DWORD nativeError = GetLastError();
                    stepDetail = DurableNativeError(
                        "RemoveOwned", tombstone,
                        static_cast<int>(nativeError));
                    return false;
                }
                attributesChanged = true;
                const DWORD readback = GetFileAttributesW(tombstone.c_str());
                if (readback == INVALID_FILE_ATTRIBUTES) {
                    const DWORD nativeError = GetLastError();
                    stepDetail = DurableNativeError(
                        "RemoveOwned", tombstone,
                        static_cast<int>(nativeError));
                    return false;
                }
                if (readback != updated) {
                    stepDetail = DurableNativeError(
                        "RemoveOwned", tombstone, ERROR_INVALID_DATA);
                    return false;
                }
                return true;
            }
            case TerrainWindowsDurableAction::DELETE_RESERVATION: {
                return RemoveOwnedPhysical(
                    tombstone, stepDetail, expectedIdentity,
                    expectedHash, deleteMode);
            }
            case TerrainWindowsDurableAction::VERIFY_ABSENCE:
                if (!PhysicalPathAbsent(tombstone)) {
                    stepDetail = DurableNativeError(
                        "RemoveOwned", tombstone, ERROR_INVALID_DATA);
                    return false;
                }
                return true;
            default:
                stepDetail = "unexpected Windows remove-owned action";
                return false;
            }
        },
        detail);
    if (!executed) {
        const std::string actionDetail = detail;
        if (!restoreAttributes()) {
            return false;
        }
        detail = actionDetail;
    }
    return executed;
#else
    (void)kind;
    (void)transactionId;
    if (!tombstoneAbsent) {
        detail = std::format("unexpected POSIX tombstone: {}",
                             tombstone.generic_string());
        return false;
    }
    if (pathAbsent) {
        return true;
    }
    if (!VerifyRecordedOwnedArtifact(
            path, expectedHash, expectedMode, expectedIdentity, detail)) {
        return false;
    }
    if (!reservationFaultPoint.empty() && injectFault &&
        injectFault(reservationFaultPoint) !=
            TerrainReferenceFaultAction::NONE) {
        detail = std::format("injected fault {}", reservationFaultPoint);
        return false;
    }
    if (!VerifyRecordedOwnedArtifact(
            path, expectedHash, expectedMode, expectedIdentity, detail)) {
        return false;
    }
    if (injectFault) {
        const TerrainReferenceFaultAction deleteFault =
            injectFault("MANIFEST_BEFORE_TOMBSTONE_DELETE");
        if (deleteFault != TerrainReferenceFaultAction::NONE) {
            detail = "injected fault MANIFEST_BEFORE_TOMBSTONE_DELETE";
            return false;
        }
    }
    if (!VerifyRecordedOwnedArtifact(
            path, expectedHash, expectedMode, expectedIdentity, detail)) {
        return false;
    }
    return RemoveOwnedPhysical(
        path, detail, expectedIdentity, expectedHash, expectedMode);
#endif
}

std::optional<std::string> ValidatePublicationManifest(
    std::string_view json,
    const TerrainReferenceOptions& options,
    std::string& detail) {
    std::vector<TerrainManifestIssue> parseIssues;
    const auto parsed = ParseTerrainReferenceManifest(json, parseIssues);
    if (!parsed.has_value() || !parseIssues.empty() ||
        SerializeTerrainReferenceManifest(*parsed) != json) {
        detail = "publication manifest is not canonical strict JSON";
        return std::nullopt;
    }
    const auto validation = ValidateTerrainReferenceManifest(
        *parsed, options.Output, options);
    if (!validation.empty()) {
        detail = IssueSummary(validation.front());
        return std::nullopt;
    }
    auto part = parsed->Files.Height.ManifestRelativePath.begin();
    ++part;
    return part->generic_string();
}

struct ManifestRecoveryResult {
    bool Ok{false};
    std::string Detail;
    std::filesystem::path Evidence;
};

ManifestRecoveryResult RecoverManifestPublication(
    const std::filesystem::path& outputRoot,
    const std::filesystem::path& canonicalJournal,
    const std::filesystem::path& retiredJournal,
    const TerrainReferenceOptions& options,
    const TerrainReferenceFaultInjector& injectFault = {}) {
    ManifestRecoveryResult result;
    std::vector<std::filesystem::path> unexpectedRetired;
    std::error_code iterationError;
    for (std::filesystem::directory_iterator iterator{outputRoot, iterationError},
         end;
         !iterationError && iterator != end;
         iterator.increment(iterationError)) {
        const std::filesystem::path candidate = iterator->path();
        const std::string leaf = candidate.filename().generic_string();
        if (leaf.starts_with(".garner.reference-albedo.publish") &&
            leaf.ends_with(".retired") &&
            candidate.lexically_normal() != retiredJournal.lexically_normal() &&
            leaf != ".garner.reference-albedo.publish.lock.retired") {
            unexpectedRetired.push_back(candidate);
        }
    }
    if (iterationError) {
        result.Detail = std::format(
            "cannot inspect retired publication artifacts: {}",
            iterationError.message());
        result.Evidence = outputRoot;
        return result;
    }
    if (!unexpectedRetired.empty()) {
        std::ranges::sort(unexpectedRetired);
        result.Detail = std::format(
            "unexpected retired publication artifact: {}",
            unexpectedRetired.front().generic_string());
        result.Evidence = unexpectedRetired.front();
        return result;
    }
    std::error_code canonicalError;
    std::error_code retiredError;
    const std::filesystem::file_status canonicalStatus =
        std::filesystem::symlink_status(canonicalJournal, canonicalError);
    const std::filesystem::file_status retiredStatus =
        std::filesystem::symlink_status(retiredJournal, retiredError);
    bool canonicalAbsent =
        canonicalStatus.type() == std::filesystem::file_type::not_found &&
        (!canonicalError ||
         canonicalError == std::errc::no_such_file_or_directory);
    bool retiredAbsent =
        retiredStatus.type() == std::filesystem::file_type::not_found &&
        (!retiredError || retiredError == std::errc::no_such_file_or_directory);
#if defined(_WIN32)
    if (!canonicalAbsent && !retiredAbsent) {
        std::string canonicalBytes;
        std::string retiredBytes;
        std::string stateDetail;
        const auto canonicalParsed =
            ReadPhysicalText(canonicalJournal, canonicalBytes, stateDetail)
            ? ParseManifestJournal(canonicalBytes, stateDetail)
            : std::nullopt;
        const std::string expectedReservation = canonicalParsed.has_value()
            ? WindowsMoveReservationRecord(
                  canonicalJournal, retiredJournal, "retire-journal",
                  canonicalParsed->TransactionId,
                  Sha256Bytes(std::span{
                      reinterpret_cast<const std::uint8_t*>(canonicalBytes.data()),
                      canonicalBytes.size()}))
            : std::string{};
        const auto retiredIdentity =
            PhysicalIdentityNoFollow(retiredJournal, stateDetail);
        const auto retiredMode = PhysicalFileMode(retiredJournal, stateDetail);
        if (!canonicalParsed.has_value() ||
            canonicalParsed->JournalRetired.lexically_normal() !=
                retiredJournal.lexically_normal() ||
            !ReadPhysicalText(retiredJournal, retiredBytes, stateDetail) ||
            retiredBytes != expectedReservation ||
            !retiredIdentity.has_value() || !retiredMode.has_value() ||
            !RemoveOwnedPhysical(
                retiredJournal, stateDetail,
                SerializePhysicalIdentity(*retiredIdentity),
                Sha256Text(expectedReservation), *retiredMode)) {
            result.Detail = stateDetail.empty()
                ? "both canonical and retired publication journals exist"
                : stateDetail;
            result.Evidence = canonicalJournal;
            return result;
        }
        retiredAbsent = true;
    }
#endif
    if (canonicalAbsent && retiredAbsent) {
        result.Ok = true;
        return result;
    }
    if (!canonicalAbsent && !retiredAbsent) {
        result.Detail = "both canonical and retired publication journals exist";
        result.Evidence = canonicalJournal;
        return result;
    }
    const std::filesystem::path journalPath =
        canonicalAbsent ? retiredJournal : canonicalJournal;
    const std::filesystem::file_status journalStatus =
        canonicalAbsent ? retiredStatus : canonicalStatus;
    const std::error_code journalError =
        canonicalAbsent ? retiredError : canonicalError;
    if (journalError ||
        journalStatus.type() != std::filesystem::file_type::regular) {
        result.Detail = "publication journal is foreign or non-regular";
        result.Evidence = journalPath;
        return result;
    }
    std::string bytes;
    if (!ReadPhysicalText(journalPath, bytes, result.Detail)) {
        result.Evidence = journalPath;
        return result;
    }
    auto journal = ParseManifestJournal(bytes, result.Detail);
    if (!journal.has_value()) {
        result.Evidence = journalPath;
        return result;
    }
    auto journalIdentity = PhysicalIdentityNoFollow(journalPath, result.Detail);
    if (!journalIdentity.has_value()) {
        result.Evidence = journalPath;
        return result;
    }
    const std::filesystem::path expectedDestination =
        outputRoot / "garner.reference-albedo.json";
    const std::filesystem::path expectedTemp =
        outputRoot /
        std::format(".garner.reference-albedo.temp.{}",
                    journal->TransactionId);
    const std::filesystem::path expectedBackup =
        outputRoot /
        std::format(".garner.reference-albedo.backup.{}",
                    journal->TransactionId);
    const std::filesystem::path expectedTempReservation =
        outputRoot /
        std::format(".garner.reference-albedo.finalize.{}.temp",
                    journal->TransactionId);
    const std::filesystem::path expectedBackupReservation =
        outputRoot /
        std::format(".garner.reference-albedo.finalize.{}.backup",
                    journal->TransactionId);
    const std::filesystem::path expectedRollback =
        outputRoot /
        std::format(".garner.reference-albedo.rollback.{}",
                    journal->TransactionId);
    const std::filesystem::path expectedRollbackReservation =
        outputRoot /
        std::format(".garner.reference-albedo.finalize.{}.rollback",
                    journal->TransactionId);
    const std::filesystem::path expectedTempTombstone =
        outputRoot /
        std::format(".garner.reference-albedo.tombstone.{}.temp",
                    journal->TransactionId);
    const std::filesystem::path expectedBackupTombstone =
        outputRoot /
        std::format(".garner.reference-albedo.tombstone.{}.backup",
                    journal->TransactionId);
    const std::filesystem::path expectedRollbackTombstone =
        outputRoot /
        std::format(".garner.reference-albedo.tombstone.{}.rollback",
                    journal->TransactionId);
    const std::filesystem::path expectedDestinationTombstone =
        outputRoot /
        std::format(".garner.reference-albedo.tombstone.{}.destination",
                    journal->TransactionId);
    if (journal->Destination.lexically_normal() !=
            expectedDestination.lexically_normal() ||
        journal->Temp.lexically_normal() != expectedTemp.lexically_normal() ||
        !IsDirectOwnedChild(outputRoot, journal->Temp) ||
        journal->TempReservation.lexically_normal() !=
            expectedTempReservation.lexically_normal() ||
        journal->BackupReservation.lexically_normal() !=
            expectedBackupReservation.lexically_normal() ||
        journal->Rollback.lexically_normal() !=
            expectedRollback.lexically_normal() ||
        journal->RollbackReservation.lexically_normal() !=
            expectedRollbackReservation.lexically_normal() ||
        journal->TempTombstone.lexically_normal() !=
            expectedTempTombstone.lexically_normal() ||
        journal->BackupTombstone.lexically_normal() !=
            expectedBackupTombstone.lexically_normal() ||
        journal->RollbackTombstone.lexically_normal() !=
            expectedRollbackTombstone.lexically_normal() ||
        journal->DestinationTombstone.lexically_normal() !=
            expectedDestinationTombstone.lexically_normal() ||
        journal->JournalRetired.lexically_normal() !=
            retiredJournal.lexically_normal() ||
        !IsDirectOwnedChild(outputRoot, journal->TempTombstone) ||
        !IsDirectOwnedChild(outputRoot, journal->BackupTombstone) ||
        !IsDirectOwnedChild(outputRoot, journal->TempReservation) ||
        !IsDirectOwnedChild(outputRoot, journal->BackupReservation) ||
        !IsDirectOwnedChild(outputRoot, journal->Rollback) ||
        !IsDirectOwnedChild(outputRoot, journal->RollbackReservation) ||
        !IsDirectOwnedChild(outputRoot, journal->RollbackTombstone) ||
        !IsDirectOwnedChild(outputRoot, journal->DestinationTombstone) ||
        !IsDirectOwnedChild(outputRoot, journal->JournalRetired) ||
        (journal->PriorExists &&
         (journal->Backup.lexically_normal() !=
              expectedBackup.lexically_normal() ||
          !IsDirectOwnedChild(outputRoot, journal->Backup))) ||
        (!journal->PriorExists && !journal->Backup.empty())) {
        result.Detail = "publication journal path escapes output root";
        result.Evidence = journalPath;
        return result;
    }
    const auto recoveryFault = [&](std::string_view point) {
        const TerrainReferenceFaultAction action = injectFault
            ? injectFault(point)
            : TerrainReferenceFaultAction::NONE;
        if (action == TerrainReferenceFaultAction::NONE) {
            return false;
        }
        result.Detail = std::format("injected fault {} during recovery", point);
        result.Evidence = journal->Backup.empty()
            ? journalPath
            : journal->Backup;
        return true;
    };

    std::vector<PhysicalEntryToFinalize> recoveryEntries;
    if (!journal->TempIdentity.empty() &&
        (!PhysicalPathAbsent(journal->Temp) ||
         !PhysicalPathAbsent(journal->TempReservation))) {
        recoveryEntries.push_back(PhysicalEntryToFinalize{
            journal->Temp, journal->TempReservation,
            "finalize-manifest-temp", journal->TransactionId,
            journal->IntendedSha256, journal->TempIdentity,
            journal->IntendedMode});
    }
    if (journal->PriorExists && !journal->BackupIdentity.empty() &&
        (!PhysicalPathAbsent(journal->Backup) ||
         !PhysicalPathAbsent(journal->BackupReservation))) {
        recoveryEntries.push_back(PhysicalEntryToFinalize{
            journal->Backup, journal->BackupReservation,
            "finalize-manifest-backup", journal->TransactionId,
            journal->PriorSha256, journal->BackupIdentity,
            journal->PriorMode});
    }
    if (!recoveryEntries.empty() &&
        !SyncPhysicalDirectory(outputRoot, result.Detail, recoveryEntries)) {
        result.Evidence = journalPath;
        return result;
    }
    if (std::ranges::any_of(
            recoveryEntries, [](const PhysicalEntryToFinalize& entry) {
                return !PhysicalPathAbsent(entry.Reservation);
            })) {
        result.Detail = "finalization reservation remains after recovery";
        result.Evidence = journalPath;
        return result;
    }

    const auto destinationHash = [&]() -> std::optional<std::string> {
        if (PhysicalPathAbsent(journal->Destination)) {
            return std::nullopt;
        }
        std::string hashDetail;
        const auto hash = Sha256File(journal->Destination, hashDetail);
        if (!hash.has_value()) {
            result.Detail = hashDetail;
        }
        return hash;
    }();
    if (!result.Detail.empty()) {
        result.Evidence = journalPath;
        return result;
    }

    const bool committed = journal->Phase == "COMMITTED";
    if (committed) {
        if (!destinationHash.has_value() ||
            *destinationHash != journal->IntendedSha256) {
            result.Detail = "committed publication target does not match journal";
            result.Evidence = journalPath;
            return result;
        }
        const auto destinationMode =
            PhysicalFileMode(journal->Destination, result.Detail);
        if (!destinationMode.has_value() ||
            *destinationMode != journal->IntendedMode) {
            result.Detail =
                "committed publication target mode differs from journal";
            result.Evidence = journalPath;
            return result;
        }
        std::string manifestBytes;
        if (!ReadPhysicalText(journal->Destination, manifestBytes,
                              result.Detail) ||
            !ValidatePublicationManifest(manifestBytes, options,
                                         result.Detail).has_value()) {
            result.Evidence = journalPath;
            return result;
        }
    }
    else if (journal->PriorExists) {
        const bool destinationIsPrior = destinationHash.has_value() &&
            *destinationHash == journal->PriorSha256 &&
            VerifyHashAndMode(journal->Destination, journal->PriorSha256,
                              journal->PriorMode, result.Detail);
        if (!destinationIsPrior) {
            if (!result.Detail.empty() && destinationHash.has_value() &&
                *destinationHash == journal->PriorSha256) {
                result.Evidence = journalPath;
                return result;
            }
            if (!destinationHash.has_value() ||
                *destinationHash != journal->IntendedSha256 ||
                !VerifyRecordedOwnedArtifact(
                    journal->Backup, journal->PriorSha256,
                    journal->PriorMode, journal->BackupIdentity,
                    result.Detail)) {
                if (result.Detail.empty()) {
                    result.Detail =
                        "pre-commit target matches neither prior nor intended state";
                }
                result.Evidence = journalPath;
                return result;
            }
            const std::filesystem::path rollback = journal->Rollback;
            const std::array initialRollbackEntries{
                PhysicalEntryToFinalize{
                    rollback, journal->RollbackReservation,
                    "finalize-manifest-rollback", journal->TransactionId,
                    journal->PriorSha256, journal->RollbackIdentity,
                    journal->PriorMode}};
            if (PhysicalPathAbsent(rollback) &&
                !PhysicalPathAbsent(journal->RollbackReservation) &&
                !SyncPhysicalDirectory(
                    outputRoot, result.Detail, initialRollbackEntries)) {
                result.Evidence = journalPath;
                return result;
            }
            if (PhysicalPathAbsent(rollback) &&
                !PhysicalPathAbsent(journal->RollbackReservation)) {
                result.Detail = "invalid rollback finalization reservation";
                result.Evidence = journalPath;
                return result;
            }
            if (!PhysicalPathAbsent(rollback)) {
                if (!VerifyRecordedOwnedArtifact(
                        rollback, journal->PriorSha256, journal->PriorMode,
                        journal->RollbackIdentity, result.Detail)) {
                    result.Evidence = journalPath;
                    return result;
                }
            }
            else {
                if (!CopyExclusivePhysicalFile(
                        journal->Backup, rollback, journal->PriorMode,
                        result.Detail)) {
                    result.Evidence = journalPath;
                    return result;
                }
                const auto rollbackIdentity =
                    PhysicalIdentityNoFollow(rollback, result.Detail);
                if (!rollbackIdentity.has_value()) {
                    result.Evidence = journalPath;
                    return result;
                }
                const auto reconciledIdentity =
                    ReconcileTerrainWindowsFinalizationIdentity(
                        journal->RollbackIdentity,
                        SerializePhysicalIdentity(*rollbackIdentity),
                        result.Detail);
                if (!reconciledIdentity.has_value()) {
                    result.Evidence = rollback;
                    return result;
                }
                journal->RollbackIdentity = *reconciledIdentity;
                if (!WriteManifestJournal(*journal, journalPath,
                                          result.Detail)) {
                    result.Evidence = journalPath;
                    return result;
                }
                bytes = SerializeManifestJournal(*journal);
                journalIdentity =
                    PhysicalIdentityNoFollow(journalPath, result.Detail);
                if (!journalIdentity.has_value()) {
                    result.Evidence = journalPath;
                    return result;
                }
            }
            const std::array durableRollbackEntries{
                PhysicalEntryToFinalize{
                    rollback, journal->RollbackReservation,
                    "finalize-manifest-rollback", journal->TransactionId,
                    journal->PriorSha256, journal->RollbackIdentity,
                    journal->PriorMode}};
            if (!SyncPhysicalDirectory(
                    outputRoot, result.Detail, durableRollbackEntries)) {
                result.Evidence = journalPath;
                return result;
            }
            if (recoveryFault("MANIFEST_ROLLBACK_AFTER_MODE_STAGE_FLUSH") ||
                recoveryFault("MANIFEST_ROLLBACK_REPLACE")) {
                return result;
            }
            if (!ReplacePhysicalFile(rollback, journal->Destination,
                                     result.Detail)) {
                result.Evidence = journalPath;
                return result;
            }
            if (!SyncPhysicalDirectory(outputRoot, result.Detail)) {
                result.Evidence = journalPath;
                return result;
            }
            if (recoveryFault("MANIFEST_ROLLBACK_DURABILITY_BARRIER")) {
                return result;
            }
            if (!VerifyHashAndMode(journal->Destination,
                                   journal->PriorSha256,
                                   journal->PriorMode, result.Detail)) {
                result.Evidence = journalPath;
                return result;
            }
            if (recoveryFault("MANIFEST_ROLLBACK_VERIFY")) {
                return result;
            }
        }
    }
    else if (destinationHash.has_value()) {
        if (*destinationHash != journal->IntendedSha256 ||
            recoveryFault("MANIFEST_ROLLBACK_REPLACE") ||
            !RemoveRecordedOwnedArtifact(
                journal->Destination, journal->DestinationTombstone,
                "remove-owned-destination", journal->TransactionId,
                journal->IntendedSha256, journal->IntendedMode,
                journal->TempIdentity, injectFault,
                "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE",
                result.Detail)) {
            if (result.Detail.empty()) {
                result.Detail = "unexpected target during absent-prior recovery";
            }
            result.Evidence = journalPath;
            return result;
        }
        if (recoveryFault("MANIFEST_ROLLBACK_DURABILITY_BARRIER") ||
            recoveryFault("MANIFEST_ROLLBACK_VERIFY")) {
            return result;
        }
    }

    if (!journal->Temp.empty() &&
        (!PhysicalPathAbsent(journal->Temp) ||
         !PhysicalPathAbsent(journal->TempTombstone)) &&
        !RemoveRecordedOwnedArtifact(
            journal->Temp, journal->TempTombstone,
            "remove-owned-temp", journal->TransactionId,
            journal->IntendedSha256, journal->IntendedMode,
            journal->TempIdentity, injectFault,
            "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE",
            result.Detail)) {
        result.Evidence = journalPath;
        return result;
    }
    if (!journal->Backup.empty() &&
        (!PhysicalPathAbsent(journal->Backup) ||
         !PhysicalPathAbsent(journal->BackupTombstone)) &&
        !RemoveRecordedOwnedArtifact(
            journal->Backup, journal->BackupTombstone,
            "remove-owned-backup", journal->TransactionId,
            journal->PriorSha256, journal->PriorMode,
            journal->BackupIdentity, injectFault,
            "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE",
            result.Detail)) {
        result.Evidence = journalPath;
        return result;
    }
    if ((!PhysicalPathAbsent(journal->Rollback) ||
         !PhysicalPathAbsent(journal->RollbackTombstone)) &&
        !RemoveRecordedOwnedArtifact(
            journal->Rollback, journal->RollbackTombstone,
            "remove-owned-rollback", journal->TransactionId,
            journal->PriorSha256, journal->PriorMode,
            journal->RollbackIdentity, injectFault,
            "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE",
            result.Detail)) {
        result.Evidence = journalPath;
        return result;
    }
    std::filesystem::path cleanupJournal = journalPath;
    if (journalPath == canonicalJournal) {
        if (!RetireManifestJournal(
                *journal, canonicalJournal, retiredJournal,
                SerializePhysicalIdentity(*journalIdentity),
                injectFault, result.Detail)) {
            result.Evidence = canonicalJournal;
            return result;
        }
        cleanupJournal = retiredJournal;
    }
    std::string cleanupJournalBytes;
    if (!ReadPhysicalText(cleanupJournal, cleanupJournalBytes, result.Detail) ||
        cleanupJournalBytes != bytes ||
        !VerifyRecordedOwnedArtifact(
            cleanupJournal, Sha256Text(bytes), PrivatePhysicalMode(),
            SerializePhysicalIdentity(*journalIdentity), result.Detail) ||
        !RemoveOwnedPhysical(
            cleanupJournal, result.Detail,
            SerializePhysicalIdentity(*journalIdentity), Sha256Text(bytes),
            PrivatePhysicalMode())) {
        if (result.Detail.empty()) {
            result.Detail = "publication journal changed before cleanup";
        }
        result.Evidence = cleanupJournal;
        return result;
    }
    result.Ok = true;
    return result;
}

TerrainPublicationResult RecoverProductionManifestBeforeBuild(
    const TerrainReferenceOptions& options) {
    TerrainPublicationResult result;
    result.Status = TerrainPublicationStatus::WRITE_FAILED;
    std::ostringstream directoryError;
    if (!EnsurePhysicalDirectory(options.Output, "publication output",
                                 directoryError)) {
        result.Detail = directoryError.str();
        return result;
    }
    const std::filesystem::path lockPath =
        options.Output / ".garner.reference-albedo.publish.lock";
    const std::filesystem::path retiredLockPath =
        options.Output / ".garner.reference-albedo.publish.lock.retired";
    const std::filesystem::path journalPath =
        options.Output / ".garner.reference-albedo.publish.json";
    const std::filesystem::path retiredJournalPath =
        options.Output / ".garner.reference-albedo.publish.json.retired";
    std::string detail;
    auto lock = AcquirePublicationLock(lockPath, retiredLockPath, detail);
    if (!lock.has_value()) {
        result.Detail = detail;
        return result;
    }
    const ManifestRecoveryResult recovered =
        RecoverManifestPublication(options.Output, journalPath,
                                   retiredJournalPath, options);
    if (!recovered.Ok) {
        lock->Abandon();
        result.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
        result.Detail = recovered.Detail;
        result.RecoveryBackup = recovered.Evidence;
        return result;
    }
    if (!RetirePublicationLock(*lock, {}, detail)) {
        result.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
        result.Detail = detail;
        result.RecoveryBackup = lockPath;
        return result;
    }
    result.Status = TerrainPublicationStatus::OK;
    return result;
}

std::optional<TerrainHashedInputDto> HashProductionInput(
    const std::filesystem::path& path,
    std::string_view label,
    std::string& detail) {
    std::string hashDetail;
    const auto hash = Sha256File(path, hashDetail);
    if (!hash.has_value()) {
        detail = std::format("{} hash failed: {}", label, hashDetail);
        return std::nullopt;
    }
    return TerrainHashedInputDto{path.lexically_normal(), *hash};
}

std::optional<TerrainReferenceBuildProducts> BuildProductionTerrainProducts(
    const TerrainReferenceOptions& options,
    const std::filesystem::path& runDirectory,
    std::string& detail) {
    detail.clear();
    const auto mapInput = HashProductionInput(options.Map, "map", detail);
    if (!mapInput.has_value()) {
        return std::nullopt;
    }
    const auto databaseInput =
        HashProductionInput(options.Database, "database", detail);
    if (!databaseInput.has_value()) {
        return std::nullopt;
    }
    const auto alphaInput =
        HashProductionInput(options.AlphaAtlas, "alpha atlas", detail);
    if (!alphaInput.has_value()) {
        return std::nullopt;
    }

    MapDiagnostics diagnostics;
    auto reader = MapSectionReader::Open(options.Map, diagnostics);
    if (!reader.has_value()) {
        detail = std::format("map open failed: {}: {}",
                             ToString(diagnostics.Status), diagnostics.Detail);
        return std::nullopt;
    }
    MapRasterStats rasterStats;
    if (WriteTerrainRasters(*reader, runDirectory / "garner",
                            rasterStats, detail) != MapWriteStatus::OK) {
        detail = std::format("map raster write failed: {}", detail);
        return std::nullopt;
    }
    auto catalog = TerrainCatalog::Load(
        options.Database, options.ClientRoot, detail);
    if (!catalog.has_value()) {
        detail = std::format("terrain catalog load failed: {}", detail);
        return std::nullopt;
    }
    TerrainBakeResult bake = BakeTerrainPage(
        *reader, *catalog, options.Page, options.AlphaAtlas,
        runDirectory, options.Bake, detail);
    if (!bake.Ok) {
        detail = std::format("terrain page bake failed: {}", detail);
        return std::nullopt;
    }
    diagnostics = {};
    auto page = reader->ReadWindow(
        bake.SourceCellBounds, 1u, 1u, diagnostics);
    if (!page.has_value()) {
        detail = std::format("map page read failed: {}: {}",
                             ToString(diagnostics.Status), diagnostics.Detail);
        return std::nullopt;
    }
    TerrainPageMeshResult mesh = WriteTerrainPageMesh(
        *page, options.Page, runDirectory, options.Mesh, detail);
    if (!mesh.Ok) {
        detail = std::format("terrain page mesh failed: {}", detail);
        return std::nullopt;
    }

    std::error_code pathError;
    const std::filesystem::path canonicalClient =
        std::filesystem::canonical(options.ClientRoot, pathError);
    if (pathError) {
        detail = std::format("client root canonicalization failed: {}",
                             pathError.message());
        return std::nullopt;
    }
    TerrainManifestSourceDto source;
    source.Map = *mapInput;
    source.Database = *databaseInput;
    source.ClientRoot = options.ClientRoot.lexically_normal();
    source.AlphaAtlas = *alphaInput;
    for (const std::uint8_t textureId : bake.UsedTextureIds) {
        const auto texture = catalog->Resolve(textureId);
        if (!texture.has_value()) {
            detail = std::format("used terrain texture {} is unresolved",
                                 textureId);
            return std::nullopt;
        }
        pathError.clear();
        const std::filesystem::path canonicalTexture =
            std::filesystem::canonical(*texture, pathError);
        if (pathError || !IsContainedBy(canonicalClient, canonicalTexture)) {
            detail = std::format("used terrain texture {} escapes client root",
                                 textureId);
            return std::nullopt;
        }
        const std::filesystem::path relativeTexture =
            canonicalTexture.lexically_relative(canonicalClient);
        if (!IsNormalizedRelativePath(relativeTexture)) {
            detail = std::format("used terrain texture {} has invalid path",
                                 textureId);
            return std::nullopt;
        }
        std::string hashDetail;
        const auto hash = Sha256File(canonicalTexture, hashDetail);
        if (!hash.has_value()) {
            detail = std::format("used terrain texture {} hash failed: {}",
                                 textureId, hashDetail);
            return std::nullopt;
        }
        source.UsedTextures.push_back({
            textureId,
            (options.ClientRoot / relativeTexture).lexically_normal(),
            *hash,
        });
    }

    TerrainReferenceBuildProducts products;
    products.Bake = std::move(bake);
    products.Mesh = std::move(mesh);
    products.Source = std::move(source);
    const auto built = [&runDirectory](std::string_view leaf) {
        return TerrainBuiltFile{runDirectory / leaf, {}};
    };
    products.Files.Height = built(kTerrainReferenceLeaves[0]);
    products.Files.Block = built(kTerrainReferenceLeaves[1]);
    products.Files.Region = built(kTerrainReferenceLeaves[2]);
    products.Files.TerrainMetadata = built(kTerrainReferenceLeaves[3]);
    products.Files.Albedo = built(kTerrainReferenceLeaves[4]);
    products.Files.MeshGltf = built(kTerrainReferenceLeaves[5]);
    products.Files.MeshBin = built(kTerrainReferenceLeaves[6]);
    return products;
}

} // namespace

TerrainPublicationResult PublishTerrainReferenceManifestForTesting(
    const std::filesystem::path& destination,
    std::string_view json,
    const TerrainReferenceOptions& options,
    const TerrainReferenceFaultInjector& injectFault) {
    TerrainPublicationResult result;
    result.Status = TerrainPublicationStatus::WRITE_FAILED;
    const std::filesystem::path outputRoot = destination.parent_path();
    const std::filesystem::path expectedDestination =
        options.Output / "garner.reference-albedo.json";
    const std::string recoveryCommand =
        TerrainReferenceRecoveryCommand(options);
    if (destination.lexically_normal() != expectedDestination.lexically_normal()) {
        result.Detail = "publication destination differs from configured output";
        return result;
    }
    std::ostringstream directoryError;
    if (!EnsurePhysicalDirectory(outputRoot, "publication output",
                                 directoryError)) {
        result.Detail = directoryError.str();
        return result;
    }

    const std::filesystem::path lockPath =
        outputRoot / ".garner.reference-albedo.publish.lock";
    const std::filesystem::path retiredLockPath =
        outputRoot / ".garner.reference-albedo.publish.lock.retired";
    const std::filesystem::path journalPath =
        outputRoot / ".garner.reference-albedo.publish.json";
    const std::filesystem::path retiredJournalPath =
        outputRoot / ".garner.reference-albedo.publish.json.retired";
    std::string detail;
    auto lock = AcquirePublicationLock(lockPath, retiredLockPath, detail);
    if (!lock.has_value()) {
        result.Detail = detail;
        return result;
    }

    const ManifestRecoveryResult startupRecovery =
        RecoverManifestPublication(outputRoot, journalPath,
                                   retiredJournalPath, options, injectFault);
    if (!startupRecovery.Ok) {
        lock->Abandon();
        result.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
        result.Detail = startupRecovery.Detail;
        result.RecoveryBackup = startupRecovery.Evidence;
        result.RecoveryCommand = recoveryCommand;
        return result;
    }

    const auto runId = ValidatePublicationManifest(json, options, detail);
    if (!runId.has_value()) {
        std::string retirementDetail;
        if (!RetirePublicationLock(*lock, {}, retirementDetail)) {
            result.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
            result.Detail = retirementDetail;
            result.RecoveryBackup = lockPath;
            result.RecoveryCommand = recoveryCommand;
            return result;
        }
        result.Detail = detail;
        return result;
    }

    const std::string transactionId = NextTransactionId();
    ManifestPublicationJournal journal;
    journal.Phase = "SNAPSHOT";
    journal.TransactionId = transactionId;
    journal.Destination = destination;
    journal.Temp = outputRoot /
        std::format(".garner.reference-albedo.temp.{}", transactionId);
    journal.Backup = outputRoot /
        std::format(".garner.reference-albedo.backup.{}", transactionId);
    journal.TempReservation = outputRoot /
        std::format(".garner.reference-albedo.finalize.{}.temp", transactionId);
    journal.BackupReservation = outputRoot /
        std::format(".garner.reference-albedo.finalize.{}.backup", transactionId);
    journal.Rollback = outputRoot /
        std::format(".garner.reference-albedo.rollback.{}", transactionId);
    journal.RollbackReservation = outputRoot /
        std::format(".garner.reference-albedo.finalize.{}.rollback", transactionId);
    journal.TempTombstone = outputRoot /
        std::format(".garner.reference-albedo.tombstone.{}.temp", transactionId);
    journal.BackupTombstone = outputRoot /
        std::format(".garner.reference-albedo.tombstone.{}.backup", transactionId);
    journal.RollbackTombstone = outputRoot /
        std::format(".garner.reference-albedo.tombstone.{}.rollback", transactionId);
    journal.DestinationTombstone = outputRoot /
        std::format(".garner.reference-albedo.tombstone.{}.destination",
                    transactionId);
    journal.JournalRetired = retiredJournalPath;
    journal.IntendedSha256 = Sha256Text(json);
    journal.IntendedMode = PrivatePhysicalMode();
    journal.RunId = *runId;

    std::error_code destinationError;
    const std::filesystem::file_status destinationStatus =
        std::filesystem::symlink_status(destination, destinationError);
    if (destinationStatus.type() == std::filesystem::file_type::not_found &&
        (!destinationError ||
         destinationError == std::errc::no_such_file_or_directory)) {
        journal.PriorExists = false;
        journal.Backup.clear();
    }
    else if (!destinationError &&
             destinationStatus.type() == std::filesystem::file_type::regular) {
        journal.PriorExists = true;
        std::string hashDetail;
        const auto priorHash = Sha256File(destination, hashDetail);
        const auto priorMode = PhysicalFileMode(destination, detail);
        if (!priorHash.has_value() || !priorMode.has_value()) {
            lock->Abandon();
            result.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
            result.Detail = priorHash.has_value() ? detail : hashDetail;
            result.RecoveryBackup = destination;
            result.RecoveryCommand = recoveryCommand;
            return result;
        }
        journal.PriorSha256 = *priorHash;
        journal.PriorMode = *priorMode;
    }
    else {
        lock->Abandon();
        result.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
        result.Detail = "publication destination is a symlink or non-regular path";
        result.RecoveryBackup = destination;
        result.RecoveryCommand = recoveryCommand;
        return result;
    }

    bool journalDurable = false;
    bool committed = false;
    const auto failure = [&](std::string message,
                             TerrainReferenceFaultAction action)
        -> TerrainPublicationResult {
        TerrainPublicationResult failed;
        failed.Status = TerrainPublicationStatus::WRITE_FAILED;
        failed.Detail = std::move(message);
        if (action == TerrainReferenceFaultAction::CRASH) {
            lock->Abandon();
            if (committed) {
                failed.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
                failed.RecoveryCommand = recoveryCommand;
            }
            failed.RecoveryBackup = journalDurable ? journalPath : destination;
            return failed;
        }
        if (committed) {
            lock->Abandon();
            failed.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
            failed.RecoveryBackup = journalPath;
            failed.RecoveryCommand = recoveryCommand;
            return failed;
        }
        if (journalDurable) {
            const ManifestRecoveryResult recovered =
                RecoverManifestPublication(outputRoot, journalPath,
                                           retiredJournalPath, options,
                                           injectFault);
            if (!recovered.Ok) {
                lock->Abandon();
                failed.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
                failed.Detail += std::format("; rollback failed: {}",
                                             recovered.Detail);
                failed.RecoveryBackup = recovered.Evidence;
                failed.RecoveryCommand = recoveryCommand;
                return failed;
            }
        }
        std::string retirementDetail;
        if (!RetirePublicationLock(*lock, {}, retirementDetail)) {
            failed.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
            failed.Detail += std::format("; lock retirement failed: {}",
                                         retirementDetail);
            failed.RecoveryBackup = lockPath;
            failed.RecoveryCommand = recoveryCommand;
        }
        return failed;
    };
    const auto inject = [&](std::string_view point)
        -> std::optional<TerrainPublicationResult> {
        const TerrainReferenceFaultAction action = injectFault
            ? injectFault(point)
            : TerrainReferenceFaultAction::NONE;
        if (action == TerrainReferenceFaultAction::NONE) {
            return std::nullopt;
        }
        return failure(std::format("injected fault {}", point), action);
    };

    if (!WriteManifestJournal(journal, journalPath, detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    journalDurable = true;
    TerrainReferenceFaultAction exclusiveCreateAction =
        TerrainReferenceFaultAction::NONE;
    if (!WriteExclusiveText(
            journal.Temp, json, PrivatePhysicalMode(), detail,
            [&] {
                exclusiveCreateAction = injectFault
                    ? injectFault("MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE")
                    : TerrainReferenceFaultAction::NONE;
            })) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    const auto tempIdentity = PhysicalIdentityNoFollow(journal.Temp, detail);
    if (!tempIdentity.has_value()) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    journal.TempIdentity = SerializePhysicalIdentity(*tempIdentity);
    if (!WriteManifestJournal(journal, journalPath, detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (exclusiveCreateAction != TerrainReferenceFaultAction::NONE) {
        return failure(
            "injected fault MANIFEST_TEMP_AFTER_EXCLUSIVE_CREATE",
            exclusiveCreateAction);
    }
    std::string tempBytes;
    if (!ReadPhysicalText(journal.Temp, tempBytes, detail) ||
        !VerifyPhysicalFile(journal.Temp, json, journal.IntendedSha256,
                            PrivatePhysicalMode(), detail) ||
        !ValidatePublicationManifest(tempBytes, options, detail).has_value()) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (auto injected = inject("MANIFEST_AFTER_TEMP_FLUSH")) {
        return *injected;
    }

    if (journal.PriorExists) {
        if (!CopyExclusivePhysicalFile(destination, journal.Backup,
                                       journal.PriorMode, detail) ||
            !VerifyHashAndMode(journal.Backup, journal.PriorSha256,
                               journal.PriorMode, detail)) {
            return failure(detail, TerrainReferenceFaultAction::FAIL);
        }
        const auto backupIdentity =
            PhysicalIdentityNoFollow(journal.Backup, detail);
        if (!backupIdentity.has_value()) {
            return failure(detail, TerrainReferenceFaultAction::FAIL);
        }
        journal.BackupIdentity = SerializePhysicalIdentity(*backupIdentity);
        if (!WriteManifestJournal(journal, journalPath, detail)) {
            return failure(detail, TerrainReferenceFaultAction::FAIL);
        }
    }
    std::vector<PhysicalEntryToFinalize> publicationEntries{
        PhysicalEntryToFinalize{
            journal.Temp, journal.TempReservation, "finalize-manifest-temp",
            journal.TransactionId, journal.IntendedSha256,
            journal.TempIdentity, journal.IntendedMode}};
    if (journal.PriorExists) {
        publicationEntries.push_back(PhysicalEntryToFinalize{
            journal.Backup, journal.BackupReservation,
            "finalize-manifest-backup", journal.TransactionId,
            journal.PriorSha256, journal.BackupIdentity,
            journal.PriorMode});
    }
    if (!SyncPhysicalDirectory(outputRoot, detail, publicationEntries)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (auto injected = inject("MANIFEST_AFTER_BACKUP_FLUSH")) {
        return *injected;
    }

    journal.Phase = "PREPARED";
    if (!WriteManifestJournal(journal, journalPath, detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (auto injected =
            inject("MANIFEST_AFTER_PREPARED_JOURNAL_DURABLE")) {
        return *injected;
    }
    if (!ReplacePhysicalFile(journal.Temp, destination, detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (auto injected = inject("MANIFEST_AFTER_REPLACE")) {
        return *injected;
    }
    if (!SyncPhysicalDirectory(outputRoot, detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (auto injected =
            inject("MANIFEST_AFTER_REPLACE_DURABILITY_BARRIER")) {
        return *injected;
    }

    journal.Phase = "REPLACED";
    if (!WriteManifestJournal(journal, journalPath, detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (auto injected =
            inject("MANIFEST_AFTER_REPLACED_JOURNAL_DURABLE")) {
        return *injected;
    }
    std::string publishedBytes;
    if (!ReadPhysicalText(destination, publishedBytes, detail) ||
        !VerifyPhysicalFile(destination, json, journal.IntendedSha256,
                            PrivatePhysicalMode(), detail) ||
        !ValidatePublicationManifest(publishedBytes, options, detail).has_value()) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (auto injected = inject("MANIFEST_AFTER_READBACK_VERIFY")) {
        return *injected;
    }

    journal.Phase = "COMMITTED";
    if (!WriteManifestJournal(journal, journalPath, detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    committed = true;
    if (auto injected =
            inject("MANIFEST_AFTER_COMMITTED_JOURNAL_DURABLE")) {
        return *injected;
    }
    if (journal.PriorExists &&
        (!PhysicalPathAbsent(journal.Backup) ||
         !PhysicalPathAbsent(journal.BackupTombstone)) &&
        !RemoveRecordedOwnedArtifact(
            journal.Backup, journal.BackupTombstone,
            "remove-owned-backup", journal.TransactionId,
            journal.PriorSha256, journal.PriorMode,
            journal.BackupIdentity, injectFault,
            "MANIFEST_AFTER_TOMBSTONE_RESERVATION_DURABLE", detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    const auto journalIdentity = PhysicalIdentityNoFollow(journalPath, detail);
    if (!journalIdentity.has_value()) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (!RetireManifestJournal(
            journal, journalPath, retiredJournalPath,
            SerializePhysicalIdentity(*journalIdentity), injectFault, detail)) {
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    std::string retiredJournalBytes;
    if (!ReadPhysicalText(retiredJournalPath, retiredJournalBytes, detail) ||
        retiredJournalBytes != SerializeManifestJournal(journal) ||
        !VerifyRecordedOwnedArtifact(
            retiredJournalPath, Sha256Text(retiredJournalBytes),
            PrivatePhysicalMode(), SerializePhysicalIdentity(*journalIdentity),
            detail) ||
        !RemoveOwnedPhysical(
            retiredJournalPath, detail,
            SerializePhysicalIdentity(*journalIdentity),
            Sha256Text(retiredJournalBytes), PrivatePhysicalMode())) {
        if (detail.empty()) {
            detail = "publication journal changed before cleanup";
        }
        return failure(detail, TerrainReferenceFaultAction::FAIL);
    }
    if (!RetirePublicationLock(*lock, injectFault, detail)) {
        result.Status = TerrainPublicationStatus::RECOVERY_REQUIRED;
        result.Detail = detail;
        result.RecoveryBackup = lockPath;
        result.RecoveryCommand = recoveryCommand;
        return result;
    }
    result.Status = TerrainPublicationStatus::OK;
    result.Detail = std::format("manifestSha256={} runId={}",
                                journal.IntendedSha256, journal.RunId);
    return result;
}

int RunTerrainReference(
    const TerrainReferenceOptions& options,
    const TerrainReferenceDependencies& dependencies,
    std::ostream& output,
    std::ostream& error) {
    std::string optionDetail;
    if (!ValidateTerrainReferenceOptionsForCommand(options, optionDetail)) {
        error << optionDetail << '\n';
        return 1;
    }
    if (!dependencies.BuildProducts || !dependencies.AtomicPublish) {
        error << "terrain-reference dependencies are incomplete\n";
        return 1;
    }

    if (dependencies.RecoverBeforeBuild) {
        const TerrainPublicationResult recovery =
            dependencies.RecoverBeforeBuild(options);
        if (recovery.Status != TerrainPublicationStatus::OK) {
            error << (recovery.Status ==
                              TerrainPublicationStatus::RECOVERY_REQUIRED
                          ? "RECOVERY_REQUIRED "
                          : "WRITE_FAILED ")
                  << recovery.Detail;
            if (!recovery.RecoveryBackup.empty()) {
                error << " evidence="
                      << recovery.RecoveryBackup.generic_string();
            }
            error << '\n';
            return 1;
        }
    }

    const auto runDirectory = CreateUniqueRunDirectory(options.Output, error);
    if (!runDirectory.has_value()) {
        return 1;
    }
    std::string detail;
    const auto products =
        dependencies.BuildProducts(options, *runDirectory, detail);
    if (!products.has_value()) {
        error << "terrain-reference build failed: " << detail << '\n';
        return 1;
    }
    if (!products->Bake.Ok) {
        error << "BakeTerrainPage result is invalid\n";
        return 1;
    }
    if (!products->Mesh.Ok) {
        error << "WriteTerrainPageMesh result is invalid\n";
        return 1;
    }

    const std::uint64_t expectedX =
        static_cast<std::uint64_t>(options.Page.X) * options.Bake.CellsPerPage;
    const std::uint64_t expectedY =
        static_cast<std::uint64_t>(options.Page.Y) * options.Bake.CellsPerPage;
    const std::uint64_t expectedPixels =
        static_cast<std::uint64_t>(options.Bake.CellsPerPage) *
        options.Bake.PixelsPerCell;
    const bool supportedMeshStep =
        products->Mesh.Step == 4u || products->Mesh.Step == 2u ||
        products->Mesh.Step == 1u;
    const std::uint64_t meshVerticesPerAxis = supportedMeshStep
        ? static_cast<std::uint64_t>(options.Bake.CellsPerPage) /
                products->Mesh.Step + 1u
        : 0u;
    const std::uint64_t expectedMeshSamples =
        meshVerticesPerAxis * meshVerticesPerAxis;
    if (expectedX > UINT32_MAX || expectedY > UINT32_MAX ||
        expectedPixels > UINT32_MAX ||
        products->Bake.SourceCellBounds.X != expectedX ||
        products->Bake.SourceCellBounds.Y != expectedY ||
        products->Bake.SourceCellBounds.Width != options.Bake.CellsPerPage ||
        products->Bake.SourceCellBounds.Height != options.Bake.CellsPerPage ||
        products->Bake.AlgorithmVersion != "legacy-fixed-pipeline-v1" ||
        products->Bake.PngPath !=
            *runDirectory / kTerrainReferenceLeaves[4] ||
        products->Mesh.GltfPath !=
            *runDirectory / kTerrainReferenceLeaves[5] ||
        products->Mesh.BinPath !=
            *runDirectory / kTerrainReferenceLeaves[6] ||
        !supportedMeshStep ||
        products->Mesh.Error.Samples != expectedMeshSamples ||
        products->Mesh.ActorWorldXcm != static_cast<double>(expectedX) * 100.0 ||
        products->Mesh.ActorWorldYcm != -static_cast<double>(expectedY) * 100.0) {
        error << "terrain-reference build metadata does not match request\n";
        return 1;
    }

    TerrainReferenceManifestDto manifest;
    manifest.SchemaVersion = 1u;
    manifest.AlgorithmVersion = products->Bake.AlgorithmVersion;
    manifest.Source = products->Source;
    manifest.Page.Id = options.Page;
    manifest.Page.SourceCellBounds = products->Bake.SourceCellBounds;
    manifest.Page.PixelsPerCell = options.Bake.PixelsPerCell;
    manifest.Page.PixelWidth = static_cast<std::uint32_t>(expectedPixels);
    manifest.Page.PixelHeight = static_cast<std::uint32_t>(expectedPixels);
    manifest.Page.Ambient = {1.0, 1.0, 1.0};
    manifest.Page.DwTColor = 0u;
    manifest.RequiredPresentRect = options.RequiredPresent;
    manifest.UsedTextureIds = products->Bake.UsedTextureIds;
    manifest.SectionPresence = {
        products->Bake.SectionOriginX,
        products->Bake.SectionOriginY,
        products->Bake.SectionGridWidth,
        products->Bake.SectionGridHeight,
        products->Bake.SectionPresenceMask,
    };

    std::array<const TerrainBuiltFile*, 7> builtFiles{
        &products->Files.Height,
        &products->Files.Block,
        &products->Files.Region,
        &products->Files.TerrainMetadata,
        &products->Files.Albedo,
        &products->Files.MeshGltf,
        &products->Files.MeshBin,
    };
    std::array<TerrainOutputFile*, 7> outputFiles{
        &manifest.Files.Height,
        &manifest.Files.Block,
        &manifest.Files.Region,
        &manifest.Files.TerrainMetadata,
        &manifest.Files.Albedo,
        &manifest.Files.MeshGltf,
        &manifest.Files.MeshBin,
    };
    std::array<std::filesystem::path, 7> physicalPaths{};
    std::array<PhysicalFileIdentity, 7> physicalIdentities{};
    std::set<PhysicalFileIdentity> distinctPhysicalIdentities;
    std::uint64_t totalOutputBytes = 0u;
    for (std::size_t index = 0u; index < builtFiles.size(); ++index) {
        const std::filesystem::path expected =
            *runDirectory / kTerrainReferenceLeaves[index];
        physicalPaths[index] = expected;
        if (builtFiles[index]->RunLeafPath.lexically_normal() !=
            expected.lexically_normal()) {
            error << "terrain-reference product path mismatch: "
                  << kTerrainReferenceLeaves[index] << '\n';
            return 1;
        }
        std::error_code filesystemError;
        const std::filesystem::file_status physical =
            std::filesystem::symlink_status(expected, filesystemError);
        if (filesystemError ||
            physical.type() != std::filesystem::file_type::regular) {
            error << "terrain-reference product is not a physical regular file: "
                  << kTerrainReferenceLeaves[index] << '\n';
            return 1;
        }
        std::string identityDetail;
        const auto identity = PhysicalIdentityNoFollow(expected, identityDetail);
        if (!identity.has_value() ||
            !distinctPhysicalIdentities.insert(*identity).second) {
            error << "terrain-reference products must have seven distinct "
                     "physical identities\n";
            return 1;
        }
        physicalIdentities[index] = *identity;
        const std::uintmax_t size =
            std::filesystem::file_size(expected, filesystemError);
        if (filesystemError || size == 0u || size > UINT64_MAX ||
            static_cast<std::uint64_t>(size) > UINT64_MAX - totalOutputBytes) {
            error << "terrain-reference product has invalid size: "
                  << kTerrainReferenceLeaves[index] << '\n';
            return 1;
        }
        std::string hashDetail;
        const auto hash = Sha256File(expected, hashDetail);
        if (!hash.has_value()) {
            error << "terrain-reference product hash failed: "
                  << kTerrainReferenceLeaves[index] << ": "
                  << hashDetail << '\n';
            return 1;
        }
        totalOutputBytes += static_cast<std::uint64_t>(size);
        *outputFiles[index] = {
            std::filesystem::path{"runs"} / runDirectory->filename() /
                kTerrainReferenceLeaves[index],
            *hash,
            static_cast<std::uint64_t>(size),
        };
    }
    std::error_code iteratorError;
    std::size_t entries = 0u;
    for (std::filesystem::directory_iterator iterator{*runDirectory,
                                                       iteratorError};
         !iteratorError && iterator != std::filesystem::directory_iterator{};
         iterator.increment(iteratorError)) {
        ++entries;
    }
    if (iteratorError || entries != kTerrainReferenceLeaves.size()) {
        error << "terrain-reference run must contain exactly seven products\n";
        return 1;
    }

    const auto durabilize = dependencies.DurabilizeRunProducts
        ? dependencies.DurabilizeRunProducts
        : DurabilizeRunProductsPortable;
    detail.clear();
    if (!durabilize(physicalPaths, detail)) {
        error << "terrain-reference run durability failed: " << detail << '\n';
        return 1;
    }
    distinctPhysicalIdentities.clear();
    for (std::size_t index = 0u; index < physicalPaths.size(); ++index) {
        std::error_code filesystemError;
        const std::filesystem::file_status physical =
            std::filesystem::symlink_status(physicalPaths[index], filesystemError);
        const std::uintmax_t size = filesystemError
            ? 0u
            : std::filesystem::file_size(physicalPaths[index], filesystemError);
        std::string hashDetail;
        const auto hash = filesystemError
            ? std::optional<std::string>{}
            : Sha256File(physicalPaths[index], hashDetail);
        std::string identityDetail;
        const auto identity = filesystemError
            ? std::optional<PhysicalFileIdentity>{}
            : PhysicalIdentityNoFollow(physicalPaths[index], identityDetail);
        if (filesystemError ||
            physical.type() != std::filesystem::file_type::regular ||
            !hash.has_value() || size != outputFiles[index]->SizeBytes ||
            *hash != outputFiles[index]->Sha256 || !identity.has_value() ||
            *identity != physicalIdentities[index] ||
            !distinctPhysicalIdentities.insert(*identity).second) {
            error << "terrain-reference product changed during durability: "
                  << kTerrainReferenceLeaves[index] << '\n';
            if (!identity.has_value() ||
                (identity.has_value() &&
                 *identity != physicalIdentities[index]) ||
                distinctPhysicalIdentities.size() != index + 1u) {
                error << "terrain-reference physical identity changed during "
                         "durability\n";
            }
            return 1;
        }
    }
    iteratorError.clear();
    entries = 0u;
    for (std::filesystem::directory_iterator iterator{*runDirectory,
                                                       iteratorError};
         !iteratorError && iterator != std::filesystem::directory_iterator{};
         iterator.increment(iteratorError)) {
        ++entries;
    }
    if (iteratorError || entries != kTerrainReferenceLeaves.size()) {
        error << "terrain-reference run changed during durability\n";
        return 1;
    }
    if (products->Bake.OutputBytes != manifest.Files.Albedo.SizeBytes) {
        error << "BakeTerrainPage output size does not match disk\n";
        return 1;
    }

    manifest.Metrics = {
        static_cast<std::uint64_t>(products->Bake.PeakRssBytes),
        static_cast<std::uint64_t>(products->Bake.PeakTextureCacheBytes),
        static_cast<std::uint64_t>(products->Bake.PeakRgbaRowBytes),
        manifest.Files.Albedo.SizeBytes,
        totalOutputBytes,
        products->Mesh.Error.MaxAbsCm,
        products->Mesh.Error.RmsCm,
        products->Mesh.Error.SharedBoundaryMaxCm,
        static_cast<std::uint64_t>(products->Bake.AbsentSections),
        static_cast<std::uint64_t>(products->Bake.UnresolvedLayers),
    };

    const std::string json = SerializeTerrainReferenceManifest(manifest);
    std::vector<TerrainManifestIssue> parseIssues;
    const auto parsed = ParseTerrainReferenceManifest(json, parseIssues);
    if (!parsed.has_value() || !parseIssues.empty() ||
        SerializeTerrainReferenceManifest(*parsed) != json) {
        error << "terrain-reference manifest roundtrip failed\n";
        return 1;
    }
    const std::vector<TerrainManifestIssue> validationIssues =
        ValidateTerrainReferenceManifest(*parsed, options.Output, options);
    if (!validationIssues.empty()) {
        error << IssueSummary(validationIssues.front()) << '\n';
        return 1;
    }

    const std::filesystem::path destination =
        options.Output / "garner.reference-albedo.json";
    const TerrainPublicationResult publication =
        dependencies.AtomicPublish(destination, json);
    if (publication.Status != TerrainPublicationStatus::OK) {
        error << (publication.Status ==
                          TerrainPublicationStatus::RECOVERY_REQUIRED
                      ? "RECOVERY_REQUIRED "
                      : "WRITE_FAILED ")
              << publication.Detail;
        if (!publication.RecoveryBackup.empty()) {
            error << " backup=" << publication.RecoveryBackup.generic_string();
        }
        if (!publication.RecoveryCommand.empty()) {
            error << " command=" << publication.RecoveryCommand;
        }
        error << '\n';
        return 1;
    }
    const auto jsonBytes = std::span{
        reinterpret_cast<const std::uint8_t*>(json.data()), json.size()};
    output << "manifest=" << destination.generic_string()
           << " sha256=" << Sha256Bytes(jsonBytes)
           << " runId=" << runDirectory->filename().generic_string() << '\n';
    return 0;
}

int RunTerrainReference(
    const TerrainReferenceOptions& options,
    std::ostream& output,
    std::ostream& error) {
    return RunTerrainReference(
        options, MakeProductionTerrainReferenceDependencies(), output, error);
}

TerrainReferenceDependencies MakeProductionTerrainReferenceDependencies() {
    struct Context {
        std::optional<TerrainReferenceOptions> Options;
    };
    const auto context = std::make_shared<Context>();
    TerrainReferenceDependencies dependencies;
    dependencies.RecoverBeforeBuild =
        [context](const TerrainReferenceOptions& options) {
            TerrainPublicationResult result =
                RecoverProductionManifestBeforeBuild(options);
            if (result.Status == TerrainPublicationStatus::OK) {
                context->Options = options;
            }
            return result;
        };
    dependencies.BuildProducts =
        [context](const TerrainReferenceOptions& options,
                  const std::filesystem::path& runDirectory,
                  std::string& detail) {
            context->Options = options;
            return BuildProductionTerrainProducts(
                options, runDirectory, detail);
        };
    dependencies.DurabilizeRunProducts = DurabilizeRunProductsPortable;
    dependencies.AtomicPublish =
        [context](const std::filesystem::path& destination,
                  std::string_view json) {
            if (!context->Options.has_value()) {
                return TerrainPublicationResult{
                    TerrainPublicationStatus::WRITE_FAILED,
                    "production publication has no validated options", {}, {}};
            }
            return PublishTerrainReferenceManifestForTesting(
                destination, json, *context->Options, {});
        };
    return dependencies;
}

std::optional<int> TryRunTerrainReferenceSubcommand(
    std::span<const std::string_view> arguments,
    const TerrainReferenceDependencies& dependencies,
    std::ostream& output,
    std::ostream& error) {
    if (arguments.size() < 2u || arguments[1] != "terrain-reference") {
        return std::nullopt;
    }
    std::string detail;
    const auto options = ParseTerrainReferenceArguments(arguments.subspan(2u), detail);
    if (!options.has_value()) {
        error << kUsage << '\n' << detail << '\n';
        return 2;
    }
    return RunTerrainReference(*options, dependencies, output, error);
}

std::optional<int> TryRunTerrainReferenceSubcommand(
    std::span<const std::string_view> arguments,
    std::ostream& output,
    std::ostream& error) {
    return TryRunTerrainReferenceSubcommand(
        arguments, MakeProductionTerrainReferenceDependencies(), output, error);
}

} // namespace Corsairs::Tools::AssetConverter
