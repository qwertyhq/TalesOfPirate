#include "Corsairs/Tools/AssetConverter/GltfWriter.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/ImageCodec.h"
#include "Corsairs/Tools/AssetConverter/JsonWriter.h"

#include <cctype>
#include <cmath>
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

} // namespace

void ConvertMatrixToGltf(const float* in, float* out) {
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
}

GltfStatus WriteGltf(const LgoGeomObj& obj, const std::filesystem::path& gltfPath,
                     std::string& detail, const GltfTextureOptions& textures) {
    const LgoMesh& mesh = obj.Mesh;

    if (mesh.Positions.empty() || mesh.Indices.empty() || mesh.Subsets.empty()) {
        detail = "меш не содержит вершин, индексов или подсетов";
        return GltfStatus::EMPTY_MESH;
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

    std::vector<GltfImage> images;
    std::vector<std::int64_t> materialToImage;
    CollectImages(obj, textures, gltfPath.parent_path(), images, materialToImage);

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
        json.Value(static_cast<std::int64_t>(texcoords.size()));
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
            json.Value(material.TextureName(0).empty()
                           ? std::format("material_{}", m)
                           : material.TextureName(0));

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

            if (material.Opacity < 1.0f) {
                json.Key("alphaMode");
                json.Value("BLEND");
            }
            json.Key("doubleSided");
            json.Value(true);
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

    // Положение объекта внутри модели. Без него все части `.lmo` схлопываются
    // в начало координат: у 417 моделей из 639 матрица неединичная, а у зданий
    // из нескольких объектов это означает груду деталей в одной точке.
    //
    // Скиннутый меш исключён намеренно: по спецификации glTF трансформ узла
    // скиннутого меша игнорируется, вершины полностью задаются суставами.
    // Записывать туда матрицу бессмысленно, а вводить в заблуждение — вредно.
    if (!hasSkin) {
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

    // В сцену обязаны попасть ВСЕ узлы, включая суставы: спецификация glTF
    // требует, чтобы суставы skin'а находились в той же сцене, что и скиннутый
    // меш. Импортёр Blender обходит skin.joints напрямую и стерпит их
    // отсутствие, а Interchange в UE строит скелет от корней сцены и падает на
    // ensure(SkeletonNodeUid).
    const std::size_t sceneNodeCount =
        1 + obj.Helper.Dummies.size() + (hasSkin ? mesh.BoneIndices.size() : 0);

    json.Key("scenes");
    json.BeginArray();
    json.BeginObject();
    json.Key("nodes");
    json.BeginArray();
    for (std::size_t i = 0; i < sceneNodeCount; ++i) {
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
