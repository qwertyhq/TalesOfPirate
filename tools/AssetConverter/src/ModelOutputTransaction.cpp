#include "Corsairs/Tools/AssetConverter/ModelOutputTransaction.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::string_view kOwnershipHeader =
    "CORSAIRS_ASSET_CONVERTER_MODEL_OUTPUTS_V1";
constexpr std::string_view kOwnershipSuffix =
    ".lmo.assetconverter-owned-v1";

struct Ownership {
    std::string Source;
    std::set<std::string> Artifacts;
};

struct OutputArtifact {
    std::string Name;
    std::filesystem::path Final;
    std::filesystem::path Staged;
};

struct BackupEntry {
    std::filesystem::path Original;
    std::filesystem::path Backup;
};

bool IsSafeRelativeSource(const std::filesystem::path& source) {
    if (source.empty() || source.is_absolute()) {
        return false;
    }
    const std::string serialized = source.generic_string();
    if (serialized.find_first_of("\r\n") != std::string::npos) {
        return false;
    }
    for (const auto& component : source) {
        if (component == "." || component == "..") {
            return false;
        }
    }
    return true;
}

bool HasLineBreak(std::string_view value) {
    return value.find_first_of("\r\n") != std::string_view::npos;
}

std::string OwnershipKey(std::string_view value) {
    std::string key{value};
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return key;
}

std::optional<std::string> OwnershipBase(
    const std::filesystem::path& sidecar) {
    const std::string name = sidecar.filename().string();
    if (!name.starts_with('.') || !name.ends_with(kOwnershipSuffix) ||
        name.size() <= 1u + kOwnershipSuffix.size() || HasLineBreak(name)) {
        return std::nullopt;
    }
    return name.substr(1u, name.size() - 1u - kOwnershipSuffix.size());
}

std::filesystem::path OwnershipPath(
    const std::filesystem::path& logicalGltfPath) {
    return logicalGltfPath.parent_path() /
        std::format(".{}.lmo.assetconverter-owned-v1",
                    logicalGltfPath.stem().string());
}

bool IsOwnedArtifactName(std::string_view name, std::string_view base) {
    const std::filesystem::path path{name};
    if (name.empty() || path.filename() != path || name == "." || name == "..") {
        return false;
    }
    const std::string extension = path.extension().string();
    if (extension != ".gltf" && extension != ".bin") {
        return false;
    }
    const std::string stem = path.stem().string();
    if (stem == base) {
        return true;
    }
    const std::string prefix = std::format("{}_", base);
    if (!stem.starts_with(prefix) || stem.size() == prefix.size()) {
        return false;
    }
    return std::all_of(
        stem.begin() + static_cast<std::ptrdiff_t>(prefix.size()), stem.end(),
        [](unsigned char character) { return std::isdigit(character) != 0; });
}

