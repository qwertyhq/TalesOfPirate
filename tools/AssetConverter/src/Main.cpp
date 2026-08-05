#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/ConversionReport.h"
#include "Corsairs/Tools/AssetConverter/GltfSkeletonWriter.h"
#include "Corsairs/Tools/AssetConverter/GltfWriter.h"
#include "Corsairs/Tools/AssetConverter/LabParser.h"
#include "Corsairs/Tools/AssetConverter/LgoParser.h"
#include "Corsairs/Tools/AssetConverter/LmoParser.h"

#include <cctype>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

void PrintUsage() {
    std::cout <<
        "Использование:\n"
        "  AssetConverter <входной-каталог> <выходной-каталог> [--report <файл.csv>]\n"
        "\n"
        "Рекурсивно обходит входной каталог и конвертирует в glTF 2.0:\n"
        "  .lgo — геометрия, материалы и точки крепления;\n"
        "  .lab — скелет и анимационная дорожка.\n"
        "Результат сохраняется с той же относительной структурой каталогов.\n"
        "Код возврата: 0 — все файлы обработаны, 1 — есть ошибки, 2 — неверные аргументы.\n";
}

// Конвертирует один .lmo — модель из нескольких геометрических объектов.
// Каждый объект пишется отдельным glTF рядом, с суффиксом номера: объединение
// в один файл требует общего буфера и переиндексации, что относится к работе
// со сценами, а не к разбору формата.
bool ConvertModel(const std::filesystem::path& input, const std::filesystem::path& output,
                  std::string_view relative, AC::ConversionReport& report) {
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

        std::string detail;
        const AC::GltfStatus status = AC::WriteGltf(model->Objects[i], part, detail);
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
                std::string_view relative, AC::ConversionReport& report) {
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

    std::string detail;
    const AC::GltfStatus status = AC::WriteGltf(*obj, output, detail);
    if (status != AC::GltfStatus::OK) {
        const std::string_view name =
            status == AC::GltfStatus::EMPTY_MESH ? "EMPTY_MESH" : "WRITE_FAILED";
        report.AddFailure(relative, name, detail);
        return false;
    }

    report.AddSuccess(relative, AC::ToString(diag.Status));
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        PrintUsage();
        return 2;
    }

    const std::filesystem::path inputRoot{argv[1]};
    const std::filesystem::path outputRoot{argv[2]};
    std::filesystem::path reportPath;

    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--report" && i + 1 < argc) {
            reportPath = argv[i + 1];
            ++i;
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

    AC::ConversionReport report;

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
        if (!isGeometry && !isAnimation && !isModel) {
            continue;
        }

        const std::filesystem::path relative =
            std::filesystem::relative(entry.path(), inputRoot);
        std::filesystem::path output = outputRoot / relative;
        output.replace_extension(".gltf");

        if (isGeometry) {
            ConvertOne(entry.path(), output, relative.generic_string(), report);
        }
        else if (isModel) {
            ConvertModel(entry.path(), output, relative.generic_string(), report);
        }
        else {
            ConvertAnimation(entry.path(), output, relative.generic_string(), report);
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
