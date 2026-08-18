#pragma once

// Named-zone frame profiler (engine plan Phase 8n). Answers "where did the
// frame go", per subsystem, which nothing in this engine could answer before:
// the overlay's fps and frame-millisecond counters are end-to-end numbers and
// attribute time to nobody.
//
// Zones nest and report INCLUSIVE time - a parent's total contains its
// children's, and the report carries the depth so a reader can see the tree.
// Exclusive time is deliberately not computed: it is a display convenience
// that invites double-counting bugs, and the tree already answers the
// question.
//
// The report keeps a ring of recent frames rather than a running total,
// because mean tells you where the frame goes and max tells you about the
// hitch - and a hitch is exactly what an averaged fps counter hides.
//
// No allocation in the frame path: the zone table and its history are sized
// once at construction. A profiler that allocates is measuring itself.

#include <cstdint>
#include <vector>

namespace sol::core {

inline constexpr std::uint32_t kInvalidZone = 0xFFFFFFFFu;

struct ZoneReport
{
    const char* name = nullptr;
    std::uint32_t depth = 0;             // nesting level; 0 = frame root
    std::uint32_t parent = kInvalidZone; // zone index of the enclosing zone
    double lastMilliseconds = 0.0;       // last completed frame
    double meanMilliseconds = 0.0;       // over the history window
    double maxMilliseconds = 0.0;        // over the history window
    std::uint32_t calls = 0;             // entries during the last frame
    std::uint64_t counter = 0;           // input size when the zone last ran; 0 if unused
};

// Not thread-safe by design: this measures the main frame loop, which is a
// single thread (engine plan 2.5, "fixed order first"). Job-system zones need
// a per-worker instance and a merge, which is not this item's business.
class Profiler
{
public:
    static constexpr std::uint32_t kMaxZones = 64;
    static constexpr std::uint32_t kHistoryFrames = 120;
    static constexpr std::uint32_t kMaxDepth = 16;

    Profiler();

    // Publishes the frame just ended into the history ring and clears the
    // accumulators. Zones left open across the call are dropped, not carried:
    // an unbalanced zone is a bug, and smearing it over two frames would hide
    // it rather than report it.
    void beginFrame();

    // Opens a zone. `name` must outlive the profiler - a string literal.
    // Returns kInvalidZone once the table is full, which endZone tolerates,
    // so running out of zones costs measurements and never correctness.
    [[nodiscard]] std::uint32_t beginZone(const char* name);
    void endZone(std::uint32_t zone);

    // Attaches a per-frame magnitude to a zone: body counts, pair counts,
    // entity counts. Time says a pass is slow; this says how big its input
    // was, which is the difference between finding a cost and confirming a
    // suspicion. Accumulates across entries within a frame, and the published
    // value carries forward across frames the zone did not run in (see
    // publishFrame) so a 60 Hz zone stays readable under a faster render loop.
    void addCounter(std::uint32_t zone, std::uint64_t amount);

    // Off by default so a profiler that is never turned on cannot cost
    // anything but a branch. Toggling mid-run keeps the history rather than
    // clearing it, so you can stop the clock and still read the numbers.
    [[nodiscard]] bool enabled() const { return m_enabled; }

    void setEnabled(bool enabled) { m_enabled = enabled; }

    // Zones are indexed in first-encounter order, which for a stable frame is
    // execution order - so iterating 0..zoneCount() reads as the frame does.
    [[nodiscard]] std::uint32_t zoneCount() const { return m_zoneCount; }

    [[nodiscard]] ZoneReport report(std::uint32_t zone) const;
    [[nodiscard]] std::uint32_t findZone(const char* name) const;

    [[nodiscard]] std::uint64_t frameCount() const { return m_frameCount; }

    // Drops every zone and all history. For tests and for a fresh capture.
    void reset();

    // Test seam: the source of elapsed nanoseconds. Without it the ring
    // statistics could only be asserted against wall-clock durations, which
    // is how you get a suite that fails on a busy machine. nullptr restores
    // steady_clock.
    using ClockFn = std::int64_t (*)();
    void setClock(ClockFn clock);

private:
    struct Zone
    {
        const char* name = nullptr;
        std::uint32_t depth = 0;
        std::uint32_t parent = kInvalidZone;
        std::uint64_t firstFrame = 0; // history before this is not this zone's

        double frameNanoseconds = 0.0; // accumulating, current frame
        std::uint32_t frameCalls = 0;
        std::uint64_t frameCounter = 0;

        double lastNanoseconds = 0.0; // published, last completed frame
        std::uint32_t lastCalls = 0;
        std::uint64_t lastCounter = 0;

        std::vector<double> history; // nanoseconds per frame, ring
    };

    struct OpenZone
    {
        std::uint32_t zone = kInvalidZone;
        std::int64_t startNanoseconds = 0;
    };

    void publishFrame();
    [[nodiscard]] std::int64_t now() const;

    ClockFn m_clock = nullptr; // nullptr = steady_clock
    std::vector<Zone> m_zones;
    std::uint32_t m_zoneCount = 0;
    OpenZone m_stack[kMaxDepth];
    std::uint32_t m_depth = 0;
    std::uint64_t m_frameCount = 0;
    bool m_enabled = false;
};

// Process-global frame profiler, reached the way logging is (see log.hpp):
// instrumentation has to be callable from any layer without threading a
// reference through every signature that might contain a cost. The class
// above stays a plain object so tests can drive their own instance.
[[nodiscard]] Profiler& frameProfiler();

// RAII zone guard. Prefer the macro below.
class ProfileScope
{
public:
    ProfileScope(Profiler& profiler, const char* name)
        : m_profiler(&profiler), m_zone(profiler.beginZone(name))
    {
    }

    ~ProfileScope() { m_profiler->endZone(m_zone); }

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
    ProfileScope(ProfileScope&&) = delete;
    ProfileScope& operator=(ProfileScope&&) = delete;

    [[nodiscard]] std::uint32_t zone() const { return m_zone; }

private:
    Profiler* m_profiler;
    std::uint32_t m_zone;
};

} // namespace sol::core

#define SOL_PROFILE_CONCAT_INNER(a, b) a##b
#define SOL_PROFILE_CONCAT(a, b) SOL_PROFILE_CONCAT_INNER(a, b)

// Times the enclosing scope against the global frame profiler.
#define SOL_PROFILE_ZONE(name)                                                                               \
    ::sol::core::ProfileScope SOL_PROFILE_CONCAT(solProfileScope_, __LINE__)                                 \
    {                                                                                                        \
        ::sol::core::frameProfiler(), name                                                                   \
    }

// As SOL_PROFILE_ZONE, but binds the scope to `var` so the body can attach a
// counter to it via SOL_PROFILE_COUNT.
#define SOL_PROFILE_ZONE_NAMED(var, name)                                                                    \
    ::sol::core::ProfileScope var                                                                            \
    {                                                                                                        \
        ::sol::core::frameProfiler(), name                                                                   \
    }

#define SOL_PROFILE_COUNT(var, amount)                                                                       \
    ::sol::core::frameProfiler().addCounter((var).zone(), static_cast<std::uint64_t>(amount))
