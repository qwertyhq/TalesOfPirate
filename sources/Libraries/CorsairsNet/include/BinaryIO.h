#pragma once

// BinaryIO  inline- /  (big-endian).
//  F# BinaryIO   Corsairs.Platform.Network.
//     BE, float   LE (  F# BinaryPrimitives).

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <version>

namespace Corsairs::Net::Detail {

// Перестановка байт. std::byteswap появился только в C++23, а заголовок
// подключается и из UE-модулей, которые собираются под C++20, — поэтому при
// его отсутствии используется ручная реализация. Раньше здесь были
// MSVC-интринсики _byteswap_* из <intrin.h>, недоступные вне Windows.
template <typename T>
[[nodiscard]] constexpr T ByteSwap(T value) noexcept {
#if defined(__cpp_lib_byteswap) && __cpp_lib_byteswap >= 202110L
    return std::byteswap(value);
#else
    static_assert(std::is_integral_v<T>, "ByteSwap работает только с целыми типами");
    if constexpr (sizeof(T) == 2) {
        const auto v = static_cast<std::uint16_t>(value);
        return static_cast<T>(static_cast<std::uint16_t>((v << 8) | (v >> 8)));
    }
    else if constexpr (sizeof(T) == 4) {
        const auto v = static_cast<std::uint32_t>(value);
        return static_cast<T>(((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
                              ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24));
    }
    else {
        static_assert(sizeof(T) == 2 || sizeof(T) == 4, "поддерживаются 2 и 4 байта");
    }
#endif
}

} // namespace Corsairs::Net::Detail

namespace Corsairs::Net {

//  Write (big-endian) 

inline void writeUInt8(uint8_t* dst, uint8_t v) {
    dst[0] = v;
}

inline void writeInt8(uint8_t* dst, int8_t v) {
    dst[0] = static_cast<uint8_t>(v);
}

inline void writeUInt16(uint8_t* dst, uint16_t v) {
    uint16_t be = Detail::ByteSwap(v);
    std::memcpy(dst, &be, 2);
}

inline void WriteInt64(uint8_t* dst, int16_t v) {
    writeUInt16(dst, static_cast<uint16_t>(v));
}

inline void writeUInt32(uint8_t* dst, uint32_t v) {
    uint32_t be = Detail::ByteSwap(v);
    std::memcpy(dst, &be, 4);
}

inline void WriteInt64(uint8_t* dst, int32_t v) {
    writeUInt32(dst, static_cast<uint32_t>(v));
}

inline void writeInt64(uint8_t* dst, int64_t v) {
    //   F#: high word first, low word second (BE)
    uint64_t uv = static_cast<uint64_t>(v);
    writeUInt32(dst, static_cast<uint32_t>(uv >> 32));
    writeUInt32(dst + 4, static_cast<uint32_t>(uv & 0xFFFFFFFF));
}

inline void writeUInt64(uint8_t* dst, uint64_t v) {
    writeInt64(dst, static_cast<int64_t>(v));
}

// Float32  LE ( F# BinaryPrimitives.WriteSingleLittleEndian)
inline void writeFloat32(uint8_t* dst, float v) {
    std::memcpy(dst, &v, 4);
}

//  Read (big-endian) 

inline uint8_t readUInt8(const uint8_t* src) {
    return src[0];
}

inline int8_t readInt8(const uint8_t* src) {
    return static_cast<int8_t>(src[0]);
}

inline uint16_t readUInt16(const uint8_t* src) {
    uint16_t be;
    std::memcpy(&be, src, 2);
    return Detail::ByteSwap(be);
}

inline int16_t readInt16(const uint8_t* src) {
    return static_cast<int16_t>(readUInt16(src));
}

inline uint32_t readUInt32(const uint8_t* src) {
    uint32_t be;
    std::memcpy(&be, src, 4);
    return Detail::ByteSwap(be);
}

inline int32_t readInt32(const uint8_t* src) {
    return static_cast<int32_t>(readUInt32(src));
}

inline int64_t readInt64(const uint8_t* src) {
    //   F#: high word first, low word second
    uint64_t high = static_cast<uint64_t>(readUInt32(src));
    uint64_t low  = static_cast<uint64_t>(readUInt32(src + 4));
    return static_cast<int64_t>((high << 32) | low);
}

inline uint64_t readUInt64(const uint8_t* src) {
    return static_cast<uint64_t>(readInt64(src));
}

// Float32  LE
inline float readFloat32(const uint8_t* src) {
    float v;
    std::memcpy(&v, src, 4);
    return v;
}

} // namespace Corsairs::Net
