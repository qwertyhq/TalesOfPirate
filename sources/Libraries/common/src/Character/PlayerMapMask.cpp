#include "Character/PlayerMapMask.h"

#include "algo.h"

#include <algorithm>
#include <cstring>

namespace Corsairs::Common::Character {

namespace {

constexpr std::size_t BitsByteCount(std::int32_t cellsX, std::int32_t cellsY) {
    return static_cast<std::size_t>((cellsX * cellsY) / 8 + 1);
}

} // namespace

void PlayerMapMask::AddMap(std::string_view name, std::int32_t width, std::int32_t height) {
    MapBitmap map;
    map.Name = std::string(name);
    map.Width = width;
    map.Height = height;
    map.CellsX = width / CellSize + 1;
    map.CellsY = height / CellSize + 1;
    map.Bits.assign(BitsByteCount(map.CellsX, map.CellsY), std::uint8_t{0});
    _maps.push_back(std::move(map));
}

const PlayerMapMask::MapBitmap* PlayerMapMask::Find(std::string_view name) const {
    auto it = std::ranges::find_if(_maps, [&](const MapBitmap& m) { return m.Name == name; });
    return it == _maps.end() ? nullptr : &*it;
}

PlayerMapMask::MapBitmap* PlayerMapMask::Find(std::string_view name) {
    auto it = std::ranges::find_if(_maps, [&](const MapBitmap& m) { return m.Name == name; });
    return it == _maps.end() ? nullptr : &*it;
}

bool PlayerMapMask::LoadBase64(std::string_view mapName, std::string_view base64Data) {
    auto* map = Find(mapName);
    if (!map) {
        return false;
    }

    if (!base64Data.empty() && base64Data.front() == '0') {
        std::ranges::fill(map->Bits, std::uint8_t{0});
        return true;
    }
    if (base64Data.empty()) {
        return false;
    }

    const auto decoded = Corsairs::Util::Base64Decode(base64Data);
    const std::size_t bitsLen = map->Bits.size();

    if (decoded.size() == bitsLen) {
        std::memcpy(map->Bits.data(), decoded.data(), bitsLen);
        return true;
    }
    if (decoded.size() == LegacyHeaderSize + bitsLen) {
        std::memcpy(map->Bits.data(), decoded.data() + LegacyHeaderSize, bitsLen);
        return true;
    }
    return false;
}

std::string PlayerMapMask::SaveBase64(std::string_view mapName) const {
    const auto* map = Find(mapName);
    if (!map || map->Bits.empty()) {
        return {};
    }

    return Corsairs::Util::Base64Encode(std::span<const std::uint8_t>(map->Bits));
}

std::span<const std::uint8_t> PlayerMapMask::GetBits(std::string_view mapName) const {
    const auto* map = Find(mapName);
    if (!map) {
        return {};
    }
    return map->Bits;
}

std::vector<std::uint8_t> PlayerMapMask::SerializeLegacyWire(std::string_view mapName) const {
    const auto* map = Find(mapName);
    if (!map) {
        return {};
    }

    // Legacy header layout (тот же, что у клиентского sMask):
    //   char pszName[32]; int lenx; int leny; long lpos; long llen;
    // На MSVC x64 long = 4 байта, итого 32 + 4 + 4 + 4 + 4 = 48.
    constexpr std::size_t kHeaderSize = LegacyHeaderSize;
    constexpr std::size_t kNameLen = 32;

    std::vector<std::uint8_t> buf(kHeaderSize + map->Bits.size(), std::uint8_t{0});

    const auto nameCopyLen = (std::min)(map->Name.size(), kNameLen - 1);
    std::memcpy(buf.data(), map->Name.data(), nameCopyLen);

    auto writeI32 = [&](std::size_t offset, std::int32_t value) {
        std::memcpy(buf.data() + offset, &value, sizeof(value));
    };
    writeI32(kNameLen,            map->CellsX);
    writeI32(kNameLen + 4,        map->CellsY);
    writeI32(kNameLen + 8,        0);                                                // lpos — не используется клиентом
    writeI32(kNameLen + 12,       static_cast<std::int32_t>(kHeaderSize + map->Bits.size())); // llen

    std::memcpy(buf.data() + kHeaderSize, map->Bits.data(), map->Bits.size());
    return buf;
}

float PlayerMapMask::GetOpenScalePercent(std::string_view mapName) const {
    const auto* map = Find(mapName);
    if (!map || map->CellsX == 0 || map->CellsY == 0) {
        return 0.0f;
    }

    std::int32_t openCells = 0;
    const auto total = map->CellsX * map->CellsY;
    for (std::int32_t i = 0; i < total; ++i) {
        const auto byteIdx = static_cast<std::size_t>(i) / 8;
        const auto bitIdx = static_cast<std::size_t>(i) % 8;
        if (map->Bits[byteIdx] & (std::uint8_t{1} << bitIdx)) {
            ++openCells;
        }
    }
    return (static_cast<float>(openCells) / static_cast<float>(total)) * 100.0f;
}

bool PlayerMapMask::Update(std::string_view mapName, std::int32_t worldX, std::int32_t worldY,
                           std::int32_t radiusCells) {
    auto* map = Find(mapName);
    if (!map) {
        return false;
    }
    if (radiusCells < 0) {
        radiusCells = 0;
    }

    const auto cx = worldX / CellSize;
    const auto cy = worldY / CellSize;

    bool changed = false;
    for (std::int32_t dy = -radiusCells; dy <= radiusCells; ++dy) {
        for (std::int32_t dx = -radiusCells; dx <= radiusCells; ++dx) {
            const auto nx = cx + dx;
            const auto ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= map->CellsX || ny >= map->CellsY) {
                continue;
            }
            const auto bitPos = static_cast<std::size_t>(ny) * map->CellsX + nx;
            const auto byteIdx = bitPos / 8;
            const auto bitIdx = bitPos % 8;
            const auto mask = static_cast<std::uint8_t>(std::uint8_t{1} << bitIdx);
            if ((map->Bits[byteIdx] & mask) == 0) {
                map->Bits[byteIdx] |= mask;
                changed = true;
            }
        }
    }
    return changed;
}

} // namespace Corsairs::Common::Character
