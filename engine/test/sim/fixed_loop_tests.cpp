#include "sol/sim/fixed_loop.hpp"
#include "sol/test/test.hpp"

namespace {

using sol::sim::FixedLoop;

int drainTicks(FixedLoop& loop)
{
    int ticks = 0;
    while (loop.shouldTick()) {
        ++ticks;
    }
    return ticks;
}

} // namespace

SOL_TEST(sim_exactFrameDeltaTicksOncePerFrame)
{
    // Powers of two keep the double arithmetic exact: 32 Hz tick, 1/32 s frames.
    FixedLoop loop(32.0);
    for (int frame = 0; frame < 100; ++frame) {
        loop.beginFrame(1.0 / 32.0);
        SOL_CHECK(drainTicks(loop) == 1);
    }
    SOL_CHECK(loop.tickCount() == 100);
    SOL_CHECK(loop.alpha() == 0.0f);
}

SOL_TEST(sim_fastFramesAccumulateFractionalTicks)
{
    FixedLoop loop(32.0);
    // Half a tick per frame: ticks on every second frame.
    loop.beginFrame(1.0 / 64.0);
    SOL_CHECK(drainTicks(loop) == 0);
    SOL_CHECK(loop.alpha() == 0.5f);
    loop.beginFrame(1.0 / 64.0);
    SOL_CHECK(drainTicks(loop) == 1);
    SOL_CHECK(loop.alpha() == 0.0f);
}

SOL_TEST(sim_slowFrameRunsMultipleTicks)
{
    FixedLoop loop(32.0);
    loop.beginFrame(4.0 / 32.0);
    SOL_CHECK(drainTicks(loop) == 4);
    SOL_CHECK(loop.alpha() == 0.0f);
}

SOL_TEST(sim_stallClampsToMaxTicksPerFrame)
{
    FixedLoop loop(32.0, 8);
    loop.beginFrame(100.0); // multi-second stall must not spiral
    SOL_CHECK(drainTicks(loop) == 8);
    SOL_CHECK(loop.alpha() == 0.0f);
}

SOL_TEST(sim_totalTicksIndependentOfFrameChunking)
{
    // 2 sim-seconds delivered as uneven (exactly representable) chunks.
    FixedLoop even(32.0);
    for (int i = 0; i < 64; ++i) {
        even.beginFrame(1.0 / 32.0);
        (void)drainTicks(even);
    }

    FixedLoop uneven(32.0);
    const double chunks[] = {1.0 / 64.0, 3.0 / 32.0, 1.0 / 128.0, 1.0 / 128.0, 1.0 / 16.0};
    double fed = 0.0;
    while (fed < 2.0) {
        for (const double chunk : chunks) {
            uneven.beginFrame(chunk);
            (void)drainTicks(uneven);
            fed += chunk;
            if (fed >= 2.0) {
                break;
            }
        }
    }
    // fed ends at exactly 2.0: the chunk pattern sums to 11/64 per cycle and
    // hits 2.0 after an integral number of chunks (128/64 * 64 = 2.0).
    SOL_CHECK(even.tickCount() == 64);
    SOL_CHECK(uneven.tickCount() == even.tickCount());
}

SOL_TEST(sim_simClockAdvancesByTickDelta)
{
    FixedLoop loop(64.0, 64); // cap above the backlog so all 32 ticks run
    loop.beginFrame(0.5);
    (void)drainTicks(loop);
    SOL_CHECK(loop.tickCount() == 32);
    SOL_CHECK(loop.simTimeSeconds() == 0.5);
    SOL_CHECK(loop.tickDelta() == 1.0 / 64.0);
}

SOL_TEST(sim_negativeDeltaIsIgnored)
{
    FixedLoop loop(32.0);
    loop.beginFrame(-1.0);
    SOL_CHECK(drainTicks(loop) == 0);
    SOL_CHECK(loop.alpha() == 0.0f);
}
