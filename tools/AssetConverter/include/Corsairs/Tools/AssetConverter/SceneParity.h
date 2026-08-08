#pragma once

#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

struct SceneSelection {
    SceneFileHeader SourceHeader{};
    std::vector<PlacedObject> Models;
    std::vector<PlacedObject> DeferredEffects;
    std::vector<SceneSourceKey> ReferenceKeys;
};

enum class SceneRecordDisposition : std::uint8_t {
    SceneModel,
    DeferredEffect,
};

enum class SceneSelectionStatus : std::uint32_t {
    OK = 0,
    UNKNOWN_OBJECT_TYPE,
    INVALID_SOURCE_KEY,
};

struct ReferenceZone {
    std::int32_t CenterX{223325};
    std::int32_t CenterY{278475};
    std::int32_t RadiusCm{8000};
};

[[nodiscard]] SceneSelectionStatus BuildSceneSelection(
    const SceneObjects& scene,
    const ReferenceZone& zone,
    SceneSelection& output,
    std::string& detail);

} // namespace Corsairs::Tools::AssetConverter
