#include "Sol/Proto/Harness/ProcessMetrics.h"

#include <windows.h>

#include <psapi.h>

namespace sol::proto {

ProcessMetrics captureProcessMetrics() noexcept {
    ProcessMetrics metrics;

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        metrics.peakWorkingSetBytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
        metrics.peakPagefileBytes = static_cast<std::uint64_t>(counters.PeakPagefileUsage);
    }

    return metrics;
}

} // namespace sol::proto
