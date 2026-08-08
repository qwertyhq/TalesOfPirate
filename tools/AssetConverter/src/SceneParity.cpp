#include "Corsairs/Tools/AssetConverter/SceneParity.h"

#include <format>
#include <limits>
#include <optional>
#include <utility>

namespace Corsairs::Tools::AssetConverter {

namespace {

bool CheckedMultiply(std::uint64_t left,
                     std::uint64_t right,
                     std::uint64_t& result) {
    if (left != 0u && right > std::numeric_limits<std::uint64_t>::max() / left) {
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

std::string KeyText(const SceneSourceKey& key) {
    return std::format("({},{},{})",
                       key.SectionIndex,
                       key.SlotIndex,
                       key.ByteOffset);
}

SceneSelectionStatus InvalidKey(const SceneSourceKey& key,
                                std::string_view reason,
                                std::string& detail) {
    detail = std::format("invalid scene source key {}: {}", KeyText(key), reason);
    return SceneSelectionStatus::INVALID_SOURCE_KEY;
}

bool IsInsideReferenceZone(std::int32_t worldX,
                           std::int32_t worldY,
                           const ReferenceZone& zone) {
    const std::int64_t radius = zone.RadiusCm;
    const std::int64_t deltaX =
        static_cast<std::int64_t>(worldX) - static_cast<std::int64_t>(zone.CenterX);
    const std::int64_t deltaY =
        static_cast<std::int64_t>(worldY) - static_cast<std::int64_t>(zone.CenterY);
    const std::int64_t absoluteX = deltaX < 0 ? -deltaX : deltaX;
    const std::int64_t absoluteY = deltaY < 0 ? -deltaY : deltaY;
    if (absoluteX > radius || absoluteY > radius) {
        return false;
    }
    return deltaX * deltaX + deltaY * deltaY <= radius * radius;
}

struct ValidatedHeader {
    std::uint64_t SectionCount{0};
    std::uint64_t PrefixBytes{0};
};

std::optional<ValidatedHeader> ValidateHeader(const SceneFileHeader& header) {
    if (header.FileSize <= 0 ||
        header.SectionCntX <= 0 || header.SectionCntY <= 0 ||
        header.SectionWidth <= 0 || header.SectionHeight <= 0 ||
        header.SectionObjNum <= 0) {
        return std::nullopt;
    }

    ValidatedHeader validated;
    std::uint64_t tableBytes = 0u;
    if (!CheckedMultiply(static_cast<std::uint64_t>(header.SectionCntX),
                         static_cast<std::uint64_t>(header.SectionCntY),
                         validated.SectionCount) ||
        !CheckedMultiply(validated.SectionCount, sizeof(SectionIndex), tableBytes) ||
        !CheckedAdd(sizeof(SceneFileHeader), tableBytes, validated.PrefixBytes) ||
        validated.PrefixBytes > static_cast<std::uint64_t>(header.FileSize)) {
        return std::nullopt;
    }
    return validated;
}

} // namespace

SceneSelectionStatus BuildSceneSelection(const SceneObjects& scene,
                                         const ReferenceZone& zone,
                                         SceneSelection& output,
                                         std::string& detail) {
    if (zone.RadiusCm < 0) {
        detail = "invalid reference zone: negative radius";
        return SceneSelectionStatus::INVALID_SOURCE_KEY;
    }

    const auto header = ValidateHeader(scene.Header);
    if (!header.has_value()) {
        detail = "invalid scene source header";
        return SceneSelectionStatus::INVALID_SOURCE_KEY;
    }

    SceneSelection temporary;
    temporary.SourceHeader = scene.Header;
    temporary.Models.reserve(scene.Objects.size());
    temporary.DeferredEffects.reserve(scene.Objects.size());
    temporary.ReferenceKeys.reserve(scene.Objects.size());

    std::optional<SceneSourceKey> previousKey;
    for (const PlacedObject& object : scene.Objects) {
        const SceneSourceKey& key = object.Source;
        if (previousKey.has_value() && !(previousKey.value() < key)) {
            return InvalidKey(key, "keys are not strictly ordered", detail);
        }
        if (key.SectionIndex >= header->SectionCount ||
            key.SlotIndex >= static_cast<std::uint32_t>(scene.Header.SectionObjNum)) {
            return InvalidKey(key, "section or slot is outside the source header", detail);
        }

        const auto expectedSectionX = static_cast<std::uint32_t>(
            key.SectionIndex % static_cast<std::uint32_t>(scene.Header.SectionCntX));
        const auto expectedSectionY = static_cast<std::uint32_t>(
            key.SectionIndex / static_cast<std::uint32_t>(scene.Header.SectionCntX));
        if (object.SectionX != expectedSectionX || object.SectionY != expectedSectionY ||
            object.SectionWidth != static_cast<std::uint32_t>(scene.Header.SectionWidth) ||
            object.SectionHeight != static_cast<std::uint32_t>(scene.Header.SectionHeight)) {
            return InvalidKey(key, "record section metadata does not match the header", detail);
        }

        std::uint64_t recordEnd = 0u;
        if (key.ByteOffset < header->PrefixBytes ||
            !CheckedAdd(key.ByteOffset, sizeof(SceneObjInfo), recordEnd) ||
            recordEnd > static_cast<std::uint64_t>(scene.Header.FileSize)) {
            return InvalidKey(key, "record range is outside the source file", detail);
        }

        const auto worldX = object.TryWorldX();
        const auto worldY = object.TryWorldY();
        if (!worldX.has_value() || !worldY.has_value()) {
            return InvalidKey(key, "world coordinate is outside int32", detail);
        }

        const std::int16_t type = object.Info.Type();
        if (type == 0) {
            temporary.Models.push_back(object);
            if (IsInsideReferenceZone(*worldX, *worldY, zone)) {
                temporary.ReferenceKeys.push_back(key);
            }
        }
        else if (type == 1) {
            temporary.DeferredEffects.push_back(object);
        }
        else {
            detail = std::format("unknown scene object type {} at source key {}",
                                 type, KeyText(key));
            return SceneSelectionStatus::UNKNOWN_OBJECT_TYPE;
        }

        previousKey = key;
    }

    output = std::move(temporary);
    detail.clear();
    return SceneSelectionStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
