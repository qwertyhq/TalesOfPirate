#include "Corsairs/Tools/AssetConverter/StreamingPngWriter.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>

#include <zlib.h>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::size_t kIdatBufferBytes = 64u * 1024u;
constexpr std::array<std::uint8_t, 8> kPngSignature{
    0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
};

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::array<std::uint8_t, 4> BigEndian32(std::uint32_t value) {
    return {
        static_cast<std::uint8_t>(value >> 24),
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value),
    };
}

bool MultiplyOverflows(std::size_t left, std::size_t right) {
    return left != 0 && right > std::numeric_limits<std::size_t>::max() / left;
}

} // namespace

class StreamingPngWriter::Impl {
public:
    Impl(std::filesystem::path path, std::uint32_t width, std::uint32_t height,
         std::size_t rowBytes)
        : _path(std::move(path)),
          _width(width),
          _height(height),
          _rowBytes(rowBytes) {
    }

    ~Impl() {
        if (!_finished) {
            CleanupIncomplete();
        }
        else {
            EndZlib();
        }
    }

    [[nodiscard]] bool Initialize(std::string& detail) {
        detail.clear();

        std::error_code ec;
        if (!_path.parent_path().empty()) {
            std::filesystem::create_directories(_path.parent_path(), ec);
            if (ec) {
                detail = "не удалось создать каталог PNG: " + ec.message();
                return false;
            }
        }

        _stream.open(_path, std::ios::binary | std::ios::trunc);
        if (!_stream) {
            detail = "не удалось открыть PNG: " + PathToUtf8(_path);
            return false;
        }

        if (deflateInit(&_zstream, Z_DEFAULT_COMPRESSION) != Z_OK) {
            detail = "zlib не инициализировал поток PNG";
            CleanupIncomplete();
            return false;
        }
        _zlibInitialized = true;
        ResetOutputBuffer();

        if (!WriteBytes(std::span{kPngSignature})) {
            return Fail(detail, "не удалось записать сигнатуру PNG");
        }

        std::array<std::uint8_t, 13> ihdr{};
        const auto widthBytes = BigEndian32(_width);
        const auto heightBytes = BigEndian32(_height);
        std::copy(widthBytes.begin(), widthBytes.end(), ihdr.begin());
        std::copy(heightBytes.begin(), heightBytes.end(), ihdr.begin() + 4);
        ihdr[8] = 8;
        ihdr[9] = 6;
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        if (!WriteChunk("IHDR", ihdr)) {
            return Fail(detail, "не удалось записать IHDR PNG");
        }
        return true;
    }

    [[nodiscard]] bool WriteRgbaRow(
        std::span<const std::uint8_t> row, std::string& detail) {
        detail.clear();
        if (_failed) {
            detail = "поток PNG уже завершился ошибкой";
            return false;
        }
        if (_finished) {
            detail = "PNG уже завершён";
            return false;
        }
        if (_rowsWritten >= _height) {
            return Fail(detail, "получена лишняя строка PNG");
        }
        if (row.size() != _rowBytes) {
            detail = "длина строки RGBA не совпадает с шириной PNG";
            return false;
        }

        const std::array<std::uint8_t, 1> filter{0};
        if (!Feed(filter, Z_NO_FLUSH, detail)) {
            return false;
        }
        if (!Feed(row, Z_NO_FLUSH, detail)) {
            return false;
        }

        ++_rowsWritten;
        _peakRgbaRowBytes = std::max(_peakRgbaRowBytes, row.size());
        return true;
    }

    [[nodiscard]] bool Finish(std::string& detail) {
        detail.clear();
        if (_failed) {
            detail = "поток PNG уже завершился ошибкой";
            return false;
        }
        if (_finished) {
            detail = "PNG уже завершён";
            return false;
        }
        if (_rowsWritten != _height) {
            return Fail(detail, "PNG нельзя завершить до записи всех строк");
        }

        int result = Z_OK;
        do {
            result = deflate(&_zstream, Z_FINISH);
            if (result != Z_OK && result != Z_STREAM_END) {
                return Fail(detail, "zlib не завершил поток PNG");
            }
            if (_zstream.avail_out == 0) {
                if (!FlushFullIdat(detail)) {
                    return false;
                }
            }
        } while (result != Z_STREAM_END);

        const std::size_t finalBytes = kIdatBufferBytes - _zstream.avail_out;
        if (finalBytes > 0 && !WriteChunk(
                "IDAT", std::span<const std::uint8_t>{_compressed}.first(finalBytes))) {
            return Fail(detail, "не удалось записать финальный IDAT PNG");
        }

        EndZlib();
        if (!WriteChunk("IEND", {})) {
            return Fail(detail, "не удалось записать IEND PNG");
        }
        _stream.flush();
        if (!_stream) {
            return Fail(detail, "не удалось завершить запись PNG");
        }
        _stream.close();
        if (_stream.fail()) {
            return Fail(detail, "не удалось закрыть PNG");
        }

        _finished = true;
        return true;
    }

