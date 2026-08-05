#include "Corsairs/Tools/AssetConverter/GltfSkeletonWriter.h"

#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::int64_t kComponentTypeFloat = 5126;

struct BufferView {
    std::size_t ByteOffset;
    std::size_t ByteLength;
};

BufferView AppendToBuffer(std::vector<std::uint8_t>& buffer, const void* data,
                          std::size_t bytes) {
    while (buffer.size() % 4 != 0) {
        buffer.push_back(0);
    }
    const std::size_t offset = buffer.size();
    buffer.resize(offset + bytes);
    if (bytes > 0) {
        std::memcpy(buffer.data() + offset, data, bytes);
    }
    return BufferView{offset, bytes};
}

bool WriteFile(const std::filesystem::path& path, const void* data, std::size_t bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    if (bytes > 0) {
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    }
    return static_cast<bool>(stream);
}

void WriteFloatArray(JsonWriter& json, const float* values, std::size_t count) {
    json.BeginArray();
    for (std::size_t i = 0; i < count; ++i) {
        json.Value(static_cast<double>(values[i]));
    }
    json.EndArray();
}

} // namespace

void ConvertQuaternionToGltf(const Quaternion& in, float* out) {
    out[0] = -in.X;
    out[1] = -in.Y;
    out[2] = in.Z;
    out[3] = in.W;

    // glTF требует нормализованных кватернионов в каналах поворота.
    const float lengthSq = out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3];
    if (lengthSq > 0.0f) {
        const float inv = 1.0f / std::sqrt(lengthSq);
        out[0] *= inv;
        out[1] *= inv;
        out[2] *= inv;
        out[3] *= inv;
    }
    else {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
    }
}

Trs DecomposeGltfMatrix(const float* m) {
    Trs trs{};

    // column-major: элемент (строка i, столбец j) лежит по индексу j*4+i.
    trs.Translation[0] = m[12];
    trs.Translation[1] = m[13];
    trs.Translation[2] = m[14];

    float basis[3][3];
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            basis[row][col] = m[col * 4 + row];
        }
    }

    for (int col = 0; col < 3; ++col) {
        const float length = std::sqrt(basis[0][col] * basis[0][col] +
                                       basis[1][col] * basis[1][col] +
                                       basis[2][col] * basis[2][col]);
        trs.Scale[col] = length;
        if (length > 0.0f) {
            const float inv = 1.0f / length;
            basis[0][col] *= inv;
            basis[1][col] *= inv;
            basis[2][col] *= inv;
        }
    }

    // Извлечение кватерниона из матрицы поворота (метод Шеппарда): выбирается
    // ветка с наибольшим знаменателем, иначе теряется точность и возможно
    // деление на ноль при повороте на 180°.
    const float trace = basis[0][0] + basis[1][1] + basis[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        trs.Rotation[3] = 0.25f * s;
        trs.Rotation[0] = (basis[2][1] - basis[1][2]) / s;
        trs.Rotation[1] = (basis[0][2] - basis[2][0]) / s;
        trs.Rotation[2] = (basis[1][0] - basis[0][1]) / s;
    }
    else if (basis[0][0] > basis[1][1] && basis[0][0] > basis[2][2]) {
        const float s = std::sqrt(1.0f + basis[0][0] - basis[1][1] - basis[2][2]) * 2.0f;
        trs.Rotation[3] = (basis[2][1] - basis[1][2]) / s;
        trs.Rotation[0] = 0.25f * s;
        trs.Rotation[1] = (basis[0][1] + basis[1][0]) / s;
        trs.Rotation[2] = (basis[0][2] + basis[2][0]) / s;
    }
    else if (basis[1][1] > basis[2][2]) {
        const float s = std::sqrt(1.0f + basis[1][1] - basis[0][0] - basis[2][2]) * 2.0f;
        trs.Rotation[3] = (basis[0][2] - basis[2][0]) / s;
        trs.Rotation[0] = (basis[0][1] + basis[1][0]) / s;
        trs.Rotation[1] = 0.25f * s;
        trs.Rotation[2] = (basis[1][2] + basis[2][1]) / s;
    }
    else {
        const float s = std::sqrt(1.0f + basis[2][2] - basis[0][0] - basis[1][1]) * 2.0f;
        trs.Rotation[3] = (basis[1][0] - basis[0][1]) / s;
        trs.Rotation[0] = (basis[0][2] + basis[2][0]) / s;
        trs.Rotation[1] = (basis[1][2] + basis[2][1]) / s;
        trs.Rotation[2] = 0.25f * s;
    }

    return trs;
}

