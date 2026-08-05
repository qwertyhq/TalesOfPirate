#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"

#include "Corsairs/Tools/AssetConverter/BinaryReader.h"

#include <cstring>
#include <format>

namespace Corsairs::Tools::AssetConverter {

std::string_view ToString(SceneObjStatus status) {
    switch (status) {
    case SceneObjStatus::OK:                      return "OK";
    case SceneObjStatus::HEADER_TRUNCATED:        return "HEADER_TRUNCATED";
    case SceneObjStatus::BAD_MAGIC:               return "BAD_MAGIC";
    case SceneObjStatus::VERSION_UNSUPPORTED:     return "VERSION_UNSUPPORTED";
    case SceneObjStatus::SECTION_TABLE_TRUNCATED: return "SECTION_TABLE_TRUNCATED";
    case SceneObjStatus::BODY_TRUNCATED:          return "BODY_TRUNCATED";
    }
    return "UNKNOWN";
}

std::optional<SceneObjects> ParseSceneObj(std::span<const std::uint8_t> bytes,
                                          SceneObjDiagnostics& diag) {
    diag = {};
    BinaryReader reader{bytes};

    SceneObjects scene;

    if (!reader.Read(scene.Header)) {
        diag.Status = SceneObjStatus::HEADER_TRUNCATED;
        diag.Detail = std::format("файл {} байт, нужно минимум {}",
                                  bytes.size(), sizeof(SceneFileHeader));
        return std::nullopt;
    }
    diag.Version = scene.Header.Version;

    if (std::strncmp(scene.Header.Title, kObjMagic, 15) != 0) {
        diag.Status = SceneObjStatus::BAD_MAGIC;
        diag.Detail = "заголовок не начинается с \"HF Object File!\"";
        return std::nullopt;
    }

    // Версия 500 конвертируется движком в 600 при загрузке; конвертер работает
    // только с уже обновлёнными файлами и честно сообщает о старых.
    if (scene.Header.Version != kObjVersionCurrent) {
        diag.Status = SceneObjStatus::VERSION_UNSUPPORTED;
        diag.Detail = std::format("version={} (поддерживается {})",
                                  scene.Header.Version, kObjVersionCurrent);
        return std::nullopt;
    }

    if (scene.Header.SectionCntX <= 0 || scene.Header.SectionCntY <= 0 ||
        scene.Header.SectionObjNum <= 0) {
        diag.Status = SceneObjStatus::HEADER_TRUNCATED;
        diag.Detail = std::format("некорректные размеры: cntX={}, cntY={}, objNum={}",
                                  scene.Header.SectionCntX, scene.Header.SectionCntY,
                                  scene.Header.SectionObjNum);
        return std::nullopt;
    }

    const std::size_t sectionCount =
        static_cast<std::size_t>(scene.Header.SectionCntX) *
        static_cast<std::size_t>(scene.Header.SectionCntY);

    std::vector<SectionIndex> table(sectionCount);
    if (!reader.ReadArray(table.data(), sectionCount)) {
        diag.Status = SceneObjStatus::SECTION_TABLE_TRUNCATED;
        diag.Detail = std::format("не прочитана таблица из {} секций", sectionCount);
        return std::nullopt;
    }

    const std::size_t perSection = static_cast<std::size_t>(scene.Header.SectionObjNum);

    for (std::size_t s = 0; s < sectionCount; ++s) {
        const SectionIndex& index = table[s];
        if (index.ObjNum <= 0) {
            continue;
        }

        if (index.ObjNum > static_cast<std::int32_t>(perSection)) {
            diag.Status = SceneObjStatus::BODY_TRUNCATED;
            diag.Detail = std::format("секция {}: ObjNum={} превышает лимит {}",
                                      s, index.ObjNum, perSection);
            return std::nullopt;
        }

        const std::size_t offset = static_cast<std::size_t>(index.ObjInfoPos);
        const std::size_t needed = perSection * sizeof(SceneObjInfo);
        if (offset + needed > bytes.size()) {
            diag.Status = SceneObjStatus::BODY_TRUNCATED;
            diag.Detail = std::format(
                "секция {}: смещение {} + {} байт выходит за пределы файла ({} байт)",
                s, offset, needed, bytes.size());
            return std::nullopt;
        }

        // В файле всегда лежит полный массив на SectionObjNum записей, но
        // валидны только первые ObjNum из них.
        BinaryReader sectionReader{bytes.subspan(offset, needed)};
        std::vector<SceneObjInfo> raw(perSection);
        if (!sectionReader.ReadArray(raw.data(), perSection)) {
            diag.Status = SceneObjStatus::BODY_TRUNCATED;
            diag.Detail = std::format("секция {}: не прочитаны объекты", s);
            return std::nullopt;
        }

        const std::int32_t sectionX =
            static_cast<std::int32_t>(s % static_cast<std::size_t>(scene.Header.SectionCntX));
        const std::int32_t sectionY =
            static_cast<std::int32_t>(s / static_cast<std::size_t>(scene.Header.SectionCntX));

        for (std::int32_t i = 0; i < index.ObjNum; ++i) {
            scene.Objects.push_back(PlacedObject{raw[static_cast<std::size_t>(i)],
                                                 sectionX, sectionY});
        }
        ++scene.NonEmptySections;
    }

    diag.Status = SceneObjStatus::OK;
    return scene;
}

} // namespace Corsairs::Tools::AssetConverter
