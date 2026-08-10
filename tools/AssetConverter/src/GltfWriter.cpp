#include "Corsairs/Tools/AssetConverter/GltfWriter.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/GltfSkeletonWriter.h"
#include "Corsairs/Tools/AssetConverter/ImageCodec.h"
#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::int64_t kComponentTypeFloat = 5126;
constexpr std::int64_t kComponentTypeUnsignedInt = 5125;
constexpr std::int64_t kComponentTypeUnsignedByte = 5121;
constexpr std::int64_t kTargetArrayBuffer = 34962;
constexpr std::int64_t kTargetElementArrayBuffer = 34963;
constexpr std::int64_t kModeTriangles = 4;

struct BufferView {
    std::size_t ByteOffset;
    std::size_t ByteLength;
    std::int64_t Target;
};

// Дописывает данные в буфер с выравниванием на 4 байта, как требует glTF 2.0.
BufferView AppendToBuffer(std::vector<std::uint8_t>& buffer, const void* data,
                          std::size_t bytes, std::int64_t target) {
    while (buffer.size() % 4 != 0) {
        buffer.push_back(0);
    }
    const std::size_t offset = buffer.size();
    buffer.resize(offset + bytes);
    if (bytes > 0) {
        std::memcpy(buffer.data() + offset, data, bytes);
    }
    return BufferView{offset, bytes, target};
}

void WriteAccessorBounds(JsonWriter& json, const std::vector<Vector3>& values) {
    float minX = std::numeric_limits<float>::max();
    float minY = minX;
    float minZ = minX;
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = maxX;
    float maxZ = maxX;

    for (const Vector3& v : values) {
        minX = v.X < minX ? v.X : minX;
        minY = v.Y < minY ? v.Y : minY;
        minZ = v.Z < minZ ? v.Z : minZ;
        maxX = v.X > maxX ? v.X : maxX;
        maxY = v.Y > maxY ? v.Y : maxY;
        maxZ = v.Z > maxZ ? v.Z : maxZ;
    }

    json.Key("min");
    json.BeginArray();
    json.Value(static_cast<double>(minX));
    json.Value(static_cast<double>(minY));
    json.Value(static_cast<double>(minZ));
    json.EndArray();

    json.Key("max");
    json.BeginArray();
    json.Value(static_cast<double>(maxX));
    json.Value(static_cast<double>(maxY));
    json.Value(static_cast<double>(maxZ));
    json.EndArray();
}

bool WriteFile(const std::filesystem::path& path, const void* data, std::size_t bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    if (bytes > 0) {
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    }
    stream.flush();
    return static_cast<bool>(stream);
}

std::optional<std::filesystem::path> UniqueSiblingPath(
    const std::filesystem::path& destination,
    std::string_view role) {
    static std::atomic<std::uint64_t> sequence{0};
    for (std::size_t attempt = 0; attempt < 64; ++attempt) {
        const std::uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed);
        const std::filesystem::path candidate = destination.parent_path() /
            std::format(".{}.{}.{}", destination.filename().string(), role, id);
        std::error_code error;
        const bool exists = std::filesystem::exists(candidate, error);
        if (!error && !exists) {
            return candidate;
        }
    }
    return std::nullopt;
}

void RemoveAttemptFile(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

bool InspectReplaceableTarget(const std::filesystem::path& path,
                              bool& exists,
                              std::string& detail) {
    std::error_code error;
    exists = std::filesystem::exists(path, error);
    if (error) {
        detail = std::format(
            "не удалось проверить destination {}: {}",
            path.generic_string(), error.message());
        return false;
    }
    if (exists && !std::filesystem::is_regular_file(path, error)) {
        detail = std::format(
            "destination {} существует и не является обычным файлом",
            path.generic_string());
        return false;
    }
    if (error) {
        detail = std::format(
            "не удалось проверить тип destination {}: {}",
            path.generic_string(), error.message());
        return false;
    }
    return true;
}

bool RenameFile(const std::filesystem::path& source,
                const std::filesystem::path& destination,
                std::string_view operation,
                std::string& detail) {
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (!error) {
        return true;
    }
    detail = std::format(
        "{}: {} -> {}: {}", operation, source.generic_string(),
        destination.generic_string(), error.message());
    return false;
}

void RestorePriorFile(const std::optional<std::filesystem::path>& backup,
                      const std::filesystem::path& destination,
                      bool hadPrior,
                      std::string& detail) {
    if (!hadPrior || !backup) {
        return;
    }
    std::string restoreDetail;
    if (!RenameFile(*backup, destination, "не удалось восстановить prior output",
                    restoreDetail)) {
        detail += std::format("; rollback: {}", restoreDetail);
    }
}

bool PublishGltfPair(const std::filesystem::path& gltfTemp,
                     const std::filesystem::path& binTemp,
                     const std::filesystem::path& gltfPath,
                     const std::filesystem::path& binPath,
                     std::string& detail) {
    bool hadGltf = false;
    bool hadBin = false;
    if (!InspectReplaceableTarget(gltfPath, hadGltf, detail) ||
        !InspectReplaceableTarget(binPath, hadBin, detail)) {
        RemoveAttemptFile(gltfTemp);
        RemoveAttemptFile(binTemp);
        return false;
    }

    const auto gltfBackup = hadGltf ? UniqueSiblingPath(gltfPath, "backup")
                                    : std::optional<std::filesystem::path>{};
    const auto binBackup = hadBin ? UniqueSiblingPath(binPath, "backup")
                                  : std::optional<std::filesystem::path>{};
    if ((hadGltf && !gltfBackup) || (hadBin && !binBackup)) {
        detail = "не удалось выделить sibling backup для glTF-пары";
        RemoveAttemptFile(gltfTemp);
        RemoveAttemptFile(binTemp);
        return false;
    }

    if (hadGltf && !RenameFile(
            gltfPath, *gltfBackup, "не удалось сохранить prior .gltf", detail)) {
        RemoveAttemptFile(gltfTemp);
        RemoveAttemptFile(binTemp);
        return false;
    }
    if (hadBin && !RenameFile(
            binPath, *binBackup, "не удалось сохранить prior .bin", detail)) {
        RestorePriorFile(gltfBackup, gltfPath, hadGltf, detail);
        RemoveAttemptFile(gltfTemp);
        RemoveAttemptFile(binTemp);
        return false;
    }

    if (!RenameFile(binTemp, binPath, "не удалось опубликовать .bin", detail)) {
        RestorePriorFile(binBackup, binPath, hadBin, detail);
        RestorePriorFile(gltfBackup, gltfPath, hadGltf, detail);
        RemoveAttemptFile(gltfTemp);
        RemoveAttemptFile(binTemp);
        return false;
    }
    if (!RenameFile(gltfTemp, gltfPath, "не удалось опубликовать .gltf", detail)) {
        RemoveAttemptFile(binPath);
        RestorePriorFile(binBackup, binPath, hadBin, detail);
        RestorePriorFile(gltfBackup, gltfPath, hadGltf, detail);
        RemoveAttemptFile(gltfTemp);
        return false;
    }

    if (gltfBackup) {
        RemoveAttemptFile(*gltfBackup);
    }
    if (binBackup) {
        RemoveAttemptFile(*binBackup);
    }
    return true;
}

// Изображение, попавшее в glTF: URI относительно каталога .gltf.
struct GltfImage {
    std::string Uri;
};

// Заменяет нечисловые компоненты на запасное значение. Возвращает число
// заменённых вершин.
//
// Проверять нужно каждую компоненту: в исходных данных встречается и вершина
// с одной испорченной осью, и полностью нечисловая.
template <typename T>
std::size_t SanitizeVectors(std::vector<T>& values, const T& fallback) {
    std::size_t fixed = 0;
    for (T& value : values) {
        const float* components = reinterpret_cast<const float*>(&value);
        const std::size_t count = sizeof(T) / sizeof(float);

        bool broken = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(components[i])) {
                broken = true;
                break;
            }
        }
        if (broken) {
            value = fallback;
            ++fixed;
        }
    }
    return fixed;
}