bool ParseOwnership(const std::filesystem::path& path,
                    Ownership& ownership,
                    std::string& detail) {
    const auto base = OwnershipBase(path);
    const auto bytes = ReadWholeFile(path);
    if (!base.has_value() || !bytes.has_value()) {
        detail = std::format("не прочитан ownership sidecar {}",
                             path.generic_string());
        return false;
    }
    const std::string text{
        reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    if (text.empty() || !text.ends_with('\n')) {
        detail = std::format("ownership sidecar {} не завершён newline",
                             path.generic_string());
        return false;
    }

    std::vector<std::string> lines;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t newline = text.find('\n', offset);
        if (newline == std::string::npos) {
            detail = "ownership sidecar содержит незавершённую строку";
            return false;
        }
        lines.push_back(text.substr(offset, newline - offset));
        offset = newline + 1u;
    }
    if (lines.size() < 4u || lines[0] != kOwnershipHeader ||
        !lines[1].starts_with("source=")) {
        detail = std::format("ownership sidecar {} имеет неверный header",
                             path.generic_string());
        return false;
    }

    ownership = {};
    ownership.Source = lines[1].substr(std::string_view{"source="}.size());
    if (!IsSafeRelativeSource(ownership.Source)) {
        detail = std::format("ownership sidecar {} имеет небезопасный source",
                             path.generic_string());
        return false;
    }

    std::vector<std::string> names(lines.begin() + 2, lines.end());
    if (!std::is_sorted(names.begin(), names.end()) ||
        std::adjacent_find(names.begin(), names.end()) != names.end()) {
        detail = std::format("ownership sidecar {} не sorted/unique",
                             path.generic_string());
        return false;
    }
    for (const std::string& name : names) {
        if (!IsOwnedArtifactName(name, *base)) {
            detail = std::format("ownership artifact {} недопустим", name);
            return false;
        }
        ownership.Artifacts.insert(name);
    }
    if (ownership.Artifacts.size() != names.size() ||
        ownership.Artifacts.size() % 2u != 0u) {
        detail = "ownership sidecar не содержит уникальные glTF/bin pairs";
        return false;
    }
    for (const std::string& name : ownership.Artifacts) {
        std::filesystem::path mate{name};
        mate.replace_extension(mate.extension() == ".gltf" ? ".bin" : ".gltf");
        if (!ownership.Artifacts.contains(mate.filename().string())) {
            detail = std::format("ownership artifact {} не имеет пары", name);
            return false;
        }
    }

    bool hasSingleLayout = false;
    std::set<std::uint32_t> suffixIndices;
    const std::string suffixPrefix = std::format("{}_", *base);
    for (const std::string& name : ownership.Artifacts) {
        const std::filesystem::path artifact{name};
        if (artifact.extension() != ".gltf") {
            continue;
        }
        const std::string stem = artifact.stem().string();
        if (stem == *base) {
            hasSingleLayout = true;
            continue;
        }
        const std::string_view digits{stem.data() + suffixPrefix.size(),
                                      stem.size() - suffixPrefix.size()};
        std::uint32_t index = 0;
        const auto result = std::from_chars(
            digits.data(), digits.data() + digits.size(), index);
        if (result.ec != std::errc{} ||
            result.ptr != digits.data() + digits.size()) {
            detail = std::format("ownership artifact {} имеет неверный suffix",
                                 name);
            return false;
        }
        suffixIndices.insert(index);
    }
    if (hasSingleLayout) {
        if (!suffixIndices.empty() || ownership.Artifacts.size() != 2u) {
            detail = "ownership sidecar смешивает single и suffix layouts";
            return false;
        }
    }
    else {
        if (suffixIndices.empty() || *suffixIndices.begin() != 0u ||
            ownership.Artifacts.size() != suffixIndices.size() * 2u) {
            detail = "ownership suffix layout не начинается с нуля";
            return false;
        }
        std::uint32_t expected = 0;
        for (const std::uint32_t index : suffixIndices) {
            if (index != expected++) {
                detail = "ownership suffix layout содержит разрыв";
                return false;
            }
        }
    }
    return true;
}

bool IsRegularFileNoFollow(const std::filesystem::path& path,
                           bool& exists,
                           std::string& detail) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        exists = false;
        return true;
    }
    if (error) {
        detail = std::format("не проверен output {}: {}",
                             path.generic_string(), error.message());
        return false;
    }
    exists = status.type() != std::filesystem::file_type::not_found;
    if (exists && !std::filesystem::is_regular_file(status)) {
        detail = std::format("output {} не является regular file",
                             path.generic_string());
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> CreateStageDirectory(
    const std::filesystem::path& logicalGltfPath,
    std::string& detail) {
    static std::atomic<std::uint64_t> sequence{0};
    const std::filesystem::path parent = logicalGltfPath.parent_path();
    for (std::size_t attempt = 0; attempt < 64u; ++attempt) {
        const std::uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed);
        const std::filesystem::path candidate = parent / std::format(
            ".{}.lmo.stage.{}", logicalGltfPath.stem().string(), id);
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error && error != std::errc::file_exists) {
            detail = std::format("не создан model stage {}: {}",
                                 candidate.generic_string(), error.message());
            return std::nullopt;
        }
    }
    detail = "не удалось выделить уникальный model stage";
    return std::nullopt;
}

