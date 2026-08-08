#include "Corsairs/Tools/AssetConverter/TerrainTextureCache.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::size_t kHardDecodedByteLimit = 32u * 1024u * 1024u;

std::filesystem::path CacheKey(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

} // namespace

TerrainTextureCache::TerrainTextureCache(std::size_t maxDecodedBytes)
    : _maxDecodedBytes{std::min(maxDecodedBytes, kHardDecodedByteLimit)} {
}

std::shared_ptr<const DecodedImage> TerrainTextureCache::Load(
    const std::filesystem::path& path, std::string& detail) {
    const std::filesystem::path key = CacheKey(path);
    if (const auto found = _entries.find(key); found != _entries.end()) {
        _recency.splice(_recency.begin(), _recency, found->second.Recency);
        detail.clear();
        return found->second.Image;
    }

    std::optional<DecodedImage> decoded = DecodeImageFile(path, detail);
    if (!decoded.has_value()) {
        return nullptr;
    }

    auto image = std::make_shared<const DecodedImage>(std::move(*decoded));
    const std::size_t bytes = image->Pixels.size();
    if (bytes > _maxDecodedBytes) {
        return image;
    }

    while (!_recency.empty() && _decodedBytes > _maxDecodedBytes - bytes) {
        const std::filesystem::path& evictedKey = _recency.back();
        const auto evicted = _entries.find(evictedKey);
        _decodedBytes -= evicted->second.Bytes;
        _entries.erase(evicted);
        _recency.pop_back();
    }

    _recency.push_front(key);
    _decodedBytes += bytes;
    _peakDecodedBytes = std::max(_peakDecodedBytes, _decodedBytes);
    _entries.emplace(key, Entry{image, bytes, _recency.begin()});
    return image;
}

std::size_t TerrainTextureCache::DecodedBytes() const noexcept {
    return _decodedBytes;
}

std::size_t TerrainTextureCache::PeakDecodedBytes() const noexcept {
    return _peakDecodedBytes;
}

std::size_t TerrainTextureCache::EntryCount() const noexcept {
    return _entries.size();
}

} // namespace Corsairs::Tools::AssetConverter
