#include "Corsairs/Tools/AssetConverter/LegacySceneBake.h"

#include "Corsairs/Tools/AssetConverter/LmoParser.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

bool AllFinite(const std::array<float, 16>& matrix) {
    return std::all_of(matrix.begin(), matrix.end(),
                       [](float value) { return std::isfinite(value); });
}

} // namespace

std::uint32_t LegacySampleFrameAfterOwnedTicks(std::uint32_t completedTicks,
                                               std::uint32_t frameCount) {
    if (completedTicks == 0 || frameCount == 0) {
        return 0;
    }
    return (completedTicks - 1u) % frameCount;
}

LegacyBakeStatus BakeLegacyCaptureState(LgoGeomObj& object,
                                        std::uint32_t completedTicks,
                                        std::string& detail,
                                        LegacyBoneCapturePolicy bonePolicy) {
    detail.clear();
    if (completedTicks == 0) {
        detail = "capture tick должен быть положительным";
        return LegacyBakeStatus::INVALID_TICK;
    }
    const bool hasBoneController = object.Animation.BoneDataSize > 0;
    if (hasBoneController && bonePolicy == LegacyBoneCapturePolicy::Reject) {
        detail = std::format("BONE controller size={} пока не поддержан",
                             object.Animation.BoneDataSize);
        return LegacyBakeStatus::UNSUPPORTED_BONE_CONTROLLER;
    }
    if (hasBoneController &&
        (object.Mesh.Blends.empty() || object.Mesh.BoneIndices.empty())) {
        detail = std::format(
            "BONE controller size={} не принадлежит skinned mesh",
            object.Animation.BoneDataSize);
        return LegacyBakeStatus::UNSUPPORTED_BONE_CONTROLLER;
    }
    if (object.Animation.MaterialOpacityControllerCount > 0) {
        detail = "MTLOPACITY controller нельзя молча отбросить";
        return LegacyBakeStatus::UNSUPPORTED_MATERIAL_CONTROLLER;
    }
    if (object.Animation.TexImageControllerCount > 0) {
        detail = "TEXIMG controller нельзя молча отбросить";
        return LegacyBakeStatus::UNSUPPORTED_TEXTURE_IMAGE_CONTROLLER;
    }

    if (object.Animation.Matrix.has_value()) {
        const auto& frames = object.Animation.Matrix->Frames;
        for (std::size_t frame = 0; frame < frames.size(); ++frame) {
            if (!AllFinite(frames[frame])) {
                detail = std::format("MAT frame {} содержит NaN/Inf", frame);
                return LegacyBakeStatus::NON_FINITE_CONTROLLER;
            }
        }
    }
    for (const LgoTexUvAnimation& controller : object.Animation.TexUv) {
        for (std::size_t frame = 0; frame < controller.Frames.size(); ++frame) {
            if (!AllFinite(controller.Frames[frame])) {
                detail = std::format(
                    "TEXUV subset={} stage={} frame {} содержит NaN/Inf",
                    controller.Subset, controller.Stage, frame);
                return LegacyBakeStatus::NON_FINITE_CONTROLLER;
            }
        }
    }

    float bakedModelMatrix[16]{};
    std::copy_n(object.MatModel, 16, bakedModelMatrix);
    std::vector<Vector2> bakedTexcoords = object.Mesh.Texcoords[0];
    LegacyCaptureBakeMetadata metadata;
    metadata.Applied = true;
    metadata.CaptureTick = completedTicks;
    if (hasBoneController) {
        metadata.BoneDataSize = object.Animation.BoneDataSize;
        if (bonePolicy == LegacyBoneCapturePolicy::StaticReferencePose) {
            metadata.BoneStaticReferencePose = true;
        }
        else {
            if (!object.Animation.Bone.has_value() ||
                object.Animation.Bone->Header.FrameNum == 0) {
                detail = std::format(
                    "BONE controller size={} не был разобран в анимацию",
                    object.Animation.BoneDataSize);
                return LegacyBakeStatus::UNSUPPORTED_BONE_CONTROLLER;
            }
            metadata.BonePreserveAnimated = true;
            metadata.BoneFrameCount = object.Animation.Bone->Header.FrameNum;
            metadata.BoneSampleFrame = LegacySampleFrameAfterOwnedTicks(
                completedTicks, metadata.BoneFrameCount);
        }
    }

    if (object.Animation.Matrix.has_value()) {
        const auto& frames = object.Animation.Matrix->Frames;
        if (frames.empty()) {
            detail = "MAT controller не содержит кадров";
            return LegacyBakeStatus::EMPTY_CONTROLLER;
        }
        const std::uint32_t frameCount = static_cast<std::uint32_t>(frames.size());
        const std::uint32_t sample = LegacySampleFrameAfterOwnedTicks(
            completedTicks, frameCount);
        std::copy(frames[sample].begin(), frames[sample].end(), bakedModelMatrix);
        metadata.MatrixFrameCount = frameCount;
        metadata.MatrixSampleFrame = sample;
    }

    // Сначала учитываем subset без TEXUV: общую с animated subset вершину
    // нельзя изменить в единственном glTF TEXCOORD_0 без vertex split.
    std::vector<bool> controlledSubsets(object.Mesh.Subsets.size(), false);
    for (const LgoTexUvAnimation& controller : object.Animation.TexUv) {
        if (controller.Stage != 0) {
            detail = std::format(
                "TEXUV subset={} stage={} не попадает в glTF stage 0",
                controller.Subset, controller.Stage);
            return LegacyBakeStatus::UNSUPPORTED_TEXTURE_STAGE;
        }
        if (controller.Frames.empty()) {
            detail = std::format("TEXUV subset={} stage=0 не содержит кадров",
                                 controller.Subset);
            return LegacyBakeStatus::EMPTY_CONTROLLER;
        }
        if (controller.Subset >= object.Mesh.Subsets.size()) {
            detail = std::format("TEXUV subset={} вне {} mesh subsets",
                                 controller.Subset, object.Mesh.Subsets.size());
            return LegacyBakeStatus::INVALID_SUBSET;
        }
        controlledSubsets[controller.Subset] = true;
    }
    std::vector<bool> usedByUncontrolledSubset(bakedTexcoords.size(), false);
    if (!object.Animation.TexUv.empty()) {
        for (std::size_t subsetIndex = 0;
             subsetIndex < object.Mesh.Subsets.size(); ++subsetIndex) {
            if (controlledSubsets[subsetIndex]) {
                continue;
            }
            const SubsetInfo& subset = object.Mesh.Subsets[subsetIndex];
            const std::uint64_t indexBegin = subset.StartIndex;
            const std::uint64_t indexEnd = indexBegin +
                static_cast<std::uint64_t>(subset.PrimitiveNum) * 3u;
            if (indexEnd > object.Mesh.Indices.size()) {
                detail = std::format(
                    "subset={} indices [{}..{}) вне {}", subsetIndex,
                    indexBegin, indexEnd, object.Mesh.Indices.size());
                return LegacyBakeStatus::INVALID_SUBSET;
            }
            for (std::uint64_t indexOffset = indexBegin;
                 indexOffset < indexEnd; ++indexOffset) {
                const std::uint32_t vertex = object.Mesh.Indices[indexOffset];
                if (vertex >= bakedTexcoords.size()) {
                    detail = std::format("subset={} vertex {} вне {} UV",
                                         subsetIndex, vertex,
                                         bakedTexcoords.size());
                    return LegacyBakeStatus::INVALID_VERTEX_INDEX;
                }
                usedByUncontrolledSubset[vertex] = true;
            }
        }
    }

    // Индекс владельца не даёт применить две разные subset-матрицы к общей
    // вершине: такой случай нельзя корректно представить одним TEXCOORD_0.
    std::vector<std::int32_t> uvOwner(bakedTexcoords.size(), -1);
    for (std::size_t controllerIndex = 0;
         controllerIndex < object.Animation.TexUv.size(); ++controllerIndex) {
        const LgoTexUvAnimation& controller =
            object.Animation.TexUv[controllerIndex];
        const std::uint32_t frameCount =
            static_cast<std::uint32_t>(controller.Frames.size());
        const std::uint32_t sample = LegacySampleFrameAfterOwnedTicks(
            completedTicks, frameCount);
        const auto& matrix = controller.Frames[sample];
        const SubsetInfo& subset = object.Mesh.Subsets[controller.Subset];
        const std::uint64_t indexBegin = subset.StartIndex;
        const std::uint64_t indexCount =
            static_cast<std::uint64_t>(subset.PrimitiveNum) * 3u;
        const std::uint64_t indexEnd = indexBegin + indexCount;
        if (indexEnd > object.Mesh.Indices.size()) {
            detail = std::format(
                "TEXUV subset={} indices [{}..{}) вне {}",
                controller.Subset, indexBegin, indexEnd,
                object.Mesh.Indices.size());
            return LegacyBakeStatus::INVALID_SUBSET;
        }

        std::vector<bool> transformed(bakedTexcoords.size(), false);
        for (std::uint64_t indexOffset = indexBegin;
             indexOffset < indexEnd; ++indexOffset) {
            const std::uint32_t vertex = object.Mesh.Indices[indexOffset];
            if (vertex >= bakedTexcoords.size()) {
                detail = std::format("TEXUV vertex {} вне {} UV",
                                     vertex, bakedTexcoords.size());
                return LegacyBakeStatus::INVALID_VERTEX_INDEX;
            }
            if (usedByUncontrolledSubset[vertex]) {
                detail = std::format(
                    "vertex {} разделён TEXUV subset {} и uncontrolled subset",
                    vertex, controller.Subset);
                return LegacyBakeStatus::CONFLICTING_SHARED_VERTEX;
            }
            if (transformed[vertex]) {
                continue;
            }
            if (uvOwner[vertex] >= 0 &&
                static_cast<std::size_t>(uvOwner[vertex]) != controllerIndex) {
                const auto& prior = object.Animation.TexUv[
                    static_cast<std::size_t>(uvOwner[vertex])];
                const std::uint32_t priorSample = LegacySampleFrameAfterOwnedTicks(
                    completedTicks,
                    static_cast<std::uint32_t>(prior.Frames.size()));
                if (prior.Frames[priorSample] != matrix) {
                    detail = std::format(
                        "vertex {} разделён TEXUV subsets {} и {}",
                        vertex, prior.Subset, controller.Subset);
                    return LegacyBakeStatus::CONFLICTING_SHARED_VERTEX;
                }
                transformed[vertex] = true;
                continue;
            }

            const Vector2 source = bakedTexcoords[vertex];
            // D3DTTFF_COUNT2: runtime использует _31/_32 для UV translation.
            const float bakedX =
                source.X * matrix[0] + source.Y * matrix[4] + matrix[8];
            const float bakedY =
                source.X * matrix[1] + source.Y * matrix[5] + matrix[9];
            if (!std::isfinite(bakedX) || !std::isfinite(bakedY)) {
                detail = std::format(
                    "TEXUV subset={} vertex {} дал NaN/Inf после bake",
                    controller.Subset, vertex);
                return LegacyBakeStatus::NON_FINITE_CONTROLLER;
            }
            bakedTexcoords[vertex].X = bakedX;
            bakedTexcoords[vertex].Y = bakedY;
            uvOwner[vertex] = static_cast<std::int32_t>(controllerIndex);
            transformed[vertex] = true;
        }
        metadata.TexUvSamples.push_back(LegacyTexUvSample{
            controller.Subset, controller.Stage, frameCount, sample});
    }

    std::copy_n(bakedModelMatrix, 16, object.MatModel);
    object.Mesh.Texcoords[0] = std::move(bakedTexcoords);
    object.CaptureBake = std::move(metadata);
    return LegacyBakeStatus::OK;
}

