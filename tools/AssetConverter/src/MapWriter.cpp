#include "Corsairs/Tools/AssetConverter/MapWriter.h"

#include "Corsairs/Tools/AssetConverter/JsonWriter.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Corsairs::Tools::AssetConverter {

namespace {

// Раскладка TileInfo: три верхних слоя, у каждого 6 бит номера текстуры и 4
// бита прозрачности. Базовый слой лежит отдельным полем BaseTex и всегда
// непрозрачен. Значения совпадают с MPMapDef.h — они и есть спецификация.
constexpr int kTileTex1Shift = 26;
constexpr int kTileAlpha1Shift = 22;
constexpr int kTileTex2Shift = 16;
constexpr int kTileAlpha2Shift = 12;
constexpr int kTileTex3Shift = 6;
constexpr int kTileAlpha3Shift = 2;
constexpr std::uint32_t kTileTexMask = 0x3Fu;
constexpr std::uint32_t kTileAlphaMask = 0x0Fu;
constexpr std::uint8_t kBaseAlphaOpaque = 15;

} // namespace


namespace {

bool WriteBinary(const std::filesystem::path& path, const void* data, std::size_t bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    if (bytes > 0) {
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    }
    return static_cast<bool>(stream);
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream{path, std::ios::trunc};
    if (!stream) {
        return false;
    }
    stream << text;
    return static_cast<bool>(stream);
}

std::filesystem::path WithSuffix(const std::filesystem::path& base, const char* suffix) {
    std::filesystem::path out = base;
    out += suffix;
    return out;
}

bool CheckedMultiply(std::uint64_t left,
                     std::uint64_t right,
                     std::uint64_t& result) {
    if (left != 0u &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool CheckedAdd(std::uint64_t left,
                std::uint64_t right,
                std::uint64_t& result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool IsLowerSha256(std::string_view hash) {
    return hash.size() == 64u &&
           std::all_of(hash.begin(), hash.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool SameSceneHeader(const SceneFileHeader& left,
                     const SceneFileHeader& right) {
    return std::memcmp(&left, &right, sizeof(SceneFileHeader)) == 0;
}

std::string SourceKeyText(const SceneSourceKey& key) {
    return std::format("({},{},{})", key.SectionIndex, key.SlotIndex,
                       key.ByteOffset);
}

struct SceneManifestRecord {
    SceneSourceKey Source;
    std::int16_t ModelId{0};
    std::int16_t Type{0};
    bool SceneModel{false};
    std::int32_t X{0};
    std::int32_t Y{0};
    std::int16_t HeightOff{0};
    std::int16_t SourceYawDegrees{0};
    std::int16_t SourceScaleDiagnostic{0};
    float SurfaceHeightCm{0.0f};
    float ZCm{0.0f};
    bool TerrainSectionPresent{false};
    std::uint8_t Island{0u};
    std::uint16_t TileColor565{0u};
    bool InReferenceSet{false};
};

struct PreparedSceneManifest {
    SceneFileHeader Header{};
    std::string SourceMapSha256;
    std::string SourceObjectSha256;
    SceneManifestStats Stats;
    std::vector<SceneManifestRecord> Records;
};

struct ValidatedSceneHeader {
    std::uint64_t SectionCount{0u};
    std::uint64_t PrefixBytes{0u};
    std::uint64_t GridWidth{0u};
    std::uint64_t GridHeight{0u};
};

std::optional<ValidatedSceneHeader> ValidateSceneManifestHeader(
    const SceneFileHeader& header,
    std::string& detail) {
    constexpr std::size_t magicBytes = 15u;
    if (std::memcmp(header.Title, kObjMagic, magicBytes) != 0 ||
        header.Version != kObjVersionCurrent || header.FileSize <= 0 ||
        header.SectionCntX <= 0 ||
        header.SectionCntY <= 0 || header.SectionWidth <= 0 ||
        header.SectionHeight <= 0 || header.SectionObjNum <= 0) {
        detail = "invalid scene source header dimensions";
        return std::nullopt;
    }

    ValidatedSceneHeader result;
    std::uint64_t tableBytes = 0u;
    if (!CheckedMultiply(
            static_cast<std::uint64_t>(header.SectionCntX),
            static_cast<std::uint64_t>(header.SectionCntY),
            result.SectionCount) ||
        !CheckedMultiply(result.SectionCount, sizeof(SectionIndex),
                         tableBytes) ||
        !CheckedAdd(sizeof(SceneFileHeader), tableBytes,
                    result.PrefixBytes) ||
        result.PrefixBytes > static_cast<std::uint64_t>(header.FileSize) ||
        !CheckedMultiply(
            static_cast<std::uint64_t>(header.SectionCntX),
            static_cast<std::uint64_t>(header.SectionWidth),
            result.GridWidth) ||
        !CheckedMultiply(
            static_cast<std::uint64_t>(header.SectionCntY),
            static_cast<std::uint64_t>(header.SectionHeight),
            result.GridHeight)) {
        detail = "scene source header arithmetic overflow";
        return std::nullopt;
    }
    return result;
}

bool ValidatePlacedObject(const PlacedObject& object,
                          const SceneFileHeader& header,
                          const ValidatedSceneHeader& validated,
                          std::string& detail) {
    const SceneSourceKey& key = object.Source;
    if (key.SectionIndex >= validated.SectionCount ||
        key.SlotIndex >= static_cast<std::uint32_t>(header.SectionObjNum)) {
        detail = std::format("scene source key {} is outside header bounds",
                             SourceKeyText(key));
        return false;
    }
    const auto expectedX = static_cast<std::uint32_t>(
        key.SectionIndex % static_cast<std::uint32_t>(header.SectionCntX));
    const auto expectedY = static_cast<std::uint32_t>(
        key.SectionIndex / static_cast<std::uint32_t>(header.SectionCntX));
    if (object.SectionX != expectedX || object.SectionY != expectedY ||
        object.SectionWidth != static_cast<std::uint32_t>(header.SectionWidth) ||
        object.SectionHeight !=
            static_cast<std::uint32_t>(header.SectionHeight)) {
        detail = std::format(
            "scene source key {} has inconsistent section metadata",
            SourceKeyText(key));
        return false;
    }
    std::uint64_t recordEnd = 0u;
    if (key.ByteOffset < validated.PrefixBytes ||
        !CheckedAdd(key.ByteOffset, sizeof(SceneObjInfo), recordEnd) ||
        recordEnd > static_cast<std::uint64_t>(header.FileSize)) {
        detail = std::format("scene source key {} has impossible byte range",
                             SourceKeyText(key));
        return false;
    }
    if (!object.TryWorldX().has_value() || !object.TryWorldY().has_value()) {
        detail = std::format("scene source key {} has overflowing world position",
                             SourceKeyText(key));
        return false;
    }
    return true;
}

SceneManifestStatus PrepareSceneManifest(
    const SceneSelection& selection,
    IMapTileSource& terrain,
    const SceneManifestSourceContext& context,
    PreparedSceneManifest& prepared,
    std::string& detail) {
    if (!SameSceneHeader(selection.SourceHeader, context.ObjectHeader)) {
        detail = "selection header differs from independently parsed object header";
        return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
    }
    if (!IsLowerSha256(context.SourceMapSha256) ||
        !IsLowerSha256(context.SourceObjectSha256)) {
        detail = "source hashes must be lowercase 64-character SHA-256 values";
        return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
    }

    const auto header = ValidateSceneManifestHeader(
        selection.SourceHeader, detail);
    if (!header.has_value() ||
        header->GridWidth > std::numeric_limits<std::size_t>::max() ||
        header->GridHeight > std::numeric_limits<std::size_t>::max() ||
        terrain.GridWidth() < static_cast<std::size_t>(header->GridWidth) ||
        terrain.GridHeight() < static_cast<std::size_t>(header->GridHeight)) {
        if (header.has_value()) {
            detail = std::format(
                "terrain grid {}x{} does not cover scene extent {}x{}",
                terrain.GridWidth(), terrain.GridHeight(),
                header->GridWidth, header->GridHeight);
        }
        return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
    }

    const auto validatePartition = [&](const std::vector<PlacedObject>& records,
                                       std::int16_t requiredType,
                                       std::string_view name)
        -> SceneManifestStatus {
        std::optional<SceneSourceKey> previous;
        for (const PlacedObject& object : records) {
            const std::int16_t type = object.Info.Type();
            if (type != 0 && type != 1) {
                detail = std::format(
                    "unknown scene object type {} at source key {}",
                    type, SourceKeyText(object.Source));
                return SceneManifestStatus::UNKNOWN_OBJECT_TYPE;
            }
            if (type != requiredType) {
                detail = std::format(
                    "{} contains type {} at source key {}", name, type,
                    SourceKeyText(object.Source));
                return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
            }
            if (previous.has_value() && !(previous.value() < object.Source)) {
                detail = std::format(
                    "{} source keys are not strictly ordered at {}", name,
                    SourceKeyText(object.Source));
                return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
            }
            if (!ValidatePlacedObject(
                    object, selection.SourceHeader, *header, detail)) {
                return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
            }
            previous = object.Source;
        }
        return SceneManifestStatus::OK;
    };

    SceneManifestStatus status = validatePartition(
        selection.Models, 0, "models partition");
    if (status != SceneManifestStatus::OK) {
        return status;
    }
    status = validatePartition(
        selection.DeferredEffects, 1, "deferred-effects partition");
    if (status != SceneManifestStatus::OK) {
        return status;
    }

    for (std::size_t index = 0u; index < selection.ReferenceKeys.size();
         ++index) {
        if (index > 0u &&
            !(selection.ReferenceKeys[index - 1u] <
              selection.ReferenceKeys[index])) {
            detail = "reference source keys are not strictly ordered";
            return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
        }
        const SceneSourceKey& key = selection.ReferenceKeys[index];
        const auto model = std::lower_bound(
            selection.Models.begin(), selection.Models.end(), key,
            [](const PlacedObject& object, const SceneSourceKey& candidate) {
                return object.Source < candidate;
            });
        if (model == selection.Models.end() || model->Source != key) {
            detail = std::format(
                "reference source key {} is not a scene model",
                SourceKeyText(key));
            return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
        }
    }

    std::vector<std::pair<const PlacedObject*, bool>> ordered;
    ordered.reserve(selection.Models.size() +
                    selection.DeferredEffects.size());
    std::size_t modelIndex = 0u;
    std::size_t effectIndex = 0u;
    while (modelIndex < selection.Models.size() ||
           effectIndex < selection.DeferredEffects.size()) {
        if (modelIndex == selection.Models.size()) {
            ordered.emplace_back(
                &selection.DeferredEffects[effectIndex++], false);
            continue;
        }
        if (effectIndex == selection.DeferredEffects.size()) {
            ordered.emplace_back(&selection.Models[modelIndex++], true);
            continue;
        }
        const SceneSourceKey& modelKey =
            selection.Models[modelIndex].Source;
        const SceneSourceKey& effectKey =
            selection.DeferredEffects[effectIndex].Source;
        if (modelKey == effectKey) {
            detail = std::format("duplicate scene source key {}",
                                 SourceKeyText(modelKey));
            return SceneManifestStatus::INVALID_SOURCE_CONTEXT;
        }
        if (modelKey < effectKey) {
            ordered.emplace_back(&selection.Models[modelIndex++], true);
        }
        else {
            ordered.emplace_back(
                &selection.DeferredEffects[effectIndex++], false);
        }
    }

    SceneManifestStats actual;
    actual.SourceRecordCount = static_cast<std::uint64_t>(ordered.size());
    actual.SceneModelCount =
        static_cast<std::uint64_t>(selection.Models.size());
    actual.DeferredEffectCount =
        static_cast<std::uint64_t>(selection.DeferredEffects.size());
    actual.ReferenceObjectCount =
        static_cast<std::uint64_t>(selection.ReferenceKeys.size());

    if (actual.SourceRecordCount != context.ExpectedSourceRecordCount ||
        actual.SceneModelCount != context.ExpectedSceneModelCount ||
        actual.DeferredEffectCount != context.ExpectedDeferredEffectCount ||
        actual.ReferenceObjectCount != context.ExpectedReferenceObjectCount) {
        detail = std::format(
            "scene count mismatch actual={}/{}/{}/{} expected={}/{}/{}/{}",
            actual.SourceRecordCount, actual.SceneModelCount,
            actual.DeferredEffectCount, actual.ReferenceObjectCount,
            context.ExpectedSourceRecordCount,
            context.ExpectedSceneModelCount,
            context.ExpectedDeferredEffectCount,
            context.ExpectedReferenceObjectCount);
        return SceneManifestStatus::COUNT_MISMATCH;
    }

    PreparedSceneManifest temporary;
    temporary.Header = selection.SourceHeader;
    temporary.SourceMapSha256 = context.SourceMapSha256;
    temporary.SourceObjectSha256 = context.SourceObjectSha256;
    temporary.Records.reserve(ordered.size());
    for (const auto& [object, sceneModel] : ordered) {
        const std::int32_t worldX = object->WorldX();
        const std::int32_t worldY = object->WorldY();
        const TerrainAnchorSample terrainSample =
            SampleTerrainAnchor(terrain, worldX, worldY);
        if (!terrain.LastError().empty()) {
            detail = std::format(
                "terrain read failed at source key {}: {}",
                SourceKeyText(object->Source), terrain.LastError());
            return SceneManifestStatus::TERRAIN_READ_FAILED;
        }
        const bool inReferenceSet = sceneModel && std::binary_search(
            selection.ReferenceKeys.begin(), selection.ReferenceKeys.end(),
            object->Source);
        if (inReferenceSet) {
            ++actual.ReferenceIslandCounts[terrainSample.Island];
        }
        temporary.Records.push_back(SceneManifestRecord{
            object->Source,
            object->Info.Id(),
            object->Info.Type(),
            sceneModel,
            worldX,
            worldY,
            object->Info.HeightOff,
            object->Info.YawAngle,
            object->Info.Scale,
            terrainSample.SurfaceHeightCm,
            terrainSample.SurfaceHeightCm +
                static_cast<float>(object->Info.HeightOff),
            terrainSample.SectionPresent,
            terrainSample.Island,
            terrainSample.TileColor565,
            inReferenceSet});
    }
    if (actual.ReferenceIslandCounts !=
        context.ExpectedReferenceIslandCounts) {
        detail = "reference island count mismatch";
        return SceneManifestStatus::COUNT_MISMATCH;
    }
    temporary.Stats = actual;
    prepared = std::move(temporary);
    detail.clear();
    return SceneManifestStatus::OK;
}

void WriteSceneManifestStats(JsonWriter& json,
                             const SceneManifestStats& stats) {
    json.BeginObject();
    json.Key("sourceRecordCount");
    json.Value(static_cast<std::int64_t>(stats.SourceRecordCount));
    json.Key("sceneModelCount");
    json.Value(static_cast<std::int64_t>(stats.SceneModelCount));
    json.Key("deferredEffectCount");
    json.Value(static_cast<std::int64_t>(stats.DeferredEffectCount));
    json.Key("referenceObjectCount");
    json.Value(static_cast<std::int64_t>(stats.ReferenceObjectCount));
    json.Key("referenceIslandCounts");
    json.BeginObject();
    for (const auto& [island, count] : stats.ReferenceIslandCounts) {
        json.Key(std::format("{}", island));
        json.Value(static_cast<std::int64_t>(count));
    }
    json.EndObject();
    json.EndObject();
}

std::string SerializeSceneSourceManifest(
    const PreparedSceneManifest& manifest) {
    JsonWriter json;
    json.BeginObject();
    json.Key("schemaVersion"); json.Value(std::int64_t{2});
    json.Key("sourceMapSha256"); json.Value(manifest.SourceMapSha256);
    json.Key("sourceObjectSha256"); json.Value(manifest.SourceObjectSha256);
    json.Key("sectionCntX");
    json.Value(static_cast<std::int64_t>(manifest.Header.SectionCntX));
    json.Key("sectionCntY");
    json.Value(static_cast<std::int64_t>(manifest.Header.SectionCntY));
    json.Key("sectionWidth");
    json.Value(static_cast<std::int64_t>(manifest.Header.SectionWidth));
    json.Key("sectionHeight");
    json.Value(static_cast<std::int64_t>(manifest.Header.SectionHeight));
    json.Key("stats"); WriteSceneManifestStats(json, manifest.Stats);
    json.Key("records");
    json.BeginArray();
    for (const SceneManifestRecord& record : manifest.Records) {
        json.BeginObject();
        json.Key("sourceKey");
        json.BeginObject();
        json.Key("sectionIndex");
        json.Value(static_cast<std::int64_t>(record.Source.SectionIndex));
        json.Key("slotIndex");
        json.Value(static_cast<std::int64_t>(record.Source.SlotIndex));
        json.Key("byteOffset");
        json.Value(static_cast<std::int64_t>(record.Source.ByteOffset));
        json.EndObject();
        json.Key("modelId"); json.Value(static_cast<std::int64_t>(record.ModelId));
        json.Key("type"); json.Value(static_cast<std::int64_t>(record.Type));
        json.Key("disposition");
        json.Value(record.SceneModel ? "scene-model" : "deferred-effect");
        json.Key("x"); json.Value(static_cast<std::int64_t>(record.X));
        json.Key("y"); json.Value(static_cast<std::int64_t>(record.Y));
        json.Key("heightOff");
        json.Value(static_cast<std::int64_t>(record.HeightOff));
        json.Key("sourceYawDegrees");
        json.Value(static_cast<std::int64_t>(record.SourceYawDegrees));
        json.Key("sourceScaleDiagnostic");
        json.Value(static_cast<std::int64_t>(record.SourceScaleDiagnostic));
        json.Key("surfaceHeightCm");
        json.Value(static_cast<double>(record.SurfaceHeightCm));
        json.Key("zCm"); json.Value(static_cast<double>(record.ZCm));
        json.Key("terrainSectionPresent");
        json.Value(record.TerrainSectionPresent);
        json.Key("island");
        json.Value(static_cast<std::int64_t>(record.Island));
        json.Key("tileColor565");
        json.Value(static_cast<std::int64_t>(record.TileColor565));
        json.Key("inReferenceSet"); json.Value(record.InReferenceSet);
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    return json.Str();
}

class CanonicalSceneManifestReader {
public:
    explicit CanonicalSceneManifestReader(std::string_view bytes)
        : _bytes(bytes) {
    }

    bool Take(std::string_view expected) {
        if (!_bytes.substr(_offset).starts_with(expected)) {
            return false;
        }
        _offset += expected.size();
        return true;
    }

    template<typename T>
    bool Number(T expected) {
        return Take(std::format("{}", expected));
    }

    bool String(std::string_view expected) {
        return Take(std::format("\"{}\"", expected));
    }

    bool Boolean(bool expected) {
        return Take(expected ? "true" : "false");
    }

    [[nodiscard]] bool Done() const noexcept {
        return _offset == _bytes.size();
    }

private:
    std::string_view _bytes;
    std::size_t _offset{0u};
};

bool ReparseAndValidateSceneManifest(
    std::string_view bytes,
    const PreparedSceneManifest& manifest,
    std::string& detail) {
    CanonicalSceneManifestReader reader{bytes};
    const auto number = [&](std::string_view prefix, auto value) {
        return reader.Take(prefix) && reader.Number(value);
    };
    if (!number("{\"schemaVersion\":", 2) ||
        !reader.Take(",\"sourceMapSha256\":") ||
        !reader.String(manifest.SourceMapSha256) ||
        !reader.Take(",\"sourceObjectSha256\":") ||
        !reader.String(manifest.SourceObjectSha256) ||
        !number(",\"sectionCntX\":", manifest.Header.SectionCntX) ||
        !number(",\"sectionCntY\":", manifest.Header.SectionCntY) ||
        !number(",\"sectionWidth\":", manifest.Header.SectionWidth) ||
        !number(",\"sectionHeight\":", manifest.Header.SectionHeight) ||
        !number(",\"stats\":{\"sourceRecordCount\":",
                manifest.Stats.SourceRecordCount) ||
        !number(",\"sceneModelCount\":", manifest.Stats.SceneModelCount) ||
        !number(",\"deferredEffectCount\":",
                manifest.Stats.DeferredEffectCount) ||
        !number(",\"referenceObjectCount\":",
                manifest.Stats.ReferenceObjectCount) ||
        !reader.Take(",\"referenceIslandCounts\":{") ) {
        detail = "manifest read-back has invalid top-level schema or stats";
        return false;
    }
    bool firstIsland = true;
    for (const auto& [island, count] : manifest.Stats.ReferenceIslandCounts) {
        if ((!firstIsland && !reader.Take(",")) ||
            !reader.String(std::format("{}", island)) ||
            !reader.Take(":") || !reader.Number(count)) {
            detail = "manifest read-back has invalid island stats";
            return false;
        }
        firstIsland = false;
    }
    if (!reader.Take("}},\"records\":[")) {
        detail = "manifest read-back has invalid records member";
        return false;
    }
    for (std::size_t index = 0u; index < manifest.Records.size(); ++index) {
        const SceneManifestRecord& record = manifest.Records[index];
        if ((index > 0u && !reader.Take(",")) ||
            !number("{\"sourceKey\":{\"sectionIndex\":",
                    record.Source.SectionIndex) ||
            !number(",\"slotIndex\":", record.Source.SlotIndex) ||
            !number(",\"byteOffset\":", record.Source.ByteOffset) ||
            !number("},\"modelId\":", record.ModelId) ||
            !number(",\"type\":", record.Type) ||
            !reader.Take(",\"disposition\":") ||
            !reader.String(record.SceneModel
                               ? std::string_view{"scene-model"}
                               : std::string_view{"deferred-effect"}) ||
            !number(",\"x\":", record.X) ||
            !number(",\"y\":", record.Y) ||
            !number(",\"heightOff\":", record.HeightOff) ||
            !number(",\"sourceYawDegrees\":", record.SourceYawDegrees) ||
            !number(",\"sourceScaleDiagnostic\":",
                    record.SourceScaleDiagnostic) ||
            !number(",\"surfaceHeightCm\":",
                    static_cast<double>(record.SurfaceHeightCm)) ||
            !number(",\"zCm\":", static_cast<double>(record.ZCm)) ||
            !reader.Take(",\"terrainSectionPresent\":") ||
            !reader.Boolean(record.TerrainSectionPresent) ||
            !number(",\"island\":", record.Island) ||
            !number(",\"tileColor565\":", record.TileColor565) ||
            !reader.Take(",\"inReferenceSet\":") ||
            !reader.Boolean(record.InReferenceSet) ||
            !reader.Take("}")) {
            detail = std::format(
                "manifest read-back record {} differs from prepared source",
                index);
            return false;
        }
    }
    if (!reader.Take("]}") || !reader.Done()) {
        detail = "manifest read-back is truncated or has trailing data";
        return false;
    }
    detail.clear();
    return true;
}

std::string HashText(std::string_view bytes) {
    return Sha256Bytes(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

std::filesystem::path PhysicalParent(
    const std::filesystem::path& path,
    std::string& detail) {
    if (!path.parent_path().empty()) {
        return path.parent_path();
    }
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    if (error) {
        detail = std::format("cannot resolve manifest parent: {}",
                             error.message());
        return {};
    }
    return current;
}

bool ReadPhysicalText(const std::filesystem::path& path,
                      std::string& bytes,
                      std::string& detail) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (error || status.type() != std::filesystem::file_type::regular) {
        detail = std::format("manifest path is not a regular file: {} ({})",
                             path.generic_string(),
                             error ? error.message() : "invalid type");
        return false;
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        detail = std::format("cannot open manifest file for read: {}",
                             path.generic_string());
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>{input},
                 std::istreambuf_iterator<char>{});
    if (input.bad()) {
        detail = std::format("cannot read complete manifest file: {}",
                             path.generic_string());
        return false;
    }
    return true;
}

bool FlushPhysicalFile(const std::filesystem::path& path,
                       std::string& detail) {
#if defined(_WIN32)
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        detail = std::format("cannot open manifest file for flush: {} win32={}",
                             path.generic_string(), GetLastError());
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            file, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
        !FlushFileBuffers(file)) {
        const DWORD nativeError = GetLastError();
        CloseHandle(file);
        detail = std::format("cannot flush regular manifest file: {} win32={}",
                             path.generic_string(), nativeError);
        return false;
    }
    if (!CloseHandle(file)) {
        detail = std::format("cannot close flushed manifest file: {} win32={}",
                             path.generic_string(), GetLastError());
        return false;
    }
    return true;
#else
    const int descriptor =
        ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        detail = std::format("cannot open manifest file for flush: {} errno={}",
                             path.generic_string(), errno);
        return false;
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        ::fsync(descriptor) != 0) {
        const int nativeError = errno == 0 ? EINVAL : errno;
        ::close(descriptor);
        detail = std::format("cannot flush regular manifest file: {} errno={}",
                             path.generic_string(), nativeError);
        return false;
    }
    if (::close(descriptor) != 0) {
        detail = std::format("cannot close flushed manifest file: {} errno={}",
                             path.generic_string(), errno);
        return false;
    }
    return true;
#endif
}

bool SyncPhysicalDirectory(const std::filesystem::path& directory,
                           std::string& detail) {
#if defined(_WIN32)
    // Write-through moves plus flushed file handles are the Windows entry
    // durability barrier, matching the converter's other durable publisher.
    (void)directory;
    detail.clear();
    return true;
#else
    const int descriptor =
        ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        detail = std::format("cannot open manifest parent for fsync: {} errno={}",
                             directory.generic_string(), errno);
        return false;
    }
    if (::fsync(descriptor) != 0) {
        const int nativeError = errno;
        ::close(descriptor);
        detail = std::format("cannot fsync manifest parent: {} errno={}",
                             directory.generic_string(), nativeError);
        return false;
    }
    if (::close(descriptor) != 0) {
        detail = std::format("cannot close manifest parent: {} errno={}",
                             directory.generic_string(), errno);
        return false;
    }
    return true;
#endif
}

bool WriteExclusivePhysicalText(const std::filesystem::path& path,
                                std::string_view bytes,
                                std::string& detail,
                                bool& created,
                                bool flush = true,
                                std::optional<std::filesystem::perms> mode =
                                    std::nullopt) {
    created = false;
#if defined(_WIN32)
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        detail = std::format("cannot create exclusive manifest file: {} win32={}",
                             path.generic_string(), GetLastError());
        return false;
    }
    created = true;
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0u;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            const DWORD nativeError = GetLastError();
            CloseHandle(file);
            detail = std::format("cannot write manifest file: {} win32={}",
                                 path.generic_string(), nativeError);
            return false;
        }
        offset += written;
    }
    if (mode.has_value()) {
        // The exclusive writer stays open across the readonly transition so
        // FlushFileBuffers never has to reopen a readonly pathname.
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(
                file, FileBasicInfo, &basic, sizeof(basic))) {
            const DWORD nativeError = GetLastError();
            CloseHandle(file);
            detail = std::format(
                "cannot read exclusive manifest mode: {} win32={}",
                path.generic_string(), nativeError);
            return false;
        }
        constexpr std::filesystem::perms writeMask =
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_write |
            std::filesystem::perms::others_write;
        const bool readOnly = (*mode & writeMask) ==
                              std::filesystem::perms::none;
        if (readOnly) {
            basic.FileAttributes &= ~FILE_ATTRIBUTE_NORMAL;
            basic.FileAttributes |= FILE_ATTRIBUTE_READONLY;
        }
        else {
            basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            if (basic.FileAttributes == 0u) {
                basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }
        }
        if (!SetFileInformationByHandle(
                file, FileBasicInfo, &basic, sizeof(basic))) {
            const DWORD nativeError = GetLastError();
            CloseHandle(file);
            detail = std::format(
                "cannot apply exclusive manifest mode: {} win32={}",
                path.generic_string(), nativeError);
            return false;
        }
        FILE_BASIC_INFO readBack{};
        const bool readBackOk = GetFileInformationByHandleEx(
            file, FileBasicInfo, &readBack, sizeof(readBack));
        const bool modeMatches =
            readBackOk &&
            (((readBack.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0u) ==
             readOnly);
        if (!modeMatches) {
            const DWORD nativeError = readBackOk
                ? ERROR_INVALID_DATA
                : GetLastError();
            CloseHandle(file);
            detail = std::format(
                "cannot verify exclusive manifest mode: {} win32={}",
                path.generic_string(), nativeError);
            return false;
        }
    }
    if (flush && !FlushFileBuffers(file)) {
        const DWORD nativeError = GetLastError();
        CloseHandle(file);
        detail = std::format("cannot flush manifest file: {} win32={}",
                             path.generic_string(), nativeError);
        return false;
    }
    if (!CloseHandle(file)) {
        detail = std::format("cannot close manifest file: {} win32={}",
                             path.generic_string(), GetLastError());
        return false;
    }
    return true;
#else
    const int descriptor = ::open(
        path.c_str(), O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY | O_CLOEXEC,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        detail = std::format("cannot create exclusive manifest file: {} errno={}",
                             path.generic_string(), errno);
        return false;
    }
    created = true;
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) {
            const int nativeError = errno;
            ::close(descriptor);
            detail = std::format("cannot write manifest file: {} errno={}",
                                 path.generic_string(), nativeError);
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (mode.has_value()) {
        // fchmod does not revoke the access already granted to this exclusive
        // descriptor; fsync below therefore covers even an exact 0400 clone.
        const mode_t nativeMode = static_cast<mode_t>(
            static_cast<unsigned>(*mode & std::filesystem::perms::mask));
        struct stat readBack {};
        if (::fchmod(descriptor, nativeMode) != 0 ||
            ::fstat(descriptor, &readBack) != 0 ||
            (readBack.st_mode & static_cast<mode_t>(07777)) != nativeMode) {
            const int nativeError = errno == 0 ? EINVAL : errno;
            ::close(descriptor);
            detail = std::format(
                "cannot apply exclusive manifest mode: {} errno={}",
                path.generic_string(), nativeError);
            return false;
        }
    }
    if (flush && ::fsync(descriptor) != 0) {
        const int nativeError = errno;
        ::close(descriptor);
        detail = std::format("cannot flush manifest file: {} errno={}",
                             path.generic_string(), nativeError);
        return false;
    }
    if (::close(descriptor) != 0) {
        detail = std::format("cannot close manifest file: {} errno={}",
                             path.generic_string(), errno);
        return false;
    }
    return true;
#endif
}

bool ReplacePhysicalFile(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         std::string& detail) {
    if (source.parent_path().lexically_normal() !=
        destination.parent_path().lexically_normal()) {
        detail = "manifest replacement is not in one directory";
        return false;
    }
#if defined(_WIN32)
    constexpr DWORD moveFlags =
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    if (MoveFileExW(source.c_str(), destination.c_str(), moveFlags)) {
        return true;
    }
    DWORD nativeError = GetLastError();
    const DWORD destinationAttributes =
        GetFileAttributesW(destination.c_str());
    const bool readOnlyDestination =
        nativeError == ERROR_ACCESS_DENIED &&
        destinationAttributes != INVALID_FILE_ATTRIBUTES &&
        (destinationAttributes & FILE_ATTRIBUTE_READONLY) != 0u &&
        (destinationAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0u;
    if (readOnlyDestination) {
        // MoveFileEx cannot replace a readonly Windows destination. The prior
        // bytes/mode are already in the durable backup; restore the attribute
        // if the retry itself fails so ordinary failure can still roll back.
        DWORD writableAttributes =
            destinationAttributes & ~FILE_ATTRIBUTE_READONLY;
        if (writableAttributes == 0u) {
            writableAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        if (SetFileAttributesW(destination.c_str(), writableAttributes) &&
            MoveFileExW(source.c_str(), destination.c_str(), moveFlags)) {
            return true;
        }
        nativeError = GetLastError();
        SetFileAttributesW(destination.c_str(), destinationAttributes);
    }
    detail = std::format("cannot atomically replace manifest: {} -> {} win32={}",
                         source.generic_string(),
                         destination.generic_string(), nativeError);
    return false;
#else
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        detail = std::format("cannot atomically replace manifest: {} -> {} errno={}",
                             source.generic_string(),
                             destination.generic_string(), errno);
        return false;
    }
#endif
    return true;
}

bool RemovePhysicalFile(const std::filesystem::path& path,
                        std::string& detail) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory)) {
        return true;
    }
    if (error || status.type() != std::filesystem::file_type::regular) {
        detail = std::format("refusing to remove non-regular manifest artifact: {}",
                             path.generic_string());
        return false;
    }
#if defined(_WIN32)
    const HANDLE file = CreateFileW(
        path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        0u, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        detail = std::format(
            "cannot open manifest artifact for removal: {} win32={}",
            path.generic_string(), GetLastError());
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_BASIC_INFO basic{};
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!GetFileInformationByHandleEx(
            file, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
        !GetFileInformationByHandleEx(
            file, FileBasicInfo, &basic, sizeof(basic))) {
        const DWORD nativeError = GetLastError();
        CloseHandle(file);
        detail = std::format(
            "refusing to remove non-regular manifest artifact: {} win32={}",
            path.generic_string(), nativeError);
        return false;
    }
    FILE_BASIC_INFO originalBasic = basic;
    bool changedReadOnly = false;
    if ((basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0u) {
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (basic.FileAttributes == 0u) {
            basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        if (!SetFileInformationByHandle(
                file, FileBasicInfo, &basic, sizeof(basic))) {
            const DWORD nativeError = GetLastError();
            CloseHandle(file);
            detail = std::format(
                "cannot clear readonly manifest artifact: {} win32={}",
                path.generic_string(), nativeError);
            return false;
        }
        changedReadOnly = true;
    }
    if (!SetFileInformationByHandle(
            file, FileDispositionInfo, &disposition, sizeof(disposition))) {
        const DWORD nativeError = GetLastError();
        if (changedReadOnly) {
            SetFileInformationByHandle(
                file, FileBasicInfo, &originalBasic, sizeof(originalBasic));
        }
        CloseHandle(file);
        detail = std::format("cannot remove manifest artifact: {} win32={}",
                             path.generic_string(), nativeError);
        return false;
    }
    if (!CloseHandle(file)) {
        detail = std::format(
            "cannot close removed manifest artifact: {} win32={}",
            path.generic_string(), GetLastError());
        return false;
    }
#else
    if (::unlink(path.c_str()) != 0) {
        detail = std::format("cannot remove manifest artifact: {} errno={}",
                             path.generic_string(), errno);
        return false;
    }
#endif
    return true;
}

bool PhysicalPathAbsent(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    return status.type() == std::filesystem::file_type::not_found &&
           (!error || error == std::errc::no_such_file_or_directory);
}

bool ReadPhysicalMode(const std::filesystem::path& path,
                      std::filesystem::perms& mode,
                      std::string& detail) {
    std::error_code error;
    mode = std::filesystem::status(path, error).permissions();
    if (error || mode == std::filesystem::perms::unknown) {
        detail = std::format("cannot read manifest mode: {} ({})",
                             path.generic_string(),
                             error ? error.message() : "unknown mode");
        return false;
    }
    return true;
}

bool VerifyPhysicalFile(const std::filesystem::path& path,
                        std::string_view expectedBytes,
                        std::string_view expectedHash,
                        std::optional<std::filesystem::perms> expectedMode,
                        std::string& detail) {
    std::string bytes;
    if (!ReadPhysicalText(path, bytes, detail) || bytes != expectedBytes ||
        HashText(bytes) != expectedHash) {
        if (detail.empty()) {
            detail = std::format("manifest verification failed: {}",
                                 path.generic_string());
        }
        return false;
    }
    if (expectedMode.has_value()) {
        std::filesystem::perms actualMode{};
        if (!ReadPhysicalMode(path, actualMode, detail) ||
            actualMode != *expectedMode) {
            if (detail.empty()) {
                detail = std::format("manifest mode verification failed: {}",
                                     path.generic_string());
            }
            return false;
        }
    }
    return true;
}

bool CopyPhysicalFileExact(const std::filesystem::path& path,
                           std::string_view bytes,
                           std::filesystem::perms mode,
                           std::string_view hash,
                           bool& created,
                           std::string& detail) {
    return WriteExclusivePhysicalText(
               path, bytes, detail, created, true, mode) &&
           VerifyPhysicalFile(path, bytes, hash, mode, detail);
}

std::filesystem::path UniqueManifestArtifact(
    const std::filesystem::path& destination,
    std::string_view kind) {
    static std::atomic<std::uint64_t> sequence{0u};
    const std::uint64_t serial = sequence.fetch_add(1u) + 1u;
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return destination.parent_path() /
           std::format("{}.{}.{}.{}", destination.filename().string(), kind,
                       ticks, serial);
}

bool InjectedSceneManifestFault(
    const SceneManifestFaultInjector& injectFault,
    std::string_view point) {
    return injectFault &&
           injectFault(point) == SceneManifestFaultAction::FAIL;
}

struct PriorManifestState {
    bool Exists{false};
    std::string Bytes;
    std::string Sha256;
    std::filesystem::perms Mode{std::filesystem::perms::unknown};
    std::filesystem::path Backup;
    bool BackupOwned{false};
    bool BackupDurable{false};
};

bool VerifiedPriorBackupPresent(const PriorManifestState& prior,
                                std::string& detail) {
    if (!prior.BackupOwned || prior.Backup.empty()) {
        detail = "no owned prior backup path";
        return false;
    }
    return VerifyPhysicalFile(prior.Backup, prior.Bytes, prior.Sha256,
                              prior.Mode, detail);
}

bool EnsureDurablePriorBackup(PriorManifestState& prior,
                              const std::filesystem::path& parent,
                              const SceneManifestFaultInjector& injectFault,
                              std::string_view cleanupPoint,
                              std::string& detail) {
    prior.BackupDurable = false;
    if (!prior.BackupOwned || prior.Backup.empty()) {
        detail = "cannot recreate an unowned prior backup";
        return false;
    }

    // A cleanup-directory barrier can fail after unlink already succeeded.
    // Recreate the same owned name, then durably verify the net directory
    // state before any diagnostic is allowed to call the backup retained.
    if (PhysicalPathAbsent(prior.Backup)) {
        bool recreated = false;
        if (!CopyPhysicalFileExact(prior.Backup, prior.Bytes, prior.Mode,
                                   prior.Sha256, recreated, detail) ||
            !recreated) {
            if (detail.empty()) {
                detail = "prior backup recreation did not own a new file";
            }
            return false;
        }
    }
    else if (!VerifiedPriorBackupPresent(prior, detail)) {
        return false;
    }

    if (InjectedSceneManifestFault(injectFault, cleanupPoint)) {
        detail = std::format(
            "injected {} while durabilizing recreated prior backup",
            cleanupPoint);
        return false;
    }
    if (!SyncPhysicalDirectory(parent, detail) ||
        !VerifiedPriorBackupPresent(prior, detail)) {
        return false;
    }
    prior.BackupDurable = true;
    detail.clear();
    return true;
}

bool VerifyPublishedSceneManifest(
    const std::filesystem::path& path,
    std::string_view expectedBytes,
    std::string_view expectedHash,
    const PreparedSceneManifest& prepared,
    std::string& detail) {
    std::string readBack;
    return ReadPhysicalText(path, readBack, detail) &&
           readBack == expectedBytes && HashText(readBack) == expectedHash &&
           ReparseAndValidateSceneManifest(readBack, prepared, detail);
}

SceneManifestStatus NoPriorFailure(
    const std::filesystem::path& destination,
    const std::filesystem::path& temp,
    const std::filesystem::path& parent,
    const SceneManifestFaultInjector& injectFault,
    bool tempOwned,
    bool destinationOwned,
    std::string_view cause,
    std::string& detail) {
    bool recoveryRequired = false;
    std::string cleanupCause;
    if (InjectedSceneManifestFault(injectFault, "NO_PRIOR_REMOVE")) {
        recoveryRequired = true;
        cleanupCause = "injected NO_PRIOR_REMOVE";
    }
    else {
        std::string removeDetail;
        if ((tempOwned && !RemovePhysicalFile(temp, removeDetail)) ||
            (destinationOwned &&
             !RemovePhysicalFile(destination, removeDetail))) {
            recoveryRequired = true;
            cleanupCause = removeDetail;
        }
    }

    if (InjectedSceneManifestFault(
            injectFault, "NO_PRIOR_PARENT_FSYNC")) {
        recoveryRequired = true;
        if (!cleanupCause.empty()) {
            cleanupCause += "; ";
        }
        cleanupCause += "injected NO_PRIOR_PARENT_FSYNC";
    }
    else {
        std::string syncDetail;
        if (!SyncPhysicalDirectory(parent, syncDetail)) {
            recoveryRequired = true;
            if (!cleanupCause.empty()) {
                cleanupCause += "; ";
            }
            cleanupCause += syncDetail;
        }
    }

    if (InjectedSceneManifestFault(
            injectFault, "NO_PRIOR_VERIFY_ABSENT")) {
        recoveryRequired = true;
        if (!cleanupCause.empty()) {
            cleanupCause += "; ";
        }
        cleanupCause += "injected NO_PRIOR_VERIFY_ABSENT";
    }
    else if (!PhysicalPathAbsent(destination) || !PhysicalPathAbsent(temp)) {
        recoveryRequired = true;
        if (!cleanupCause.empty()) {
            cleanupCause += "; ";
        }
        cleanupCause += "destination or temp is not verified absent";
    }

    if (recoveryRequired) {
        detail = std::format(
            "RECOVERY_REQUIRED priorBackup=null destination={} temp={} "
            "cleanup/retry: remove both paths, fsync their parent, verify "
            "both absent, then rerun scene-manifest; cause={}; original={}",
            destination.generic_string(), temp.generic_string(),
            cleanupCause, cause);
        return SceneManifestStatus::RECOVERY_REQUIRED;
    }
    detail = std::string{cause};
    return SceneManifestStatus::WRITE_FAILED;
}

SceneManifestStatus PriorRecoveryRequired(
    const PriorManifestState& prior,
    const std::filesystem::path& destination,
    const std::filesystem::path& rollback,
    std::string_view cause,
    std::string& detail) {
    std::string backupDetail;
    if (prior.BackupDurable &&
        VerifiedPriorBackupPresent(prior, backupDetail)) {
        detail = std::format(
            "RECOVERY_REQUIRED priorBackup={} destination={} rollbackTemp={} "
            "cleanup/retry: atomically restore the verified prior backup, "
            "fsync the parent, verify bytes/mode/SHA, then rerun "
            "scene-manifest; cause={}",
            prior.Backup.generic_string(), destination.generic_string(),
            rollback.generic_string(), cause);
    }
    else {
        if (!prior.BackupDurable) {
            backupDetail = "backup durability was not established";
        }
        detail = std::format(
            "RECOVERY_REQUIRED priorBackup=unavailable expectedPath={} "
            "destination={} rollbackTemp={} cleanup/retry: preserve all "
            "remaining paths, restore independently verified prior "
            "bytes/mode/SHA, fsync the parent, then rerun scene-manifest; "
            "cause={}; backupCheck={}",
            prior.Backup.generic_string(), destination.generic_string(),
            rollback.generic_string(), cause, backupDetail);
    }
    return SceneManifestStatus::RECOVERY_REQUIRED;
}

SceneManifestStatus RollBackPriorDestination(
    PriorManifestState& prior,
    const std::filesystem::path& destination,
    const std::filesystem::path& parent,
    const SceneManifestFaultInjector& injectFault,
    std::string_view originalCause,
    std::string& detail) {
    const std::filesystem::path rollback =
        UniqueManifestArtifact(destination, "rollback");
    std::string operationDetail;
    bool rollbackOwned = false;
    if (!CopyPhysicalFileExact(rollback, prior.Bytes, prior.Mode,
                               prior.Sha256, rollbackOwned,
                               operationDetail) || !rollbackOwned) {
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format("rollback staging failed: {}; original={}",
                        operationDetail, originalCause),
            detail);
    }
    if (InjectedSceneManifestFault(injectFault, "ROLLBACK_REPLACE")) {
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format("injected ROLLBACK_REPLACE; original={}",
                        originalCause),
            detail);
    }
    if (!ReplacePhysicalFile(rollback, destination, operationDetail)) {
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format("rollback replace failed: {}; original={}",
                        operationDetail, originalCause),
            detail);
    }
    if (InjectedSceneManifestFault(
            injectFault, "ROLLBACK_PARENT_FSYNC")) {
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format("injected ROLLBACK_PARENT_FSYNC; original={}",
                        originalCause),
            detail);
    }
    if (!SyncPhysicalDirectory(parent, operationDetail)) {
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format("rollback parent fsync failed: {}; original={}",
                        operationDetail, originalCause),
            detail);
    }
    if (InjectedSceneManifestFault(injectFault, "ROLLBACK_VERIFY")) {
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format("injected ROLLBACK_VERIFY; original={}",
                        originalCause),
            detail);
    }
    if (!VerifyPhysicalFile(destination, prior.Bytes, prior.Sha256,
                            prior.Mode, operationDetail)) {
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format("rollback verification failed: {}; original={}",
                        operationDetail, originalCause),
            detail);
    }

    if (!RemovePhysicalFile(prior.Backup, operationDetail)) {
        std::string recoveryDetail;
        const bool recovered = EnsureDurablePriorBackup(
            prior, parent, injectFault,
            "PRIOR_ROLLBACK_CLEANUP_PARENT_FSYNC", recoveryDetail);
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format(
                "rollback cleanup failed: {}; prior backup {}: {}; "
                "original={}",
                operationDetail, recovered ? "verified" : "unavailable",
                recoveryDetail, originalCause),
            detail);
    }
    prior.BackupDurable = false;
    const bool injectedCleanupBarrier = InjectedSceneManifestFault(
        injectFault, "PRIOR_ROLLBACK_CLEANUP_PARENT_FSYNC");
    if (injectedCleanupBarrier ||
        !SyncPhysicalDirectory(parent, operationDetail)) {
        const std::string cleanupCause = injectedCleanupBarrier
            ? "injected PRIOR_ROLLBACK_CLEANUP_PARENT_FSYNC"
            : operationDetail;
        std::string recoveryDetail;
        const bool recovered = EnsureDurablePriorBackup(
            prior, parent, injectFault,
            "PRIOR_ROLLBACK_CLEANUP_PARENT_FSYNC", recoveryDetail);
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format(
                "rollback cleanup barrier failed: {}; prior backup {}: {}; "
                "original={}",
                cleanupCause, recovered ? "recreated and verified"
                                        : "unavailable",
                recoveryDetail, originalCause),
            detail);
    }
    if (!VerifyPhysicalFile(destination, prior.Bytes, prior.Sha256,
                            prior.Mode, operationDetail)) {
        std::string recoveryDetail;
        const bool recovered = EnsureDurablePriorBackup(
            prior, parent, injectFault,
            "PRIOR_ROLLBACK_CLEANUP_PARENT_FSYNC", recoveryDetail);
        return PriorRecoveryRequired(
            prior, destination, rollback,
            std::format(
                "rollback cleanup verification failed: {}; prior backup {}: "
                "{}; original={}",
                operationDetail, recovered ? "recreated and verified"
                                           : "unavailable",
                recoveryDetail, originalCause),
            detail);
    }
    detail = std::string{originalCause};
    return SceneManifestStatus::WRITE_FAILED;
}

