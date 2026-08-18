#include "sol/core/gpu_timestamp.hpp"
#include "sol/test/test.hpp"

#include <cstdint>

namespace {

using sol::core::resolveTimestampPair;
using sol::core::TimestampPair;
using sol::core::timestampDelta;
using sol::core::timestampMask;

// A plausible discrete-GPU period: one tick per nanosecond.
constexpr double kOneNsPerTick = 1.0;

[[nodiscard]] bool closeEnough(double a, double b)
{
    const double diff = a > b ? a - b : b - a;
    return diff < 1.0e-9;
}

// A pair the device says it wrote both halves of.
[[nodiscard]] TimestampPair available(std::uint64_t begin, std::uint64_t end)
{
    return TimestampPair{.begin = begin, .end = end, .beginAvailable = 1, .endAvailable = 1};
}

} // namespace

SOL_TEST(gpuTimestamp_maskCoversExactlyTheValidBits)
{
    SOL_CHECK(timestampMask(0) == std::uint64_t{0});
    SOL_CHECK(timestampMask(1) == std::uint64_t{0x1});
    SOL_CHECK(timestampMask(32) == std::uint64_t{0xFFFFFFFF});
    SOL_CHECK(timestampMask(36) == std::uint64_t{0xFFFFFFFFF});
    SOL_CHECK(timestampMask(63) == std::uint64_t{0x7FFFFFFFFFFFFFFF});
}

// 1u << 64 is undefined behaviour, so the full-width case cannot go through
// the shift the others do. This is the test that pins that it does not.
SOL_TEST(gpuTimestamp_fullWidthCounterIsNotAShift)
{
    SOL_CHECK(timestampMask(64) == ~std::uint64_t{0});
    SOL_CHECK(timestampMask(80) == ~std::uint64_t{0}); // over-wide is still full width
}

SOL_TEST(gpuTimestamp_deltaIgnoresBitsTheQueueCallsInvalid)
{
    // The high half is undefined on a 32-bit-valid counter. A raw subtraction
    // would report a delta in the billions here; the masked one reports 100.
    const std::uint64_t begin = 0xDEADBEEF00000000ull | 900ull;
    const std::uint64_t end = 0x1234567800000000ull | 1000ull;
    SOL_CHECK(timestampDelta(begin, end, 32) == std::uint64_t{100});
}

SOL_TEST(gpuTimestamp_deltaSurvivesCounterWraparound)
{
    // 32-bit counter, 10 ticks before the end, running to 4 ticks past it.
    const std::uint64_t begin = 0xFFFFFFFFull - 9ull;
    const std::uint64_t end = 4ull;
    SOL_CHECK(timestampDelta(begin, end, 32) == std::uint64_t{14});

    // And on a width that is not a byte boundary, which is where an
    // off-by-one in the mask would show up.
    SOL_CHECK(timestampDelta(timestampMask(36) - 1ull, 2ull, 36) == std::uint64_t{4});
}

SOL_TEST(gpuTimestamp_deltaIsZeroWhenTheQueueCannotTimestamp)
{
    SOL_CHECK(timestampDelta(500, 9000, 0) == std::uint64_t{0});
}

SOL_TEST(gpuTimestamp_resolveConvertsTicksToMilliseconds)
{
    double milliseconds = -1.0;
    SOL_CHECK(resolveTimestampPair(available(1000, 2500000), 64, kOneNsPerTick, milliseconds));
    SOL_CHECK(closeEnough(milliseconds, 2.499));

    // A period other than 1 ns/tick is the common case, not the exotic one.
    milliseconds = -1.0;
    SOL_CHECK(resolveTimestampPair(available(0, 1000), 64, 40.0, milliseconds));
    SOL_CHECK(closeEnough(milliseconds, 0.04));
}

// "Not measured" and "measured zero" are different facts, and the out
// parameter must not be touched in the first case - otherwise a caller that
// keeps the previous frame's value gets a zero it never measured.
SOL_TEST(gpuTimestamp_resolveRejectsAnUnwrittenResultWithoutTouchingTheOutput)
{
    double milliseconds = 7.5;

    TimestampPair pending = available(100, 900);
    pending.endAvailable = 0;
    SOL_CHECK(!resolveTimestampPair(pending, 64, kOneNsPerTick, milliseconds));
    SOL_CHECK(closeEnough(milliseconds, 7.5));

    pending = available(100, 900);
    pending.beginAvailable = 0;
    SOL_CHECK(!resolveTimestampPair(pending, 64, kOneNsPerTick, milliseconds));
    SOL_CHECK(closeEnough(milliseconds, 7.5));
}

SOL_TEST(gpuTimestamp_resolveRejectsADeviceThatCannotMeasure)
{
    double milliseconds = 7.5;
    SOL_CHECK(!resolveTimestampPair(available(100, 900), 0, kOneNsPerTick, milliseconds));
    SOL_CHECK(!resolveTimestampPair(available(100, 900), 64, 0.0, milliseconds));
    SOL_CHECK(!resolveTimestampPair(available(100, 900), 64, -1.0, milliseconds));
    SOL_CHECK(closeEnough(milliseconds, 7.5));
}

// A genuinely idle pass reads zero ticks, and that IS a measurement.
SOL_TEST(gpuTimestamp_resolveAcceptsAZeroLengthZone)
{
    double milliseconds = -1.0;
    SOL_CHECK(resolveTimestampPair(available(4242, 4242), 64, kOneNsPerTick, milliseconds));
    SOL_CHECK(closeEnough(milliseconds, 0.0));
}
