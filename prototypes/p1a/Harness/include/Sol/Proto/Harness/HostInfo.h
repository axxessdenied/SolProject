#pragma once

#include <string>

namespace sol::proto {

/// CPU identity and the instruction-set features ADR 0010 depends on.
struct CpuInfo {
    /// Processor brand string from CPUID leaves 0x80000002-0x80000004, or "unknown".
    std::string brand{"unknown"};
    std::string vendor{"unknown"};
    /// True when the CPU reports AVX2 *and* the OS has enabled YMM state saving.
    /// ADR 0010 compiles with /arch:AVX2, so a false here means the binary cannot run
    /// correctly on this host.
    bool hasAvx2{false};
    /// True when the CPU reports FMA3. Relevant because ADR 0010 requires that FMA
    /// contraction not be applied to authoritative arithmetic even where it is available.
    bool hasFma{false};
    bool hasAvx{false};
    unsigned logicalProcessorCount{0};
};

/// Operating-system build identity.
struct OsInfo {
    /// "major.minor.build", read through RtlGetVersion so it is not subject to the
    /// application-manifest version shimming that affects GetVersionEx.
    std::string version{"unknown"};
};

[[nodiscard]] CpuInfo captureCpuInfo() noexcept;
[[nodiscard]] OsInfo captureOsInfo() noexcept;

/// Current UTC time as an ISO-8601 second-resolution string, for example
/// "2026-08-12T14:03:07Z".
///
/// This is wall-clock provenance only. It is deliberately confined to the nondeterministic
/// `environment` section of a report and must never enter a determinism comparison.
[[nodiscard]] std::string captureUtcTimestamp();

} // namespace sol::proto
