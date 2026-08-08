#include "Corsairs/Tools/AssetConverter/ProcessMetrics.h"

#include <cstdint>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace Corsairs::Tools::AssetConverter {

std::optional<std::size_t> QueryPeakProcessRssBytes(std::string& detail) {
    detail.clear();
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                             static_cast<DWORD>(sizeof(counters))) == FALSE) {
        detail = "не удалось получить lifetime peak RSS процесса";
        return std::nullopt;
    }
    if (counters.PeakWorkingSetSize > std::numeric_limits<std::size_t>::max()) {
        detail = "lifetime peak RSS не помещается в size_t";
        return std::nullopt;
    }
    return static_cast<std::size_t>(counters.PeakWorkingSetSize);
#elif defined(__APPLE__) || defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
        detail = "не удалось получить lifetime peak RSS процесса";
        return std::nullopt;
    }
    const auto raw = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__APPLE__)
    if (raw > std::numeric_limits<std::size_t>::max()) {
        detail = "lifetime peak RSS не помещается в size_t";
        return std::nullopt;
    }
    return static_cast<std::size_t>(raw);
#else
    if (raw > std::numeric_limits<std::size_t>::max() / 1024u) {
        detail = "lifetime peak RSS в KiB переполняет size_t";
        return std::nullopt;
    }
    return static_cast<std::size_t>(raw * 1024u);
#endif
#else
    detail = "lifetime peak RSS не поддерживается на этой ОС";
    return std::nullopt;
#endif
}

} // namespace Corsairs::Tools::AssetConverter
