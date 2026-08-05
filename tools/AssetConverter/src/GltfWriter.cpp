#include "Corsairs/Tools/AssetConverter/GltfWriter.h"

#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <cstring>
#include <format>
#include <fstream>
#include <limits>
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
    return static_cast<bool>(stream);
}

} // namespace

void ConvertMatrixToGltf(const float* in, float* out) {
    // Единственная нужная операция — S*M*S: отрицаются элементы, у которых
    // ровно один индекс равен 2. Транспонировать НЕ нужно, см. комментарий
    // к объявлению функции.
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float value = in[row * 4 + col];
            const bool negate = (row == 2) != (col == 2);
            out[row * 4 + col] = negate ? -value : value;
        }
    }
}

GltfStatus WriteGltf(const LgoGeomObj& obj, const std::filesystem::path& gltfPath,
                     std::string& detail) {
    const LgoMesh& mesh = obj.Mesh;

    if (mesh.Positions.empty() || mesh.Indices.empty() || mesh.Subsets.empty()) {
        detail = "меш не содержит вершин, индексов или подсетов";
        return GltfStatus::EMPTY_MESH;
    }

    // Преобразование левосторонней системы координат в правостороннюю.
    std::vector<Vector3> positions = mesh.Positions;
    for (Vector3& p : positions) {
        p.Z = -p.Z;
    }

    std::vector<Vector3> normals = mesh.Normals;
    for (Vector3& n : normals) {
        n.Z = -n.Z;
    }

    // Смена порядка обхода треугольника — парная операция к инверсии Z.
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
    if (hasUv) {
        uvView = AppendToBuffer(buffer, mesh.Texcoords[0].data(),
                                mesh.Texcoords[0].size() * sizeof(Vector2),
                                kTargetArrayBuffer);
    }

    // Скиннинг. BlendInfo::Index индексирует BoneIndices (локальный индекс ->
    // глобальный id кости), а glTF JOINTS_0 индексирует массив skin.joints.
    // Строим joints в том же порядке, что BoneIndices, — тогда индексы
    // переносятся один в один без перенумерации.
    const bool hasSkin = !mesh.Blends.empty() && !mesh.BoneIndices.empty();

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
                joints[v * 4 + k] = valid ? blend.Index[k] : 0;
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

    const BufferView indexView = AppendToBuffer(
        buffer, indices.data(), indices.size() * sizeof(std::uint32_t),
        kTargetElementArrayBuffer);

    std::filesystem::path binPath = gltfPath;
    binPath.replace_extension(".bin");

    if (!WriteFile(binPath, buffer.data(), buffer.size())) {
        detail = "не удалось записать .bin";
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

    std::int64_t jointsViewIndex = -1;
    std::int64_t weightsViewIndex = -1;
    if (hasSkin) {
        jointsViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(jointsView);
        weightsViewIndex = static_cast<std::int64_t>(views.size());
        views.push_back(weightsView);
    }

    const std::int64_t indexViewIndex = static_cast<std::int64_t>(views.size());
    views.push_back(indexView);

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
        json.Key("target");
        json.Value(view.Target);
        json.EndObject();
    }
    json.EndArray();

    // Аккессоры в том же порядке: POSITION, [NORMAL], [TEXCOORD_0], индексы.
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
        json.Value(static_cast<std::int64_t>(mesh.Texcoords[0].size()));
        json.Key("type");
        json.Value("VEC2");
        json.EndObject();
    }

    std::int64_t jointsAccessor = -1;
    std::int64_t weightsAccessor = -1;
    if (hasSkin) {
        jointsAccessor = 1 + (hasNormals ? 1 : 0) + (hasUv ? 1 : 0);
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

    // По аккессору индексов на каждый подсет: они делят один bufferView,
    // отличаясь byteOffset и count.
    const std::int64_t firstSubsetAccessor =
        1 + (hasNormals ? 1 : 0) + (hasUv ? 1 : 0) + (hasSkin ? 2 : 0);

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
    json.EndArray();

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
        if (hasSkin) {
            json.Key("JOINTS_0");
            json.Value(jointsAccessor);
            json.Key("WEIGHTS_0");
            json.Value(weightsAccessor);
        }
        json.EndObject();
        json.Key("indices");
        json.Value(firstSubsetAccessor + static_cast<std::int64_t>(i));
        json.Key("mode");
        json.Value(kModeTriangles);
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    json.EndArray();

    // Узел 0 — сам меш. Следом по узлу на каждую dummy-точку крепления:
    // так UE и Blender видят их как обычные объекты сцены с трансформацией,
    // и к ним можно привязывать оружие и эффекты.
    json.Key("nodes");
    json.BeginArray();
    json.BeginObject();
    json.Key("mesh");
    json.Value(static_cast<std::int64_t>(0));
    json.Key("name");
    json.Value("mesh");
    if (hasSkin) {
        json.Key("skin");
        json.Value(static_cast<std::int64_t>(0));
    }
    json.EndObject();

    for (std::size_t i = 0; i < obj.Helper.Dummies.size(); ++i) {
        float matrix[16]{};
        ConvertMatrixToGltf(obj.Helper.Dummies[i].Mat, matrix);

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

    // Узлы-суставы. Имя несёт ГЛОБАЛЬНЫЙ id кости из BoneIndices — по нему
    // меш связывается с настоящим скелетом из .lab на этапе импорта. Сами
    // узлы плоские и без трансформаций: иерархия и bind-позы живут в .lab,
    // а какой именно .lab соответствует модели, задаётся игровыми данными.
    for (std::size_t i = 0; i < mesh.BoneIndices.size(); ++i) {
        json.BeginObject();
        json.Key("name");
        json.Value(std::format("bone_{}", mesh.BoneIndices[i]));
        json.EndObject();
    }
    json.EndArray();

    if (hasSkin) {
        // inverseBindMatrices не указываются намеренно: по спецификации это
        // необязательное поле, при его отсутствии подразумеваются единичные
        // матрицы. Настоящие обратные bind-матрицы лежат в .lab.
        json.Key("skins");
        json.BeginArray();
        json.BeginObject();
        json.Key("joints");
        json.BeginArray();
        const std::size_t firstJointNode = 1 + obj.Helper.Dummies.size();
        for (std::size_t i = 0; i < mesh.BoneIndices.size(); ++i) {
            json.Value(static_cast<std::int64_t>(firstJointNode + i));
        }
        json.EndArray();
        json.EndObject();
        json.EndArray();
    }

    json.Key("scenes");
    json.BeginArray();
    json.BeginObject();
    json.Key("nodes");
    json.BeginArray();
    for (std::size_t i = 0; i <= obj.Helper.Dummies.size(); ++i) {
        json.Value(static_cast<std::int64_t>(i));
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
        return GltfStatus::WRITE_FAILED;
    }

    detail.clear();
    return GltfStatus::OK;
}

} // namespace Corsairs::Tools::AssetConverter