// Поза кости в первом кадре — она же bind-поза скелета.
//
// Ключи бывают двух видов: раздельные позиция с поворотом либо матрица
// целиком. Второй вид приходится раскладывать, потому что glTF хранит узлы
// в виде переноса, поворота и масштаба.
void BindPoseOf(const LabAnimation& skeleton, std::uint32_t bone,
                float translation[3], float rotation[4]) {
    const LabBoneTrack& track = skeleton.Tracks[bone];

    if (!track.Positions.empty() && !track.Rotations.empty()) {
        // Та же перестановка Y и Z, что и для вершин.
        translation[0] = track.Positions[0].X;
        translation[1] = track.Positions[0].Z;
        translation[2] = track.Positions[0].Y;
        ConvertQuaternionToGltf(track.Rotations[0], rotation);
        return;
    }

    if (track.Matrices.size() >= 16) {
        float converted[16]{};
        ConvertMatrixToGltf(track.Matrices.data(), converted);
        const Trs trs = DecomposeGltfMatrix(converted);
        std::copy_n(trs.Translation, 3, translation);
        std::copy_n(trs.Rotation, 4, rotation);
        return;
    }

    // Ключей нет вовсе: кость остаётся в начале координат без поворота.
    translation[0] = translation[1] = translation[2] = 0.0f;
    rotation[0] = rotation[1] = rotation[2] = 0.0f;
    rotation[3] = 1.0f;
}

struct SceneBoneAnimation {
    std::vector<float> Times;
    std::vector<std::vector<float>> Translations;
    std::vector<std::vector<float>> Rotations;
    std::vector<std::vector<float>> Scales;
    std::uint32_t RootBone{0};
};

bool IsFiniteRange(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

bool NormalizeQuaternion(float* quaternion) {
    const float lengthSquared =
        quaternion[0] * quaternion[0] +
        quaternion[1] * quaternion[1] +
        quaternion[2] * quaternion[2] +
        quaternion[3] * quaternion[3];
    if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0f) {
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    for (std::size_t component = 0; component < 4; ++component) {
        quaternion[component] *= inverseLength;
    }
    return IsFiniteRange(quaternion, 4u);
}

bool ValidateSceneMapAnimatedSkin(const LgoMesh& mesh,
                                  const LabAnimation& skeleton,
                                  std::string& detail) {
    const std::uint32_t boneNum = skeleton.Header.BoneNum;
    const std::uint32_t frameNum = skeleton.Header.FrameNum;
    if (boneNum == 0 || boneNum > 256u || frameNum == 0 ||
        skeleton.Bones.size() != boneNum ||
        skeleton.Tracks.size() != boneNum ||
        skeleton.InverseBindMatrices.size() !=
            static_cast<std::size_t>(boneNum) * 16u ||
        skeleton.Dummies.size() != skeleton.Header.DummyNum) {
        detail = std::format(
            "BONE DTO несогласован: header bones={}/frames={}/dummies={}, "
            "arrays bones={}/tracks={}/ibm={}/dummies={}",
            boneNum, frameNum, skeleton.Header.DummyNum,
            skeleton.Bones.size(), skeleton.Tracks.size(),
            skeleton.InverseBindMatrices.size(), skeleton.Dummies.size());
        return false;
    }
    if (!IsFiniteRange(skeleton.InverseBindMatrices.data(),
                       skeleton.InverseBindMatrices.size())) {
        detail = "BONE inverse bind matrices содержат нечисловые значения";
        return false;
    }

    std::size_t rootCount = 0;
    // Полная range-validation обязана предшествовать обходу: forward-parent
    // может указывать на кость, чей собственный ParentId повреждён.
    for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
        const BoneBaseInfo& info = skeleton.Bones[bone];
        if (info.Id != bone ||
            (info.ParentId != kNoParent && info.ParentId >= boneNum)) {
            detail = std::format("BONE hierarchy повреждена на кости {}", bone);
            return false;
        }
        if (info.ParentId == kNoParent) {
            ++rootCount;
        }
    }

    for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
        std::uint32_t cursor = bone;
        bool reachesRoot = false;
        for (std::uint32_t step = 0; step <= boneNum; ++step) {
            if (cursor >= boneNum) {
                detail = std::format(
                    "BONE hierarchy вышла на индекс {} от кости {}",
                    cursor, bone);
                return false;
            }
            const std::uint32_t parent = skeleton.Bones[cursor].ParentId;
            if (parent == kNoParent) {
                reachesRoot = true;
                break;
            }
            cursor = parent;
        }
        if (!reachesRoot) {
            detail = std::format("BONE hierarchy содержит цикл от кости {}", bone);
            return false;
        }
    }

    for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
        const LabBoneTrack& track = skeleton.Tracks[bone];
        const bool hasQuat = track.Positions.size() == frameNum &&
                             track.Rotations.size() == frameNum &&
                             track.Matrices.empty();
        const bool hasMatrix = track.Positions.empty() &&
                               track.Rotations.empty() &&
                               track.Matrices.size() ==
                                   static_cast<std::size_t>(frameNum) * 16u;
        if (!hasQuat && !hasMatrix) {
            detail = std::format("BONE track {} не содержит ровно {} кадров",
                                 bone, frameNum);
            return false;
        }
        if (hasQuat) {
            for (std::uint32_t frame = 0; frame < frameNum; ++frame) {
                const Vector3& position = track.Positions[frame];
                const Quaternion& rotation = track.Rotations[frame];
                if (!std::isfinite(position.X) || !std::isfinite(position.Y) ||
                    !std::isfinite(position.Z) || !std::isfinite(rotation.X) ||
                    !std::isfinite(rotation.Y) || !std::isfinite(rotation.Z) ||
                    !std::isfinite(rotation.W)) {
                    detail = std::format(
                        "BONE QUAT track {} frame {} нечисловой", bone, frame);
                    return false;
                }
            }
        }
        else if (!IsFiniteRange(track.Matrices.data(), track.Matrices.size())) {
            detail = std::format("BONE matrix track {} нечисловой", bone);
            return false;
        }
    }
    if (rootCount != 1u) {
        detail = std::format("SceneMap BONE требует один root, найдено {}", rootCount);
        return false;
    }

    for (std::size_t dummy = 0; dummy < skeleton.Dummies.size(); ++dummy) {
        const BoneDummyInfo& info = skeleton.Dummies[dummy];
        if (info.ParentBoneId >= boneNum || !IsFiniteRange(info.Mat, 16u)) {
            detail = std::format("BONE dummy {} повреждён", dummy);
            return false;
        }
    }

    if (mesh.Blends.size() != mesh.Positions.size() ||
        mesh.BoneIndices.empty()) {
        detail = std::format(
            "skin arrays несогласованы: blends={}, positions={}, boneIndices={}",
            mesh.Blends.size(), mesh.Positions.size(), mesh.BoneIndices.size());
        return false;
    }
    for (std::size_t local = 0; local < mesh.BoneIndices.size(); ++local) {
        if (mesh.BoneIndices[local] >= boneNum) {
            detail = std::format("BoneIndices[{}]={} вне {} BONE bones",
                                 local, mesh.BoneIndices[local], boneNum);
            return false;
        }
    }
    for (std::size_t vertex = 0; vertex < mesh.Blends.size(); ++vertex) {
        float weightSum = 0.0f;
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const BlendInfo& blend = mesh.Blends[vertex];
            if (blend.Index[influence] >= mesh.BoneIndices.size() ||
                !std::isfinite(blend.Weight[influence]) ||
                blend.Weight[influence] < 0.0f) {
                detail = std::format(
                    "blend vertex {} influence {} повреждён", vertex, influence);
                return false;
            }
            weightSum += blend.Weight[influence];
        }
        if (!std::isfinite(weightSum) || weightSum <= 0.0f) {
            detail = std::format("blend vertex {} имеет сумму весов {}",
                                 vertex, weightSum);
            return false;
        }
    }
    return true;
}

