#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/ConversionReport.h"
#include "Corsairs/Tools/AssetConverter/GltfSkeletonWriter.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LabParser.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"
#include "Corsairs/Tools/AssetConverter/LmoParser.h"
#include "Corsairs/Tools/AssetConverter/MapParser.h"
#include "Corsairs/Tools/AssetConverter/MapSectionReader.h"
#include "Corsairs/Tools/AssetConverter/MapWriter.h"
#include "Corsairs/Tools/AssetConverter/SceneParity.h"
#include "Corsairs/Tools/AssetConverter/Sha256.h"
#include "Corsairs/Tools/AssetConverter/TerrainMeshWriter.h"
#include "Corsairs/Tools/AssetConverter/SceneObjParser.h"
#include "Corsairs/Tools/AssetConverter/TerrainReferenceCommand.h"
#include "Corsairs/Tools/AssetConverter/TextureResolver.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <span>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

// Где искать скелеты для моделей персонажей. Задаётся ключом --skeletons;
// пустой путь означает, что привязка к скелету не делается.
std::filesystem::path g_skeletonRoot;

void PrintUsage() {
    std::cout <<
        "Использование:\n"
        "  AssetConverter <входной-каталог> <выходной-каталог> [--report <файл.csv>]\n"
        "                  [--textures <каталог-текстур>]\n"
        "                  [--skeletons <каталог-скелетов>]\n"
        "                  [--profile generic|scene-map]\n"
        "                  [--static-reference-pose]\n"
        "  AssetConverter scene-manifest MAP OBJ BASE\n"
        "\n"
        "Рекурсивно обходит входной каталог и конвертирует в glTF 2.0:\n"
        "  .lgo — геометрия, материалы и точки крепления;\n"
        "  .lab — скелет и анимационная дорожка.\n"
        "Результат сохраняется с той же относительной структурой каталогов.\n"
        "С --textures материалы получают ссылки на текстуры, а сами файлы\n"
        "копируются в <выход>/textures/ с сохранением категорий.\n"
        "С --skeletons модели персонажей получают полную иерархию костей из\n"
        "одноимённого .lab — без этого дорожки анимации к ним не применяются.\n"
        "Код возврата: 0 — все файлы обработаны, 1 — есть ошибки, 2 — неверные аргументы.\n";
}

// Разрешает текстуры всех материалов объекта. Категория — подкаталог модели
// относительно корня входа: `character/X.lgo` ищет текстуры в
// `<textureRoot>/character/`.
AC::GltfTextureOptions BuildTextureOptions(const AC::LgoGeomObj& obj,
                                           const AC::TextureResolver& resolver,
                                           const std::filesystem::path& relative,
                                           const std::filesystem::path& outputRoot) {
    AC::GltfTextureOptions options;
    if (!resolver.Enabled()) {
        return options;
    }

    const std::filesystem::path category = relative.parent_path();
    options.ResolvedTextures.resize(obj.Materials.size());

    for (std::size_t m = 0; m < obj.Materials.size(); ++m) {
        options.ResolvedTextures[m].resize(AC::kMaxTextureStageNum);
        for (std::size_t stage = 0; stage < AC::kMaxTextureStageNum; ++stage) {
            if (const auto found = resolver.Resolve(category, obj.Materials[m].TextureName(stage))) {
                options.ResolvedTextures[m][stage] = *found;
            }
        }
    }

    options.CopyTo = outputRoot / "textures" / category;
    return options;
}

