#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include "TestHarness.h"

#include <cstdint>
#include <vector>

namespace {

using Corsairs::Tools::AssetConverter::BinaryReader;

CORSAIRS_TEST(BinaryReader_ReadsUint32LittleEndian) {
    const std::vector<std::uint8_t> data{0x04, 0x10, 0x00, 0x00};
    BinaryReader reader{data};

    std::uint32_t value = 0;
    REQUIRE(reader.Read(value));
    REQUIRE_EQ(value, 0x1004u);
    REQUIRE_EQ(reader.Offset(), 4u);
    REQUIRE_EQ(reader.Remaining(), 0u);
}

CORSAIRS_TEST(BinaryReader_RefusesReadPastEnd) {
    const std::vector<std::uint8_t> data{0x01, 0x02};
    BinaryReader reader{data};

    std::uint32_t value = 0;
    REQUIRE(!reader.Read(value));
    REQUIRE_EQ(reader.Offset(), 0u);
}

CORSAIRS_TEST(BinaryReader_ReadArrayAdvancesByElementCount) {
    const std::vector<std::uint8_t> data{
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
    };
    BinaryReader reader{data};

    std::uint32_t values[3]{};
    REQUIRE(reader.ReadArray(values, 3));
    REQUIRE_EQ(values[0], 1u);
    REQUIRE_EQ(values[2], 3u);
    REQUIRE_EQ(reader.Offset(), 12u);
}

CORSAIRS_TEST(BinaryReader_SkipAndSeek) {
    const std::vector<std::uint8_t> data(16, 0);
    BinaryReader reader{data};

    REQUIRE(reader.Skip(8));
    REQUIRE_EQ(reader.Offset(), 8u);
    REQUIRE(!reader.Skip(9));
    REQUIRE_EQ(reader.Offset(), 8u);
    REQUIRE(reader.Seek(2));
    REQUIRE_EQ(reader.Offset(), 2u);
    REQUIRE(!reader.Seek(17));
}

} // namespace
