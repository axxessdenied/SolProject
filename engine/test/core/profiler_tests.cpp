#include "sol/core/profiler.hpp"
#include "sol/test/test.hpp"

#include <cstdint>
#include <string>

namespace {

using sol::core::kInvalidZone;
using sol::core::Profiler;
using sol::core::ProfileScope;
using sol::core::ZoneReport;

// Fake clock: the profiler reads elapsed nanoseconds through this, so a test
// can state exactly how long a zone took instead of racing a real one.
std::int64_t g_clock = 0;

std::int64_t fakeClock()
{
    return g_clock;
}

void advance(std::int64_t nanoseconds)
{
    g_clock += nanoseconds;
}

[[nodiscard]] Profiler makeProfiler()
{
    g_clock = 0;
    Profiler profiler;
    profiler.setClock(&fakeClock);
    profiler.setEnabled(true);
    return profiler;
}

// Runs one zone for `nanoseconds` inside one frame.
void runFrame(Profiler& profiler, const char* name, std::int64_t nanoseconds)
{
    profiler.beginFrame();
    const std::uint32_t zone = profiler.beginZone(name);
    advance(nanoseconds);
    profiler.endZone(zone);
}

constexpr double kMillisecond = 1.0e6; // nanoseconds

[[nodiscard]] bool closeEnough(double a, double b)
{
    const double diff = a > b ? a - b : b - a;
    return diff < 1.0e-9;
}

} // namespace

SOL_TEST(profiler_isOffUntilEnabled)
{
    g_clock = 0;
    Profiler profiler;
    profiler.setClock(&fakeClock);

    profiler.beginFrame();
    const std::uint32_t zone = profiler.beginZone("sim");
    advance(5 * static_cast<std::int64_t>(kMillisecond));
    profiler.endZone(zone);

    // A profiler nobody turned on records nothing and costs a branch.
    SOL_CHECK(zone == kInvalidZone);
    SOL_CHECK(profiler.zoneCount() == 0);
}

SOL_TEST(profiler_accumulatesOneZoneAcrossAFrame)
{
    Profiler profiler = makeProfiler();
    runFrame(profiler, "sim", 4 * static_cast<std::int64_t>(kMillisecond));
    profiler.beginFrame(); // publishes the frame just run

    SOL_REQUIRE(profiler.zoneCount() == 1);
    const ZoneReport sim = profiler.report(0);
    SOL_CHECK(std::string(sim.name) == "sim");
    SOL_CHECK(closeEnough(sim.lastMilliseconds, 4.0));
    SOL_CHECK(closeEnough(sim.meanMilliseconds, 4.0));
    SOL_CHECK(closeEnough(sim.maxMilliseconds, 4.0));
    SOL_CHECK(sim.calls == 1);
    SOL_CHECK(sim.depth == 0);
    SOL_CHECK(sim.parent == kInvalidZone);
}

SOL_TEST(profiler_sumsRepeatedEntriesWithinOneFrame)
{
    // The fixed loop runs world.tick up to eight times per rendered frame; a
    // sim zone that reported one of them would be a measurement trap.
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    for (int tick = 0; tick < 8; ++tick) {
        const std::uint32_t zone = profiler.beginZone("sim.tick");
        advance(static_cast<std::int64_t>(kMillisecond) / 2);
        profiler.endZone(zone);
    }
    profiler.beginFrame();

    const ZoneReport tick = profiler.report(0);
    SOL_CHECK(closeEnough(tick.lastMilliseconds, 4.0)); // 8 x 0.5 ms
    SOL_CHECK(tick.calls == 8);
}

SOL_TEST(profiler_nestsAndReportsInclusiveTime)
{
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    const std::uint32_t frame = profiler.beginZone("frame");
    advance(static_cast<std::int64_t>(kMillisecond)); // 1 ms in the parent alone
    const std::uint32_t collide = profiler.beginZone("collide");
    advance(3 * static_cast<std::int64_t>(kMillisecond));
    profiler.endZone(collide);
    profiler.endZone(frame);
    profiler.beginFrame();

    SOL_REQUIRE(profiler.zoneCount() == 2);
    const ZoneReport parent = profiler.report(0);
    const ZoneReport child = profiler.report(1);

    SOL_CHECK(parent.depth == 0);
    SOL_CHECK(child.depth == 1);
    SOL_CHECK(child.parent == 0);
    // Inclusive: the parent's 4 ms contains the child's 3 ms.
    SOL_CHECK(closeEnough(parent.lastMilliseconds, 4.0));
    SOL_CHECK(closeEnough(child.lastMilliseconds, 3.0));
}