SceneManifestStatus RestorePriorBeforeReplace(
    PriorManifestState& prior,
    const std::filesystem::path& destination,
    const std::filesystem::path& temp,
    const std::filesystem::path& parent,
    const SceneManifestFaultInjector& injectFault,
    bool tempOwned,
    std::string_view cause,
    std::string& detail) {
    std::string operationDetail;
    if (!VerifyPhysicalFile(destination, prior.Bytes, prior.Sha256,
                            prior.Mode, operationDetail)) {
        return PriorRecoveryRequired(
            prior, destination, temp,
            std::format("prior destination changed before replace: {}; "
                        "original={}", operationDetail, cause),
            detail);
    }
    if (tempOwned && !RemovePhysicalFile(temp, operationDetail)) {
        return PriorRecoveryRequired(
            prior, destination, temp,
            std::format("temp cleanup failed: {}; original={}",
                        operationDetail, cause),
            detail);
    }
    if (prior.BackupOwned && !PhysicalPathAbsent(prior.Backup) &&
        !RemovePhysicalFile(prior.Backup, operationDetail)) {
        std::string recoveryDetail;
        const bool recovered = EnsureDurablePriorBackup(
            prior, parent, injectFault,
            "PRIOR_PRE_REPLACE_CLEANUP_PARENT_FSYNC", recoveryDetail);
        return PriorRecoveryRequired(
            prior, destination, temp,
            std::format(
                "backup cleanup failed: {}; prior backup {}: {}; original={}",
                operationDetail, recovered ? "verified" : "unavailable",
                recoveryDetail, cause),
            detail);
    }
    if (prior.BackupOwned) {
        prior.BackupDurable = false;
    }
    const bool injectedCleanupBarrier = InjectedSceneManifestFault(
        injectFault, "PRIOR_PRE_REPLACE_CLEANUP_PARENT_FSYNC");
    if (injectedCleanupBarrier ||
        !SyncPhysicalDirectory(parent, operationDetail)) {
        const std::string cleanupCause = injectedCleanupBarrier
            ? "injected PRIOR_PRE_REPLACE_CLEANUP_PARENT_FSYNC"
            : operationDetail;
        std::string recoveryDetail;
        const bool recovered = EnsureDurablePriorBackup(
            prior, parent, injectFault,
            "PRIOR_PRE_REPLACE_CLEANUP_PARENT_FSYNC", recoveryDetail);
        return PriorRecoveryRequired(
            prior, destination, temp,
            std::format(
                "pre-replace cleanup barrier failed: {}; prior backup {}: {}; "
                "original={}",
                cleanupCause, recovered ? "recreated and verified"
                                        : "unavailable",
                recoveryDetail, cause),
            detail);
    }
    if (!VerifyPhysicalFile(destination, prior.Bytes, prior.Sha256,
                            prior.Mode, operationDetail) ||
        !PhysicalPathAbsent(temp) ||
        (!prior.Backup.empty() && !PhysicalPathAbsent(prior.Backup))) {
        if (operationDetail.empty()) {
            operationDetail = "publication artifact is not verified absent";
        }
        std::string recoveryDetail;
        const bool recovered = EnsureDurablePriorBackup(
            prior, parent, injectFault,
            "PRIOR_PRE_REPLACE_CLEANUP_PARENT_FSYNC", recoveryDetail);
        return PriorRecoveryRequired(
            prior, destination, temp,
            std::format(
                "pre-replace cleanup verification failed: {}; prior backup "
                "{}: {}; original={}",
                operationDetail, recovered ? "recreated and verified"
                                           : "unavailable",
                recoveryDetail, cause),
            detail);
    }
    detail = std::string{cause};
    return SceneManifestStatus::WRITE_FAILED;
}

