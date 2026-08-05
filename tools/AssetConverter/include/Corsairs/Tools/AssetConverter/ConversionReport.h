#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter {

struct ReportEntry {
    std::string Path;
    std::string Status;
    std::string Detail;
    bool Succeeded{false};
};

// Накапливает результат пакетного прогона. Ни один файл не пропускается молча:
// каждый обработанный файл попадает в отчёт со своим статусом.
class ConversionReport {
public:
    void AddSuccess(std::string_view path, std::string_view status);
    void AddFailure(std::string_view path, std::string_view status, std::string_view detail);

    [[nodiscard]] std::size_t SuccessCount() const {
        return _successCount;
    }

    [[nodiscard]] std::size_t FailureCount() const {
        return _entries.size() - _successCount;
    }

    [[nodiscard]] std::size_t TotalCount() const {
        return _entries.size();
    }

    // Человекочитаемая сводка: количество файлов по каждому статусу.
    [[nodiscard]] std::string Summary() const;

    [[nodiscard]] bool WriteCsv(const std::filesystem::path& path) const;

private:
    std::vector<ReportEntry> _entries;
    std::size_t _successCount{0};
};

} // namespace Corsairs::Tools::AssetConverter
