#include "sol/core/random.hpp"

#include "sol/test/test.hpp"

#include <cstdint>

namespace {

using sol::core::Rng;

} // namespace

SOL_TEST(random_sameSeedSameStreamIsDeterministic)
{
    Rng a(12345, 7);
    Rng b(12345, 7);
    for (int i = 0; i < 1000; ++i) {
        SOL_CHECK(a.nextU32() == b.nextU32());
    }
}

SOL_TEST(random_distinctStreamsDiverge)
{
    Rng a(12345, 0);
    Rng b(12345, 1);
    int equal = 0;
    for (int i = 0; i < 100; ++i) {
        if (a.nextU32() == b.nextU32()) {
            ++equal;
        }
    }
    SOL_CHECK(equal < 5); // chance collisions only, never lockstep
}

SOL_TEST(random_reseedingReproducesSequence)
{
    Rng rng(42, 3);
    const std::uint32_t first = rng.nextU32();
    const std::uint32_t second = rng.nextU32();
    rng.seed(42, 3);
    SOL_CHECK(rng.nextU32() == first);
    SOL_CHECK(rng.nextU32() == second);
}

SOL_TEST(random_rangeStaysInBoundsAndHitsAllValues)
{
    Rng rng(99, 0);
    int histogram[8] = {};
    for (int i = 0; i < 8000; ++i) {
        const std::uint32_t v = rng.range(8);
        SOL_CHECK(v < 8);
        ++histogram[v];
    }
    for (const int count : histogram) {
        // Uniform expectation is 1000 per bucket; catastrophic bias would
        // zero a bucket or concentrate the mass.
        SOL_CHECK(count > 800 && count < 1200);
    }
}

SOL_TEST(random_floatAndDoubleAreInHalfOpenUnitInterval)
{
    Rng rng(7, 0);
    for (int i = 0; i < 10000; ++i) {
        const float f = rng.nextFloat01();
        SOL_CHECK(f >= 0.0f && f < 1.0f);
        const double d = rng.nextDouble01();
        SOL_CHECK(d >= 0.0 && d < 1.0);
    }
}

SOL_TEST(random_rangeFloatRespectsBounds)
{
    Rng rng(11, 2);
    for (int i = 0; i < 1000; ++i) {
        const float v = rng.rangeFloat(-3.0f, 5.0f);
        SOL_CHECK(v >= -3.0f && v <= 5.0f);
    }
}
