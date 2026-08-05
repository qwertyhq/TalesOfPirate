#include "Corsairs/Tools/AssetConverter/LmoParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <format>

namespace Corsairs::Tools::AssetConverter {

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

    diag.Status = worst;
    diag.Detail.clear();
    return model;
}

} // namespace Corsairs::Tools::AssetConverter
