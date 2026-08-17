#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/LmoParser.h"

#include "TestHarness.h"

#include <algorithm>
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

// Собирает объект с заданными id, родителем и переносом. Остальное неважно:
// ResolveModelMatrices смотрит только на Id, ParentId и MatLocal.
AC::LgoGeomObj MakeObj(std::uint32_t id, std::uint32_t parentId,
                       float tx, float ty, float tz) {
    AC::LgoGeomObj obj;
    obj.Header.Id = id;
    obj.Header.ParentId = parentId;
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::copy_n(identity, 16, obj.Header.MatLocal);
    obj.Header.MatLocal[12] = tx;
    obj.Header.MatLocal[13] = ty;
    obj.Header.MatLocal[14] = tz;
    std::copy_n(obj.Header.MatLocal, 16, obj.MatModel);
    return obj;
}

constexpr std::uint32_t kNoParent = 0xFFFFFFFFu;

CORSAIRS_TEST(ResolveModelMatrices_RootKeepsOwnMatrix) {
    std::vector<AC::LgoGeomObj> objects{MakeObj(0, kNoParent, 1.0f, 2.0f, 3.0f)};
    AC::ResolveModelMatrices(objects);

    REQUIRE_EQ(objects[0].MatModel[12], 1.0f);
    REQUIRE_EQ(objects[0].MatModel[13], 2.0f);
    REQUIRE_EQ(objects[0].MatModel[14], 3.0f);
}

CORSAIRS_TEST(ResolveModelMatrices_ChildAccumulatesParentTranslation) {
    // Родитель сдвинут на (10,0,0), ребёнок относительно него — на (0,5,0).
    // В пространстве модели ребёнок обязан оказаться в (10,5,0).
    std::vector<AC::LgoGeomObj> objects{
        MakeObj(1, kNoParent, 10.0f, 0.0f, 0.0f),
        MakeObj(2, 1, 0.0f, 5.0f, 0.0f),
    };
    AC::ResolveModelMatrices(objects);

    REQUIRE_EQ(objects[1].MatModel[12], 10.0f);
    REQUIRE_EQ(objects[1].MatModel[13], 5.0f);
    REQUIRE_EQ(objects[1].MatModel[14], 0.0f);
}

CORSAIRS_TEST(ResolveModelMatrices_ComposesThroughGrandparent) {
    // Порядок в файле обратный порядку зависимостей: внук идёт первым.
    // Свёртка обязана подняться по цепочке независимо от порядка записей.
    std::vector<AC::LgoGeomObj> objects{
        MakeObj(3, 2, 0.0f, 0.0f, 1.0f),
        MakeObj(2, 1, 0.0f, 5.0f, 0.0f),
        MakeObj(1, kNoParent, 10.0f, 0.0f, 0.0f),
    };
    AC::ResolveModelMatrices(objects);

    REQUIRE_EQ(objects[0].MatModel[12], 10.0f);
    REQUIRE_EQ(objects[0].MatModel[13], 5.0f);
    REQUIRE_EQ(objects[0].MatModel[14], 1.0f);
}

CORSAIRS_TEST(ResolveModelMatrices_BreaksCycleInsteadOfHanging) {
    // Взаимные ссылки в битых данных не должны зацикливать конвертацию.
    std::vector<AC::LgoGeomObj> objects{
        MakeObj(1, 2, 1.0f, 0.0f, 0.0f),
        MakeObj(2, 1, 0.0f, 1.0f, 0.0f),
    };
    AC::ResolveModelMatrices(objects);

    // Достаточно того, что вызов завершился; конкретные значения при цикле
    // не определены, но матрицы обязаны остаться конечными.
    REQUIRE(objects[0].MatModel[12] == objects[0].MatModel[12]);
    REQUIRE(objects[1].MatModel[13] == objects[1].MatModel[13]);
}

CORSAIRS_TEST(ResolveModelMatrices_MissingParentLeavesObjectAsRoot) {
    std::vector<AC::LgoGeomObj> objects{MakeObj(5, 99, 7.0f, 0.0f, 0.0f)};
    AC::ResolveModelMatrices(objects);

    REQUIRE_EQ(objects[0].MatModel[12], 7.0f);
}

CORSAIRS_TEST(LmoParser_FillsModelMatrixFromFile) {
    // by-bd001 — Argent Palace: четыре корневых объекта, каждый со своим
    // сдвигом. Именно эти сдвиги терялись, пока MatLocal никуда не шла.
    const auto bytes = AC::ReadWholeFile(ModelFile("scene/by-bd001.lmo"));
    REQUIRE(bytes.has_value());

    AC::LgoDiagnostics diag;
    const auto model = AC::ParseLmo(*bytes, diag);
    REQUIRE(model.has_value());
    REQUIRE_EQ(model->Objects.size(), 4u);

    // Ни один объект не стоит в начале координат — иначе части здания
    // схлопнулись бы в одну точку.
    for (const auto& obj : model->Objects) {
        const bool atOrigin = obj.MatModel[12] == 0.0f &&
                              obj.MatModel[13] == 0.0f &&
                              obj.MatModel[14] == 0.0f;
        REQUIRE(!atOrigin);
    }

    // Первый объект: перенос (-1.24, -3.08, 0) с точностью до сотых.
    REQUIRE(model->Objects[0].MatModel[12] < -1.2f);
    REQUIRE(model->Objects[0].MatModel[12] > -1.3f);
    REQUIRE(model->Objects[0].MatModel[13] < -3.0f);
    REQUIRE(model->Objects[0].MatModel[13] > -3.1f);
}

} // namespace
