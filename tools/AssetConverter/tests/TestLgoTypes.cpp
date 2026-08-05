#include "Corsairs/Tools/AssetConverter/LgoTypes.h"

#include "TestHarness.h"

#include <cstdint>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

CORSAIRS_TEST(LgoTypes_StructSizesMatchOnDiskLayout) {
    REQUIRE_EQ(sizeof(AC::ColorValue4f), 16u);
    REQUIRE_EQ(sizeof(AC::Material), 68u);
    REQUIRE_EQ(sizeof(AC::RenderStateAtom), 12u);
    REQUIRE_EQ(sizeof(AC::TexInfo), 208u);
    REQUIRE_EQ(sizeof(AC::MtlTexInfo), 1004u);
    REQUIRE_EQ(sizeof(AC::MeshInfoHeader), 128u);
    REQUIRE_EQ(sizeof(AC::SubsetInfo), 16u);
    REQUIRE_EQ(sizeof(AC::BlendInfo), 20u);
    REQUIRE_EQ(sizeof(AC::GeomObjHeader), 116u);
}

CORSAIRS_TEST(LgoTypes_NamedConstantsMatchSizeof) {
    REQUIRE_EQ(AC::kGeomObjHeaderSize, sizeof(AC::GeomObjHeader));
    REQUIRE_EQ(AC::kMtlTexInfoSize, sizeof(AC::MtlTexInfo));
    REQUIRE_EQ(AC::kMeshInfoHeaderSize, sizeof(AC::MeshInfoHeader));
}

CORSAIRS_TEST(LgoTypes_KnownVersions) {
    REQUIRE(AC::IsKnownVersion(0x0000u));
    REQUIRE(AC::IsKnownVersion(0x1000u));
    REQUIRE(AC::IsKnownVersion(0x1004u));
    REQUIRE(AC::IsKnownVersion(0x1005u));
    REQUIRE(!AC::IsKnownVersion(0x1006u));
    REQUIRE(!AC::IsKnownVersion(0xDEADBEEFu));
}

} // namespace