bool BuildSceneBoneAnimation(const LabAnimation& skeleton,
                             SceneBoneAnimation& output,
                             std::string& detail) {
    const std::uint32_t boneNum = skeleton.Header.BoneNum;
    const std::uint32_t frameNum = skeleton.Header.FrameNum;
    output = {};
    output.Times.resize(frameNum);
    output.Translations.resize(boneNum);
    output.Rotations.resize(boneNum);
    output.Scales.resize(boneNum);

    for (std::uint32_t frame = 0; frame < frameNum; ++frame) {
        output.Times[frame] = static_cast<float>(frame) / kAnimFramesPerSecond;
    }

    for (std::uint32_t bone = 0; bone < boneNum; ++bone) {
        if (skeleton.Bones[bone].ParentId == kNoParent) {
            output.RootBone = bone;
        }
        output.Translations[bone].resize(static_cast<std::size_t>(frameNum) * 3u);
        output.Rotations[bone].resize(static_cast<std::size_t>(frameNum) * 4u);
        output.Scales[bone].resize(static_cast<std::size_t>(frameNum) * 3u);
        const LabBoneTrack& track = skeleton.Tracks[bone];
        const bool hasQuat = !track.Positions.empty();

        for (std::uint32_t frame = 0; frame < frameNum; ++frame) {
            float* translation = output.Translations[bone].data() +
                static_cast<std::size_t>(frame) * 3u;
            float* rotation = output.Rotations[bone].data() +
                static_cast<std::size_t>(frame) * 4u;
            float* scale = output.Scales[bone].data() +
                static_cast<std::size_t>(frame) * 3u;

            if (hasQuat) {
                const Vector3& position = track.Positions[frame];
                translation[0] = position.X;
                translation[1] = position.Z;
                translation[2] = position.Y;
                ConvertQuaternionToGltf(track.Rotations[frame], rotation);
                scale[0] = 1.0f;
                scale[1] = 1.0f;
                scale[2] = 1.0f;
            }
            else {
                float converted[16]{};
                ConvertMatrixToGltf(
                    track.Matrices.data() + static_cast<std::size_t>(frame) * 16u,
                    converted, GltfCoordinateProfile::Generic);
                const Trs trs = DecomposeGltfMatrix(converted);
                std::copy_n(trs.Translation, 3u, translation);
                std::copy_n(trs.Rotation, 4u, rotation);
                std::copy_n(trs.Scale, 3u, scale);
            }
            if (!IsFiniteRange(translation, 3u) ||
                !IsFiniteRange(rotation, 4u) ||
                !IsFiniteRange(scale, 3u) ||
                !NormalizeQuaternion(rotation)) {
                detail = std::format(
                    "BONE track {} frame {} дал нечисловой TRS", bone, frame);
                return false;
            }
            if (frame > 0u) {
                const float* previous = rotation - 4;
                const float dot = previous[0] * rotation[0] +
                                  previous[1] * rotation[1] +
                                  previous[2] * rotation[2] +
                                  previous[3] * rotation[3];
                if (dot < 0.0f) {
                    for (std::size_t component = 0; component < 4; ++component) {
                        rotation[component] = -rotation[component];
                    }
                }
            }
        }
    }
    return true;
}

// Расширение сравнивается без учёта регистра: в исходных данных встречается
// и .dds, и .DDS.
bool IsDdsFile(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension == ".dds";
}

// Распаковывает DDS и пишет PNG. Неудача не прерывает конвертацию: модель
// получит ссылку на несуществующий файл и останется без текстуры — ровно то
// же, что было бы при копировании нечитаемого DDS, но без остановки прогона.
bool ConvertDdsToPng(const std::filesystem::path& source,
                     const std::filesystem::path& target) {
    const auto bytes = ReadWholeFile(source);
    if (!bytes) {
        return false;
    }

    DdsStatus status = DdsStatus::OK;
    const auto image = DecodeDds(*bytes, status);
    if (!image) {
        return false;
    }

    return WritePng(target, *image);
}

// Готовит список изображений и отображение «материал -> индекс текстуры».
// Одна и та же текстура, использованная несколькими материалами, попадает в
// список один раз.
//
// В glTF записывается только стадия 0: остальные стадии в MindPower3D — это
// лайтмапы и слои смешивания фиксированного конвейера DX9, которым в PBR-модели
// нет прямого соответствия. По решению из спеки материалы всё равно делаются
// заново средствами UE, здесь важно донести базовую текстуру и имена.
void CollectImages(const LgoGeomObj& obj, const GltfTextureOptions& textures,
                   const std::filesystem::path& gltfDir,
                   std::vector<GltfImage>& images,
                   std::vector<std::int64_t>& materialToImage) {
    materialToImage.assign(obj.Materials.size(), -1);

    for (std::size_t m = 0; m < obj.Materials.size(); ++m) {
        if (m >= textures.ResolvedTextures.size() || textures.ResolvedTextures[m].empty()) {
            continue;
        }
        const std::filesystem::path& source = textures.ResolvedTextures[m][0];
        if (source.empty()) {
            continue;
        }

        std::filesystem::path target = source;
        if (!textures.CopyTo.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(textures.CopyTo, ec);

            // DDS распаковывается в PNG, остальное копируется как есть.
            //
            // Спецификация glTF 2.0 допускает в image только PNG и JPEG;
            // ссылка на .dds делает файл формально невалидным. Interchange
            // такое изображение молча пропускает, материал остаётся без
            // текстуры, и модель выглядит белой — так и было со всеми 2005
            // моделями сцены из 2021.
            if (IsDdsFile(source)) {
                target = textures.CopyTo / source.filename();
                target.replace_extension(".png");
                if (!std::filesystem::exists(target)) {
                    ConvertDdsToPng(source, target);
                }
            }
            else {
                target = textures.CopyTo / source.filename();
                // copy_options::skip_existing — текстура может быть общей для
                // множества моделей, копировать её каждый раз незачем.
                std::filesystem::copy_file(source, target,
                                           std::filesystem::copy_options::skip_existing, ec);
            }

            // Ссылка на несуществующий файл хуже отсутствия ссылки: glTF
            // становится формально битым, а модель выглядит так же — белой.
            // Один раз распаковка молча не срабатывала на всех файлах сразу, и
            // заметить это удалось только по пустому каталогу текстур.
            if (!std::filesystem::exists(target)) {
                continue;
            }
        }

        std::error_code ec;
        std::filesystem::path relative = std::filesystem::relative(target, gltfDir, ec);
        const std::string uri = ec || relative.empty()
            ? target.generic_string()
            : relative.generic_string();

        // Дедупликация по итоговому URI.
        std::int64_t existing = -1;
        for (std::size_t i = 0; i < images.size(); ++i) {
            if (images[i].Uri == uri) {
                existing = static_cast<std::int64_t>(i);
                break;
            }
        }
        if (existing < 0) {
            existing = static_cast<std::int64_t>(images.size());
            images.push_back(GltfImage{uri});
        }
        materialToImage[m] = existing;
    }
}

std::string_view LegacyMaterialModeName(LegacyMaterialMode mode) {
    switch (mode) {
    case LegacyMaterialMode::Opaque:      return "opaque";
    case LegacyMaterialMode::Masked:      return "masked";
    case LegacyMaterialMode::Alpha:       return "alpha";
    case LegacyMaterialMode::Additive:    return "additive";
    case LegacyMaterialMode::Subtractive: return "subtractive";
    }
    return "opaque";
}

void WriteLegacyMaterialExtras(JsonWriter& json,
                               const LegacyMaterialMetadata& metadata) {
    json.Key("extras");
    json.BeginObject();
    json.Key("corsairsLegacyMaterial");
    json.BeginObject();
    json.Key("schemaVersion");
    json.Value(static_cast<std::int64_t>(1));
    json.Key("mode");
    json.Value(LegacyMaterialModeName(metadata.Mode));
    json.Key("opacity");
    json.Value(static_cast<double>(metadata.Opacity));
    json.Key("rawTranspType");
    json.Value(static_cast<std::int64_t>(metadata.RawTranspType));
    json.Key("effectiveTranspType");
    json.Value(static_cast<std::int64_t>(metadata.EffectiveTranspType));
    json.Key("alphaTestEnabled");
    json.Value(metadata.AlphaTestEnabled);
    json.Key("alphaRef");
    json.Value(static_cast<std::int64_t>(metadata.AlphaRef));
    json.Key("alphaFunc");
    json.Value(static_cast<std::int64_t>(metadata.AlphaFunc));
    json.Key("alphaBlendEnabled");
    json.Value(metadata.AlphaBlendEnabled);
    json.Key("srcBlend");
    json.Value(static_cast<std::int64_t>(metadata.SrcBlend));
    json.Key("destBlend");
    json.Value(static_cast<std::int64_t>(metadata.DestBlend));
    json.EndObject();
    json.EndObject();
}