SOL_TEST(profiler_indexesZonesInFirstEncounterOrder)
{
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    for (const char* name : {"input", "sim", "render"}) {
        const std::uint32_t zone = profiler.beginZone(name);
        advance(static_cast<std::int64_t>(kMillisecond));
        profiler.endZone(zone);
    }
    profiler.beginFrame();

    SOL_REQUIRE(profiler.zoneCount() == 3);
    SOL_CHECK(std::string(profiler.report(0).name) == "input");
    SOL_CHECK(std::string(profiler.report(1).name) == "sim");
    SOL_CHECK(std::string(profiler.report(2).name) == "render");
}

SOL_TEST(profiler_findsZoneByContentNotPointerIdentity)
{
    // Literals from different translation units need not share an address,
    // so a pointer-only lookup would silently split one zone into two.
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    const std::uint32_t first = profiler.beginZone("collision.resolve");
    advance(static_cast<std::int64_t>(kMillisecond));
    profiler.endZone(first);

    std::string copy = "collision.resolve";
    const std::uint32_t second = profiler.beginZone(copy.c_str());
    advance(static_cast<std::int64_t>(kMillisecond));
    profiler.endZone(second);

    SOL_CHECK(first == second);
    SOL_CHECK(profiler.zoneCount() == 1);
    SOL_CHECK(profiler.findZone("collision.resolve") == first);
    SOL_CHECK(profiler.findZone("nothing.here") == kInvalidZone);
}

SOL_TEST(profiler_maxKeepsTheSpikeThatTheMeanHides)
{
    // The stated reason the report is a ring and not a running total.
    Profiler profiler = makeProfiler();
    for (int frame = 0; frame < 9; ++frame) {
        runFrame(profiler, "sim", static_cast<std::int64_t>(kMillisecond));
    }
    runFrame(profiler, "sim", 21 * static_cast<std::int64_t>(kMillisecond)); // the hitch
    profiler.beginFrame();

    const ZoneReport sim = profiler.report(0);
    SOL_CHECK(closeEnough(sim.lastMilliseconds, 21.0));
    SOL_CHECK(closeEnough(sim.meanMilliseconds, 3.0)); // (9 x 1 + 21) / 10
    SOL_CHECK(closeEnough(sim.maxMilliseconds, 21.0)); // the spike survives averaging
}

SOL_TEST(profiler_meanIgnoresFramesFromBeforeTheZoneExisted)
{
    // A zone first seen on frame 50 must not read as near-idle for its first
    // fifty frames' worth of window.
    Profiler profiler = makeProfiler();
    for (int frame = 0; frame < 20; ++frame) {
        runFrame(profiler, "sim", static_cast<std::int64_t>(kMillisecond));
    }
    // "docked" appears only now, and takes 6 ms every frame it runs.
    for (int frame = 0; frame < 2; ++frame) {
        profiler.beginFrame();
        const std::uint32_t zone = profiler.beginZone("docked");
        advance(6 * static_cast<std::int64_t>(kMillisecond));
        profiler.endZone(zone);
    }
    profiler.beginFrame();

    const std::uint32_t docked = profiler.findZone("docked");
    SOL_REQUIRE(docked != kInvalidZone);
    const ZoneReport report = profiler.report(docked);
    SOL_CHECK(report.calls == 1);
    SOL_CHECK(closeEnough(report.meanMilliseconds, 6.0)); // not 6 x 2 / 22
    SOL_CHECK(closeEnough(report.maxMilliseconds, 6.0));
}

