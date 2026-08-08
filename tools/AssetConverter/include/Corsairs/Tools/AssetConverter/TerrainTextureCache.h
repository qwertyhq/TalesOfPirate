#pragma once

#include "Corsairs/Tools/AssetConverter/ImageCodec.h"

#include <cstddef>
#include <filesystem>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

namespace Corsairs::Tools::AssetConverter {

class TerrainTextureCache {
public:
    explicit TerrainTextureCache(std::size_t maxDecodedBytes);

    [[nodiscard]] std::shared_ptr<const DecodedImage> Load(
        const std::filesystem::path& path, std::string& detail);

    [[nodiscard]] std::size_t DecodedBytes() const noexcept;
    [[nodiscard]] std::size_t PeakDecodedBytes() const noexcept;
    [[nodiscard]] std::size_t EntryCount() const noexcept;

private:
    struct Entry {
        std::shared_ptr<const DecodedImage> Image;
        std::size_t Bytes{0};
        std::list<std::string>::iterator Recency;
    };

    std::size_t _maxDecodedBytes{0};
    std::size_t _decodedBytes{0};
    std::size_t _peakDecodedBytes{0};
    std::list<std::string> _recency;
    std::unordered_map<std::string, Entry> _entries;
};

} // namespace Corsairs::Tools::AssetConverter
