#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace Corsairs::Tools::AssetConverter {

enum class ModelOutputStatus : std::uint32_t {
    OK = 0,
    INVALID_REQUEST,
    OUTPUT_DIR_FAILED,
    STAGING_FAILED,
    OUTPUT_OWNERSHIP_INVALID,
    OUTPUT_OWNERSHIP_CONFLICT,
    PUBLISH_FAILED,
    RECOVERY_REQUIRED,
};

struct ModelOutputRequest {
    std::filesystem::path LogicalGltfPath;
    std::filesystem::path SourceRelativePath;
    std::size_t PartCount{0};
};

// Callback пишет одну завершённую glTF/bin-пару по переданному staged .gltf.
// До успешного завершения всех callback ни один final output не изменяется.
using ModelPartStager = std::function<bool(
    std::size_t,
    const std::filesystem::path&,
    std::string&)>;

// Узкий seam для детерминированной проверки rollback. Пустой callback
// использует std::filesystem::rename.
struct ModelOutputDependencies {
    std::function<bool(const std::filesystem::path&,
                       const std::filesystem::path&,
                       std::string&)> Rename;
};

// Публикует весь набор частей модели как одну транзакцию. Точные owned-файлы
// хранятся в строгом sidecar; legacy suffixes без sidecar не присваиваются и
// не удаляются автоматически.
[[nodiscard]] ModelOutputStatus StageAndPublishModelOutputs(
    const ModelOutputRequest& request,
    const ModelPartStager& stagePart,
    std::string& detail,
    const ModelOutputDependencies& dependencies = {});

} // namespace Corsairs::Tools::AssetConverter
