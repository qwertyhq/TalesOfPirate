#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace Corsairs::Tools::AssetConverter {

class StreamingPngWriter {
public:
    ~StreamingPngWriter();

    StreamingPngWriter(const StreamingPngWriter&) = delete;
    StreamingPngWriter& operator=(const StreamingPngWriter&) = delete;

    static std::unique_ptr<StreamingPngWriter> Open(
        const std::filesystem::path& path,
        std::uint32_t width,
        std::uint32_t height,
        std::string& detail);

    [[nodiscard]] bool WriteRgbaRow(
        std::span<const std::uint8_t> row, std::string& detail);
    [[nodiscard]] bool Finish(std::string& detail);
    [[nodiscard]] std::size_t PeakRgbaRowBytes() const noexcept;

private:
    class Impl;

    explicit StreamingPngWriter(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> _impl;
};

} // namespace Corsairs::Tools::AssetConverter
