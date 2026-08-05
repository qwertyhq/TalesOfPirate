#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Corsairs::Tools::AssetConverter {

// Минимальный потоковый генератор JSON. Достаточен для glTF: сам расставляет
// запятые между элементами и экранирует строки. Не проверяет корректность
// вложенности — вызывающий код обязан парно закрывать объекты и массивы.
class JsonWriter {
public:
    void BeginObject();
    void EndObject();
    void BeginArray();
    void EndArray();

    void Key(std::string_view key);

    void Value(std::string_view value);
    // Обязательная перегрузка: без неё `Value("текст")` уходит в Value(bool),
    // потому что const char* -> bool это стандартное преобразование, а
    // const char* -> string_view требует пользовательского конструктора и
    // проигрывает при разрешении перегрузки.
    void Value(const char* value);
    void Value(double value);
    void Value(std::int64_t value);
    void Value(bool value);

    [[nodiscard]] const std::string& Str() const {
        return _out;
    }

private:
    void Separate();
    void WriteEscaped(std::string_view value);

    std::string _out;
    bool _needComma{false};
};

} // namespace Corsairs::Tools::AssetConverter
