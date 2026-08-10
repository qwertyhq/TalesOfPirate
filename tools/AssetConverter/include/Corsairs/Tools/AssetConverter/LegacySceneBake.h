#pragma once

#include "Corsairs/Tools/AssetConverter/LgoParser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

enum class LegacyBakeStatus : std::uint32_t {
    OK = 0,
    INVALID_TICK,
    EMPTY_CONTROLLER,
    UNSUPPORTED_BONE_CONTROLLER,
    UNSUPPORTED_TEXTURE_STAGE,
    INVALID_SUBSET,
    INVALID_VERTEX_INDEX,
    CONFLICTING_SHARED_VERTEX,
    UNSUPPORTED_MATERIAL_CONTROLLER,
    UNSUPPORTED_TEXTURE_IMAGE_CONTROLLER,
    NON_FINITE_CONTROLLER,
};

enum class LegacyBoneCapturePolicy : std::uint8_t {
    Reject,
    StaticReferencePose,
    PreserveAnimated,
};

// После N завершённых owned update-тактов первый показанный source frame — 0,
// поэтому capture tick N выбирает `(N - 1) % frameCount`.
[[nodiscard]] std::uint32_t LegacySampleFrameAfterOwnedTicks(
    std::uint32_t completedTicks,
    std::uint32_t frameCount);

// Сворачивает embedded MAT/TEXUV в статическое состояние одного кадра.
// MAT заменяет MatModel так же, как runtime primitive заменяет local matrix;
// TEXUV stage 0 применяется к вершинам соответствующего subset.
// BONE по умолчанию запрещён. StaticReferencePose намеренно удаляет skin,
// PreserveAnimated сохраняет полную дорожку и фиксирует sample tick в metadata.
[[nodiscard]] LegacyBakeStatus BakeLegacyCaptureState(
    LgoGeomObj& object,
    std::uint32_t completedTicks,
    std::string& detail,
    LegacyBoneCapturePolicy bonePolicy = LegacyBoneCapturePolicy::Reject);

// Атомарно сворачивает все части .lmo, затем заново вычисляет MatModel из
// sampled local MAT и цепочки ParentId. При ошибке исходный vector не меняется.
[[nodiscard]] LegacyBakeStatus BakeLegacyModelCaptureState(
    std::vector<LgoGeomObj>& objects,
    std::uint32_t completedTicks,
    std::string& detail,
    LegacyBoneCapturePolicy bonePolicy = LegacyBoneCapturePolicy::Reject);

} // namespace Corsairs::Tools::AssetConverter