SceneManifestStatus PublishPreparedSceneManifest(
    const PreparedSceneManifest& prepared,
    std::string_view json,
    const std::filesystem::path& destination,
    const SceneManifestFaultInjector& injectFault,
    std::string& detail) {
    const std::string intendedHash = HashText(json);
    const std::filesystem::path parent = PhysicalParent(destination, detail);
    if (parent.empty()) {
        return SceneManifestStatus::WRITE_FAILED;
    }
    std::error_code statusError;
    const std::filesystem::file_status parentStatus =
        std::filesystem::symlink_status(parent, statusError);
    if (statusError ||
        parentStatus.type() != std::filesystem::file_type::directory) {
        detail = std::format("manifest parent is not a directory: {}",
                             parent.generic_string());
        return SceneManifestStatus::WRITE_FAILED;
    }

    PriorManifestState prior;
    statusError.clear();
    const std::filesystem::file_status destinationStatus =
        std::filesystem::symlink_status(destination, statusError);
    if (destinationStatus.type() == std::filesystem::file_type::regular &&
        !statusError) {
        prior.Exists = true;
        if (!ReadPhysicalText(destination, prior.Bytes, detail) ||
            !ReadPhysicalMode(destination, prior.Mode, detail)) {
            return SceneManifestStatus::WRITE_FAILED;
        }
        prior.Sha256 = HashText(prior.Bytes);
    }
    else if (destinationStatus.type() !=
                 std::filesystem::file_type::not_found ||
             (statusError &&
              statusError != std::errc::no_such_file_or_directory)) {
        detail = std::format(
            "manifest destination is not a regular file or absent: {}",
            destination.generic_string());
        return SceneManifestStatus::WRITE_FAILED;
    }

    const std::filesystem::path temp =
        UniqueManifestArtifact(destination, "tmp");
    bool replaced = false;
    bool tempOwned = false;
    const auto failure = [&](std::string_view cause) {
        if (!prior.Exists) {
            return NoPriorFailure(destination, temp, parent, injectFault,
                                  tempOwned, replaced, cause, detail);
        }
        if (replaced) {
            return RollBackPriorDestination(
                prior, destination, parent, injectFault, cause, detail);
        }
        return RestorePriorBeforeReplace(
            prior, destination, temp, parent, injectFault, tempOwned, cause,
            detail);
    };

    if (!WriteExclusivePhysicalText(
            temp, json, detail, tempOwned, false)) {
        const std::string cause = detail;
        return failure(cause);
    }
    if (InjectedSceneManifestFault(
            injectFault, "AFTER_TEMP_SERIALIZATION")) {
        return failure("injected AFTER_TEMP_SERIALIZATION");
    }
    if (!FlushPhysicalFile(temp, detail)) {
        const std::string cause = detail;
        return failure(cause);
    }
    if (InjectedSceneManifestFault(injectFault, "AFTER_TEMP_FSYNC")) {
        return failure("injected AFTER_TEMP_FSYNC");
    }
    std::string tempBytes;
    if (!ReadPhysicalText(temp, tempBytes, detail) || tempBytes != json ||
        HashText(tempBytes) != intendedHash ||
        !ReparseAndValidateSceneManifest(tempBytes, prepared, detail)) {
        const std::string cause = detail.empty()
            ? "temp manifest read-back mismatch"
            : detail;
        return failure(cause);
    }
    if (prior.Exists) {
        prior.Backup = UniqueManifestArtifact(destination, "backup");
        if (!CopyPhysicalFileExact(prior.Backup, prior.Bytes, prior.Mode,
                                   prior.Sha256, prior.BackupOwned, detail) ||
            !VerifyPhysicalFile(destination, prior.Bytes, prior.Sha256,
                                prior.Mode, detail)) {
            const std::string cause = detail;
            return failure(cause);
        }
        if (InjectedSceneManifestFault(
                injectFault, "BACKUP_PARENT_FSYNC")) {
            return failure("injected BACKUP_PARENT_FSYNC");
        }
        if (!SyncPhysicalDirectory(parent, detail)) {
            const std::string cause = detail;
            return failure(cause);
        }
        if (!VerifyPhysicalFile(prior.Backup, prior.Bytes, prior.Sha256,
                                prior.Mode, detail)) {
            const std::string cause = detail;
            return failure(cause);
        }
        prior.BackupDurable = true;
        if (!VerifyPhysicalFile(destination, prior.Bytes, prior.Sha256,
                                prior.Mode, detail)) {
            const std::string cause = detail;
            return failure(cause);
        }
    }

    if (InjectedSceneManifestFault(injectFault, "BEFORE_REPLACE")) {
        return failure("injected BEFORE_REPLACE");
    }
    if (InjectedSceneManifestFault(injectFault, "DESTINATION_REPLACE")) {
        return failure("injected DESTINATION_REPLACE");
    }
    if (!ReplacePhysicalFile(temp, destination, detail)) {
        const std::string cause = detail;
        return failure(cause);
    }
    replaced = true;
    tempOwned = false;
    if (InjectedSceneManifestFault(injectFault, "AFTER_REPLACE")) {
        return failure("injected AFTER_REPLACE");
    }
    if (!VerifyPublishedSceneManifest(destination, json, intendedHash,
                                      prepared, detail)) {
        const std::string cause = detail.empty()
            ? "published manifest read-back mismatch"
            : detail;
        return failure(cause);
    }
    if (InjectedSceneManifestFault(
            injectFault, "POST_REPLACE_PARENT_FSYNC")) {
        return failure("injected POST_REPLACE_PARENT_FSYNC");
    }
    if (!SyncPhysicalDirectory(parent, detail)) {
        const std::string cause = detail;
        return failure(cause);
    }
    if (!VerifyPublishedSceneManifest(destination, json, intendedHash,
                                      prepared, detail)) {
        const std::string cause = detail.empty()
            ? "published manifest read-back mismatch"
            : detail;
        return failure(cause);
    }

    // The durable publication is committed. Backup cleanup cannot downgrade
    // it; an injected or physical cleanup failure retains/recreates evidence.
    if (prior.Exists) {
        if (InjectedSceneManifestFault(injectFault, "BACKUP_REMOVE")) {
            std::string backupDetail;
            if (prior.BackupDurable &&
                VerifiedPriorBackupPresent(prior, backupDetail)) {
                detail = std::format(
                    "manifest committed; retained verified prior backup {}",
                    prior.Backup.generic_string());
            }
            else {
                if (!prior.BackupDurable) {
                    backupDetail =
                        "backup durability was not established";
                }
                detail = std::format(
                    "manifest committed; prior backup unavailable at {}: {}",
                    prior.Backup.generic_string(), backupDetail);
            }
            return SceneManifestStatus::OK;
        }
        std::string cleanupDetail;
        if (!RemovePhysicalFile(prior.Backup, cleanupDetail)) {
            std::string recoveryDetail;
            const bool recovered = EnsureDurablePriorBackup(
                prior, parent, injectFault,
                "POST_COMMIT_BACKUP_CLEANUP_PARENT_FSYNC", recoveryDetail);
            if (recovered) {
                detail = std::format(
                    "manifest committed; backup cleanup failed, retained "
                    "verified prior backup {}: {}",
                    prior.Backup.generic_string(), cleanupDetail);
            }
            else {
                detail = std::format(
                    "manifest committed; backup cleanup failed and prior "
                    "backup is unavailable at {}: {}; backupCheck={}",
                    prior.Backup.generic_string(), cleanupDetail,
                    recoveryDetail);
            }
            return SceneManifestStatus::OK;
        }
        prior.BackupDurable = false;
        const bool injectedCleanupBarrier = InjectedSceneManifestFault(
            injectFault, "POST_COMMIT_BACKUP_CLEANUP_PARENT_FSYNC");
        if (injectedCleanupBarrier ||
            !SyncPhysicalDirectory(parent, cleanupDetail)) {
            const std::string cleanupCause = injectedCleanupBarrier
                ? "injected POST_COMMIT_BACKUP_CLEANUP_PARENT_FSYNC"
                : cleanupDetail;
            std::string recoveryDetail;
            const bool recovered = EnsureDurablePriorBackup(
                prior, parent, injectFault,
                "POST_COMMIT_BACKUP_CLEANUP_PARENT_FSYNC", recoveryDetail);
            if (recovered) {
                detail = std::format(
                    "manifest committed; backup cleanup barrier failed, "
                    "retained verified prior backup {}: {}",
                    prior.Backup.generic_string(), cleanupCause);
            }
            else {
                detail = std::format(
                    "manifest committed; backup cleanup barrier failed and "
                    "prior backup is unavailable at {}: {}; backupCheck={}",
                    prior.Backup.generic_string(), cleanupCause,
                    recoveryDetail);
            }
            return SceneManifestStatus::OK;
        }
    }
    detail.clear();
    return SceneManifestStatus::OK;
}

} // namespace

