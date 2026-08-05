#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/LmoParser.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path ModelFile(const char* relative) {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "model" / relative;
}

CORSAIRS_TEST(LmoParser_ParsesSceneModel) {
    const auto bytes = AC::ReadWholeFile(ModelFile("scene/nml-bd114.lmo"));
    REQUIRE(bytes.has_value());
    REQUIRE_EQ(bytes->size(), 29356u);

    AC::LgoDiagnostics diag;
    const auto model = AC::ParseLmo(*bytes, diag);
    REQUIRE(model.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::OK));

    // Оглавление: геометрия (addr=32, size=28924) и helper (addr=28956, size=400).
    REQUIRE_EQ(model->Version, 0x1004u);
    REQUIRE_EQ(model->Objects.size(), 1u);
    REQUIRE(model->Objects[0].Mesh.Header.VertexNum > 0u);
    REQUIRE(!model->Objects[0].Mesh.Positions.empty());
}

CORSAIRS_TEST(LmoParser_HelperEntryFillsModelHelper) {
    const auto bytes = AC::ReadWholeFile(ModelFile("scene/nml-bd114.lmo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto model = AC::ParseLmo(*bytes, diag);
    REQUIRE(model.has_value());

    // Запись типа HELPER заполняет общий helper модели, а не объекта.
    REQUIRE(model->Helper.Type != 0u);
}

CORSAIRS_TEST(LmoParser_RejectsEntryPastEndOfFile) {
    // version + objNum + одна запись, указывающая за пределы файла.
    std::vector<std::uint8_t> bytes(20, 0);
    bytes[0] = 0x04;
    bytes[1] = 0x10;              // version 0x1004
    bytes[4] = 0x01;              // objNum = 1
    bytes[8] = 0x01;              // Type = GEOMETRY
    bytes[12] = 0x10;             // Addr = 16
    bytes[16] = 0xFF;
    bytes[17] = 0xFF;             // Size = 65535 — заведомо больше файла

    AC::LgoDiagnostics diag;
    const auto model = AC::ParseLmo(bytes, diag);
    REQUIRE(!model.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::BLOCK_SIZES_INCONSISTENT));
}

CORSAIRS_TEST(LmoParser_RejectsUnknownEntryType) {
    std::vector<std::uint8_t> bytes(20, 0);
    bytes[0] = 0x04;
    bytes[1] = 0x10;
    bytes[4] = 0x01;              // objNum = 1
    bytes[8] = 0x09;              // Type = 9 — не GEOMETRY и не HELPER
    bytes[12] = 0x14;             // Addr = 20
    bytes[16] = 0x00;             // Size = 0

    AC::LgoDiagnostics diag;
    const auto model = AC::ParseLmo(bytes, diag);
    REQUIRE(!model.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::LgoStatus::BLOCK_SIZES_INCONSISTENT));
}

} // namespace