GltfSkeletonStatus WriteSkeletonGltf(const LabAnimation& anim,
                                     std::string_view animationName,
                                     const std::filesystem::path& gltfPath,
                                     std::string& detail) {
    const std::uint32_t boneNum = anim.Header.BoneNum;
    const std::uint32_t frameNum = anim.Header.FrameNum;

    if (boneNum == 0 || frameNum == 0) {
        detail = std::format("скелет пуст: boneNum={}, frameNum={}", boneNum, frameNum);
        return GltfSkeletonStatus::EMPTY_SKELETON;
    }

    // Приводим ключи всех типов к общему виду TRS в системе координат glTF.
    std::vector<std::vector<float>> translations(boneNum);
    std::vector<std::vector<float>> rotations(boneNum);

    for (std::uint32_t b = 0; b < boneNum; ++b) {
        translations[b].resize(static_cast<std::size_t>(frameNum) * 3);
        rotations[b].resize(static_cast<std::size_t>(frameNum) * 4);

        const LabBoneTrack& track = anim.Tracks[b];
        const bool hasTrs = !track.Positions.empty() && !track.Rotations.empty();

        for (std::uint32_t f = 0; f < frameNum; ++f) {
            if (hasTrs) {
                translations[b][f * 3 + 0] = track.Positions[f].X;
                translations[b][f * 3 + 1] = track.Positions[f].Y;
                translations[b][f * 3 + 2] = -track.Positions[f].Z;
                ConvertQuaternionToGltf(track.Rotations[f], rotations[b].data() + f * 4);
            }
            else {
                float converted[16]{};
                ConvertMatrixToGltf(track.Matrices.data() + static_cast<std::size_t>(f) * 16,
                                    converted);
                const Trs trs = DecomposeGltfMatrix(converted);
                translations[b][f * 3 + 0] = trs.Translation[0];
                translations[b][f * 3 + 1] = trs.Translation[1];
                translations[b][f * 3 + 2] = trs.Translation[2];
                rotations[b][f * 4 + 0] = trs.Rotation[0];
                rotations[b][f * 4 + 1] = trs.Rotation[1];
                rotations[b][f * 4 + 2] = trs.Rotation[2];
                rotations[b][f * 4 + 3] = trs.Rotation[3];
            }
        }
    }

    std::vector<float> times(frameNum);
    for (std::uint32_t f = 0; f < frameNum; ++f) {
        times[f] = static_cast<float>(f) / kAnimFramesPerSecond;
    }

    std::vector<float> inverseBind(static_cast<std::size_t>(boneNum) * 16);
    for (std::uint32_t b = 0; b < boneNum; ++b) {
        ConvertMatrixToGltf(anim.InverseBindMatrices.data() + static_cast<std::size_t>(b) * 16,
                            inverseBind.data() + static_cast<std::size_t>(b) * 16);
    }

    // Раскладка буфера: времена, затем по кости перенос и поворот, затем
    // обратные bind-матрицы. Порядок здесь и порядок аккессоров ниже обязаны
    // совпадать.
    std::vector<std::uint8_t> buffer;
    std::vector<BufferView> views;

    views.push_back(AppendToBuffer(buffer, times.data(), times.size() * sizeof(float)));
    const std::int64_t timeView = 0;

    std::vector<std::int64_t> translationViews(boneNum);
    std::vector<std::int64_t> rotationViews(boneNum);
    for (std::uint32_t b = 0; b < boneNum; ++b) {
        translationViews[b] = static_cast<std::int64_t>(views.size());
        views.push_back(AppendToBuffer(buffer, translations[b].data(),
                                       translations[b].size() * sizeof(float)));
        rotationViews[b] = static_cast<std::int64_t>(views.size());
        views.push_back(AppendToBuffer(buffer, rotations[b].data(),
                                       rotations[b].size() * sizeof(float)));
    }

    const std::int64_t inverseBindView = static_cast<std::int64_t>(views.size());
    views.push_back(AppendToBuffer(buffer, inverseBind.data(),
                                   inverseBind.size() * sizeof(float)));

    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");
    if (!WriteFile(binPath, buffer.data(), buffer.size())) {
        detail = "не удалось записать .bin";
        return GltfSkeletonStatus::WRITE_FAILED;
    }

    JsonWriter json;
    json.BeginObject();

    json.Key("asset");
    json.BeginObject();
    json.Key("version");
    json.Value("2.0");
    json.Key("generator");
    json.Value("Corsairs AssetConverter");
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
        json.EndObject();
    }
    json.EndArray();

    // Аккессоры: [0] времена, затем по два на кость, последний — bind-матрицы.
    json.Key("accessors");
    json.BeginArray();

    json.BeginObject();
    json.Key("bufferView");
    json.Value(timeView);
    json.Key("componentType");
    json.Value(kComponentTypeFloat);
    json.Key("count");
    json.Value(static_cast<std::int64_t>(frameNum));
    json.Key("type");
    json.Value("SCALAR");
    // Для входного аккессора анимации min/max обязательны по спецификации.
    json.Key("min");
    json.BeginArray();
    json.Value(static_cast<double>(times.front()));
    json.EndArray();
    json.Key("max");
    json.BeginArray();
    json.Value(static_cast<double>(times.back()));
    json.EndArray();
    json.EndObject();

    for (std::uint32_t b = 0; b < boneNum; ++b) {
        json.BeginObject();
        json.Key("bufferView");
        json.Value(translationViews[b]);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(frameNum));
        json.Key("type");
        json.Value("VEC3");
        json.EndObject();

        json.BeginObject();
        json.Key("bufferView");
        json.Value(rotationViews[b]);
        json.Key("componentType");
        json.Value(kComponentTypeFloat);
        json.Key("count");
        json.Value(static_cast<std::int64_t>(frameNum));
        json.Key("type");
        json.Value("VEC4");
        json.EndObject();
    }

    const std::int64_t inverseBindAccessor = 1 + static_cast<std::int64_t>(boneNum) * 2;
    json.BeginObject();
    json.Key("bufferView");
    json.Value(inverseBindView);
    json.Key("componentType");
    json.Value(kComponentTypeFloat);
    json.Key("count");
    json.Value(static_cast<std::int64_t>(boneNum));
    json.Key("type");
    json.Value("MAT4");
    json.EndObject();
    json.EndArray();

    // Узлы: по одному на кость, затем dummy-точки скелета. Иерархия строится
    // из ParentId; поза берётся из первого кадра.
    json.Key("nodes");
    json.BeginArray();
    for (std::uint32_t b = 0; b < boneNum; ++b) {
        json.BeginObject();
        json.Key("name");
        const std::string name = BoneName(anim.Bones[b]);
        json.Value(name.empty() ? std::format("bone_{}", b) : name);

        json.Key("translation");
        WriteFloatArray(json, translations[b].data(), 3);
        json.Key("rotation");
        WriteFloatArray(json, rotations[b].data(), 4);

        // Дети — кости, чей ParentId указывает на эту, плюс dummy на ней.
        std::vector<std::int64_t> children;
        for (std::uint32_t other = 0; other < boneNum; ++other) {
            if (anim.Bones[other].ParentId == b) {
                children.push_back(static_cast<std::int64_t>(other));
            }
        }
        for (std::size_t d = 0; d < anim.Dummies.size(); ++d) {
            if (anim.Dummies[d].ParentBoneId == b) {
                children.push_back(static_cast<std::int64_t>(boneNum + d));
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

    for (std::size_t d = 0; d < anim.Dummies.size(); ++d) {
        float matrix[16]{};
        ConvertMatrixToGltf(anim.Dummies[d].Mat, matrix);

        json.BeginObject();
        json.Key("name");
        json.Value(std::format("bone_dummy_{}", d));
        json.Key("matrix");
        WriteFloatArray(json, matrix, 16);
        json.EndObject();
    }
    json.EndArray();

    // Skin без меша: joints и обратные bind-матрицы сохраняются, чтобы меш
    // можно было привязать к этому скелету на следующем этапе.
    json.Key("skins");
    json.BeginArray();
    json.BeginObject();
    json.Key("inverseBindMatrices");
    json.Value(inverseBindAccessor);
    json.Key("joints");
    json.BeginArray();
    for (std::uint32_t b = 0; b < boneNum; ++b) {
        json.Value(static_cast<std::int64_t>(b));
    }
    json.EndArray();
    json.EndObject();
    json.EndArray();

    json.Key("animations");
    json.BeginArray();
    json.BeginObject();
    json.Key("name");
    json.Value(animationName);

    json.Key("samplers");
    json.BeginArray();
    for (std::uint32_t b = 0; b < boneNum; ++b) {
        json.BeginObject();
        json.Key("input");
        json.Value(static_cast<std::int64_t>(0));
        json.Key("output");
        json.Value(1 + static_cast<std::int64_t>(b) * 2);
        json.Key("interpolation");
        json.Value("LINEAR");
        json.EndObject();

        json.BeginObject();
        json.Key("input");
        json.Value(static_cast<std::int64_t>(0));
        json.Key("output");
        json.Value(2 + static_cast<std::int64_t>(b) * 2);
        json.Key("interpolation");
        json.Value("LINEAR");
        json.EndObject();
    }
    json.EndArray();

    json.Key("channels");
    json.BeginArray();
    for (std::uint32_t b = 0; b < boneNum; ++b) {
        json.BeginObject();
        json.Key("sampler");
        json.Value(static_cast<std::int64_t>(b) * 2);
        json.Key("target");
        json.BeginObject();
        json.Key("node");
        json.Value(static_cast<std::int64_t>(b));
        json.Key("path");
        json.Value("translation");
        json.EndObject();
        json.EndObject();

        json.BeginObject();
        json.Key("sampler");
        json.Value(static_cast<std::int64_t>(b) * 2 + 1);
        json.Key("target");
        json.BeginObject();
        json.Key("node");
        json.Value(static_cast<std::int64_t>(b));
        json.Key("path");
        json.Value("rotation");
        json.EndObject();
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    json.EndArray();

    json.Key("scenes");
    json.BeginArray();
    json.BeginObject();
    json.Key("nodes");
    json.BeginArray();
    for (std::uint32_t b = 0; b < boneNum; ++b) {
        if (anim.Bones[b].ParentId == kNoParent) {
            json.Value(static_cast<std::int64_t>(b));
        }
    }
    json.EndArray();
    json.EndObject();
    json.EndArray();

    json.Key("scene");
    json.Value(static_cast<std::int64_t>(0));

    json.EndObject();

    const std::string& text = json.Str();
    if (!WriteFile(gltfPath, text.data(), text.size())) {
        detail = "не удалось записать .gltf";
        return GltfSkeletonStatus::WRITE_FAILED;
    }

    detail.clear();
    return GltfSkeletonStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
