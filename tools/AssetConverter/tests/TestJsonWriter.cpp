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

} // namespace