MapWriteStatus WriteTerrain(const MapTerrain& terrain,
                            const std::filesystem::path& basePath,
                            std::string& detail) {
    const std::size_t tileCount = terrain.Tiles.size();

    std::vector<std::uint16_t> heights(tileCount);
    std::vector<std::uint8_t> blocks(tileCount * 4);
    std::vector<std::uint16_t> regions(tileCount);

    std::int32_t minHeight = std::numeric_limits<std::int32_t>::max();
    std::int32_t maxHeight = std::numeric_limits<std::int32_t>::lowest();
    std::size_t blockedTiles = 0;

    for (std::size_t i = 0; i < tileCount; ++i) {
        const MapTile& tile = terrain.Tiles[i];

        const std::int32_t raw = tile.Height;
        minHeight = raw < minHeight ? raw : minHeight;
        maxHeight = raw > maxHeight ? raw : maxHeight;

        // Знаковый байт -> беззнаковые 16 бит без потери диапазона.
        heights[i] = static_cast<std::uint16_t>((raw + 128) * 256);

        bool anyBlocked = false;
        for (std::size_t q = 0; q < 4; ++q) {
            blocks[i * 4 + q] = tile.Block[q];
            anyBlocked = anyBlocked || tile.Block[q] != 0;
        }
        if (anyBlocked) {
            ++blockedTiles;
        }

        regions[i] = static_cast<std::uint16_t>(tile.Region);
    }

    if (!WriteBinary(WithSuffix(basePath, ".height.r16"),
                     heights.data(), heights.size() * sizeof(std::uint16_t))) {
        detail = "не удалось записать .height.r16";
        return MapWriteStatus::WRITE_FAILED;
    }

    if (!WriteBinary(WithSuffix(basePath, ".block.raw"), blocks.data(), blocks.size())) {
        detail = "не удалось записать .block.raw";
        return MapWriteStatus::WRITE_FAILED;
    }

    if (!WriteBinary(WithSuffix(basePath, ".region.raw"),
                     regions.data(), regions.size() * sizeof(std::uint16_t))) {
        detail = "не удалось записать .region.raw";
        return MapWriteStatus::WRITE_FAILED;
    }

    JsonWriter json;
    json.BeginObject();
    json.Key("mapFlag");
    json.Value(static_cast<std::int64_t>(terrain.Header.MapFlag));
    json.Key("width");
    json.Value(static_cast<std::int64_t>(terrain.Header.Width));
    json.Key("height");
    json.Value(static_cast<std::int64_t>(terrain.Header.Height));
    // Размер растров ниже — именно gridWidth x gridHeight, а не width x height.
    json.Key("gridWidth");
    json.Value(static_cast<std::int64_t>(terrain.GridWidth));
    json.Key("gridHeight");
    json.Value(static_cast<std::int64_t>(terrain.GridHeight));
    json.Key("sectionWidth");
    json.Value(static_cast<std::int64_t>(terrain.Header.SectionWidth));
    json.Key("sectionHeight");
    json.Value(static_cast<std::int64_t>(terrain.Header.SectionHeight));
    json.Key("sectionsPresent");
    json.Value(static_cast<std::int64_t>(terrain.PresentSections));
    json.Key("sectionsTotal");
    json.Value(static_cast<std::int64_t>(terrain.TotalSections));
    json.Key("blockedTiles");
    json.Value(static_cast<std::int64_t>(blockedTiles));

    json.Key("heightRangeRaw");
    json.BeginArray();
    json.Value(static_cast<std::int64_t>(tileCount > 0 ? minHeight : 0));
    json.Value(static_cast<std::int64_t>(tileCount > 0 ? maxHeight : 0));
    json.EndArray();

    // Единица высоты в исходных данных — 10 см.
    json.Key("heightUnitMeters");
    json.Value(0.1);
    json.Key("heightEncoding");
    json.Value("uint16 = (rawHeight + 128) * 256");
    json.EndObject();

    if (!WriteText(WithSuffix(basePath, ".terrain.json"), json.Str())) {
        detail = "не удалось записать .terrain.json";
        return MapWriteStatus::WRITE_FAILED;
    }

    detail.clear();
    return MapWriteStatus::OK;
}

