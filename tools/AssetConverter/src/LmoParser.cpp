#include "Corsairs/Tools/AssetConverter/LmoParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

namespace {

constexpr std::uint32_t kLmoNoParent = 0xFFFFFFFFu;

// Перемножение 4x4 в порядке движка: lwMatrix44Multiply(&out, &a, &b) даёт
// out = a * b при строчных векторах DirectX, то есть сначала применяется `a`.
void Multiply(const float* a, const float* b, float* out) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + col];
            }
            out[row * 4 + col] = sum;
        }
    }
}

} // namespace

void ResolveModelMatrices(std::vector<LgoGeomObj>& objects) {
    std::unordered_map<std::uint32_t, std::size_t> byId;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        byId.emplace(objects[i].Header.Id, i);
    }

    std::vector<char> done(objects.size(), 0);

    // Итеративный подъём вместо рекурсии: глубина деревьев не ограничена
    // форматом, а стек — ограничен.
    for (std::size_t start = 0; start < objects.size(); ++start) {
        if (done[start]) {
            continue;
        }

        std::vector<std::size_t> chain;
        std::vector<char> visiting(objects.size(), 0);
        std::size_t current = start;

        while (!done[current]) {
            if (visiting[current]) {
                // Цикл: обрываем, оставляя уже накопленное.
                break;
            }
            visiting[current] = 1;
            chain.push_back(current);

            const std::uint32_t parentId = objects[current].Header.ParentId;
            if (parentId == kLmoNoParent) {
                break;
            }
            const auto it = byId.find(parentId);
            if (it == byId.end() || it->second == current) {
                break;
            }
            current = it->second;
        }

        // Идём от самого верхнего к исходному, домножая на уже готовую матрицу
        // родителя.
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const std::size_t index = *it;
            const std::uint32_t parentId = objects[index].Header.ParentId;
            const auto parent = byId.find(parentId);
            if (parentId != kLmoNoParent && parent != byId.end() &&
                parent->second != index && done[parent->second]) {
                float composed[16]{};
                Multiply(objects[index].Header.MatLocal,
                         objects[parent->second].MatModel, composed);
                std::copy_n(composed, 16, objects[index].MatModel);
            }
            done[index] = 1;
        }
    }
}

std::optional<LmoModel> ParseLmo(std::span<const std::uint8_t> bytes,
                                 LgoDiagnostics& diag) {
    diag = {};
    BinaryReader reader{bytes};

    LmoModel model;

    if (!reader.Read(model.Version)) {
        diag.Status = LgoStatus::VERSION_TRUNCATED;
        diag.Detail = "файл короче 4 байт";
        return std::nullopt;
    }
    diag.Version = model.Version;

    if (!IsKnownVersion(model.Version)) {
        diag.Status = LgoStatus::VERSION_UNKNOWN;
        diag.Detail = std::format("version=0x{:08X}, ожидалось 0x0000 или 0x1000..0x1005",
                                  model.Version);
        return std::nullopt;
    }

    if (!IsSupportedGeomVersion(model.Version)) {
        diag.Status = LgoStatus::VERSION_UNSUPPORTED;
        diag.Detail = std::format(
            "version=0x{:08X}: поддерживаются 0x0000 и 0x{:04X}+",
            model.Version, kMinSupportedVersion);
        return std::nullopt;
    }

    std::uint32_t objNum = 0;
    if (!reader.Read(objNum)) {
        diag.Status = LgoStatus::HEADER_TRUNCATED;
        diag.Detail = "не прочитан ObjNum";
        return std::nullopt;
    }

    std::vector<ModelObjEntry> entries(objNum);
    if (objNum > 0 && !reader.ReadArray(entries.data(), objNum)) {
        diag.Status = LgoStatus::HEADER_TRUNCATED;
        diag.Detail = std::format("не прочитано оглавление из {} записей", objNum);
        return std::nullopt;
    }

    LgoStatus worst = LgoStatus::OK;

    for (std::uint32_t i = 0; i < objNum; ++i) {
        const ModelObjEntry& entry = entries[i];

        const std::uint64_t end =
            static_cast<std::uint64_t>(entry.Addr) + static_cast<std::uint64_t>(entry.Size);
        if (end > bytes.size()) {
            diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
            diag.Detail = std::format(
                "запись {}: addr={} size={} выходит за пределы файла ({} байт)",
                i, entry.Addr, entry.Size, bytes.size());
            return std::nullopt;
        }

        if (!reader.Seek(entry.Addr)) {
            diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
            diag.Detail = std::format("запись {}: недостижимое смещение {}", i, entry.Addr);
            return std::nullopt;
        }

        std::size_t available = entry.Size;

        switch (static_cast<ModelObjType>(entry.Type)) {
        case ModelObjType::GEOMETRY: {
            // Вложенная версия перед телом объекта читается только для записей
            // геометрии: helper-блок читает свою сам, как и в движке. Читать её
            // здесь для обеих записей — значит сместиться на 4 байта и получить
            // мусорный Type у helper'а.
            if (model.Version == kLegacyVersion) {
                std::uint32_t innerVersion = 0;
                if (!reader.Read(innerVersion)) {
                    diag.Status = LgoStatus::HEADER_TRUNCATED;
                    diag.Detail = std::format("запись {}: не прочитана вложенная версия", i);
                    return std::nullopt;
                }
                available -= 4;
            }

            LgoGeomObj obj;
            if (!ParseGeomObjBody(reader, model.Version, available, obj, diag)) {
                diag.Detail = std::format("запись {} (геометрия): {}", i, diag.Detail);
                return std::nullopt;
            }
            if (diag.Status == LgoStatus::OK_WITH_TRAILING_DATA) {
                worst = LgoStatus::OK_WITH_TRAILING_DATA;
            }
            model.Objects.push_back(std::move(obj));
            break;
        }

        case ModelObjType::HELPER:
            if (!ParseHelperBlock(reader, static_cast<std::uint32_t>(available),
                                  model.Version, model.Helper, diag)) {
                diag.Detail = std::format("запись {} (helper): {}", i, diag.Detail);
                return std::nullopt;
            }
            break;

        default:
            diag.Status = LgoStatus::BLOCK_SIZES_INCONSISTENT;
            diag.Detail = std::format("запись {}: неизвестный тип {}", i, entry.Type);
            return std::nullopt;
        }
    }

    ResolveModelMatrices(model.Objects);

    diag.Status = worst;
    diag.Detail.clear();
    return model;
}

} // namespace Corsairs::Tools::AssetConverter
