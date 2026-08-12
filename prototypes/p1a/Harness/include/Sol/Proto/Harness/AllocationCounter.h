#pragma once

#include <cstdint>

namespace sol::proto {

/// Process-wide allocation totals observed through the replaced global operator new/delete.
struct AllocationCounts {
    std::uint64_t allocationCount{0};
    std::uint64_t deallocationCount{0};
    /// Total bytes requested across all allocations since the last reset.
    std::uint64_t totalAllocatedBytes{0};
    /// Bytes currently outstanding. Computed from the allocator's usable block size, so it
    /// may exceed the sum of requested sizes.
    std::uint64_t liveBytes{0};
};

/// Allocation accounting for P1a scenarios.
///
/// The P1a milestone plan makes allocation counts a mandatory measurement, so the harness
/// replaces the global operator new/delete family rather than sampling an allocator.
///
/// Scope and caveats, all of which belong in any report that quotes these numbers:
///   - Counting is process-wide and covers every translation unit linked into the
///     executable, including the C++ standard library. It is not per-subsystem.
///   - Counters are atomic and thread-safe, but the snapshot is not a consistent
///     cross-counter instant; read it while no other thread is allocating.
///   - Allocations made through malloc/HeapAlloc directly, rather than through operator
///     new, are invisible here.
///   - Allocations that happen before main() are included, so reset() at the top of a
///     scenario is what makes a measurement attributable to that scenario.
///   - The replacement operators route through malloc/_aligned_malloc and size blocks with
///     _msize/_aligned_msize. That is safe only while every operator-new allocation in the
///     process reaches these operators. A future DLL that allocates with its own operator
///     new and frees through ours would corrupt the heap, not merely miscount. P1a links
///     only static prototype executables, so the situation cannot arise today; revisit this
///     before any dynamic boundary is introduced.
///
/// Linkage note: the counters and these functions share one translation unit, so calling
/// any of them guarantees the replacement operators are linked in from the static library.
/// A scenario that never calls into this namespace would silently get the default operators.
namespace allocations {

/// Returns the counts accumulated since the last reset().
[[nodiscard]] AllocationCounts snapshot() noexcept;

/// Zeroes every counter, including liveBytes.
///
/// Call at the start of a measured region. Because liveBytes is zeroed too, deallocations
/// of blocks allocated before the reset will drive it negative in principle; it saturates
/// at zero instead of wrapping.
void reset() noexcept;

} // namespace allocations
} // namespace sol::proto