void WriteLegacyCaptureExtras(JsonWriter& json,
                              const LegacyCaptureBakeMetadata& metadata) {
    if (!metadata.Applied) {
        return;
    }
    json.Key("extras");
    json.BeginObject();
    json.Key("corsairsLegacyCapture");
    json.BeginObject();
    json.Key("schemaVersion");
    json.Value(static_cast<std::int64_t>(1));
    json.Key("captureTick");
    json.Value(static_cast<std::int64_t>(metadata.CaptureTick));
    json.Key("matrix");
    if (metadata.MatrixFrameCount > 0) {
        json.BeginObject();
        json.Key("frameCount");
        json.Value(static_cast<std::int64_t>(metadata.MatrixFrameCount));
        json.Key("sampleFrame");
        json.Value(static_cast<std::int64_t>(metadata.MatrixSampleFrame));
        json.EndObject();
    }
    else {
        json.Null();
    }
    json.Key("texUv");
    json.BeginArray();
    for (const LegacyTexUvSample& sample : metadata.TexUvSamples) {
        json.BeginObject();
        json.Key("subset");
        json.Value(static_cast<std::int64_t>(sample.Subset));
        json.Key("stage");
        json.Value(static_cast<std::int64_t>(sample.Stage));
        json.Key("frameCount");
        json.Value(static_cast<std::int64_t>(sample.FrameCount));
        json.Key("sampleFrame");
        json.Value(static_cast<std::int64_t>(sample.SampleFrame));
        json.EndObject();
    }
    json.EndArray();
    if (metadata.BoneDataSize > 0 &&
        (metadata.BoneStaticReferencePose || metadata.BonePreserveAnimated)) {
        json.Key("bone");
        json.BeginObject();
        json.Key("dataSize");
        json.Value(static_cast<std::int64_t>(metadata.BoneDataSize));
        json.Key("policy");
        if (metadata.BoneStaticReferencePose) {
            json.Value("staticReferencePose");
        }
        else {
            json.Value("preserveAnimated");
            json.Key("frameCount");
            json.Value(static_cast<std::int64_t>(metadata.BoneFrameCount));
            json.Key("sampleFrame");
            json.Value(static_cast<std::int64_t>(metadata.BoneSampleFrame));
        }
        json.EndObject();
    }
    json.EndObject();
    json.EndObject();
}

} // namespace

void ConvertMatrixToGltf(const float* in, float* out,
                         GltfCoordinateProfile profile) {
    // Замена базиса P*M*P, где P переставляет оси Y и Z. P обратна самой себе,
    // поэтому обе стороны — она же. На уровне элементов это значит взять
    // элемент с переставленными индексами строки и столбца.
    //
    // Транспонировать НЕ нужно, см. комментарий к объявлению функции.
    constexpr auto swapAxis = [](int index) { return index == 1 ? 2 : (index == 2 ? 1 : index); };

    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[row * 4 + col] = in[swapAxis(row) * 4 + swapAxis(col)];
        }
    }
    static_cast<void>(profile);
}

