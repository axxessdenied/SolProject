#include "sol/core/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace sol::core {

namespace {

// steady_clock rather than the platform timer: core sits below platform and
// may not reach it, and on every target we build for this is the same
// high-resolution counter behind a portable name.
[[nodiscard]] std::int64_t steadyNanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr double kNanosecondsPerMillisecond = 1.0e6;

} // namespace

std::int64_t Profiler::now() const
{
    return m_clock != nullptr ? m_clock() : steadyNanoseconds();
}

void Profiler::setClock(ClockFn clock)
{
    m_clock = clock;
}

Profiler::Profiler()
{
    // Sized once, here, so nothing in the frame path ever allocates.
    m_zones.resize(kMaxZones);
    for (Zone& zone : m_zones) {
        zone.history.assign(kHistoryFrames, 0.0);
    }
}

void Profiler::beginFrame()
{
    publishFrame();
    ++m_frameCount;

    // An unbalanced zone is a bug in the caller. Dropping the stack reports
    // it as a missing measurement next frame rather than smearing one zone
    // across two frames, which would look like a plausible number.
    m_depth = 0;
}

void Profiler::publishFrame()
{
    const std::uint32_t slot = static_cast<std::uint32_t>(m_frameCount % kHistoryFrames);
    for (std::uint32_t i = 0; i < m_zoneCount; ++i) {
        Zone& zone = m_zones[i];
        zone.lastNanoseconds = zone.frameNanoseconds;
        zone.lastCalls = zone.frameCalls;
        // Time is a property of the frame and reports 0 for a frame the zone
        // did not run in. The counter is a property of the zone's INPUT, so it
        // carries forward instead: the sim ticks at 60 Hz under a render loop
        // running several times faster, so most frames run no tick at all and
        // a counter that zeroed with them would read 0 almost every time you
        // asked it. It means "the last time this ran, its input was N".
        if (zone.frameCalls > 0) {
            zone.lastCounter = zone.frameCounter;
        }
        zone.history[slot] = zone.frameNanoseconds;
        zone.frameNanoseconds = 0.0;
        zone.frameCalls = 0;
        zone.frameCounter = 0;
    }
}

std::uint32_t Profiler::beginZone(const char* name)
{
    if (!m_enabled || name == nullptr || m_depth >= kMaxDepth) {
        return kInvalidZone;
    }

    std::uint32_t zone = findZone(name);
    if (zone == kInvalidZone) {
        if (m_zoneCount >= kMaxZones) {
            return kInvalidZone; // out of table: costs a measurement, not correctness
        }
        zone = m_zoneCount++;
        Zone& fresh = m_zones[zone];
        fresh.name = name;
        fresh.depth = m_depth;
        fresh.parent = m_depth > 0 ? m_stack[m_depth - 1].zone : kInvalidZone;
        fresh.firstFrame = m_frameCount;
        // A zone created mid-run inherits a window of zeros it was never
        // present for; firstFrame is what keeps those out of its mean.
        std::fill(fresh.history.begin(), fresh.history.end(), 0.0);
    }

    m_stack[m_depth] = OpenZone{.zone = zone, .startNanoseconds = now()};
    ++m_depth;
    return zone;
}

void Profiler::endZone(std::uint32_t zone)
{
    if (zone == kInvalidZone || m_depth == 0) {
        return;
    }
    // Tolerate a mismatched close rather than asserting: instrumentation must
    // never be the thing that takes the game down.
    if (m_stack[m_depth - 1].zone != zone) {
        return;
    }
    --m_depth;
    const std::int64_t elapsed = now() - m_stack[m_depth].startNanoseconds;
    Zone& entry = m_zones[zone];
    entry.frameNanoseconds += static_cast<double>(elapsed);
    ++entry.frameCalls;
}

void Profiler::addCounter(std::uint32_t zone, std::uint64_t amount)
{
    if (zone == kInvalidZone || zone >= m_zoneCount) {
        return;
    }
    m_zones[zone].frameCounter += amount;
}

std::uint32_t Profiler::findZone(const char* name) const
{
    if (name == nullptr) {
        return kInvalidZone;
    }
    // Linear over at most kMaxZones entries, pointer-compared first: the
    // literals repeat every frame from the same address, so the strcmp is
    // the cold path across translation units rather than the common one.
    for (std::uint32_t i = 0; i < m_zoneCount; ++i) {
        const char* candidate = m_zones[i].name;
        if (candidate == name || std::strcmp(candidate, name) == 0) {
            return i;
        }
    }
    return kInvalidZone;
}

ZoneReport Profiler::report(std::uint32_t zone) const
{
    if (zone >= m_zoneCount) {
        return ZoneReport{};
    }
    const Zone& entry = m_zones[zone];

    // Only frames this zone has actually existed for count toward its mean,
    // otherwise a zone first seen on frame 500 reads as near-idle for its
    // first two seconds.
    const std::uint64_t lived = m_frameCount - entry.firstFrame;
    const std::uint32_t samples = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(lived, static_cast<std::uint64_t>(kHistoryFrames)));

    double total = 0.0;
    double peak = 0.0;
    for (std::uint32_t i = 0; i < samples; ++i) {
        // Walk back from the most recently published slot.
        const std::uint64_t frame = m_frameCount - 1 - i;
        const double value = entry.history[static_cast<std::size_t>(frame % kHistoryFrames)];
        total += value;
        peak = std::max(peak, value);
    }

    return ZoneReport{
        .name = entry.name,
        .depth = entry.depth,
        .parent = entry.parent,
        .lastMilliseconds = entry.lastNanoseconds / kNanosecondsPerMillisecond,
        .meanMilliseconds =
            samples > 0 ? total / static_cast<double>(samples) / kNanosecondsPerMillisecond : 0.0,
        .maxMilliseconds = peak / kNanosecondsPerMillisecond,
        .calls = entry.lastCalls,
        .counter = entry.lastCounter,
    };
}

void Profiler::reset()
{
    for (std::uint32_t i = 0; i < m_zoneCount; ++i) {
        m_zones[i] = Zone{};
        m_zones[i].history.assign(kHistoryFrames, 0.0);
    }
    m_zoneCount = 0;
    m_depth = 0;
    m_frameCount = 0;
}

Profiler& frameProfiler()
{
    static Profiler profiler;
    return profiler;
}

} // namespace sol::core