MapWriteStatus WriteSceneManifest(const SceneObjects& scene,
                                  const std::filesystem::path& basePath,
                                  std::string& detail) {
    JsonWriter json;
    json.BeginObject();

    json.Key("version");
    json.Value(static_cast<std::int64_t>(scene.Header.Version));
    json.Key("sectionCntX");
    json.Value(static_cast<std::int64_t>(scene.Header.SectionCntX));
    json.Key("sectionCntY");
    json.Value(static_cast<std::int64_t>(scene.Header.SectionCntY));
    json.Key("sectionWidth");
    json.Value(static_cast<std::int64_t>(scene.Header.SectionWidth));
    json.Key("sectionHeight");
    json.Value(static_cast<std::int64_t>(scene.Header.SectionHeight));
    json.Key("nonEmptySections");
    json.Value(static_cast<std::int64_t>(scene.NonEmptySections));
    json.Key("objectCount");
    json.Value(static_cast<std::int64_t>(scene.Objects.size()));

    json.Key("objects");
    json.BeginArray();
    for (const PlacedObject& placed : scene.Objects) {
        json.BeginObject();
        json.Key("modelId");
        json.Value(static_cast<std::int64_t>(placed.Info.Id()));
        json.Key("type");
        json.Value(static_cast<std::int64_t>(placed.Info.Type()));
        // Мировые координаты, а не сырые: в файле они относительны началу
        // секции.
        json.Key("x");
        json.Value(static_cast<std::int64_t>(placed.WorldX()));
        json.Key("y");
        json.Value(static_cast<std::int64_t>(placed.WorldY()));
        json.Key("sectionX");
        json.Value(static_cast<std::int64_t>(placed.SectionX));
        json.Key("sectionY");
        json.Value(static_cast<std::int64_t>(placed.SectionY));
        json.Key("heightOff");
        json.Value(static_cast<std::int64_t>(placed.Info.HeightOff));
        json.Key("yaw");
        json.Value(static_cast<std::int64_t>(placed.Info.YawAngle));
        json.Key("scale");
        json.Value(static_cast<std::int64_t>(placed.Info.Scale));
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();

    if (!WriteText(WithSuffix(basePath, ".objects.json"), json.Str())) {
        detail = "не удалось записать .objects.json";
        return MapWriteStatus::WRITE_FAILED;
    }

    detail.clear();
    return MapWriteStatus::OK;
}

SceneManifestStatus WriteSceneSourceManifestForTesting(
    const SceneSelection& selection,
    IMapTileSource& terrain,
    const SceneManifestSourceContext& context,
    const std::filesystem::path& basePath,
    SceneManifestStats& stats,
    std::string& detail,
    const SceneManifestFaultInjector& injectFault) {
    PreparedSceneManifest prepared;
    const SceneManifestStatus preparation = PrepareSceneManifest(
        selection, terrain, context, prepared, detail);
    if (preparation != SceneManifestStatus::OK) {
        return preparation;
    }

    const std::string json = SerializeSceneSourceManifest(prepared);
    if (!ReparseAndValidateSceneManifest(json, prepared, detail)) {
        return SceneManifestStatus::WRITE_FAILED;
    }
    const std::filesystem::path destination =
        WithSuffix(basePath, ".objects.json");
    const SceneManifestStatus publication = PublishPreparedSceneManifest(
        prepared, json, destination, injectFault, detail);
    if (publication == SceneManifestStatus::OK) {
        stats = prepared.Stats;
    }
    return publication;
}

SceneManifestStatus WriteSceneSourceManifest(
    const SceneSelection& selection,
    IMapTileSource& terrain,
    const SceneManifestSourceContext& context,
    const std::filesystem::path& basePath,
    SceneManifestStats& stats,
    std::string& detail) {
    return WriteSceneSourceManifestForTesting(
        selection, terrain, context, basePath, stats, detail, {});
}


MapWriteStatus WriteTerrainLayers(const MapTerrain& terrain,
                                  const std::filesystem::path& basePath,
                                  std::string& detail) {
    // Восемь байт на клетку: четыре пары «номер текстуры, прозрачность».
    // Плоский двоичный формат выбран по той же причине, что и для карты
    // высот: Unreal читает сырые данные без кодеков, а конвертер обходится
    // без внешних зависимостей.
    std::vector<std::uint8_t> layers;
    layers.resize(terrain.Tiles.size() * 8);

    for (std::size_t i = 0; i < terrain.Tiles.size(); ++i) {
        const MapTile& tile = terrain.Tiles[i];
        std::uint8_t* out = layers.data() + i * 8;

        out[0] = tile.BaseTex;
        out[1] = kBaseAlphaOpaque;
        out[2] = static_cast<std::uint8_t>((tile.TileInfo >> kTileTex1Shift) & kTileTexMask);
        out[3] = static_cast<std::uint8_t>((tile.TileInfo >> kTileAlpha1Shift) & kTileAlphaMask);
        out[4] = static_cast<std::uint8_t>((tile.TileInfo >> kTileTex2Shift) & kTileTexMask);
        out[5] = static_cast<std::uint8_t>((tile.TileInfo >> kTileAlpha2Shift) & kTileAlphaMask);
        out[6] = static_cast<std::uint8_t>((tile.TileInfo >> kTileTex3Shift) & kTileTexMask);
        out[7] = static_cast<std::uint8_t>((tile.TileInfo >> kTileAlpha3Shift) & kTileAlphaMask);
    }

    std::filesystem::path path = basePath;
    path.replace_filename(basePath.filename().string() + ".layers.raw");

    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        detail = "не удалось открыть файл слоёв";
        return MapWriteStatus::WRITE_FAILED;
    }
    stream.write(reinterpret_cast<const char*>(layers.data()),
                 static_cast<std::streamsize>(layers.size()));
    if (!stream) {
        detail = "не удалось записать слои";
        return MapWriteStatus::WRITE_FAILED;
    }

    detail.clear();
    return MapWriteStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