GltfStatus WriteGltf(const LgoGeomObj& obj, const std::filesystem::path& gltfPath,
                     std::string& detail, const GltfTextureOptions& textures,
                     const LabAnimation* skeleton,
                     GltfCoordinateProfile profile,
                     GltfSkinPolicy skinPolicy,
                     const GltfAssetMetadata& assetMetadata) {
    const LgoMesh& mesh = obj.Mesh;

    if (assetMetadata.TerrainCoordinateProfile !=
            GltfTerrainCoordinateProfile::None &&
        assetMetadata.TerrainCoordinateProfile !=
            GltfTerrainCoordinateProfile::RigidQ) {
        detail = "неизвестный glTF terrain coordinate profile";
        return GltfStatus::WRITE_FAILED;
    }
    if (mesh.Positions.empty() || mesh.Indices.empty() || mesh.Subsets.empty()) {
        detail = "меш не содержит вершин, индексов или подсетов";
        return GltfStatus::EMPTY_MESH;
    }

    const bool sourceHasSkinData =
        !mesh.Blends.empty() || !mesh.BoneIndices.empty();
    const bool hasSkin = skinPolicy == GltfSkinPolicy::Preserve &&
                         !mesh.Blends.empty() && !mesh.BoneIndices.empty();
    std::optional<SceneBoneAnimation> sceneAnimation;
    if (profile == GltfCoordinateProfile::SceneMap &&
        sourceHasSkinData && skinPolicy == GltfSkinPolicy::Preserve) {
        if (!hasSkin || skeleton == nullptr) {
            detail = "SceneMap animated skin требует полные mesh skin arrays и BONE DTO";
            // Сохраняем прежний observable status для SceneMap skin без
            // skeleton; детально повреждённый BONE использует INVALID_SKIN_DATA.
            return GltfStatus::WRITE_FAILED;
        }
        if (!ValidateSceneMapAnimatedSkin(mesh, *skeleton, detail)) {
            return GltfStatus::INVALID_SKIN_DATA;
        }
        SceneBoneAnimation builtAnimation;
        if (!BuildSceneBoneAnimation(*skeleton, builtAnimation, detail)) {
            return GltfStatus::INVALID_SKIN_DATA;
        }
        sceneAnimation = std::move(builtAnimation);
    }

    std::vector<LegacyMaterialMetadata> resolvedMaterials;
    if (profile == GltfCoordinateProfile::SceneMap) {
        resolvedMaterials.resize(obj.Materials.size());
        for (std::size_t i = 0; i < obj.Materials.size(); ++i) {
            std::string materialDetail;
            const LegacyMaterialStatus materialStatus = ResolveLegacyMaterial(
                obj.Materials[i], resolvedMaterials[i], materialDetail);
            if (materialStatus != LegacyMaterialStatus::OK) {
                detail = std::format("материал {}: {}", i, materialDetail);
                return GltfStatus::UNSUPPORTED_MATERIAL_MODE;
            }
        }
    }

    // Перевод в систему координат glTF перестановкой Y и Z.
    //
    // MindPower3D держит высоту по Z и левосторонен. Спецификация glTF
    // фиксирует «вверх — это +Y» и не предусматривает метаданных об
    // ориентации: импортёр не спрашивает, а знает. Перестановка двух осей
    // делает сразу обе нужные вещи — ставит высоту на Y и меняет рукость,
    // потому что определитель такой замены базиса равен минус единице.
    //
    // Прежний вариант отрицал Z. Рукость он менял правильно, но роли осей
    // оставлял как есть, и модели приезжали в UE лежащими на боку. Blender
    // это скрывал: его импортёр glTF позволяет выбрать ось вверх и
    // подстраивается, поэтому проверка через него ошибку пропускала.
    std::vector<Vector3> positions = mesh.Positions;
    for (Vector3& p : positions) {
        std::swap(p.Y, p.Z);
    }

    std::vector<Vector3> normals = mesh.Normals;
    for (Vector3& n : normals) {
        std::swap(n.Y, n.Z);
    }

    // Исходные данные местами содержат NaN и бесконечности — в нормалях у
    // восемнадцати моделей, в развёртке у трёх. Спецификация glTF запрещает
    // нечисловые значения, а Interchange заменяет их нулями, что для нормали
    // означает отсутствие направления: освещение такой грани ломается.
    // Подставляем осмысленные значения на месте.
    SanitizeVectors(positions, Vector3{0.0f, 0.0f, 0.0f});
    SanitizeVectors(normals, Vector3{0.0f, 1.0f, 0.0f});

    // Смена порядка обхода треугольника — парная операция к смене рукости.
    std::vector<std::uint32_t> indices = mesh.Indices;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t tmp = indices[i + 1];
        indices[i + 1] = indices[i + 2];
        indices[i + 2] = tmp;
    }

    std::vector<std::uint8_t> buffer;
    const BufferView positionView = AppendToBuffer(
        buffer, positions.data(), positions.size() * sizeof(Vector3), kTargetArrayBuffer);

    BufferView normalView{0, 0, kTargetArrayBuffer};
    const bool hasNormals = !normals.empty();
    if (hasNormals) {
        normalView = AppendToBuffer(buffer, normals.data(),
                                    normals.size() * sizeof(Vector3), kTargetArrayBuffer);
    }

    BufferView uvView{0, 0, kTargetArrayBuffer};
    const bool hasUv = !mesh.Texcoords[0].empty();
    std::vector<Vector2> texcoords;
    if (hasUv) {
        texcoords = mesh.Texcoords[0];
        SanitizeVectors(texcoords, Vector2{0.0f, 0.0f});
        uvView = AppendToBuffer(buffer, texcoords.data(),
                                texcoords.size() * sizeof(Vector2),
                                kTargetArrayBuffer);
    }

    // D3DCOLOR в source хранится как 0xAARRGGBB. В little-endian памяти это
    // BGRA, поэтому сырой uint32_t нельзя публиковать как glTF COLOR_0:
    // Interchange прочитает красный и синий наоборот. SceneMap получает
    // канонические RGBA-байты; Generic намеренно остаётся byte-identical.
    const bool hasVertexColors =
        profile == GltfCoordinateProfile::SceneMap &&
        !mesh.VertexColors.empty();
    if (hasVertexColors && mesh.VertexColors.size() != positions.size()) {
        detail = std::format("цветов вершин {}, позиций {}",
                             mesh.VertexColors.size(), positions.size());
        return GltfStatus::WRITE_FAILED;
    }
    BufferView colorView{0, 0, kTargetArrayBuffer};
    std::vector<std::uint8_t> vertexColors;
    if (hasVertexColors) {
        vertexColors.reserve(mesh.VertexColors.size() * 4u);
        for (const std::uint32_t argb : mesh.VertexColors) {
            vertexColors.push_back(static_cast<std::uint8_t>((argb >> 16u) & 0xffu));
            vertexColors.push_back(static_cast<std::uint8_t>((argb >> 8u) & 0xffu));
            vertexColors.push_back(static_cast<std::uint8_t>(argb & 0xffu));
            vertexColors.push_back(static_cast<std::uint8_t>((argb >> 24u) & 0xffu));
        }
        colorView = AppendToBuffer(buffer, vertexColors.data(), vertexColors.size(),
                                   kTargetArrayBuffer);
    }

    // Скиннинг. BlendInfo::Index индексирует BoneIndices (локальный индекс ->
    // глобальный id кости), а glTF JOINTS_0 индексирует массив skin.joints.
    // Строим joints в том же порядке, что BoneIndices, — тогда индексы
    // переносятся один в один без перенумерации.
    // Скелет прикладывается, только если его кости индексируются одним байтом:
    // JOINTS_0 здесь пишется как UNSIGNED_BYTE, и 256-я кость в него не влезет.
    // Ни один скелет в наборе такого размера не достигает, но молча испортить
    // привязку хуже, чем обойтись без скелета.
    const std::uint32_t skeletonBoneNum =
        skeleton != nullptr ? skeleton->Header.BoneNum : 0;
    const bool hasSkeleton = hasSkin && skeleton != nullptr &&
                             skeletonBoneNum > 0 && skeletonBoneNum <= 256 &&
                             skeleton->InverseBindMatrices.size() >=
                                 static_cast<std::size_t>(skeletonBoneNum) * 16 &&
                             skeleton->Tracks.size() >= skeletonBoneNum;

    BufferView jointsView{0, 0, kTargetArrayBuffer};
    BufferView weightsView{0, 0, kTargetArrayBuffer};
    if (hasSkin) {
        std::vector<std::uint8_t> joints(mesh.Blends.size() * 4);
        std::vector<float> weights(mesh.Blends.size() * 4);

        for (std::size_t v = 0; v < mesh.Blends.size(); ++v) {
            const BlendInfo& blend = mesh.Blends[v];
            float sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                // Индекс вне диапазона обнуляем вместе с весом: иначе glTF
                // невалиден, а вершина всё равно не должна им управляться.
                const bool valid = blend.Index[k] < mesh.BoneIndices.size();
                // Со скелетом сустав адресуется номером кости в нём, без него —
                // позицией в BoneIndices. Номер кости совпадает с её индексом в
                // массиве: движок сравнивает ParentId именно с индексом.
                std::uint32_t joint = valid ? blend.Index[k] : 0;
                if (valid && hasSkeleton) {
                    const std::uint32_t bone = mesh.BoneIndices[blend.Index[k]];
                    joint = bone < skeletonBoneNum ? bone : 0;
                }
                joints[v * 4 + k] = static_cast<std::uint8_t>(joint);
                weights[v * 4 + k] = valid ? blend.Weight[k] : 0.0f;
                sum += weights[v * 4 + k];
            }
            // glTF требует, чтобы веса в сумме давали единицу.
            if (sum > 0.0f) {
                for (std::size_t k = 0; k < 4; ++k) {
                    weights[v * 4 + k] /= sum;
                }
            }
            else {
                weights[v * 4] = 1.0f;
            }
        }

        jointsView = AppendToBuffer(buffer, joints.data(), joints.size(), kTargetArrayBuffer);
        weightsView = AppendToBuffer(buffer, weights.data(),
                                     weights.size() * sizeof(float), kTargetArrayBuffer);
    }

    // Обратные bind-матрицы скелета. Без них импортёр считает bind-позу
    // единичной, и меш выходит смятым в точку.
    BufferView inverseBindView{0, 0, 0};
    std::vector<float> inverseBind;
    if (hasSkeleton) {
        inverseBind.resize(static_cast<std::size_t>(skeletonBoneNum) * 16);
        for (std::uint32_t b = 0; b < skeletonBoneNum; ++b) {
            ConvertMatrixToGltf(skeleton->InverseBindMatrices.data() +
                                    static_cast<std::size_t>(b) * 16,
                                inverseBind.data() + static_cast<std::size_t>(b) * 16,
                                profile);
        }
        // Цель не указывается: матрицы не идут в вершинный конвейер, и
        // спецификация запрещает помечать такой блок как ARRAY_BUFFER.
        inverseBindView = AppendToBuffer(buffer, inverseBind.data(),
                                         inverseBind.size() * sizeof(float), 0);
    }

    const BufferView indexView = AppendToBuffer(
        buffer, indices.data(), indices.size() * sizeof(std::uint32_t),
        kTargetElementArrayBuffer);

    BufferView animationTimeView{0, 0, 0};
    std::vector<BufferView> animationTranslationViews;
    std::vector<BufferView> animationRotationViews;
    std::vector<BufferView> animationScaleViews;
    if (sceneAnimation.has_value()) {
        animationTimeView = AppendToBuffer(
            buffer, sceneAnimation->Times.data(),
            sceneAnimation->Times.size() * sizeof(float), 0);
        animationTranslationViews.reserve(skeletonBoneNum);
        animationRotationViews.reserve(skeletonBoneNum);
        animationScaleViews.reserve(skeletonBoneNum);
        for (std::uint32_t bone = 0; bone < skeletonBoneNum; ++bone) {
            animationTranslationViews.push_back(AppendToBuffer(
                buffer, sceneAnimation->Translations[bone].data(),
                sceneAnimation->Translations[bone].size() * sizeof(float), 0));
            animationRotationViews.push_back(AppendToBuffer(
                buffer, sceneAnimation->Rotations[bone].data(),
                sceneAnimation->Rotations[bone].size() * sizeof(float), 0));
            animationScaleViews.push_back(AppendToBuffer(
                buffer, sceneAnimation->Scales[bone].data(),
                sceneAnimation->Scales[bone].size() * sizeof(float), 0));
        }
    }

    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");

    const auto gltfTemp = UniqueSiblingPath(gltfPath, "tmp");
    const auto binTemp = UniqueSiblingPath(binPath, "tmp");
    if (!gltfTemp || !binTemp) {
        detail = "не удалось выделить sibling temp для glTF-пары";
        return GltfStatus::WRITE_FAILED;
    }

    if (!WriteFile(*binTemp, buffer.data(), buffer.size())) {
        RemoveAttemptFile(*gltfTemp);
        RemoveAttemptFile(*binTemp);
        detail = "не удалось записать временный .bin";
        return GltfStatus::WRITE_FAILED;
    }

    // Собираем список bufferView в том же порядке, в каком они попадут в JSON.
    std::vector<BufferView> views;
    views.push_back(positionView);
    const std::int64_t positionViewIndex = 0;

    std::int64_t normalViewIndex = -1;
    if (hasNormals) {
        normalViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(normalView);
    }

    std::int64_t uvViewIndex = -1;
    if (hasUv) {
        uvViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(uvView);
    }

    std::int64_t colorViewIndex = -1;
    if (hasVertexColors) {
        colorViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(colorView);
    }

    std::int64_t jointsViewIndex = -1;
    std::int64_t weightsViewIndex = -1;
    if (hasSkin) {
        jointsViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(jointsView);
        weightsViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(weightsView);
    }

    std::int64_t inverseBindViewIndex = -1;
    if (hasSkeleton) {
        inverseBindViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(inverseBindView);
    }

    const std::int64_t indexViewIndex = static_cast<std::int64_t>(views.size());
    views.push_back(indexView);

    std::int64_t animationTimeViewIndex = -1;
    std::vector<std::int64_t> animationTranslationViewIndices;
    std::vector<std::int64_t> animationRotationViewIndices;
    std::vector<std::int64_t> animationScaleViewIndices;
    if (sceneAnimation.has_value()) {
        animationTimeViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(animationTimeView);
        animationTranslationViewIndices.resize(skeletonBoneNum);
        animationRotationViewIndices.resize(skeletonBoneNum);
        animationScaleViewIndices.resize(skeletonBoneNum);
        for (std::uint32_t bone = 0; bone < skeletonBoneNum; ++bone) {
            animationTranslationViewIndices[bone] =
                static_cast<std::int64_t>(views.size());
            views.push_back(animationTranslationViews[bone]);
            animationRotationViewIndices[bone] =
                static_cast<std::int64_t>(views.size());
            views.push_back(animationRotationViews[bone]);
            animationScaleViewIndices[bone] =
                static_cast<std::int64_t>(views.size());
            views.push_back(animationScaleViews[bone]);
        }
    }

    std::vector<GltfImage> images;
    std::vector<std::int64_t> materialToImage;
    const std::filesystem::path uriBase = textures.UriBase.empty()
        ? gltfPath.parent_path()
        : textures.UriBase;
    CollectImages(obj, textures, uriBase, images, materialToImage);

    JsonWriter json;
    json.BeginObject();

    json.Key("asset");
    json.BeginObject();
    json.Key("version");
    json.Value("2.0");
    json.Key("generator");
    json.Value("Corsairs AssetConverter");
    if (assetMetadata.TerrainCoordinateProfile ==
        GltfTerrainCoordinateProfile::RigidQ) {
        json.Key("extras");
        json.BeginObject();
        json.Key("corsairsTerrainCoordinateProfile");
        json.Value("RigidQ");
        json.EndObject();
    }
    json.EndObject();

    json.Key("buffers");
    json.BeginArray();
    json.BeginObject();
    json.Key("uri");
    json.Value(binPath.filename().string());
    json.Key("byteLength");
    json.Value(static_cast<std::int64_t>(buffer.size()));
    json.EndObject();
    json.EndArray();

    json.Key("bufferViews");
    json.BeginArray();
    for (const BufferView& view : views) {
        json.BeginObject();
        json.Key("buffer");
        json.Value(static_cast<std::int64_t>(0));
        json.Key("byteOffset");
        json.Value(static_cast<std::int64_t>(view.ByteOffset));
        json.Key("byteLength");
        json.Value(static_cast<std::int64_t>(view.ByteLength));
        // target допустим только для ARRAY_BUFFER/ELEMENT_ARRAY_BUFFER.
        // Generic сохраняет прежние literal bytes; SceneMap для IBM и
        // animation views корректно опускает отсутствующий target.
        if (view.Target != 0 || profile == GltfCoordinateProfile::Generic) {
            json.Key("target");
            json.Value(view.Target);
        }
        json.EndObject();
    }
    json.EndArray();

    // Аккессоры в том же порядке: POSITION, [NORMAL], [TEXCOORD_0],
    // [COLOR_0], skin и индексы.
    json.Key("accessors");
    json.BeginArray();

    json.BeginObject();
    json.Key("bufferView");
    json.Value(positionViewIndex);
    json.Key("componentType");
    json.Value(kComponentTypeFloat);
    json.Key("count");
    json.Value(static_cast<std::int64_t>(positions.size()));
    json.Key("type");
    json.Value("VEC3");
    WriteAccessorBounds(json, positions);
    json.EndObject();

    std::int64_t normalAccessor = -1;
    if (hasNormals) {
        normalAccessor = 1;
        json.BeginObject();
        json.Key("bufferView");
        json.Value(normalViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(normals.size()));
        json.Key("type");
        json.Value("VEC3");
        json.EndObject();
    }

    std::int64_t uvAccessor = -1;
    if (hasUv) {
        uvAccessor = hasNormals ? 2 : 1;
        json.BeginObject();
        json.Key("bufferView");
        json.Value(uvViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(texcoords.size()));
        json.Key("type");
        json.Value("VEC2");
        json.EndObject();
    }

    std::int64_t colorAccessor = -1;
    if (hasVertexColors) {
        colorAccessor = 1 + (hasNormals ? 1 : 0) + (hasUv ? 1 : 0);
        json.BeginObject();
        json.Key("bufferView");
        json.Value(colorViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeUnsignedByte);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(mesh.VertexColors.size()));
        json.Key("type");
        json.Value("VEC4");
        json.Key("normalized");
        json.Value(true);
        json.EndObject();
    }

    std::int64_t jointsAccessor = -1;
    std::int64_t weightsAccessor = -1;
    if (hasSkin) {
        jointsAccessor = 1 + (hasNormals ? 1 : 0) + (hasUv ? 1 : 0) +
                         (hasVertexColors ? 1 : 0);
        weightsAccessor = jointsAccessor + 1;

        json.BeginObject();
        json.Key("bufferView");
        json.Value(jointsViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeUnsignedByte);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(mesh.Blends.size()));
        json.Key("type");
        json.Value("VEC4");
        json.EndObject();

        json.BeginObject();
        json.Key("bufferView");
        json.Value(weightsViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(mesh.Blends.size()));
        json.Key("type");
        json.Value("VEC4");
        json.EndObject();
    }

    std::int64_t inverseBindAccessor = -1;
    if (hasSkeleton) {
        inverseBindAccessor = 1 + (hasNormals ? 1 : 0) + (hasUv ? 1 : 0) +
                              (hasVertexColors ? 1 : 0) + 2;

        json.BeginObject();
        json.Key("bufferView");
        json.Value(inverseBindViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(skeletonBoneNum));
        json.Key("type");
        json.Value("MAT4");
        json.EndObject();
    }

    // По аккессору индексов на каждый подсет: они делят один bufferView,
    // отличаясь byteOffset и count.
    const std::int64_t firstSubsetAccessor =
        1 + (hasNormals ? 1 : 0) + (hasUv ? 1 : 0) + (hasSkin ? 2 : 0) +
        (hasVertexColors ? 1 : 0) + (hasSkeleton ? 1 : 0);

    std::int64_t animationTimeAccessor = -1;
    std::vector<std::int64_t> animationTranslationAccessors;
    std::vector<std::int64_t> animationRotationAccessors;
    std::vector<std::int64_t> animationScaleAccessors;

    for (const SubsetInfo& subset : mesh.Subsets) {
        json.BeginObject();
        json.Key("bufferView");
        json.Value(indexViewIndex);
        json.Key("byteOffset");
        json.Value(static_cast<std::int64_t>(subset.StartIndex) *
                   static_cast<std::int64_t>(sizeof(std::uint32_t)));
        json.Key("componentType");
        json.Value(kComponentTypeUnsignedInt);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(subset.PrimitiveNum) * 3);
        json.Key("type");
        json.Value("SCALAR");
        json.EndObject();
    }

    if (sceneAnimation.has_value()) {
        animationTimeAccessor = firstSubsetAccessor +
            static_cast<std::int64_t>(mesh.Subsets.size());
        json.BeginObject();
        json.Key("bufferView");
        json.Value(animationTimeViewIndex);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(sceneAnimation->Times.size()));
        json.Key("type");
        json.Value("SCALAR");
        json.Key("min");
        json.BeginArray();
        json.Value(static_cast<double>(sceneAnimation->Times.front()));
        json.EndArray();
        json.Key("max");
        json.BeginArray();
        json.Value(static_cast<double>(sceneAnimation->Times.back()));
        json.EndArray();
        json.EndObject();

        animationTranslationAccessors.resize(skeletonBoneNum);
        animationRotationAccessors.resize(skeletonBoneNum);
        animationScaleAccessors.resize(skeletonBoneNum);
        std::int64_t nextAccessor = animationTimeAccessor + 1;
        for (std::uint32_t bone = 0; bone < skeletonBoneNum; ++bone) {
            animationTranslationAccessors[bone] = nextAccessor++;
            json.BeginObject();
            json.Key("bufferView");
            json.Value(animationTranslationViewIndices[bone]);
            json.Key("componentType");
            json.Value(kComponentTypeFloat);
            json.Key("count");
            json.Value(static_cast<std::int64_t>(skeleton->Header.FrameNum));
            json.Key("type");
            json.Value("VEC3");
            json.EndObject();

            animationRotationAccessors[bone] = nextAccessor++;
            json.BeginObject();
            json.Key("bufferView");
            json.Value(animationRotationViewIndices[bone]);
            json.Key("componentType");
            json.Value(kComponentTypeFloat);
            json.Key("count");
            json.Value(static_cast<std::int64_t>(skeleton->Header.FrameNum));
            json.Key("type");
            json.Value("VEC4");
            json.EndObject();

            animationScaleAccessors[bone] = nextAccessor++;
            json.BeginObject();
            json.Key("bufferView");
            json.Value(animationScaleViewIndices[bone]);
            json.Key("componentType");
            json.Value(kComponentTypeFloat);
            json.Key("count");
            json.Value(static_cast<std::int64_t>(skeleton->Header.FrameNum));
            json.Key("type");
            json.Value("VEC3");
            json.EndObject();
        }
    }
    json.EndArray();

    if (!images.empty()) {
        json.Key("images");
        json.BeginArray();
        for (const GltfImage& image : images) {
            json.BeginObject();
            json.Key("uri");
            json.Value(image.Uri);
            json.EndObject();
        }
        json.EndArray();

        // Один сэмплер на всё: исходный движок не хранит режимы фильтрации
        // отдельно по текстурам в переносимом виде.
        json.Key("samplers");
        json.BeginArray();
        json.BeginObject();
        json.Key("wrapS");
        json.Value(static_cast<std::int64_t>(10497));   // REPEAT
        json.Key("wrapT");
        json.Value(static_cast<std::int64_t>(10497));
        json.EndObject();
        json.EndArray();

        json.Key("textures");
        json.BeginArray();
        for (std::size_t i = 0; i < images.size(); ++i) {
            json.BeginObject();
            json.Key("source");
            json.Value(static_cast<std::int64_t>(i));
            json.Key("sampler");
            json.Value(static_cast<std::int64_t>(0));
            json.EndObject();
        }
        json.EndArray();
    }

    if (!obj.Materials.empty()) {
        json.Key("materials");
        json.BeginArray();
        for (std::size_t m = 0; m < obj.Materials.size(); ++m) {
            const LgoMaterial& material = obj.Materials[m];

            json.BeginObject();
            json.Key("name");
            const std::string textureName = material.TextureName(0);
            if (profile == GltfCoordinateProfile::SceneMap) {
                json.Value(textureName.empty()
                               ? std::format("material_{}", m)
                               : std::format("{}{}", textureName, m));
            }
            else {
                json.Value(textureName.empty()
                               ? std::format("material_{}", m)
                               : textureName);
            }

            json.Key("pbrMetallicRoughness");
            json.BeginObject();
            if (m < materialToImage.size() && materialToImage[m] >= 0) {
                json.Key("baseColorTexture");
                json.BeginObject();
                json.Key("index");
                json.Value(materialToImage[m]);
                json.EndObject();
            }
            json.Key("baseColorFactor");
            json.BeginArray();
            json.Value(static_cast<double>(material.Mtl.Dif.R));
            json.Value(static_cast<double>(material.Mtl.Dif.G));
            json.Value(static_cast<double>(material.Mtl.Dif.B));
            json.Value(static_cast<double>(material.Opacity));
            json.EndArray();
            // Исходные материалы — фиксированный конвейер DX9 без PBR.
            // Металличность нулевая, шероховатость максимальная: это
            // нейтральная отправная точка, поверх которой в UE делается
            // настоящий материал.
            json.Key("metallicFactor");
            json.Value(0.0);
            json.Key("roughnessFactor");
            json.Value(1.0);
            json.EndObject();

            if (profile == GltfCoordinateProfile::SceneMap) {
                const LegacyMaterialMetadata& metadata = resolvedMaterials[m];
                json.Key("alphaMode");
                switch (metadata.Mode) {
                case LegacyMaterialMode::Opaque:
                    json.Value("OPAQUE");
                    break;
                case LegacyMaterialMode::Masked:
                    json.Value("MASK");
                    json.Key("alphaCutoff");
                    json.Value((static_cast<double>(metadata.AlphaRef) + 1.0) / 255.0);
                    break;
                case LegacyMaterialMode::Alpha:
                case LegacyMaterialMode::Additive:
                case LegacyMaterialMode::Subtractive:
                    json.Value("BLEND");
                    break;
                }
                json.Key("doubleSided");
                json.Value(true);
                WriteLegacyMaterialExtras(json, metadata);
            }
            else {
                if (material.Opacity < 1.0f) {
                    json.Key("alphaMode");
                    json.Value("BLEND");
                }
                json.Key("doubleSided");
                json.Value(true);
            }
            json.EndObject();
        }
        json.EndArray();
    }

    json.Key("meshes");
    json.BeginArray();
    json.BeginObject();
    json.Key("primitives");
    json.BeginArray();
    for (std::size_t i = 0; i < mesh.Subsets.size(); ++i) {
        json.BeginObject();
        json.Key("attributes");
        json.BeginObject();
        json.Key("POSITION");
        json.Value(static_cast<std::int64_t>(0));
        if (hasNormals) {
            json.Key("NORMAL");
            json.Value(normalAccessor);
        }
        if (hasUv) {
            json.Key("TEXCOORD_0");
            json.Value(uvAccessor);
        }
        if (hasVertexColors) {
            json.Key("COLOR_0");
            json.Value(colorAccessor);
        }
        if (hasSkin) {
            json.Key("JOINTS_0");
            json.Value(jointsAccessor);
            json.Key("WEIGHTS_0");
            json.Value(weightsAccessor);
        }
        json.EndObject();
        json.Key("indices");
        json.Value(firstSubsetAccessor + static_cast<std::int64_t>(i));
        // Подсет i рисуется материалом i — так их связывает движок.
        if (i < obj.Materials.size()) {
            json.Key("material");
            json.Value(static_cast<std::int64_t>(i));
        }
        json.Key("mode");
        json.Value(kModeTriangles);
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    json.EndArray();

    // В профиле SceneMap узел 0 — корень части. Он единственный владеет
    // MatModel, а меш и dummy-точки остаются в локальном пространстве части.
    // Generic сохраняет прежний плоский граф: узел 0 — сам меш.
    json.Key("nodes");
    json.BeginArray();
    if (profile == GltfCoordinateProfile::SceneMap) {
        float matrix[16]{};
        ConvertMatrixToGltf(obj.MatModel, matrix, profile);

        json.BeginObject();
        json.Key("name");
        json.Value("part_root");
        json.Key("children");
        json.BeginArray();
        for (std::size_t i = 0; i < 1 + obj.Helper.Dummies.size(); ++i) {
            json.Value(static_cast<std::int64_t>(i + 1));
        }
        if (hasSkeleton) {
            const std::size_t firstBoneNode = 2u + obj.Helper.Dummies.size();
            for (std::uint32_t bone = 0; bone < skeletonBoneNum; ++bone) {
                if (skeleton->Bones[bone].ParentId == kNoParent) {
                    json.Value(static_cast<std::int64_t>(firstBoneNode + bone));
                }
            }
        }
        json.EndArray();
        json.Key("matrix");
        json.BeginArray();
        for (const float value : matrix) {
            json.Value(static_cast<double>(value));
        }
        json.EndArray();
        WriteLegacyCaptureExtras(json, obj.CaptureBake);
        json.EndObject();
    }

    json.BeginObject();
    json.Key("mesh");
    json.Value(static_cast<std::int64_t>(0));
    json.Key("name");
    json.Value("mesh");
    if (hasSkin) {
        json.Key("skin");
        json.Value(static_cast<std::int64_t>(0));
    }

    // Положение объекта внутри модели. Без него все части `.lmo` схлопываются
    // в начало координат: у 417 моделей из 639 матрица неединичная, а у зданий
    // из нескольких объектов это означает груду деталей в одной точке.
    //
    // Скиннутый меш исключён намеренно: по спецификации glTF трансформ узла
    // скиннутого меша игнорируется, вершины полностью задаются суставами.
    // Записывать туда матрицу бессмысленно, а вводить в заблуждение — вредно.
    if (!hasSkin && profile == GltfCoordinateProfile::Generic) {
        float matrix[16]{};
        ConvertMatrixToGltf(obj.MatModel, matrix);

        constexpr float kIdentity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        bool isIdentity = true;
        for (std::size_t i = 0; i < 16; ++i) {
            if (std::fabs(matrix[i] - kIdentity[i]) > 1e-6f) {
                isIdentity = false;
                break;
            }
        }

        if (!isIdentity) {
            json.Key("matrix");
            json.BeginArray();
            for (const float value : matrix) {
                json.Value(static_cast<double>(value));
            }
            json.EndArray();
        }
    }
    json.EndObject();

    for (std::size_t i = 0; i < obj.Helper.Dummies.size(); ++i) {
        float matrix[16]{};
        ConvertMatrixToGltf(obj.Helper.Dummies[i].Mat, matrix, profile);

        json.BeginObject();
        json.Key("name");
        json.Value(std::format("dummy_{}", i));
        json.Key("matrix");
        json.BeginArray();
        for (const float value : matrix) {
            json.Value(static_cast<double>(value));
        }
        json.EndArray();
        json.EndObject();
    }

    // Узлы-суставы.
    //
    // Со скелетом пишется его полная иерархия: настоящие имена костей, связи
    // через ParentId и поза первого кадра. Импортёр строит скелет по тому, что
    // видит в файле меша, поэтому только так дорожки анимации из того же .lab
    // оказываются применимы.
    //
    // Без скелета остаётся плоский список тех костей, которыми меш реально
    // пользуется; имя несёт глобальный номер, по которому связь можно
    // восстановить позже.
    if (hasSkeleton) {
        const std::size_t firstBoneNode =
            (profile == GltfCoordinateProfile::SceneMap ? 2u : 1u) +
            obj.Helper.Dummies.size();

        for (std::uint32_t b = 0; b < skeletonBoneNum; ++b) {
            json.BeginObject();
            json.Key("name");
            const std::string boneName = BoneName(skeleton->Bones[b]);
            json.Value(boneName.empty() ? std::format("bone_{}", b) : boneName);

            float translation[3]{};
            float rotation[4]{0, 0, 0, 1};
            float scale[3]{1, 1, 1};
            if (sceneAnimation.has_value()) {
                std::copy_n(sceneAnimation->Translations[b].data(), 3u, translation);
                std::copy_n(sceneAnimation->Rotations[b].data(), 4u, rotation);
                std::copy_n(sceneAnimation->Scales[b].data(), 3u, scale);
            }
            else {
                BindPoseOf(*skeleton, b, translation, rotation);
            }

            json.Key("translation");
            json.BeginArray();
            for (const float value : translation) {
                json.Value(static_cast<double>(value));
            }
            json.EndArray();

            json.Key("rotation");
            json.BeginArray();
            for (const float value : rotation) {
                json.Value(static_cast<double>(value));
            }
            json.EndArray();

            if (sceneAnimation.has_value()) {
                json.Key("scale");
                json.BeginArray();
                for (const float value : scale) {
                    json.Value(static_cast<double>(value));
                }
                json.EndArray();
            }

            // Дети адресуются с учётом смещения: перед костями идут узел меша
            // и точки крепления самой модели.
            std::vector<std::int64_t> children;
            for (std::uint32_t other = 0; other < skeletonBoneNum; ++other) {
                if (skeleton->Bones[other].ParentId == b) {
                    children.push_back(static_cast<std::int64_t>(firstBoneNode + other));
                }
            }
            // Точки крепления скелета тоже входят в иерархию: импортёр считает
            // их костями, и без них дерево меша окажется короче, чем у
            // анимации, — 56 против 77, и дорожка к мешу не применится.
            for (std::size_t d = 0; d < skeleton->Dummies.size(); ++d) {
                if (skeleton->Dummies[d].ParentBoneId == b) {
                    children.push_back(static_cast<std::int64_t>(
                        firstBoneNode + skeletonBoneNum + d));
                }
            }
            if (!children.empty()) {
                json.Key("children");
                json.BeginArray();
                for (const std::int64_t child : children) {
                    json.Value(child);
                }
                json.EndArray();
            }
            json.EndObject();
        }

        for (std::size_t d = 0; d < skeleton->Dummies.size(); ++d) {
            float matrix[16]{};
            ConvertMatrixToGltf(skeleton->Dummies[d].Mat, matrix, profile);

            json.BeginObject();
            json.Key("name");
            json.Value(std::format("bone_dummy_{}", d));
            json.Key("matrix");
            json.BeginArray();
            for (const float value : matrix) {
                json.Value(static_cast<double>(value));
            }
            json.EndArray();
            json.EndObject();
        }
    }
    else if (skinPolicy == GltfSkinPolicy::Preserve &&
             profile == GltfCoordinateProfile::Generic) {
        for (std::size_t i = 0; i < mesh.BoneIndices.size(); ++i) {
            json.BeginObject();
            json.Key("name");
            json.Value(std::format("bone_{}", mesh.BoneIndices[i]));
            json.EndObject();
        }
    }
    json.EndArray();

    if (hasSkin) {
        // Плоский Generic skin без skeleton оставляет единичные IBM по
        // умолчанию. Parsed BONE всегда публикует исходные inverse bind
        // matrices и полную иерархию.
        json.Key("skins");
        json.BeginArray();
        json.BeginObject();
        json.Key("joints");
        json.BeginArray();
        const std::size_t firstJointNode =
            (profile == GltfCoordinateProfile::SceneMap ? 2u : 1u) +
            obj.Helper.Dummies.size();
        const std::size_t jointCount =
            hasSkeleton ? skeletonBoneNum : mesh.BoneIndices.size();
        for (std::size_t i = 0; i < jointCount; ++i) {
            json.Value(static_cast<std::int64_t>(firstJointNode + i));
        }
        json.EndArray();
        if (sceneAnimation.has_value()) {
            json.Key("skeleton");
            json.Value(static_cast<std::int64_t>(
                firstJointNode + sceneAnimation->RootBone));
        }
        if (hasSkeleton) {
            json.Key("inverseBindMatrices");
            json.Value(inverseBindAccessor);
        }
        json.EndObject();
        json.EndArray();
    }

    if (sceneAnimation.has_value()) {
        const std::size_t firstBoneNode = 2u + obj.Helper.Dummies.size();
        json.Key("animations");
        json.BeginArray();
        json.BeginObject();
        json.Key("name");
        json.Value("legacy_bone");

        json.Key("samplers");
        json.BeginArray();
        for (std::uint32_t bone = 0; bone < skeletonBoneNum; ++bone) {
            const std::array<std::int64_t, 3> outputs{
                animationTranslationAccessors[bone],
                animationRotationAccessors[bone],
                animationScaleAccessors[bone]};
            for (const std::int64_t outputAccessor : outputs) {
                json.BeginObject();
                json.Key("input");
                json.Value(animationTimeAccessor);
                json.Key("output");
                json.Value(outputAccessor);
                json.Key("interpolation");
                json.Value("LINEAR");
                json.EndObject();
            }
        }
        json.EndArray();

        json.Key("channels");
        json.BeginArray();
        for (std::uint32_t bone = 0; bone < skeletonBoneNum; ++bone) {
            constexpr std::array<std::string_view, 3> paths{
                "translation", "rotation", "scale"};
            for (std::size_t channel = 0; channel < paths.size(); ++channel) {
                json.BeginObject();
                json.Key("sampler");
                json.Value(static_cast<std::int64_t>(bone) * 3 +
                           static_cast<std::int64_t>(channel));
                json.Key("target");
                json.BeginObject();
                json.Key("node");
                json.Value(static_cast<std::int64_t>(firstBoneNode + bone));
                json.Key("path");
                json.Value(paths[channel]);
                json.EndObject();
                json.EndObject();
            }
        }
        json.EndArray();
        json.EndObject();
        json.EndArray();
    }

    // В сцену обязаны попасть ВСЕ узлы, включая суставы: спецификация glTF
    // требует, чтобы суставы skin'а находились в той же сцене, что и скиннутый
    // меш. Импортёр Blender обходит skin.joints напрямую и стерпит их
    // отсутствие, а Interchange в UE строит скелет от корней сцены и падает на
    // ensure(SkeletonNodeUid).
    const std::size_t sceneNodeCount =
        1 + obj.Helper.Dummies.size() +
        (hasSkeleton ? skeletonBoneNum + skeleton->Dummies.size()
                     : (hasSkin ? mesh.BoneIndices.size() : 0));

    json.Key("scenes");
    json.BeginArray();
    json.BeginObject();
    json.Key("nodes");
    json.BeginArray();
    if (profile == GltfCoordinateProfile::SceneMap) {
        json.Value(static_cast<std::int64_t>(0));
    }
    else {
        for (std::size_t i = 0; i < sceneNodeCount; ++i) {
            json.Value(static_cast<std::int64_t>(i));
        }
    }
    json.EndArray();
    json.EndObject();
    json.EndArray();

    json.Key("scene");
    json.Value(static_cast<std::int64_t>(0));

    json.EndObject();

    const std::string& text = json.Str();
    if (!WriteFile(*gltfTemp, text.data(), text.size())) {
        RemoveAttemptFile(*gltfTemp);
        RemoveAttemptFile(*binTemp);
        detail = "не удалось записать временный .gltf";
        return GltfStatus::WRITE_FAILED;
    }
    if (!PublishGltfPair(*gltfTemp, *binTemp, gltfPath, binPath, detail)) {
        return GltfStatus::WRITE_FAILED;
    }

    detail.clear();
    return GltfStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