SOL_TEST(profiler_ringWindowForgetsFramesOlderThanItsCapacity)
{
    Profiler profiler = makeProfiler();
    // One expensive frame, then a full window of cheap ones: the spike must
    // age out rather than sit in max forever.
    runFrame(profiler, "sim", 50 * static_cast<std::int64_t>(kMillisecond));
    for (std::uint32_t frame = 0; frame < Profiler::kHistoryFrames; ++frame) {
        runFrame(profiler, "sim", static_cast<std::int64_t>(kMillisecond));
    }
    profiler.beginFrame();

    const ZoneReport sim = profiler.report(0);
    SOL_CHECK(closeEnough(sim.maxMilliseconds, 1.0));
    SOL_CHECK(closeEnough(sim.meanMilliseconds, 1.0));
}

SOL_TEST(profiler_countersAccumulateWithinAFrameAndResetAcrossIt)
{
    // Time says a pass is slow; the counter says how big its input was.
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    for (int tick = 0; tick < 3; ++tick) {
        const std::uint32_t zone = profiler.beginZone("collision.resolve");
        advance(static_cast<std::int64_t>(kMillisecond));
        profiler.addCounter(zone, 15300);
        profiler.endZone(zone);
    }
    profiler.beginFrame();
    SOL_CHECK(profiler.report(0).counter == 45900);

    // Next frame starts clean rather than carrying the last one's total.
    const std::uint32_t zone = profiler.beginZone("collision.resolve");
    advance(static_cast<std::int64_t>(kMillisecond));
    profiler.addCounter(zone, 7);
    profiler.endZone(zone);
    profiler.beginFrame();
    SOL_CHECK(profiler.report(0).counter == 7);
}

SOL_TEST(profiler_counterCarriesForwardAcrossFramesTheZoneDidNotRunIn)
{
    // The sim ticks at 60 Hz under a render loop running several times
    // faster, so most frames run no tick. A counter that zeroed with them
    // would read 0 almost every time a drive asked it - which is exactly what
    // the first 8n measurement drive saw (BODIES=0 in a system full of rocks).
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    const std::uint32_t zone = profiler.beginZone("sim.collision.build");
    advance(static_cast<std::int64_t>(kMillisecond));
    profiler.addCounter(zone, 175);
    profiler.endZone(zone);

    // Five frames in which the zone does not run at all.
    for (int frame = 0; frame < 5; ++frame) {
        profiler.beginFrame();
        const std::uint32_t other = profiler.beginZone("render");
        advance(static_cast<std::int64_t>(kMillisecond));
        profiler.endZone(other);
    }
    profiler.beginFrame();

    const ZoneReport build = profiler.report(profiler.findZone("sim.collision.build"));
    SOL_CHECK(build.counter == 175);                     // input size survives
    SOL_CHECK(closeEnough(build.lastMilliseconds, 0.0)); // but time does not
    SOL_CHECK(build.calls == 0);
}

SOL_TEST(profiler_scopeGuardTimesItsScope)
{
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    {
        ProfileScope outer(profiler, "outer");
        advance(2 * static_cast<std::int64_t>(kMillisecond));
        {
            ProfileScope inner(profiler, "inner");
            advance(5 * static_cast<std::int64_t>(kMillisecond));
            profiler.addCounter(inner.zone(), 42);
        }
    }
    profiler.beginFrame();

    SOL_REQUIRE(profiler.zoneCount() == 2);
    SOL_CHECK(closeEnough(profiler.report(0).lastMilliseconds, 7.0));
    SOL_CHECK(closeEnough(profiler.report(1).lastMilliseconds, 5.0));
    SOL_CHECK(profiler.report(1).counter == 42);
}

SOL_TEST(profiler_survivesRunningOutOfZones)
{
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    // One more distinct name than the table holds.
    std::string names[Profiler::kMaxZones + 1];
    for (std::uint32_t i = 0; i < Profiler::kMaxZones + 1; ++i) {
        names[i] = "zone" + std::to_string(i);
        const std::uint32_t zone = profiler.beginZone(names[i].c_str());
        advance(static_cast<std::int64_t>(kMillisecond));
        profiler.endZone(zone);
        if (i == Profiler::kMaxZones) {
            SOL_CHECK(zone == kInvalidZone); // costs a measurement, not correctness
        }
    }
    profiler.beginFrame();

    SOL_CHECK(profiler.zoneCount() == Profiler::kMaxZones);
    SOL_CHECK(closeEnough(profiler.report(0).lastMilliseconds, 1.0));
    // The overflowing zone is refused rather than overwriting a neighbour.
    SOL_CHECK(profiler.findZone("zone64") == kInvalidZone);
}

