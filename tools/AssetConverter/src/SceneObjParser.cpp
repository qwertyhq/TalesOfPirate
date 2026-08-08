#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"

#include <cstring>
#include <format>
#include <limits>
#include <utility>

namespace Corsairs::Tools::AssetConverter {

namespace {

bool CheckedMultiply(std::uint64_t left,
                     std::uint64_t right,
                     std::uint64_t& result) {
    if (left != 0u && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool CheckedAdd(std::uint64_t left,
                std::uint64_t right,
                std::uint64_t& result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool FitsSize(std::uint64_t value) {
    return value <= std::numeric_limits<std::size_t>::max();
}

std::optional<SceneObjects> Fail(SceneObjDiagnostics& diagnostics,
                                 SceneObjStatus status,
                                 std::string detail) {
    diagnostics.Status = status;
    diagnostics.Detail = std::move(detail);
    return std::nullopt;
}

std::optional<SceneObjects> Overflow(SceneObjDiagnostics& diagnostics,
                                     std::string_view operation) {
    return Fail(diagnostics,
                SceneObjStatus::INTEGER_OVERFLOW,
                std::format("scene integer overflow: {}", operation));
}

std::optional<std::int32_t> CalculateWorldCoordinate(std::int32_t relative,
                                                     std::uint32_t section,
                                                     std::uint32_t sectionSize) {
    std::uint64_t tileOffset = 0u;
    std::uint64_t unitOffset = 0u;
    if (!CheckedMultiply(section, sectionSize, tileOffset) ||
        !CheckedMultiply(tileOffset,
                         static_cast<std::uint64_t>(kWorldUnitsPerTile),
                         unitOffset) ||
        unitOffset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }

    const std::int64_t maximumOffset =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) -
        static_cast<std::int64_t>(relative);
    if (unitOffset > static_cast<std::uint64_t>(maximumOffset)) {
        return std::nullopt;
    }
    const std::int64_t world =
        static_cast<std::int64_t>(relative) + static_cast<std::int64_t>(unitOffset);
    return static_cast<std::int32_t>(world);
}

} // namespace

std::optional<std::int32_t> PlacedObject::TryWorldX() const {
    return CalculateWorldCoordinate(Info.X, SectionX, SectionWidth);
}

std::optional<std::int32_t> PlacedObject::TryWorldY() const {
    return CalculateWorldCoordinate(Info.Y, SectionY, SectionHeight);
}

std::int32_t PlacedObject::WorldX() const {
    return TryWorldX().value();
}

std::int32_t PlacedObject::WorldY() const {
    return TryWorldY().value();
}

std::string_view ToString(SceneObjStatus status) {
    switch (status) {
    case SceneObjStatus::OK:                      return "OK";
    case SceneObjStatus::HEADER_TRUNCATED:        return "HEADER_TRUNCATED";
    case SceneObjStatus::BAD_MAGIC:               return "BAD_MAGIC";
    case SceneObjStatus::VERSION_UNSUPPORTED:     return "VERSION_UNSUPPORTED";
    case SceneObjStatus::SECTION_TABLE_TRUNCATED: return "SECTION_TABLE_TRUNCATED";
    case SceneObjStatus::BODY_TRUNCATED:          return "BODY_TRUNCATED";
    case SceneObjStatus::INTEGER_OVERFLOW:        return "INTEGER_OVERFLOW";
    }
    return "UNKNOWN";
}

std::optional<SceneObjects> ParseSceneObj(std::span<const std::uint8_t> bytes,
                                          SceneObjDiagnostics& diag) {
    diag = {};
    if (bytes.size() < sizeof(SceneFileHeader)) {
        return Fail(diag,
                    SceneObjStatus::HEADER_TRUNCATED,
                    std::format("файл {} байт, нужно минимум {}",
                                bytes.size(), sizeof(SceneFileHeader)));
    }

    SceneObjects scene;
    std::memcpy(&scene.Header, bytes.data(), sizeof(scene.Header));
    diag.Version = scene.Header.Version;

    if (std::strncmp(scene.Header.Title, kObjMagic, 15) != 0) {
        return Fail(diag,
                    SceneObjStatus::BAD_MAGIC,
                    "заголовок не начинается с \"HF Object File!\"");
    }

    // Версия 500 конвертируется движком в 600 при загрузке; конвертер работает
    // только с уже обновлёнными файлами и честно сообщает о старых.
    if (scene.Header.Version != kObjVersionCurrent) {
        return Fail(diag,
                    SceneObjStatus::VERSION_UNSUPPORTED,
                    std::format("version={} (поддерживается {})",
                                scene.Header.Version, kObjVersionCurrent));
    }

    if (scene.Header.SectionCntX <= 0 || scene.Header.SectionCntY <= 0 ||
        scene.Header.SectionWidth <= 0 || scene.Header.SectionHeight <= 0 ||
        scene.Header.SectionObjNum <= 0 || scene.Header.FileSize <= 0) {
        return Fail(
            diag,
            SceneObjStatus::HEADER_TRUNCATED,
            std::format(
                "некорректный заголовок: fileSize={}, cntX={}, cntY={}, width={}, "
                "height={}, objNum={}",
                scene.Header.FileSize,
                scene.Header.SectionCntX,
                scene.Header.SectionCntY,
                scene.Header.SectionWidth,
                scene.Header.SectionHeight,
                scene.Header.SectionObjNum));
    }

    if (static_cast<std::uint64_t>(scene.Header.FileSize) !=
        static_cast<std::uint64_t>(bytes.size())) {
        return Fail(diag,
                    SceneObjStatus::HEADER_TRUNCATED,
                    std::format("FileSize={} не совпадает с размером файла {}",
                                scene.Header.FileSize, bytes.size()));
    }

    std::uint64_t sectionCount = 0u;
    if (!CheckedMultiply(static_cast<std::uint64_t>(scene.Header.SectionCntX),
                         static_cast<std::uint64_t>(scene.Header.SectionCntY),
                         sectionCount)) {
        return Overflow(diag, "section-count");
    }

    std::uint64_t tableBytes = 0u;
    if (!CheckedMultiply(sectionCount, sizeof(SectionIndex), tableBytes)) {
        return Overflow(diag, "section-table-bytes");
    }

    std::uint64_t prefixBytes = 0u;
    if (!CheckedAdd(sizeof(SceneFileHeader), tableBytes, prefixBytes)) {
        return Overflow(diag, "section-prefix");
    }
    if (!FitsSize(sectionCount)) {
        return Overflow(diag, "section-count");
    }
    if (!FitsSize(tableBytes)) {
        return Overflow(diag, "section-table-bytes");
    }
    if (!FitsSize(prefixBytes)) {
        return Overflow(diag, "section-prefix");
    }

    const std::uint64_t fileSize = static_cast<std::uint64_t>(scene.Header.FileSize);
    if (prefixBytes > fileSize) {
        return Fail(diag,
                    SceneObjStatus::SECTION_TABLE_TRUNCATED,
                    std::format("таблица секций заканчивается на {}, размер файла {}",
                                prefixBytes, fileSize));
    }

    const auto sectionCountSize = static_cast<std::size_t>(sectionCount);
    std::vector<SectionIndex> table(sectionCountSize);
    std::memcpy(table.data(),
                bytes.data() + sizeof(SceneFileHeader),
                static_cast<std::size_t>(tableBytes));

    const std::uint64_t perSection =
        static_cast<std::uint64_t>(scene.Header.SectionObjNum);

    for (std::uint64_t sectionIndex = 0u;
         sectionIndex < sectionCount;
         ++sectionIndex) {
        const SectionIndex& index = table[static_cast<std::size_t>(sectionIndex)];
        if (index.ObjNum < 0 || index.ObjNum > scene.Header.SectionObjNum) {
            return Fail(diag,
                        SceneObjStatus::BODY_TRUNCATED,
                        std::format("секция {}: ObjNum={} вне диапазона 0..{}",
                                    sectionIndex,
                                    index.ObjNum,
                                    scene.Header.SectionObjNum));
        }
        if (index.ObjNum == 0) {
            continue;
        }

        if (index.ObjInfoPos < 0) {
            return Fail(diag,
                        SceneObjStatus::BODY_TRUNCATED,
                        std::format("секция {}: отрицательный ObjInfoPos={}",
                                    sectionIndex, index.ObjInfoPos));
        }

        const std::uint64_t blockBegin =
            static_cast<std::uint64_t>(index.ObjInfoPos);
        std::uint64_t blockBytes = 0u;
        if (!CheckedMultiply(perSection, sizeof(SceneObjInfo), blockBytes) ||
            !FitsSize(blockBytes)) {
            return Overflow(diag, "object-block-bytes");
        }
        std::uint64_t blockEnd = 0u;
        if (!CheckedAdd(blockBegin, blockBytes, blockEnd) ||
            !FitsSize(blockEnd)) {
            return Overflow(diag, "object-block-end");
        }
        if (blockBegin < prefixBytes || blockEnd > fileSize) {
            return Fail(
                diag,
                SceneObjStatus::BODY_TRUNCATED,
                std::format("секция {}: блок [{}, {}) вне тела [{}, {})",
                            sectionIndex, blockBegin, blockEnd, prefixBytes, fileSize));
        }

        const auto sectionX = static_cast<std::uint32_t>(
            sectionIndex % static_cast<std::uint64_t>(scene.Header.SectionCntX));
        const auto sectionY = static_cast<std::uint32_t>(
            sectionIndex / static_cast<std::uint64_t>(scene.Header.SectionCntX));

        for (std::uint64_t slotIndex = 0u;
             slotIndex < static_cast<std::uint64_t>(index.ObjNum);
             ++slotIndex) {
            std::uint64_t recordDelta = 0u;
            if (!CheckedMultiply(slotIndex, sizeof(SceneObjInfo), recordDelta) ||
                !FitsSize(recordDelta)) {
                return Overflow(diag, "record-delta");
            }

            std::uint64_t recordBegin = 0u;
            std::uint64_t recordEnd = 0u;
            if (!CheckedAdd(blockBegin, recordDelta, recordBegin) ||
                !CheckedAdd(recordBegin, sizeof(SceneObjInfo), recordEnd) ||
                !FitsSize(recordBegin) || !FitsSize(recordEnd)) {
                return Overflow(diag, "record-end");
            }
            if (slotIndex >= static_cast<std::uint64_t>(index.ObjNum) ||
                recordEnd > blockEnd) {
                return Fail(diag,
                            SceneObjStatus::BODY_TRUNCATED,
                            std::format("секция {}, слот {}: запись вне блока",
                                        sectionIndex, slotIndex));
            }

            SceneObjInfo info{};
            std::memcpy(&info,
                        bytes.data() + static_cast<std::size_t>(recordBegin),
                        sizeof(info));
            PlacedObject placed{
                info,
                sectionX,
                sectionY,
                static_cast<std::uint32_t>(scene.Header.SectionWidth),
                static_cast<std::uint32_t>(scene.Header.SectionHeight),
                SceneSourceKey{
                    static_cast<std::uint32_t>(sectionIndex),
                    static_cast<std::uint32_t>(slotIndex),
                    recordBegin,
                },
            };
            if (!placed.TryWorldX().has_value() ||
                !placed.TryWorldY().has_value()) {
                return Fail(
                    diag,
                    SceneObjStatus::BODY_TRUNCATED,
                    std::format(
                        "секция {}, слот {}: мировая координата вне int32",
                        sectionIndex, slotIndex));
            }
            scene.Objects.push_back(std::move(placed));
        }
        ++scene.NonEmptySections;
    }

    diag.Status = SceneObjStatus::OK;
    diag.Detail.clear();
    return scene;
}

} // namespace Corsairs::Tools::AssetConverter