void CleanupStage(const std::filesystem::path& stage) {
    std::error_code error;
    std::filesystem::remove_all(stage, error);
}

std::vector<OutputArtifact> BuildArtifacts(
    const ModelOutputRequest& request,
    const std::filesystem::path& stage) {
    std::vector<OutputArtifact> artifacts;
    artifacts.reserve(request.PartCount * 2u);
    for (std::size_t index = 0; index < request.PartCount; ++index) {
        std::filesystem::path gltf = request.LogicalGltfPath;
        if (request.PartCount > 1u) {
            gltf.replace_filename(std::format(
                "{}_{}.gltf", request.LogicalGltfPath.stem().string(), index));
        }
        std::filesystem::path bin = gltf;
        bin.replace_extension(".bin");
        artifacts.push_back(OutputArtifact{
            gltf.filename().string(), gltf, stage / gltf.filename()});
        artifacts.push_back(OutputArtifact{
            bin.filename().string(), bin, stage / bin.filename()});
    }
    return artifacts;
}

std::string BuildOwnershipText(const ModelOutputRequest& request,
                               const std::set<std::string>& names) {
    std::string text = std::format("{}\nsource={}\n", kOwnershipHeader,
                                   request.SourceRelativePath.generic_string());
    for (const std::string& name : names) {
        text += name;
        text += '\n';
    }
    return text;
}

bool WriteText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

bool Rename(const std::filesystem::path& source,
            const std::filesystem::path& destination,
            std::string& detail,
            const ModelOutputDependencies& dependencies) {
    if (dependencies.Rename) {
        return dependencies.Rename(source, destination, detail);
    }
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (!error) {
        return true;
    }
    detail = std::format("rename {} -> {}: {}", source.generic_string(),
                         destination.generic_string(), error.message());
    return false;
}

bool Rollback(const std::vector<std::filesystem::path>& published,
              const std::vector<BackupEntry>& backups,
              std::string& detail,
              const ModelOutputDependencies& dependencies) {
    bool complete = true;
    for (auto it = published.rbegin(); it != published.rend(); ++it) {
        std::error_code error;
        std::filesystem::remove(*it, error);
        if (error) {
            complete = false;
            detail += std::format("; rollback remove {}: {}",
                                  it->generic_string(), error.message());
        }
    }
    for (auto it = backups.rbegin(); it != backups.rend(); ++it) {
        std::string restoreDetail;
        if (!Rename(it->Backup, it->Original, restoreDetail, dependencies)) {
            complete = false;
            detail += std::format("; rollback restore: {}", restoreDetail);
        }
    }
    return complete;
}

ModelOutputStatus FinishFailedPublish(
    const std::filesystem::path& stage,
    const std::vector<std::filesystem::path>& published,
    const std::vector<BackupEntry>& backups,
    std::string& detail,
    const ModelOutputDependencies& dependencies) {
    if (Rollback(published, backups, detail, dependencies)) {
        CleanupStage(stage);
        return ModelOutputStatus::PUBLISH_FAILED;
    }
    detail += std::format("; recovery={}", stage.generic_string());
    return ModelOutputStatus::RECOVERY_REQUIRED;
}

} // namespace