    [[nodiscard]] std::size_t PeakRgbaRowBytes() const noexcept {
        return _peakRgbaRowBytes;
    }

private:
    [[nodiscard]] bool Feed(
        std::span<const std::uint8_t> input, int flush, std::string& detail) {
        while (!input.empty()) {
            const std::size_t inputBytes = std::min<std::size_t>(
                input.size(), std::numeric_limits<uInt>::max());
            _zstream.next_in = const_cast<Bytef*>(
                reinterpret_cast<const Bytef*>(input.data()));
            _zstream.avail_in = static_cast<uInt>(inputBytes);

            while (_zstream.avail_in > 0) {
                const int result = deflate(&_zstream, flush);
                if (result != Z_OK) {
                    return Fail(detail, "zlib не принял строку PNG");
                }
                if (_zstream.avail_out == 0 && !FlushFullIdat(detail)) {
                    return false;
                }
            }
            input = input.subspan(inputBytes);
        }
        return true;
    }

    void ResetOutputBuffer() {
        _zstream.next_out = reinterpret_cast<Bytef*>(_compressed.data());
        _zstream.avail_out = static_cast<uInt>(_compressed.size());
    }

    [[nodiscard]] bool FlushFullIdat(std::string& detail) {
        if (!WriteChunk("IDAT", _compressed)) {
            return Fail(detail, "не удалось записать IDAT PNG");
        }
        ResetOutputBuffer();
        return true;
    }

    [[nodiscard]] bool WriteChunk(
        std::string_view type, std::span<const std::uint8_t> payload) {
        if (type.size() != 4 || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        const auto length = BigEndian32(static_cast<std::uint32_t>(payload.size()));
        if (!WriteBytes(std::span{length})) {
            return false;
        }
        if (!WriteBytes(std::as_bytes(std::span{type.data(), type.size()}))) {
            return false;
        }
        if (!WriteBytes(payload)) {
            return false;
        }

        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, reinterpret_cast<const Bytef*>(type.data()),
                    static_cast<uInt>(type.size()));
        if (!payload.empty()) {
            crc = crc32(crc, reinterpret_cast<const Bytef*>(payload.data()),
                        static_cast<uInt>(payload.size()));
        }
        const auto crcBytes = BigEndian32(static_cast<std::uint32_t>(crc));
        return WriteBytes(std::span{crcBytes});
    }

    template <typename T, std::size_t Extent>
    [[nodiscard]] bool WriteBytes(std::span<const T, Extent> bytes) {
        if (bytes.empty()) {
            return true;
        }
        _stream.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size_bytes()));
        return static_cast<bool>(_stream);
    }

    [[nodiscard]] bool Fail(std::string& detail, std::string message) {
        detail = std::move(message);
        _failed = true;
        CleanupIncomplete();
        return false;
    }

    void EndZlib() noexcept {
        if (_zlibInitialized) {
            deflateEnd(&_zstream);
            _zlibInitialized = false;
        }
    }

    void CleanupIncomplete() noexcept {
        EndZlib();
        if (_stream.is_open()) {
            _stream.close();
        }
        std::error_code ec;
        std::filesystem::remove(_path, ec);
    }

    std::filesystem::path _path;
    std::ofstream _stream;
    z_stream _zstream{};
    std::array<std::uint8_t, kIdatBufferBytes> _compressed{};
    std::uint32_t _width{0};
    std::uint32_t _height{0};
    std::uint32_t _rowsWritten{0};
    std::size_t _rowBytes{0};
    std::size_t _peakRgbaRowBytes{0};
    bool _zlibInitialized{false};
    bool _finished{false};
    bool _failed{false};
};

StreamingPngWriter::StreamingPngWriter(std::unique_ptr<Impl> impl)
    : _impl(std::move(impl)) {
}

StreamingPngWriter::~StreamingPngWriter() = default;

std::unique_ptr<StreamingPngWriter> StreamingPngWriter::Open(
    const std::filesystem::path& path,
    std::uint32_t width,
    std::uint32_t height,
    std::string& detail) {
    detail.clear();
    if (width == 0 || height == 0) {
        detail = "размеры PNG должны быть ненулевыми";
        return nullptr;
    }
    const std::size_t widthSize = width;
    if (MultiplyOverflows(widthSize, 4u)) {
        detail = "размер строки PNG переполнен";
        return nullptr;
    }

    const std::size_t rowBytes = widthSize * 4u;
    if (rowBytes == std::numeric_limits<std::size_t>::max() ||
        MultiplyOverflows(rowBytes + 1u, height)) {
        detail = "размер данных PNG переполнен";
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(path, width, height, rowBytes);
    if (!impl->Initialize(detail)) {
        return nullptr;
    }
    return std::unique_ptr<StreamingPngWriter>{
        new StreamingPngWriter{std::move(impl)}};
}

bool StreamingPngWriter::WriteRgbaRow(
    std::span<const std::uint8_t> row, std::string& detail) {
    return _impl->WriteRgbaRow(row, detail);
}

bool StreamingPngWriter::Finish(std::string& detail) {
    return _impl->Finish(detail);
}

std::size_t StreamingPngWriter::PeakRgbaRowBytes() const noexcept {
    return _impl->PeakRgbaRowBytes();
}

} // namespace Corsairs::Tools::AssetConverter
