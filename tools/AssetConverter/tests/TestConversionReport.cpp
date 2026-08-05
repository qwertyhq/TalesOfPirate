#include "Corsairs/Tools/AssetConverter/BinaryReader.h"
#include "Corsairs/Tools/AssetConverter/ConversionReport.h"

#include "TestHarness.h"

#include <filesystem>
#include <string>

namespace {

namespace AC = Corsairs::Tools::AssetConverter;

CORSAIRS_TEST(ConversionReport_CountsSuccessesAndFailures) {
    AC::ConversionReport report;
    report.AddSuccess("a.lgo", "OK");
    report.AddSuccess("b.lgo", "OK_WITH_TRAILING_DATA");
    report.AddFailure("c.lgo", "VERSION_UNKNOWN", "version=0xDEADBEEF");

    REQUIRE_EQ(report.SuccessCount(), 2u);
    REQUIRE_EQ(report.FailureCount(), 1u);
    REQUIRE_EQ(report.TotalCount(), 3u);
}

CORSAIRS_TEST(ConversionReport_SummaryGroupsByStatus) {
    AC::ConversionReport report;
    report.AddSuccess("a.lgo", "OK");
    report.AddSuccess("b.lgo", "OK");
    report.AddFailure("c.lgo", "VERSION_UNKNOWN", "");

    const std::string summary = report.Summary();
    REQUIRE(summary.find("OK") != std::string::npos);
    REQUIRE(summary.find("2") != std::string::npos);
    REQUIRE(summary.find("VERSION_UNKNOWN") != std::string::npos);
}

CORSAIRS_TEST(ConversionReport_CsvContainsEveryEntry) {
    AC::ConversionReport report;
    report.AddSuccess("a.lgo", "OK");
    report.AddFailure("c.lgo", "VERSION_UNKNOWN", "деталь, с запятой");

    const std::filesystem::path csv =
        std::filesystem::temp_directory_path() / "corsairs-report-test.csv";
    std::filesystem::remove(csv);

    REQUIRE(report.WriteCsv(csv));
    REQUIRE(std::filesystem::exists(csv));

    const auto bytes = AC::ReadWholeFile(csv);
    REQUIRE(bytes.has_value());
    const std::string text{reinterpret_cast<const char*>(bytes->data()), bytes->size()};

    REQUIRE(text.find("a.lgo") != std::string::npos);
    REQUIRE(text.find("c.lgo") != std::string::npos);
    // Поле с запятой обязано быть в кавычках, иначе CSV разъедется.
    REQUIRE(text.find("\"деталь, с запятой\"") != std::string::npos);
}

} // namespace
