#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

// Последовательное чтение POD-значений из буфера с проверкой границ. Любая
// попытка прочитать за пределами буфера возвращает false и НЕ сдвигает курсор,
// поэтому вызывающий код может корректно сообщить об усечённом файле.
//
// Файлы MindPower3D записаны на x86 (little-endian); целевые платформы тоже
// little-endian, поэтому memcpy в POD корректен без перестановки байт.
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> data)
        : _data(data) {
    }

    [[nodiscard]] std::size_t Offset() const {
        return _offset;
    }

    [[nodiscard]] std::size_t Size() const {
        return _data.size();
    }

    [[nodiscard]] std::size_t Remaining() const {
        return _data.size() - _offset;
    }

    [[nodiscard]] bool CanRead(std::size_t bytes) const {
        return Remaining() >= bytes;
    }

    template <typename T>
    [[nodiscard]] bool Read(T& out) {
        static_assert(std::is_trivially_copyable_v<T>, "BinaryReader::Read требует POD-тип");
        if (!CanRead(sizeof(T))) {
            return false;
        }
        std::memcpy(&out, _data.data() + _offset, sizeof(T));
        _offset += sizeof(T);
        return true;
    }

    template <typename T>
    [[nodiscard]] bool ReadArray(T* out, std::size_t count) {
        static_assert(std::is_trivially_copyable_v<T>, "BinaryReader::ReadArray требует POD-тип");
        if (count == 0) {
            return true;
        }
        const std::size_t bytes = sizeof(T) * count;
        if (bytes / sizeof(T) != count) {
            return false;
        }
        if (!CanRead(bytes)) {
            return false;
        }
        std::memcpy(out, _data.data() + _offset, bytes);
        _offset += bytes;
        return true;
    }

    [[nodiscard]] bool Skip(std::size_t bytes) {
        if (!CanRead(bytes)) {
            return false;
        }
        _offset += bytes;
        return true;
    }

    [[nodiscard]] bool Seek(std::size_t offset) {
        if (offset > _data.size()) {
            return false;
        }
        _offset = offset;
        return true;
    }

private:
    std::span<const std::uint8_t> _data;
    std::size_t _offset{0};
};

// Читает файл целиком. std::nullopt — файл не открылся или не читается.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> ReadWholeFile(
    const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        return std::nullopt;
    }

    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (size > 0 && !stream.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return std::nullopt;
    }
    return buffer;
}

} // namespace Corsairs::Tools::AssetConverter