LegacyBakeStatus BakeLegacyModelCaptureState(
    std::vector<LgoGeomObj>& objects,
    std::uint32_t completedTicks,
    std::string& detail,
    LegacyBoneCapturePolicy bonePolicy) {
    std::vector<LgoGeomObj> baked = objects;
    for (std::size_t index = 0; index < baked.size(); ++index) {
        const bool hasMatrixController = baked[index].Animation.Matrix.has_value();
        std::string partDetail;
        const LegacyBakeStatus status = BakeLegacyCaptureState(
            baked[index], completedTicks, partDetail, bonePolicy);
        if (status != LegacyBakeStatus::OK) {
            detail = std::format("part {}: {}", index, partDetail);
            return status;
        }
        if (hasMatrixController) {
            std::copy_n(baked[index].MatModel, 16,
                        baked[index].Header.MatLocal);
        }
    }

    ResolveModelMatrices(baked);
    for (std::size_t index = 0; index < baked.size(); ++index) {
        if (!std::all_of(baked[index].MatModel,
                         baked[index].MatModel + 16,
                         [](float value) { return std::isfinite(value); })) {
            detail = std::format(
                "part {} MatModel дал NaN/Inf после parent composition", index);
            return LegacyBakeStatus::NON_FINITE_CONTROLLER;
        }
    }
    objects = std::move(baked);
    detail.clear();
    return LegacyBakeStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
