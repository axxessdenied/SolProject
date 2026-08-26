#pragma once

// GPU timestamp arithmetic (engine plan Phase 8o), deliberately knowing
// nothing about Vulkan.
//
// There is no rhi.unit suite and this item does not add one: a test that
// needs a device is not a headless test. So the part that can be wrong
// QUIETLY lives here, in plain integers, where core.unit can assert it - and
// only the plumbing around it needs a running game. That is the same split
// radar_projection.hpp and pick.hpp already make.
//
// Three ways a timestamp reading goes wrong without saying so:
//
//   1. The counter is not 64 bits wide. A queue reports how many of its low
//      bits are meaningful, and the high ones are undefined - reading a raw
//      delta on a 32-bit-valid counter gives a number that is garbage in the
//      top half and plausible in the bottom.
//   2. It wraps. A masked counter runs out and starts again, and a naive
//      end-minus-begin then reports a huge negative interval as a huge
//      positive one, or vice versa.
//   3. The result was never written. A pool queried without WAIT answers
//      "not ready" for a frame still in flight, and the slot holds whatever
//      it held before - which on a reused pool is a plausible-looking value
//      from an older frame.
//
// Each has a function below and a test beside it.

#include <cstdint>

namespace sol::core {

// Low-bit mask for a counter with `validBits` meaningful bits. 0 bits means
// the queue cannot timestamp at all, and the mask is 0 so every delta taken
// against it is 0 rather than nonsense.
[[nodiscard]] constexpr std::uint64_t timestampMask(std::uint32_t validBits)
{
    if (validBits == 0) {
        return 0;
    }
    if (validBits >= 64) {
        return ~std::uint64_t{0}; // 1u << 64 is undefined, so this case is its own
    }
    return (std::uint64_t{1} << validBits) - 1;
}

// Ticks from `begin` to `end` on a counter of the given width. Both ends are
// masked first, and the subtraction is then done modulo the mask - which is
// what makes a wrap come out correct rather than enormous, because modular
// arithmetic does not care that the counter restarted.
//
// The cost of that correctness: an out-of-order pair (end genuinely before
// begin, which the pipeline stages below should make impossible) reads as
// nearly a full counter period rather than as an error. There is no way to
// tell the two apart from the values alone, and a wrap is the case that
// actually happens.
[[nodiscard]] constexpr std::uint64_t
timestampDelta(std::uint64_t begin, std::uint64_t end, std::uint32_t validBits)
{
    const std::uint64_t mask = timestampMask(validBits);
    if (mask == 0) {
        return 0;
    }
    return ((end & mask) - (begin & mask)) & mask;
}

// One zone's raw readback: the two counter values and whether the device
// says each was actually written. Availability is a separate word per query
// because that is how vkGetQueryPoolResults reports it, and because "0" is a
// legal timestamp - the value alone cannot tell you it is missing.
struct TimestampPair
{
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t beginAvailable = 0;
    std::uint64_t endAvailable = 0;
};

// Converts a pair to milliseconds. Returns false - leaving `outMilliseconds`
// untouched - when the device cannot timestamp, when the period is not a
// positive number, or when either half was never written. A false return is
// "no measurement", which is a different thing from a measurement of zero,
// and the caller has to keep them apart or it will publish a zero it never
// measured.
[[nodiscard]] inline bool resolveTimestampPair(const TimestampPair& pair,
                                               std::uint32_t validBits,
                                               double periodNanoseconds,
                                               double& outMilliseconds)
{
    if (validBits == 0 || !(periodNanoseconds > 0.0)) {
        return false;
    }
    if (pair.beginAvailable == 0 || pair.endAvailable == 0) {
        return false;
    }
    const std::uint64_t ticks = timestampDelta(pair.begin, pair.end, validBits);
    outMilliseconds = static_cast<double>(ticks) * periodNanoseconds * 1.0e-6;
    return true;
}

} // namespace sol::core
