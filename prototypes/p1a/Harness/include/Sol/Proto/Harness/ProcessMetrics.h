#pragma once

#include <cstdint>

namespace sol::proto {

/// Peak process memory, in bytes, as reported by the Windows process memory counters.
///
/// The P1a milestone plan makes peak process memory a mandatory measurement.
struct ProcessMetrics {
    /// Highest resident working-set size reached by the process, in bytes.
    std::uint64_t peakWorkingSetBytes{0};
    /// Highest commit charge reached by the process, in bytes. Larger than the working set
    /// whenever pages are committed but not resident.
    std::uint64_t peakPagefileBytes{0};
};

/// Reads the current peak values for this process.
///
/// The values are process-lifetime peaks and cannot be reset, so a scenario reports the
/// peak of everything it has done since start, not of one measured region. Returns zeroed
/// fields if the query fails; a report showing zero peak memory should be treated as a
/// failed measurement rather than as a real result.
[[nodiscard]] ProcessMetrics captureProcessMetrics() noexcept;

} // namespace sol::proto
