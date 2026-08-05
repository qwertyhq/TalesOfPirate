#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/MapParser.h"
#include "Corsairs/Tools/AssetConverter/MapWriter.h"
#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path MapPath(const char* name) {
    return std::filesystem::path{CORSAIRS_REPO_ROOT} / "Client" / "map" / name;
}

std::filesystem::path OutDir() {
    return std::filesystem::temp_directory_path() / "corsairs-map-tests";
}

CORSAIRS_TEST(MapParser_ParsesTerrainHeader) {
    const auto bytes = AC::ReadWholeFile(MapPath("garner.map"));
    REQUIRE(bytes.has_value());

    AC::MapDiagnostics diag;
    const auto terrain = AC::ParseMap(*bytes, diag);
    REQUIRE(terrain.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::MapStatus::OK));

    REQUIRE_EQ(terrain->Header.MapFlag, AC::kMapFlagCurrent);
    REQUIRE_EQ(terrain->Header.Width, 4096);
    REQUIRE_EQ(terrain->Header.Height, 4096);
    REQUIRE_EQ(terrain->Header.SectionWidth, 8);
    REQUIRE_EQ(terrain->Header.SectionHeight, 8);

    // 4096/8 x 4096/8 = 512x512 секций.
    REQUIRE_EQ(terrain->TotalSections, 262144u);
    REQUIRE_EQ(terrain->Tiles.size(), 4096u * 4096u);
}

CORSAIRS_TEST(MapParser_PresentSectionsFitInBody) {
    const auto bytes = AC::ReadWholeFile(MapPath("garner.map"));
    REQUIRE(bytes.has_value());

    AC::MapDiagnostics diag;
    const auto terrain = AC::ParseMap(*bytes, diag);
    REQUIRE(terrain.has_value());

    const std::size_t prefix = 20u + terrain->TotalSections * 4u;
    const std::size_t body = bytes->size() - prefix;
    const std::size_t sectionBytes = 8u * 8u * 15u;

    // Тело кратно размеру секции — независимое подтверждение раскладки.
    REQUIRE_EQ(body % sectionBytes, 0u);

    // Адресованных секций не больше, чем блоков в теле. Строгое равенство
    // здесь НЕ выполняется: у garner.map в конце тела лежат 8 блоков, на
    // которые не ссылается ни одна запись таблицы, — осиротевшие данные
    // удалённых секций, которые движок не уплотнял.
    REQUIRE(terrain->PresentSections <= body / sectionBytes);
    REQUIRE(terrain->PresentSections > 0u);
    REQUIRE(terrain->PresentSections < terrain->TotalSections);
    REQUIRE_EQ(terrain->PresentSections, 51808u);
}

CORSAIRS_TEST(MapParser_TerrainCarriesWalkabilityData) {
    const auto bytes = AC::ReadWholeFile(MapPath("garner.map"));
    REQUIRE(bytes.has_value());

    AC::MapDiagnostics diag;
    const auto terrain = AC::ParseMap(*bytes, diag);
    REQUIRE(terrain.has_value());

    std::size_t blocked = 0;
    std::size_t nonZeroHeight = 0;
    for (const AC::MapTile& tile : terrain->Tiles) {
        if (tile.Block[0] != 0 || tile.Block[1] != 0 ||
            tile.Block[2] != 0 || tile.Block[3] != 0) {
            ++blocked;
        }
        if (tile.Height != 0) {
            ++nonZeroHeight;
        }
    }

    // На реальной карте обязаны быть и непроходимые тайлы, и рельеф.
    REQUIRE(blocked > 0u);
    REQUIRE(nonZeroHeight > 0u);
}

CORSAIRS_TEST(MapParser_RejectsBadMagic) {
    std::vector<std::uint8_t> bytes(64, 0);
    bytes[0] = 0xFF;
    bytes[1] = 0xFF;

    AC::MapDiagnostics diag;
    const auto terrain = AC::ParseMap(bytes, diag);
    REQUIRE(!terrain.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::MapStatus::BAD_MAGIC));
}

CORSAIRS_TEST(SceneObjParser_ParsesRealSceneFile) {
    const auto bytes = AC::ReadWholeFile(MapPath("garner.obj"));
    REQUIRE(bytes.has_value());

    AC::SceneObjDiagnostics diag;
    const auto scene = AC::ParseSceneObj(*bytes, diag);
    REQUIRE(scene.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::SceneObjStatus::OK));

    REQUIRE_EQ(scene->Header.Version, 600);
    REQUIRE_EQ(scene->Header.SectionCntX, 512);
    REQUIRE_EQ(scene->Header.SectionCntY, 512);
    REQUIRE_EQ(scene->Header.SectionObjNum, 25);
    REQUIRE(scene->Objects.size() > 0u);
}

CORSAIRS_TEST(SceneObjParser_HeaderFileSizeMatchesActual) {
    const auto bytes = AC::ReadWholeFile(MapPath("garner.obj"));
    REQUIRE(bytes.has_value());

    AC::SceneObjDiagnostics diag;
    const auto scene = AC::ParseSceneObj(*bytes, diag);
    REQUIRE(scene.has_value());

    // Заголовок объявляет размер файла — независимая проверка раскладки.
    REQUIRE_EQ(static_cast<std::size_t>(scene->Header.FileSize), bytes->size());
}

CORSAIRS_TEST(SceneObjParser_RejectsBadMagic) {
    std::vector<std::uint8_t> bytes(64, 0);

    AC::SceneObjDiagnostics diag;
    const auto scene = AC::ParseSceneObj(bytes, diag);
    REQUIRE(!scene.has_value());
    REQUIRE_EQ(static_cast<std::uint32_t>(diag.Status),
               static_cast<std::uint32_t>(AC::SceneObjStatus::BAD_MAGIC));
}

CORSAIRS_TEST(MapWriter_WritesTerrainFiles) {
    const auto bytes = AC::ReadWholeFile(MapPath("garner.map"));
    REQUIRE(bytes.has_value());

    AC::MapDiagnostics diag;
    const auto terrain = AC::ParseMap(*bytes, diag);
    REQUIRE(terrain.has_value());

    std::filesystem::create_directories(OutDir());
    const std::filesystem::path base = OutDir() / "garner";

    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::WriteTerrain(*terrain, base, detail)),
               static_cast<std::uint32_t>(AC::MapWriteStatus::OK));

    const std::size_t tiles = 4096u * 4096u;
    REQUIRE(std::filesystem::exists(OutDir() / "garner.height.r16"));
    REQUIRE_EQ(std::filesystem::file_size(OutDir() / "garner.height.r16"), tiles * 2u);
    REQUIRE_EQ(std::filesystem::file_size(OutDir() / "garner.block.raw"), tiles * 4u);
    REQUIRE_EQ(std::filesystem::file_size(OutDir() / "garner.region.raw"), tiles * 2u);
    REQUIRE(std::filesystem::exists(OutDir() / "garner.terrain.json"));
}

} // namespace