SOL_TEST(profiler_survivesExceedingMaxDepth)
{
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    std::uint32_t opened[Profiler::kMaxDepth + 2] = {};
    std::string names[Profiler::kMaxDepth + 2];
    for (std::uint32_t i = 0; i < Profiler::kMaxDepth + 2; ++i) {
        names[i] = "deep" + std::to_string(i);
        opened[i] = profiler.beginZone(names[i].c_str());
        advance(static_cast<std::int64_t>(kMillisecond));
    }
    SOL_CHECK(opened[Profiler::kMaxDepth] == kInvalidZone);
    for (std::uint32_t i = Profiler::kMaxDepth + 2; i > 0; --i) {
        profiler.endZone(opened[i - 1]);
    }
    profiler.beginFrame();

    SOL_CHECK(profiler.zoneCount() == Profiler::kMaxDepth);
    SOL_CHECK(profiler.report(0).depth == 0);
}

SOL_TEST(profiler_dropsAnUnbalancedZoneRatherThanSmearingIt)
{
    // Leaving a zone open across a frame boundary is a caller bug. It should
    // read as a missing measurement, not as a plausible 16 ms.
    Profiler profiler = makeProfiler();
    profiler.beginFrame();
    const std::uint32_t leaked = profiler.beginZone("leaked");
    advance(3 * static_cast<std::int64_t>(kMillisecond));

    profiler.beginFrame(); // stack dropped here
    const std::uint32_t sim = profiler.beginZone("sim");
    advance(static_cast<std::int64_t>(kMillisecond));
    profiler.endZone(sim);
    profiler.endZone(leaked); // stale close from the previous frame: ignored
    profiler.beginFrame();

    const std::uint32_t leakedIndex = profiler.findZone("leaked");
    SOL_REQUIRE(leakedIndex != kInvalidZone);
    SOL_CHECK(closeEnough(profiler.report(leakedIndex).lastMilliseconds, 0.0));
    SOL_CHECK(profiler.report(leakedIndex).calls == 0);
    // The well-formed zone beside it is unaffected.
    SOL_CHECK(closeEnough(profiler.report(profiler.findZone("sim")).lastMilliseconds, 1.0));
}

SOL_TEST(profiler_resetClearsZonesAndHistory)
{
    Profiler profiler = makeProfiler();
    for (int frame = 0; frame < 5; ++frame) {
        runFrame(profiler, "sim", 2 * static_cast<std::int64_t>(kMillisecond));
    }
    profiler.beginFrame();
    SOL_CHECK(profiler.zoneCount() == 1);

    profiler.reset();
    SOL_CHECK(profiler.zoneCount() == 0);
    SOL_CHECK(profiler.frameCount() == 0);
    SOL_CHECK(profiler.report(0).name == nullptr);

    runFrame(profiler, "sim", static_cast<std::int64_t>(kMillisecond));
    profiler.beginFrame();
    SOL_REQUIRE(profiler.zoneCount() == 1);
    SOL_CHECK(closeEnough(profiler.report(0).meanMilliseconds, 1.0)); // no memory of the old 2 ms
}

SOL_TEST(profiler_reportRejectsAnOutOfRangeZone)
{
    Profiler profiler = makeProfiler();
    runFrame(profiler, "sim", static_cast<std::int64_t>(kMillisecond));
    profiler.beginFrame();

    const ZoneReport missing = profiler.report(99);
    SOL_CHECK(missing.name == nullptr);
    SOL_CHECK(closeEnough(missing.lastMilliseconds, 0.0));
    profiler.addCounter(99, 5); // must not corrupt a real zone
    SOL_CHECK(profiler.report(0).counter == 0);
}
