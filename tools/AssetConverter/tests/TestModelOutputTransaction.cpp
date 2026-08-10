#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/LmoParser.h"
#include "Corsairs/Tools/AssetConverter/ModelOutputTransaction.h"

#include "TestHarness.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

std::filesystem::path UniqueTestDirectory(std::string_view name) {
    return std::filesystem::temp_directory_path() / std::format(
        "corsairs-{}-{}", name,
        std::chrono::steady_clock::now().time_since_epoch().count());
}

bool WriteText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

bool WriteBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    return static_cast<bool>(stream);
}

std::optional<std::string> ReadText(const std::filesystem::path& path) {
    const auto bytes = AC::ReadWholeFile(path);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

std::filesystem::path PartPath(const std::filesystem::path& root,
                               std::size_t index,
                               std::string_view extension) {
    return root / std::format("model_{}.{}", index, extension);
}

bool WriteStagedPair(const std::filesystem::path& gltfPath,
                     std::string_view tag,
                     std::size_t index) {
    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");
    return WriteText(gltfPath, std::format("{}-gltf-{}", tag, index)) &&
           WriteText(binPath, std::format("{}-bin-{}", tag, index));
}

bool RenamePath(const std::filesystem::path& source,
                const std::filesystem::path& destination,
                std::string& detail) {
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (!error) {
        return true;
    }
    detail = error.message();
    return false;
}

std::string ShellQuote(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::string quoted{"\""};
    quoted += path.string();
    quoted += '"';
    return quoted;
#else
    std::string quoted{"'"};
    for (const char character : path.string()) {
        if (character == '\'') {
            quoted += "'\\''";
        }
        else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
#endif
}

std::optional<std::vector<std::uint8_t>> ExtractModernLmoGeometry(
    const std::filesystem::path& path,
    std::size_t geometryIndex) {
    const auto source = AC::ReadWholeFile(path);
    if (!source.has_value() || source->size() < 8u) {
        return std::nullopt;
    }

    std::uint32_t version = 0;
    std::uint32_t entryCount = 0;
    std::memcpy(&version, source->data(), sizeof(version));
    std::memcpy(&entryCount, source->data() + sizeof(version), sizeof(entryCount));
    const std::size_t tableBytes = static_cast<std::size_t>(entryCount) *
        sizeof(AC::ModelObjEntry);
    if (source->size() < 8u + tableBytes || version == AC::kLegacyVersion) {
        return std::nullopt;
    }

    std::size_t seenGeometry = 0;
    for (std::uint32_t index = 0; index < entryCount; ++index) {
        AC::ModelObjEntry entry{};
        std::memcpy(&entry,
                    source->data() + 8u +
                        static_cast<std::size_t>(index) * sizeof(entry),
                    sizeof(entry));
        if (entry.Type != static_cast<std::uint32_t>(AC::ModelObjType::GEOMETRY)) {
            continue;
        }
        if (seenGeometry++ != geometryIndex) {
            continue;
        }
        const std::uint64_t end = static_cast<std::uint64_t>(entry.Addr) +
            static_cast<std::uint64_t>(entry.Size);
        if (end > source->size()) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> result(sizeof(version) + entry.Size);
        std::memcpy(result.data(), &version, sizeof(version));
        std::memcpy(result.data() + sizeof(version),
                    source->data() + entry.Addr, entry.Size);
        return result;
    }
    return std::nullopt;
}

CORSAIRS_TEST(ModelOutputTransaction_LateStagingFailurePreservesPriorCompleteSet) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-late-failure");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);

    const AC::ModelOutputRequest initial{
        root / "model.gltf", "scene/model.lmo", 2u};
    std::string detail;
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   initial,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "prior", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(AC::ModelOutputStatus::OK));

    const auto priorGltf0 = ReadText(PartPath(root, 0u, "gltf"));
    const auto priorBin0 = ReadText(PartPath(root, 0u, "bin"));
    const auto priorGltf1 = ReadText(PartPath(root, 1u, "gltf"));
    const auto priorBin1 = ReadText(PartPath(root, 1u, "bin"));
    const auto priorOwnership = ReadText(
        root / ".model.lmo.assetconverter-owned-v1");
    REQUIRE(priorGltf0.has_value());
    REQUIRE(priorBin0.has_value());
    REQUIRE(priorGltf1.has_value());
    REQUIRE(priorBin1.has_value());
    REQUIRE(priorOwnership.has_value());

    const AC::ModelOutputRequest replacement{
        root / "model.gltf", "scene/model.lmo", 3u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   replacement,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string& stageDetail) {
                       if (index == 2u) {
                           stageDetail = "forced late part failure";
                           return false;
                       }
                       return WriteStagedPair(staged, "replacement", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::STAGING_FAILED));

    REQUIRE_EQ(*ReadText(PartPath(root, 0u, "gltf")), *priorGltf0);
    REQUIRE_EQ(*ReadText(PartPath(root, 0u, "bin")), *priorBin0);
    REQUIRE_EQ(*ReadText(PartPath(root, 1u, "gltf")), *priorGltf1);
    REQUIRE_EQ(*ReadText(PartPath(root, 1u, "bin")), *priorBin1);
    REQUIRE(!std::filesystem::exists(PartPath(root, 2u, "gltf")));
    REQUIRE(!std::filesystem::exists(PartPath(root, 2u, "bin")));
    REQUIRE_EQ(*ReadText(root / ".model.lmo.assetconverter-owned-v1"),
               *priorOwnership);

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelOutputTransaction_SuccessCleansOnlyManifestOwnedStaleParts) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-stale-cleanup");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);

    std::string detail;
    const AC::ModelOutputRequest initial{
        root / "model.gltf", "scene/model.lmo", 3u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   initial,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "three", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(AC::ModelOutputStatus::OK));
    REQUIRE(WriteText(PartPath(root, 99u, "gltf"), "foreign-gltf"));
    REQUIRE(WriteText(PartPath(root, 99u, "bin"), "foreign-bin"));

    const AC::ModelOutputRequest replacement{
        root / "model.gltf", "scene/model.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   replacement,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "single", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(AC::ModelOutputStatus::OK));

    REQUIRE(std::filesystem::exists(root / "model.gltf"));
    REQUIRE(std::filesystem::exists(root / "model.bin"));
    for (std::size_t index = 0; index < 3u; ++index) {
        REQUIRE(!std::filesystem::exists(PartPath(root, index, "gltf")));
        REQUIRE(!std::filesystem::exists(PartPath(root, index, "bin")));
    }
    REQUIRE_EQ(*ReadText(PartPath(root, 99u, "gltf")),
               std::string{"foreign-gltf"});
    REQUIRE_EQ(*ReadText(PartPath(root, 99u, "bin")),
               std::string{"foreign-bin"});
    const auto ownership = ReadText(
        root / ".model.lmo.assetconverter-owned-v1");
    REQUIRE(ownership.has_value());
    REQUIRE(ownership->find("model.gltf\n") != std::string::npos);
    REQUIRE(ownership->find("model.bin\n") != std::string::npos);
    REQUIRE(ownership->find("model_99") == std::string::npos);

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelOutputTransaction_RejectsCrossModelOwnershipCollision) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-owner-collision");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);

    std::string detail;
    const AC::ModelOutputRequest first{
        root / "login02.gltf", "scene/login02.lmo", 2u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   first,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "first", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(AC::ModelOutputStatus::OK));
    const auto prior = ReadText(root / "login02_1.gltf");
    REQUIRE(prior.has_value());

    const AC::ModelOutputRequest colliding{
        root / "login02_1.gltf", "scene/login02_1.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   colliding,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "second", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::OUTPUT_OWNERSHIP_CONFLICT));
    REQUIRE_EQ(*ReadText(root / "login02_1.gltf"), *prior);
    REQUIRE(!std::filesystem::exists(
        root / ".login02_1.lmo.assetconverter-owned-v1"));

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelOutputTransaction_RejectsMixedOwnershipLayoutWithoutMutation) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-mixed-ownership");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);
    REQUIRE(WriteText(root / "model.gltf", "single-gltf"));
    REQUIRE(WriteText(root / "model.bin", "single-bin"));
    REQUIRE(WriteText(root / "model_0.gltf", "suffix-gltf"));
    REQUIRE(WriteText(root / "model_0.bin", "suffix-bin"));
    REQUIRE(WriteText(
        root / ".model.lmo.assetconverter-owned-v1",
        "CORSAIRS_ASSET_CONVERTER_MODEL_OUTPUTS_V1\n"
        "source=scene/model.lmo\n"
        "model.bin\n"
        "model.gltf\n"
        "model_0.bin\n"
        "model_0.gltf\n"));

    std::string detail;
    const AC::ModelOutputRequest request{
        root / "model.gltf", "scene/model.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "replacement", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::OUTPUT_OWNERSHIP_INVALID));
    REQUIRE_EQ(*ReadText(root / "model.gltf"), std::string{"single-gltf"});
    REQUIRE_EQ(*ReadText(root / "model.bin"), std::string{"single-bin"});
    REQUIRE_EQ(*ReadText(root / "model_0.gltf"), std::string{"suffix-gltf"});
    REQUIRE_EQ(*ReadText(root / "model_0.bin"), std::string{"suffix-bin"});

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelOutputTransaction_RejectsNonContiguousOwnedSuffixes) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-gapped-ownership");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);
    REQUIRE(WriteText(root / "model_1.gltf", "suffix-gltf"));
    REQUIRE(WriteText(root / "model_1.bin", "suffix-bin"));
    REQUIRE(WriteText(
        root / ".model.lmo.assetconverter-owned-v1",
        "CORSAIRS_ASSET_CONVERTER_MODEL_OUTPUTS_V1\n"
        "source=scene/model.lmo\n"
        "model_1.bin\n"
        "model_1.gltf\n"));

    std::string detail;
    const AC::ModelOutputRequest request{
        root / "model.gltf", "scene/model.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "replacement", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::OUTPUT_OWNERSHIP_INVALID));
    REQUIRE_EQ(*ReadText(root / "model_1.gltf"), std::string{"suffix-gltf"});
    REQUIRE_EQ(*ReadText(root / "model_1.bin"), std::string{"suffix-bin"});
    REQUIRE(!std::filesystem::exists(root / "model.gltf"));
    REQUIRE(!std::filesystem::exists(root / "model.bin"));

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelOutputTransaction_DoesNotRetireOutputClaimedByAnotherSidecar) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-stale-owner-collision");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);
    REQUIRE(WriteText(root / "model_0.gltf", "shared-gltf"));
    REQUIRE(WriteText(root / "model_0.bin", "shared-bin"));
    REQUIRE(WriteText(
        root / ".model.lmo.assetconverter-owned-v1",
        "CORSAIRS_ASSET_CONVERTER_MODEL_OUTPUTS_V1\n"
        "source=scene/model.lmo\n"
        "model_0.bin\n"
        "model_0.gltf\n"));
    REQUIRE(WriteText(
        root / ".model_0.lmo.assetconverter-owned-v1",
        "CORSAIRS_ASSET_CONVERTER_MODEL_OUTPUTS_V1\n"
        "source=scene/model_0.lmo\n"
        "model_0.bin\n"
        "model_0.gltf\n"));

    std::string detail;
    const AC::ModelOutputRequest request{
        root / "model.gltf", "scene/model.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "replacement", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::OUTPUT_OWNERSHIP_CONFLICT));
    REQUIRE_EQ(*ReadText(root / "model_0.gltf"), std::string{"shared-gltf"});
    REQUIRE_EQ(*ReadText(root / "model_0.bin"), std::string{"shared-bin"});
    REQUIRE(!std::filesystem::exists(root / "model.gltf"));
    REQUIRE(!std::filesystem::exists(root / "model.bin"));

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelOutputTransaction_MidPublishFailureRestoresPriorCompleteSet) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-mid-publish-failure");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);

    std::string detail;
    const AC::ModelOutputRequest request{
        root / "model.gltf", "scene/model.lmo", 2u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "prior", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(AC::ModelOutputStatus::OK));
    const auto priorGltf0 = ReadText(PartPath(root, 0u, "gltf"));
    const auto priorBin0 = ReadText(PartPath(root, 0u, "bin"));
    const auto priorGltf1 = ReadText(PartPath(root, 1u, "gltf"));
    const auto priorBin1 = ReadText(PartPath(root, 1u, "bin"));
    const auto priorOwnership = ReadText(
        root / ".model.lmo.assetconverter-owned-v1");
    REQUIRE(priorGltf0.has_value());
    REQUIRE(priorBin0.has_value());
    REQUIRE(priorGltf1.has_value());
    REQUIRE(priorBin1.has_value());
    REQUIRE(priorOwnership.has_value());

    std::filesystem::path stageDirectory;
    std::size_t publishRenameCount = 0;
    AC::ModelOutputDependencies dependencies;
    dependencies.Rename =
        [&](const std::filesystem::path& source,
            const std::filesystem::path& destination,
            std::string& renameDetail) {
            const bool publishesFromStage = !stageDirectory.empty() &&
                source.parent_path() == stageDirectory &&
                destination.parent_path() == root;
            if (publishesFromStage && ++publishRenameCount == 3u) {
                renameDetail = "forced third publish rename failure";
                return false;
            }
            return RenamePath(source, destination, renameDetail);
        };
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [&](std::size_t index, const std::filesystem::path& staged,
                       std::string&) {
                       stageDirectory = staged.parent_path();
                       return WriteStagedPair(staged, "replacement", index);
                   },
                   detail, dependencies)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::PUBLISH_FAILED));

    REQUIRE_EQ(*ReadText(PartPath(root, 0u, "gltf")), *priorGltf0);
    REQUIRE_EQ(*ReadText(PartPath(root, 0u, "bin")), *priorBin0);
    REQUIRE_EQ(*ReadText(PartPath(root, 1u, "gltf")), *priorGltf1);
    REQUIRE_EQ(*ReadText(PartPath(root, 1u, "bin")), *priorBin1);
    REQUIRE_EQ(*ReadText(root / ".model.lmo.assetconverter-owned-v1"),
               *priorOwnership);
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        REQUIRE(!entry.path().filename().string().starts_with(
            ".model.lmo.stage."));
    }

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelOutputTransaction_IncompleteRollbackPreservesRecoveryBackup) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-recovery-backup");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);

    std::string detail;
    const AC::ModelOutputRequest request{
        root / "model.gltf", "scene/model.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "prior", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(AC::ModelOutputStatus::OK));

    std::filesystem::path stageDirectory;
    std::size_t publishRenameCount = 0;
    bool restoreFailed = false;
    AC::ModelOutputDependencies dependencies;
    dependencies.Rename =
        [&](const std::filesystem::path& source,
            const std::filesystem::path& destination,
            std::string& renameDetail) {
            const bool publishesFromStage = !stageDirectory.empty() &&
                source.parent_path() == stageDirectory &&
                destination.parent_path() == root;
            if (publishesFromStage && ++publishRenameCount == 2u) {
                renameDetail = "forced second publish rename failure";
                return false;
            }
            if (!restoreFailed && source.parent_path().filename() == "prior" &&
                destination.parent_path() == root) {
                restoreFailed = true;
                renameDetail = "forced restore failure";
                return false;
            }
            return RenamePath(source, destination, renameDetail);
        };
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [&](std::size_t index, const std::filesystem::path& staged,
                       std::string&) {
                       stageDirectory = staged.parent_path();
                       return WriteStagedPair(staged, "replacement", index);
                   },
                   detail, dependencies)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::RECOVERY_REQUIRED));
    REQUIRE(restoreFailed);
    REQUIRE(detail.find("recovery=") != std::string::npos);

    std::size_t recoveryDirectories = 0;
    std::size_t recoveryFiles = 0;
    for (const auto& entry : std::filesystem::directory_iterator{root}) {
        if (!entry.is_directory() ||
            !entry.path().filename().string().starts_with(
                ".model.lmo.stage.")) {
            continue;
        }
        ++recoveryDirectories;
        for (const auto& recovered :
             std::filesystem::recursive_directory_iterator{entry.path()}) {
            recoveryFiles += recovered.is_regular_file() ? 1u : 0u;
        }
    }
    REQUIRE_EQ(recoveryDirectories, 1u);
    REQUIRE(recoveryFiles > 0u);

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

