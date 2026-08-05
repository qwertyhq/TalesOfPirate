#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include "TestHarness.h"

#include <cstdint>
#include <string>

namespace {

using Corsairs::Tools::AssetConverter::JsonWriter;

CORSAIRS_TEST(JsonWriter_EmitsFlatObject) {
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("version");
    writer.Value("2.0");
    writer.Key("count");
    writer.Value(static_cast<std::int64_t>(3));
    writer.EndObject();

    REQUIRE_EQ(writer.Str(), std::string{R"({"version":"2.0","count":3})"});
}

CORSAIRS_TEST(JsonWriter_EmitsNestedArrays) {
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("items");
    writer.BeginArray();
    writer.BeginObject();
    writer.Key("a");
    writer.Value(static_cast<std::int64_t>(1));
    writer.EndObject();
    writer.BeginObject();
    writer.Key("a");
    writer.Value(static_cast<std::int64_t>(2));
    writer.EndObject();
    writer.EndArray();
    writer.EndObject();

    REQUIRE_EQ(writer.Str(), std::string{R"({"items":[{"a":1},{"a":2}]})"});
}

CORSAIRS_TEST(JsonWriter_EscapesStrings) {
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("path");
    writer.Value(R"(a\b"c)");
    writer.EndObject();

    REQUIRE_EQ(writer.Str(), std::string{R"({"path":"a\\b\"c"})"});
}

CORSAIRS_TEST(JsonWriter_WritesFiniteDoublesWithoutExponentLoss) {
    JsonWriter writer;
    writer.BeginArray();
    writer.Value(0.5);
    writer.Value(-1.25);
    writer.EndArray();

    REQUIRE_EQ(writer.Str(), std::string{"[0.5,-1.25]"});
}

CORSAIRS_TEST(JsonWriter_KeepsValidUtf8Verbatim) {
    // Кириллица и иероглиф в корректном UTF-8 должны пройти без изменений.
    JsonWriter writer;
    writer.Value(std::string_view{"мачта\xe5\x8f\x8d"});

    REQUIRE_EQ(writer.Str(), std::string{"\"мачта\xe5\x8f\x8d\""});
}

CORSAIRS_TEST(JsonWriter_ReplacesInvalidUtf8WithReplacementChar) {
    // `\xb7\xb4` — начало имени `反击.tga` в GBK. В UTF-8 такая пара
    // некорректна, и дословный перенос делал весь .gltf неразбираемым.
    JsonWriter writer;
    writer.Value(std::string_view{"\xb7\xb4.tga"});

    REQUIRE_EQ(writer.Str(), std::string{"\"\\ufffd\\ufffd.tga\""});
}

CORSAIRS_TEST(JsonWriter_RejectsOverlongAndSurrogateSequences) {
    // 0xC0 0x80 — переусложнённая запись нуля; 0xED 0xA0 0x80 — суррогат
    // U+D800. Обе формы многобайтовые, но в UTF-8 запрещены.
    JsonWriter overlong;
    overlong.Value(std::string_view{"\xc0\x80"});
    REQUIRE_EQ(overlong.Str(), std::string{"\"\\ufffd\\ufffd\""});

    JsonWriter surrogate;
    surrogate.Value(std::string_view{"\xed\xa0\x80"});
    REQUIRE_EQ(surrogate.Str(), std::string{"\"\\ufffd\\ufffd\\ufffd\""});
}

CORSAIRS_TEST(JsonWriter_DoesNotReadPastEndOnTruncatedSequence) {
    // Обрезанная последовательность в конце строки не должна уводить чтение
    // за границу буфера.
    JsonWriter writer;
    writer.Value(std::string_view{"a\xe5\x8f"});

    REQUIRE_EQ(writer.Str(), std::string{"\"a\\ufffd\\ufffd\""});
}

} // namespace
