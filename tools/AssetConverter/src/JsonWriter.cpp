#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <format>

namespace Corsairs::Tools::AssetConverter {

void JsonWriter::Separate() {
    if (_needComma) {
        _out += ',';
    }
    _needComma = true;
}

void JsonWriter::WriteEscaped(std::string_view value) {
    _out += '"';
    for (char c : value) {
        switch (c) {
        case '"':  _out += "\\\""; break;
        case '\\': _out += "\\\\"; break;
        case '\n': _out += "\\n";  break;
        case '\r': _out += "\\r";  break;
        case '\t': _out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                _out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            }
            else {
                _out += c;
            }
        }
    }
    _out += '"';
}

void JsonWriter::BeginObject() {
    Separate();
    _out += '{';
    _needComma = false;
}

void JsonWriter::EndObject() {
    _out += '}';
    _needComma = true;
}

void JsonWriter::BeginArray() {
    Separate();
    _out += '[';
    _needComma = false;
}

void JsonWriter::EndArray() {
    _out += ']';
    _needComma = true;
}

void JsonWriter::Key(std::string_view key) {
    Separate();
    WriteEscaped(key);
    _out += ':';
    _needComma = false;
}

void JsonWriter::Value(std::string_view value) {
    Separate();
    WriteEscaped(value);
}

void JsonWriter::Value(const char* value) {
    Value(std::string_view{value});
}

void JsonWriter::Value(double value) {
    Separate();
    // {} для double даёт кратчайшее представление, восстанавливающее исходное
    // значение при обратном разборе, и не переходит в экспоненциальную форму
    // без необходимости.
    _out += std::format("{}", value);
}

void JsonWriter::Value(std::int64_t value) {
    Separate();
    _out += std::format("{}", value);
}

void JsonWriter::Value(bool value) {
    Separate();
    _out += value ? "true" : "false";
}

} // namespace Corsairs::Tools::AssetConverter