#if !defined(_WIN32)
CORSAIRS_TEST(ModelOutputTransaction_RejectsSymlinkOwnershipSidecar) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-symlink-sidecar");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);
    REQUIRE(WriteText(root / "model.gltf", "prior-gltf"));
    REQUIRE(WriteText(root / "model.bin", "prior-bin"));
    const std::filesystem::path realSidecar = root / "real-sidecar";
    REQUIRE(WriteText(
        realSidecar,
        "CORSAIRS_ASSET_CONVERTER_MODEL_OUTPUTS_V1\n"
        "source=scene/model.lmo\n"
        "model.bin\n"
        "model.gltf\n"));
    std::filesystem::create_symlink(
        realSidecar.filename(),
        root / ".model.lmo.assetconverter-owned-v1", error);
    REQUIRE(!error);

    std::string detail;
    const AC::ModelOutputRequest request{
        root / "model.gltf", "scene/model.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "replacement", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::OUTPUT_OWNERSHIP_INVALID));
    REQUIRE_EQ(*ReadText(root / "model.gltf"), std::string{"prior-gltf"});
    REQUIRE_EQ(*ReadText(root / "model.bin"), std::string{"prior-bin"});

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}
#endif

CORSAIRS_TEST(ModelOutputTransaction_DetectsCaseInsensitiveOwnershipCollision) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-case-collision");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);
    REQUIRE(WriteText(root / "Model_0.gltf", "prior-gltf"));
    REQUIRE(WriteText(root / "Model_0.bin", "prior-bin"));
    REQUIRE(WriteText(
        root / ".Model.lmo.assetconverter-owned-v1",
        "CORSAIRS_ASSET_CONVERTER_MODEL_OUTPUTS_V1\n"
        "source=scene/Model.lmo\n"
        "Model_0.bin\n"
        "Model_0.gltf\n"));

    std::string detail;
    const AC::ModelOutputRequest request{
        root / "model_0.gltf", "scene/model_0.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "replacement", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::OUTPUT_OWNERSHIP_CONFLICT));
    REQUIRE_EQ(*ReadText(root / "Model_0.gltf"), std::string{"prior-gltf"});
    REQUIRE_EQ(*ReadText(root / "Model_0.bin"), std::string{"prior-bin"});

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelOutputTransaction_RejectsNewlineInSourceBeforeStaging) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-output-newline-source");
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(root, error));
    REQUIRE(!error);

    std::string detail;
    const AC::ModelOutputRequest request{
        root / "model.gltf", "scene/model\nother.lmo", 1u};
    REQUIRE_EQ(static_cast<std::uint32_t>(AC::StageAndPublishModelOutputs(
                   request,
                   [](std::size_t index, const std::filesystem::path& staged,
                      std::string&) {
                       return WriteStagedPair(staged, "unexpected", index);
                   },
                   detail)),
               static_cast<std::uint32_t>(
                   AC::ModelOutputStatus::INVALID_REQUEST));
    REQUIRE(!std::filesystem::exists(root / "model.gltf"));
    REQUIRE(!std::filesystem::exists(root / "model.bin"));

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelConversion_LatePartFailureDoesNotPublishEarlierParts) {
    const std::filesystem::path root =
        UniqueTestDirectory("model-conversion-late-failure");
    const std::filesystem::path input = root / "input";
    const std::filesystem::path output = root / "output";
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(input, error));
    REQUIRE(!error);
    REQUIRE(std::filesystem::create_directories(output, error));
    REQUIRE(!error);
    REQUIRE(std::filesystem::copy_file(
        std::filesystem::path{CORSAIRS_REPO_ROOT} /
            "Client/model/scene/nml-bd130.lmo",
        input / "model.lmo", error));
    REQUIRE(!error);

    REQUIRE(WriteText(PartPath(output, 0u, "gltf"), "prior-gltf-0"));
    REQUIRE(WriteText(PartPath(output, 0u, "bin"), "prior-bin-0"));
    REQUIRE(WriteText(PartPath(output, 1u, "gltf"), "prior-gltf-1"));
    REQUIRE(WriteText(PartPath(output, 1u, "bin"), "prior-bin-1"));
    REQUIRE(std::filesystem::create_directory(
        PartPath(output, 2u, "gltf"), error));
    REQUIRE(!error);
    REQUIRE(WriteText(PartPath(output, 2u, "bin"), "prior-bin-2"));

    const std::filesystem::path log = root / "converter.log";
    const std::string command = ShellQuote(ASSET_CONVERTER_PATH) + " " +
        ShellQuote(input) + " " + ShellQuote(output) + " > " +
        ShellQuote(log) + " 2>&1";
    REQUIRE(std::system(command.c_str()) != 0);

    REQUIRE_EQ(*ReadText(PartPath(output, 0u, "gltf")),
               std::string{"prior-gltf-0"});
    REQUIRE_EQ(*ReadText(PartPath(output, 0u, "bin")),
               std::string{"prior-bin-0"});
    REQUIRE_EQ(*ReadText(PartPath(output, 1u, "gltf")),
               std::string{"prior-gltf-1"});
    REQUIRE_EQ(*ReadText(PartPath(output, 1u, "bin")),
               std::string{"prior-bin-1"});
    REQUIRE(std::filesystem::is_directory(PartPath(output, 2u, "gltf")));
    REQUIRE_EQ(*ReadText(PartPath(output, 2u, "bin")),
               std::string{"prior-bin-2"});
    REQUIRE(!std::filesystem::exists(
        output / ".model.lmo.assetconverter-owned-v1"));
    for (const auto& entry : std::filesystem::directory_iterator{output}) {
        REQUIRE(!entry.path().filename().string().starts_with(
            ".model.lmo.stage."));
    }

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

