#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <format>

namespace Corsairs::Tools::AssetConverter {

void JsonWriter::Separate() {
    if (_needComma) {
        _out += ',';
    }
    _needComma = true;
}

namespace {

// Длина корректной последовательности UTF-8, начинающейся в `pos`, либо 0,
// если последовательность некорректна. Проверяются и диапазоны продолжений:
// иначе сюда пролезут переусложнённые формы (overlong) и суррогаты, которые
// формально многобайтовые, но в UTF-8 запрещены.
std::size_t Utf8SequenceLength(std::string_view text, std::size_t pos) {
    const auto at = [&](std::size_t i) -> unsigned char {
        return static_cast<unsigned char>(text[i]);
    };
    const auto isCont = [&](std::size_t i, unsigned char lo, unsigned char hi) {
        return i < text.size() && at(i) >= lo && at(i) <= hi;
    };

    const unsigned char first = at(pos);

    if (first < 0x80) {
        return 1;
    }
    if (first >= 0xC2 && first <= 0xDF) {
        return isCont(pos + 1, 0x80, 0xBF) ? 2 : 0;
    }
    if (first == 0xE0) {
        return isCont(pos + 1, 0xA0, 0xBF) && isCont(pos + 2, 0x80, 0xBF) ? 3 : 0;
    }
    if (first >= 0xE1 && first <= 0xEC) {
        return isCont(pos + 1, 0x80, 0xBF) && isCont(pos + 2, 0x80, 0xBF) ? 3 : 0;
    }
    if (first == 0xED) {
        // 0xA0..0xBF здесь дали бы суррогат U+D800..U+DFFF.
        return isCont(pos + 1, 0x80, 0x9F) && isCont(pos + 2, 0x80, 0xBF) ? 3 : 0;
    }
    if (first >= 0xEE && first <= 0xEF) {
        return isCont(pos + 1, 0x80, 0xBF) && isCont(pos + 2, 0x80, 0xBF) ? 3 : 0;
    }
    if (first == 0xF0) {
        return isCont(pos + 1, 0x90, 0xBF) && isCont(pos + 2, 0x80, 0xBF) &&
                       isCont(pos + 3, 0x80, 0xBF) ? 4 : 0;
    }
    if (first >= 0xF1 && first <= 0xF3) {
        return isCont(pos + 1, 0x80, 0xBF) && isCont(pos + 2, 0x80, 0xBF) &&
                       isCont(pos + 3, 0x80, 0xBF) ? 4 : 0;
    }
    if (first == 0xF4) {
        // Выше U+10FFFF кодовых точек не существует.
        return isCont(pos + 1, 0x80, 0x8F) && isCont(pos + 2, 0x80, 0xBF) &&
                       isCont(pos + 3, 0x80, 0xBF) ? 4 : 0;
    }
    return 0;
}

} // namespace

void JsonWriter::WriteEscaped(std::string_view value) {
    _out += '"';

    // Байты приходят из исходных файлов как есть, а там имена материалов и
    // текстур встречаются в GBK — например `反击.tga`. JSON обязан быть в
    // UTF-8, поэтому такие байты нельзя переносить дословно: получается файл,
    // который строгий разборщик отвергает целиком. Некорректные
    // последовательности заменяются на U+FFFD.
    //
    // Перекодирование GBK -> UTF-8 сохранило бы имена, но потребовало бы либо
    // таблицы кодировки, либо платформенных вызовов; на функциональность
    // конвейера имя материала не влияет — текстуры разрешаются отдельным полем.
    for (std::size_t i = 0; i < value.size();) {
        const char c = value[i];
        switch (c) {
        case '"':  _out += "\\\""; ++i; continue;
        case '\\': _out += "\\\\"; ++i; continue;
        case '\n': _out += "\\n";  ++i; continue;
        case '\r': _out += "\\r";  ++i; continue;
        case '\t': _out += "\\t";  ++i; continue;
        default: break;
        }

        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20) {
            _out += std::format("\\u{:04x}", byte);
            ++i;
            continue;
        }

        const std::size_t length = Utf8SequenceLength(value, i);
        if (length == 0) {
            _out += "\\ufffd";
            ++i;
            continue;
        }

        _out.append(value, i, length);
        i += length;
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

void JsonWriter::Null() {
    Separate();
    _out += "null";
}

} // namespace Corsairs::Tools::AssetConverter
