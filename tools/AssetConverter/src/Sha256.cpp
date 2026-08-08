#include "Corsairs/Tools/AssetConverter/Sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <string_view>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

class Sha256Context {
public:
    void Update(std::span<const std::uint8_t> bytes) {
        _totalBytes += bytes.size();
        while (!bytes.empty()) {
            const std::size_t copied = std::min(bytes.size(), _block.size() - _blockSize);
            std::copy_n(bytes.begin(), copied, _block.begin() +
                        static_cast<std::ptrdiff_t>(_blockSize));
            _blockSize += copied;
            bytes = bytes.subspan(copied);
            if (_blockSize == _block.size()) {
                Transform(_block);
                _blockSize = 0;
            }
        }
    }

    [[nodiscard]] std::string FinishHex() {
        const std::uint64_t bitLength = _totalBytes * 8u;
        _block[_blockSize++] = 0x80;
        if (_blockSize > 56) {
            std::fill(_block.begin() + static_cast<std::ptrdiff_t>(_blockSize),
                      _block.end(), 0);
            Transform(_block);
            _blockSize = 0;
        }
        std::fill(_block.begin() + static_cast<std::ptrdiff_t>(_blockSize),
                  _block.begin() + 56, 0);
        for (std::size_t i = 0; i < 8; ++i) {
            _block[56 + i] = static_cast<std::uint8_t>(bitLength >> (56 - i * 8));
        }
        Transform(_block);

        constexpr std::string_view digits = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (std::uint32_t word : _state) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                result.push_back(digits[(word >> shift) & 0x0Fu]);
            }
        }
        return result;
    }

private:
    void Transform(std::span<const std::uint8_t, 64> block) {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t offset = i * 4;
            schedule[i] = (static_cast<std::uint32_t>(block[offset]) << 24) |
                          (static_cast<std::uint32_t>(block[offset + 1]) << 16) |
                          (static_cast<std::uint32_t>(block[offset + 2]) << 8) |
                          static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t i = 16; i < schedule.size(); ++i) {
            const std::uint32_t s0 = std::rotr(schedule[i - 15], 7) ^
                                     std::rotr(schedule[i - 15], 18) ^
                                     (schedule[i - 15] >> 3);
            const std::uint32_t s1 = std::rotr(schedule[i - 2], 17) ^
                                     std::rotr(schedule[i - 2], 19) ^
                                     (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
        }

        std::uint32_t a = _state[0];
        std::uint32_t b = _state[1];
        std::uint32_t c = _state[2];
        std::uint32_t d = _state[3];
        std::uint32_t e = _state[4];
        std::uint32_t f = _state[5];
        std::uint32_t g = _state[6];
        std::uint32_t h = _state[7];

        for (std::size_t i = 0; i < schedule.size(); ++i) {
            const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                                       std::rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sum1 + choice + kRoundConstants[i] + schedule[i];
            const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                                       std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        _state[0] += a;
        _state[1] += b;
        _state[2] += c;
        _state[3] += d;
        _state[4] += e;
        _state[5] += f;
        _state[6] += g;
        _state[7] += h;
    }

    std::array<std::uint32_t, 8> _state{
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };
    std::array<std::uint8_t, 64> _block{};
    std::uint64_t _totalBytes{0};
    std::size_t _blockSize{0};
};

} // namespace

std::string Sha256Bytes(std::span<const std::uint8_t> bytes) {
    Sha256Context context;
    context.Update(bytes);
    return context.FinishHex();
}

std::optional<std::string> Sha256File(
    const std::filesystem::path& path, std::string& detail) {
    detail.clear();
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        detail = "не удалось открыть файл для SHA-256: " + PathToUtf8(path);
        return std::nullopt;
    }

    Sha256Context context;
    std::array<std::uint8_t, 64u * 1024u> buffer{};
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count > 0) {
            context.Update(std::span<const std::uint8_t>{buffer}.first(
                static_cast<std::size_t>(count)));
        }
    }
    if (!stream.eof()) {
        detail = "не удалось прочитать файл для SHA-256: " + PathToUtf8(path);
        return std::nullopt;
    }
    return context.FinishHex();
}

} // namespace Corsairs::Tools::AssetConverter