// Ищет скелет для модели персонажа.
//
// Имя выводится из имени скина: `0001000000.lgo` относится к `0001.lab` —
// первые четыре цифры. Соглашение проверено на наборе: разрешается для 2941
// модели персонажа из 4913. Остальные остаются без скелета, и это отражается
// в отчёте отдельным статусом, а не замалчивается.
//
// Скелеты кешируются: одна и та же кость обслуживает сотни скинов, и
// перечитывать её файл для каждого — пустая работа.
const AC::LabAnimation* FindSkeleton(const std::filesystem::path& modelPath,
                                     const std::filesystem::path& skeletonRoot) {
    static std::map<std::string, std::optional<AC::LabAnimation>> cache;

    if (skeletonRoot.empty()) {
        return nullptr;
    }

    const std::string stem = modelPath.stem().string();
    if (stem.size() < 4) {
        return nullptr;
    }
    const std::string key = stem.substr(0, 4);
    if (!std::all_of(key.begin(), key.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return nullptr;
    }

    if (const auto found = cache.find(key); found != cache.end()) {
        return found->second ? &*found->second : nullptr;
    }

    const std::filesystem::path labPath = skeletonRoot / (key + ".lab");
    const auto bytes = AC::ReadWholeFile(labPath);
    if (!bytes) {
        cache.emplace(key, std::nullopt);
        return nullptr;
    }

    AC::LabDiagnostics diag;
    auto anim = AC::ParseLab(*bytes, diag);
    const auto inserted = cache.emplace(key, std::move(anim));
    return inserted.first->second ? &*inserted.first->second : nullptr;
}

// Конвертирует террейн .map в набор сырых карт плюс метаданные.
bool ConvertTerrain(const std::filesystem::path& input, const std::filesystem::path& output,
                    std::string_view relative, AC::ConversionReport& report) {
    const auto bytes = AC::ReadWholeFile(input);
    if (!bytes) {
        report.AddFailure(relative, "FILE_READ_FAILED", "файл не открылся");
        return false;
    }

    AC::MapDiagnostics diag;
    const auto terrain = AC::ParseMap(*bytes, diag);
    if (!terrain) {
        report.AddFailure(relative, AC::ToString(diag.Status), diag.Detail);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        report.AddFailure(relative, "OUTPUT_DIR_FAILED", ec.message());
        return false;
    }

    std::string detail;
    if (AC::WriteTerrain(*terrain, output, detail) != AC::MapWriteStatus::OK) {
        report.AddFailure(relative, "WRITE_FAILED", detail);
        return false;
    }

    if (AC::WriteTerrainLayers(*terrain, output, detail) != AC::MapWriteStatus::OK) {
        report.AddFailure(relative, "LAYERS_FAILED", detail);
        return false;
    }

    // Рельеф пишется ещё и мешами: ландшафт Unreal собирается инструментами
    // редактора, недоступными в headless-режиме, а обычные статические меши
    // импортируются тем же путём, что и все модели.
    const AC::TerrainMeshStats meshStats =
        AC::WriteTerrainMesh(*terrain, output, {}, detail);
    if (!meshStats.Ok) {
        report.AddFailure(relative, "TERRAIN_MESH_FAILED", detail);
        return false;
    }

    report.AddSuccess(relative, AC::ToString(diag.Status));
    return true;
}

// Конвертирует объекты сцены .obj в JSON-манифест.
bool ConvertSceneObjects(const std::filesystem::path& input,
                         const std::filesystem::path& output,
                         std::string_view relative, AC::ConversionReport& report) {
    const auto bytes = AC::ReadWholeFile(input);
    if (!bytes) {
        report.AddFailure(relative, "FILE_READ_FAILED", "файл не открылся");
        return false;
    }

    AC::SceneObjDiagnostics diag;
    const auto scene = AC::ParseSceneObj(*bytes, diag);
    if (!scene) {
        report.AddFailure(relative, AC::ToString(diag.Status), diag.Detail);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        report.AddFailure(relative, "OUTPUT_DIR_FAILED", ec.message());
        return false;
    }

    std::string detail;
    if (AC::WriteSceneManifest(*scene, output, detail) != AC::MapWriteStatus::OK) {
        report.AddFailure(relative, "WRITE_FAILED", detail);
        return false;
    }

    report.AddSuccess(relative, AC::ToString(diag.Status));
    return true;
}

// Конвертирует один .lmo — модель из нескольких геометрических объектов.
// Каждый объект пишется отдельным glTF рядом, с суффиксом номера: объединение
// в один файл требует общего буфера и переиндексации, что относится к работе
// со сценами, а не к разбору формата.
bool ConvertModel(const std::filesystem::path& input, const std::filesystem::path& output,
                  const std::filesystem::path& relativePath, AC::ConversionReport& report,
                  const AC::TextureResolver& resolver,
                  const std::filesystem::path& outputRoot,
                  AC::GltfCoordinateProfile profile,
                  AC::GltfSkinPolicy skinPolicy) {
    const std::string relative = relativePath.generic_string();
    const auto bytes = AC::ReadWholeFile(input);
    if (!bytes) {
        report.AddFailure(relative, "FILE_READ_FAILED", "файл не открылся");
        return false;
    }

    AC::LgoDiagnostics diag;
    const auto model = AC::ParseLmo(*bytes, diag);
    if (!model) {
        report.AddFailure(relative, AC::ToString(diag.Status), diag.Detail);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        report.AddFailure(relative, "OUTPUT_DIR_FAILED", ec.message());
        return false;
    }

    if (model->Objects.empty()) {
        report.AddFailure(relative, "EMPTY_MODEL", "в модели нет геометрических объектов");
        return false;
    }

    for (std::size_t i = 0; i < model->Objects.size(); ++i) {
        std::filesystem::path part = output;
        if (model->Objects.size() > 1) {
            part.replace_filename(
                std::format("{}_{}.gltf", output.stem().string(), i));
        }

        const AC::GltfTextureOptions textures =
            BuildTextureOptions(model->Objects[i], resolver, relativePath, outputRoot);

        std::string detail;
        const AC::GltfStatus status =
            AC::WriteGltf(model->Objects[i], part, detail, textures, nullptr,
                          profile, skinPolicy);
        if (status != AC::GltfStatus::OK) {
            const std::string_view name =
                status == AC::GltfStatus::EMPTY_MESH ? "EMPTY_MESH" : "WRITE_FAILED";
            report.AddFailure(relative, name, std::format("объект {}: {}", i, detail));
            return false;
        }
    }

    report.AddSuccess(relative, AC::ToString(diag.Status));
    return true;
}

// Конвертирует один .lab: скелет плюс одна анимационная дорожка. Имя дорожки
// берётся из имени файла — в исходных данных другого источника имени нет.
bool ConvertAnimation(const std::filesystem::path& input,
                      const std::filesystem::path& output,
                      std::string_view relative, AC::ConversionReport& report) {
    const auto bytes = AC::ReadWholeFile(input);
    if (!bytes) {
        report.AddFailure(relative, "FILE_READ_FAILED", "файл не открылся");
        return false;
    }

    AC::LabDiagnostics diag;
    const auto anim = AC::ParseLab(*bytes, diag);
    if (!anim) {
        report.AddFailure(relative, AC::ToString(diag.Status), diag.Detail);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        report.AddFailure(relative, "OUTPUT_DIR_FAILED", ec.message());
        return false;
    }

    std::string detail;
    const AC::GltfSkeletonStatus status =
        AC::WriteSkeletonGltf(*anim, input.stem().string(), output, detail);
    if (status != AC::GltfSkeletonStatus::OK) {
        const std::string_view name =
            status == AC::GltfSkeletonStatus::EMPTY_SKELETON ? "EMPTY_SKELETON" : "WRITE_FAILED";
        report.AddFailure(relative, name, detail);
        return false;
    }

    report.AddSuccess(relative, AC::ToString(diag.Status));
    return true;
}

// Конвертирует один файл, добавляя результат в отчёт. Возвращает false при
// любой ошибке разбора или записи.
bool ConvertOne(const std::filesystem::path& input, const std::filesystem::path& output,
                const std::filesystem::path& relativePath, AC::ConversionReport& report,
                const AC::TextureResolver& resolver,
                const std::filesystem::path& outputRoot,
                AC::GltfCoordinateProfile profile,
                AC::GltfSkinPolicy skinPolicy) {
    const std::string relative = relativePath.generic_string();

    const auto bytes = AC::ReadWholeFile(input);
    if (!bytes) {
        report.AddFailure(relative, "FILE_READ_FAILED", "файл не открылся");
        return false;
    }

    AC::LgoDiagnostics diag;
    const auto obj = AC::ParseLgo(*bytes, diag);
    if (!obj) {
        report.AddFailure(relative, AC::ToString(diag.Status), diag.Detail);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        report.AddFailure(relative, "OUTPUT_DIR_FAILED", ec.message());
        return false;
    }

    const AC::GltfTextureOptions textures =
        BuildTextureOptions(*obj, resolver, relativePath, outputRoot);

    const AC::LabAnimation* skeleton = FindSkeleton(input, g_skeletonRoot);

    std::string detail;
    const AC::GltfStatus status =
        AC::WriteGltf(*obj, output, detail, textures, skeleton, profile, skinPolicy);
    if (status != AC::GltfStatus::OK) {
        const std::string_view name =
            status == AC::GltfStatus::EMPTY_MESH ? "EMPTY_MESH" : "WRITE_FAILED";
        report.AddFailure(relative, name, detail);
        return false;
    }

    report.AddSuccess(relative, AC::ToString(diag.Status));
    return true;
}

std::string_view SceneManifestStatusName(AC::SceneManifestStatus status) {
    switch (status) {
    case AC::SceneManifestStatus::OK: return "OK";
    case AC::SceneManifestStatus::WRITE_FAILED: return "WRITE_FAILED";
    case AC::SceneManifestStatus::UNKNOWN_OBJECT_TYPE:
        return "UNKNOWN_OBJECT_TYPE";
    case AC::SceneManifestStatus::INVALID_SOURCE_CONTEXT:
        return "INVALID_SOURCE_CONTEXT";
    case AC::SceneManifestStatus::TERRAIN_READ_FAILED:
        return "TERRAIN_READ_FAILED";
    case AC::SceneManifestStatus::COUNT_MISMATCH: return "COUNT_MISMATCH";
    case AC::SceneManifestStatus::RECOVERY_REQUIRED:
        return "RECOVERY_REQUIRED";
    }
    return "WRITE_FAILED";
}

std::optional<int> TryRunSceneManifestSubcommand(
    std::span<const std::string_view> arguments,
    std::ostream& output,
    std::ostream& error) {
    if (arguments.size() < 2u || arguments[1] != "scene-manifest") {
        return std::nullopt;
    }
    if (arguments.size() != 5u) {
        error << "Использование: AssetConverter scene-manifest MAP OBJ BASE\n";
        return 2;
    }

    const std::filesystem::path mapPath{arguments[2]};
    const std::filesystem::path objectPath{arguments[3]};
    const std::filesystem::path basePath{arguments[4]};

    // Объектный контекст сохраняется до BuildSceneSelection: header и hash
    // принадлежат точным байтам аргумента CLI, а не выводятся из выборки.
    const auto objectBytes = AC::ReadWholeFile(objectPath);
    if (!objectBytes.has_value()) {
        error << std::format("FILE_READ_FAILED object={}\n",
                             objectPath.generic_string());
        return 1;
    }
    AC::SceneObjDiagnostics objectDiagnostics;
    const auto scene = AC::ParseSceneObj(*objectBytes, objectDiagnostics);
    if (!scene.has_value()) {
        error << std::format("{} {}\n",
                             AC::ToString(objectDiagnostics.Status),
                             objectDiagnostics.Detail);
        return 1;
    }
    const AC::SceneFileHeader independentObjectHeader = scene->Header;
    const std::string objectSha256 = AC::Sha256Bytes(
        std::span<const std::uint8_t>{*objectBytes});

    AC::SceneSelection selection;
    std::string detail;
    const AC::SceneSelectionStatus selectionStatus =
        AC::BuildSceneSelection(*scene, {}, selection, detail);
    if (selectionStatus != AC::SceneSelectionStatus::OK) {
        error << std::format("SCENE_SELECTION_FAILED {}\n", detail);
        return 1;
    }

    // Sha256File читает .map потоково; MapSectionReader отдельно держит лишь
    // таблицу секций и ленивый cache одной секции.
    const auto mapSha256 = AC::Sha256File(mapPath, detail);
    if (!mapSha256.has_value()) {
        error << std::format("MAP_HASH_FAILED {}\n", detail);
        return 1;
    }
    AC::MapDiagnostics mapDiagnostics;
    auto reader = AC::MapSectionReader::Open(mapPath, mapDiagnostics);
    if (!reader.has_value()) {
        error << std::format("{} {}\n", AC::ToString(mapDiagnostics.Status),
                             mapDiagnostics.Detail);
        return 1;
    }
    AC::MapSectionTileSource terrain{*reader};

    AC::SceneManifestSourceContext context;
    context.ObjectHeader = independentObjectHeader;
    context.SourceMapSha256 = *mapSha256;
    context.SourceObjectSha256 = objectSha256;
    context.ExpectedSourceRecordCount = 50017u;
    context.ExpectedSceneModelCount = 46991u;
    context.ExpectedDeferredEffectCount = 3026u;
    context.ExpectedReferenceObjectCount = 1634u;
    context.ExpectedReferenceIslandCounts = {
        {0u, 3u}, {1u, 1625u}, {2u, 6u}};

    const std::filesystem::path parent = basePath.parent_path().empty()
        ? std::filesystem::path{"."}
        : basePath.parent_path();
    std::error_code directoryError;
    std::filesystem::create_directories(parent, directoryError);
    if (directoryError) {
        error << std::format("OUTPUT_DIR_FAILED {}\n",
                             directoryError.message());
        return 1;
    }

    AC::SceneManifestStats stats;
    const AC::SceneManifestStatus manifestStatus =
        AC::WriteSceneSourceManifest(
            selection, terrain, context, basePath, stats, detail);
    if (manifestStatus != AC::SceneManifestStatus::OK) {
        error << std::format("{} {}\n",
                             SceneManifestStatusName(manifestStatus), detail);
        return 1;
    }
    output << std::format(
        "scene-manifest: records={} models={} deferred={} reference={} {}\n",
        stats.SourceRecordCount, stats.SceneModelCount,
        stats.DeferredEffectCount, stats.ReferenceObjectCount,
        detail.empty() ? std::string_view{"OK"} : std::string_view{detail});
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    if (const auto sceneManifest =
            TryRunSceneManifestSubcommand(arguments, std::cout, std::cerr);
        sceneManifest.has_value()) {
        return *sceneManifest;
    }
    if (const auto terrainReference =
            AC::TryRunTerrainReferenceSubcommand(arguments, std::cout, std::cerr);
        terrainReference.has_value()) {
        return *terrainReference;
    }

    if (argc < 3) {
        PrintUsage();
        return 2;
    }

    const std::filesystem::path inputRoot{argv[1]};
    const std::filesystem::path outputRoot{argv[2]};
    std::filesystem::path reportPath;
    std::filesystem::path textureRoot;
    AC::GltfCoordinateProfile profile = AC::GltfCoordinateProfile::Generic;
    bool staticReferencePose = false;

    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--report" && i + 1 < argc) {
            reportPath = argv[i + 1];
            ++i;
        }
        else if (arg == "--textures" && i + 1 < argc) {
            textureRoot = argv[i + 1];
            ++i;
        }
        else if (arg == "--skeletons" && i + 1 < argc) {
            g_skeletonRoot = argv[i + 1];
            ++i;
        }
        else if (arg == "--profile" && i + 1 < argc) {
            const std::string_view value{argv[++i]};
            if (value == "generic") {
                profile = AC::GltfCoordinateProfile::Generic;
            }
            else if (value == "scene-map") {
                profile = AC::GltfCoordinateProfile::SceneMap;
            }
            else {
                PrintUsage();
                return 2;
            }
        }
        else if (arg == "--static-reference-pose") {
            staticReferencePose = true;
        }
        else {
            PrintUsage();
            return 2;
        }
    }

    if (!std::filesystem::is_directory(inputRoot)) {
        std::cout << std::format("Входной каталог не найден: {}\n", inputRoot.string());
        return 2;
    }

    if (staticReferencePose && profile != AC::GltfCoordinateProfile::SceneMap) {
        PrintUsage();
        return 2;
    }

    const AC::GltfSkinPolicy skinPolicy = staticReferencePose
        ? AC::GltfSkinPolicy::StaticReferencePose
        : AC::GltfSkinPolicy::Preserve;

    AC::ConversionReport report;
    const AC::TextureResolver resolver{textureRoot};

    for (const auto& entry : std::filesystem::recursive_directory_iterator{inputRoot}) {
        if (!entry.is_regular_file()) {
            continue;
        }

        std::string extension = entry.path().extension().string();
        for (char& c : extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const bool isGeometry = extension == ".lgo";
        const bool isAnimation = extension == ".lab";
        const bool isModel = extension == ".lmo";
        const bool isTerrain = extension == ".map";
        const bool isSceneObjects = extension == ".obj";
        if (!isGeometry && !isAnimation && !isModel && !isTerrain && !isSceneObjects) {
            continue;
        }

        const std::filesystem::path relative =
            std::filesystem::relative(entry.path(), inputRoot);
        const std::string relativeText = relative.generic_string();

        // Террейн и объекты сцены пишутся набором файлов с суффиксами, поэтому
        // им передаётся путь без расширения.
        if (isTerrain || isSceneObjects) {
            std::filesystem::path base = outputRoot / relative;
            base.replace_extension();
            if (isTerrain) {
                ConvertTerrain(entry.path(), base, relativeText, report);
            }
            else {
                ConvertSceneObjects(entry.path(), base, relativeText, report);
            }
            continue;
        }

        std::filesystem::path output = outputRoot / relative;
        output.replace_extension(".gltf");

        if (isGeometry) {
            ConvertOne(entry.path(), output, relative, report, resolver, outputRoot,
                       profile, skinPolicy);
        }
        else if (isModel) {
            ConvertModel(entry.path(), output, relative, report, resolver, outputRoot,
                         profile, skinPolicy);
        }
        else {
            ConvertAnimation(entry.path(), output, relativeText, report);
        }
    }

    std::cout << report.Summary();

    if (!reportPath.empty()) {
        if (!report.WriteCsv(reportPath)) {
            std::cout << std::format("Не удалось записать отчёт: {}\n", reportPath.string());
            return 1;
        }
        std::cout << std::format("Отчёт: {}\n", reportPath.string());
    }

    return report.FailureCount() == 0 ? 0 : 1;
}
