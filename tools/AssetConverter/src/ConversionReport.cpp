#include "Corsairs/Tools/AssetConverter/ConversionReport.h"

#include <format>
#include <fstream>
#include <map>

namespace Corsairs::Tools::AssetConverter {

namespace {

// Экранирует поле CSV: кавычки удваиваются, поле берётся в кавычки, если
// содержит запятую, кавычку или перевод строки.
std::string EscapeCsv(std::string_view field) {
    const bool needsQuotes =
        field.find(',') != std::string_view::npos ||
        field.find('"') != std::string_view::npos ||
        field.find('\n') != std::string_view::npos;

    if (!needsQuotes) {
        return std::string{field};
    }

    std::string out;
    out += '"';
    for (char c : field) {
        if (c == '"') {
            out += "\"\"";
        }
        else {
            out += c;
        }
    }
    out += '"';
    return out;
}

} // namespace

void ConversionReport::AddSuccess(std::string_view path, std::string_view status) {
    _entries.push_back(ReportEntry{std::string{path}, std::string{status}, {}, true});
    ++_successCount;
}

void ConversionReport::AddFailure(std::string_view path, std::string_view status,
                                  std::string_view detail) {
    _entries.push_back(
        ReportEntry{std::string{path}, std::string{status}, std::string{detail}, false});
}

std::string ConversionReport::Summary() const {
    std::map<std::string, std::size_t> byStatus;
    for (const ReportEntry& entry : _entries) {
        ++byStatus[entry.Status];
    }

    std::string out = std::format("Всего файлов: {}, успешно: {}, с ошибкой: {}\n",
                                  TotalCount(), SuccessCount(), FailureCount());
    for (const auto& [status, count] : byStatus) {
        out += std::format("  {:<28} {}\n", status, count);
    }
    return out;
}

bool ConversionReport::WriteCsv(const std::filesystem::path& path) const {
    std::ofstream stream{path, std::ios::trunc};
    if (!stream) {
        return false;
    }

    stream << "path,status,detail\n";
    for (const ReportEntry& entry : _entries) {
        stream << EscapeCsv(entry.Path) << ','
               << EscapeCsv(entry.Status) << ','
               << EscapeCsv(entry.Detail) << '\n';
    }
    return static_cast<bool>(stream);
}

} // namespace Corsairs::Tools::AssetConverter