CORSAIRS_TEST(ModelConversion_StandaloneLgoAppliesLegacyCaptureTick) {
    const std::filesystem::path root =
        UniqueTestDirectory("standalone-lgo-capture-tick");
    const std::filesystem::path input = root / "input";
    const std::filesystem::path output = root / "output";
    std::error_code error;
    REQUIRE(std::filesystem::create_directories(input, error));
    REQUIRE(!error);

    const auto part = ExtractModernLmoGeometry(
        std::filesystem::path{CORSAIRS_REPO_ROOT} /
            "Client/model/scene/by-bd015.lmo",
        3u);
    REQUIRE(part.has_value());
    REQUIRE(WriteBytes(input / "fountain.lgo", *part));

    const std::filesystem::path log = root / "converter.log";
    const std::string command = ShellQuote(ASSET_CONVERTER_PATH) + " " +
        ShellQuote(input) + " " + ShellQuote(output) +
        " --profile scene-map --legacy-capture-tick 120 > " +
        ShellQuote(log) + " 2>&1";
    REQUIRE_EQ(std::system(command.c_str()), 0);

    const auto gltf = ReadText(output / "fountain.gltf");
    REQUIRE(gltf.has_value());
    REQUIRE(gltf->find(
        R"("corsairsLegacyCapture":{"schemaVersion":1,"captureTick":120,"matrix":{"frameCount":701,"sampleFrame":119})") !=
        std::string::npos);

    std::filesystem::remove_all(root, error);
    REQUIRE(!error);
}

} // namespace