ModelOutputStatus StageAndPublishModelOutputs(
    const ModelOutputRequest& request,
    const ModelPartStager& stagePart,
    std::string& detail,
    const ModelOutputDependencies& dependencies) {
    detail.clear();
    if (request.PartCount == 0u || request.LogicalGltfPath.empty() ||
        request.LogicalGltfPath.extension() != ".gltf" || !stagePart ||
        !IsSafeRelativeSource(request.SourceRelativePath) ||
        HasLineBreak(request.LogicalGltfPath.filename().string())) {
        detail = "недопустимый model output request";
        return ModelOutputStatus::INVALID_REQUEST;
    }

    const std::filesystem::path parent = request.LogicalGltfPath.parent_path();
    std::error_code directoryError;
    std::filesystem::create_directories(parent, directoryError);
    if (directoryError) {
        detail = std::format("не создан output directory {}: {}",
                             parent.generic_string(), directoryError.message());
        return ModelOutputStatus::OUTPUT_DIR_FAILED;
    }

    const std::filesystem::path ownershipPath =
        OwnershipPath(request.LogicalGltfPath);
    Ownership priorOwnership;
    bool hasPriorOwnership = false;
    std::set<std::string> otherOwnedKeys;
    for (const auto& entry : std::filesystem::directory_iterator{parent}) {
        if (!entry.path().filename().string().ends_with(kOwnershipSuffix)) {
            continue;
        }
        std::error_code statusError;
        const auto status = entry.symlink_status(statusError);
        if (statusError || !std::filesystem::is_regular_file(status)) {
            detail = std::format(
                "ownership sidecar {} не является regular file",
                entry.path().generic_string());
            return ModelOutputStatus::OUTPUT_OWNERSHIP_INVALID;
        }
        Ownership parsed;
        if (!ParseOwnership(entry.path(), parsed, detail)) {
            return ModelOutputStatus::OUTPUT_OWNERSHIP_INVALID;
        }
        if (entry.path() == ownershipPath) {
            if (parsed.Source != request.SourceRelativePath.generic_string()) {
                detail = std::format(
                    "ownership source={} не совпадает с {}", parsed.Source,
                    request.SourceRelativePath.generic_string());
                return ModelOutputStatus::OUTPUT_OWNERSHIP_INVALID;
            }
            priorOwnership = std::move(parsed);
            hasPriorOwnership = true;
        }
        else {
            for (const std::string& name : parsed.Artifacts) {
                if (!otherOwnedKeys.insert(OwnershipKey(name)).second) {
                    detail = std::format(
                        "owned output {} одновременно заявлен другими моделями",
                        name);
                    return ModelOutputStatus::OUTPUT_OWNERSHIP_CONFLICT;
                }
            }
        }
    }
    if (hasPriorOwnership) {
        for (const std::string& name : priorOwnership.Artifacts) {
            if (otherOwnedKeys.contains(OwnershipKey(name))) {
                detail = std::format(
                    "owned output {} одновременно заявлен другой моделью", name);
                return ModelOutputStatus::OUTPUT_OWNERSHIP_CONFLICT;
            }
        }
    }

    const auto stage = CreateStageDirectory(request.LogicalGltfPath, detail);
    if (!stage.has_value()) {
        return ModelOutputStatus::OUTPUT_DIR_FAILED;
    }
    const std::vector<OutputArtifact> artifacts = BuildArtifacts(request, *stage);
    std::set<std::string> desiredNames;
    for (const OutputArtifact& artifact : artifacts) {
        desiredNames.insert(artifact.Name);
    }
    for (const std::string& name : desiredNames) {
        if (otherOwnedKeys.contains(OwnershipKey(name))) {
            detail = std::format("output {} принадлежит другой модели", name);
            CleanupStage(*stage);
            return ModelOutputStatus::OUTPUT_OWNERSHIP_CONFLICT;
        }
    }

    if (hasPriorOwnership) {
        for (const std::string& name : priorOwnership.Artifacts) {
            bool exists = false;
            if (!IsRegularFileNoFollow(parent / name, exists, detail) || !exists) {
                if (detail.empty()) {
                    detail = std::format("owned output {} отсутствует", name);
                }
                CleanupStage(*stage);
                return ModelOutputStatus::OUTPUT_OWNERSHIP_INVALID;
            }
        }
    }

    for (std::size_t index = 0; index < request.PartCount; ++index) {
        const OutputArtifact& gltf = artifacts[index * 2u];
        std::string stageDetail;
        if (!stagePart(index, gltf.Staged, stageDetail)) {
            detail = std::format("часть {}: {}", index, stageDetail);
            CleanupStage(*stage);
            return ModelOutputStatus::STAGING_FAILED;
        }
        for (std::size_t member = 0; member < 2u; ++member) {
            bool exists = false;
            const OutputArtifact& artifact = artifacts[index * 2u + member];
            if (!IsRegularFileNoFollow(artifact.Staged, exists, detail) || !exists) {
                if (detail.empty()) {
                    detail = std::format("часть {} не создала {}", index,
                                         artifact.Name);
                }
                CleanupStage(*stage);
                return ModelOutputStatus::STAGING_FAILED;
            }
        }
    }

    for (std::size_t index = 0; index < artifacts.size(); index += 2u) {
        bool hasGltf = false;
        bool hasBin = false;
        if (!IsRegularFileNoFollow(artifacts[index].Final, hasGltf, detail) ||
            !IsRegularFileNoFollow(artifacts[index + 1u].Final, hasBin, detail)) {
            CleanupStage(*stage);
            return ModelOutputStatus::PUBLISH_FAILED;
        }
        if (hasGltf != hasBin) {
            detail = std::format("destination pair {} неполна",
                                 artifacts[index].Final.stem().string());
            CleanupStage(*stage);
            return ModelOutputStatus::PUBLISH_FAILED;
        }
    }

    const std::filesystem::path stagedOwnership =
        *stage / ownershipPath.filename();
    if (!WriteText(stagedOwnership, BuildOwnershipText(request, desiredNames))) {
        detail = "не записан staged ownership sidecar";
        CleanupStage(*stage);
        return ModelOutputStatus::STAGING_FAILED;
    }

    const std::filesystem::path backupDirectory = *stage / "prior";
    std::error_code backupError;
    if (!std::filesystem::create_directory(backupDirectory, backupError) ||
        backupError) {
        detail = std::format("не создан backup directory: {}",
                             backupError.message());
        CleanupStage(*stage);
        return ModelOutputStatus::PUBLISH_FAILED;
    }

    std::set<std::filesystem::path> pathsToBackup;
    for (const OutputArtifact& artifact : artifacts) {
        bool exists = false;
        if (!IsRegularFileNoFollow(artifact.Final, exists, detail)) {
            CleanupStage(*stage);
            return ModelOutputStatus::PUBLISH_FAILED;
        }
        if (exists) {
            pathsToBackup.insert(artifact.Final);
        }
    }
    if (hasPriorOwnership) {
        for (const std::string& name : priorOwnership.Artifacts) {
            pathsToBackup.insert(parent / name);
        }
        pathsToBackup.insert(ownershipPath);
    }

    std::vector<BackupEntry> backups;
    backups.reserve(pathsToBackup.size());
    for (const std::filesystem::path& original : pathsToBackup) {
        const std::filesystem::path backup = backupDirectory /
            std::format("{}-{}", backups.size(), original.filename().string());
        if (!Rename(original, backup, detail, dependencies)) {
            return FinishFailedPublish(
                *stage, {}, backups, detail, dependencies);
        }
        backups.push_back(BackupEntry{original, backup});
    }

    std::vector<std::filesystem::path> published;
    published.reserve(artifacts.size() + 1u);
    for (const OutputArtifact& artifact : artifacts) {
        if (!Rename(artifact.Staged, artifact.Final, detail, dependencies)) {
            return FinishFailedPublish(
                *stage, published, backups, detail, dependencies);
        }
        published.push_back(artifact.Final);
    }
    if (!Rename(stagedOwnership, ownershipPath, detail, dependencies)) {
        return FinishFailedPublish(
            *stage, published, backups, detail, dependencies);
    }
    published.push_back(ownershipPath);

    CleanupStage(*stage);
    detail.clear();
    return ModelOutputStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
