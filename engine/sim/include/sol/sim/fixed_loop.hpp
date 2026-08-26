#pragma once

#include <cstdint>

namespace sol::sim {

// Fixed-timestep accumulator (engine plan 2.7). The sim advances in exact
// tickDelta() steps decoupled from render rate; alpha() is the fraction of
// the next tick already accumulated, used to blend the previous and current
// sim states when building render state. Sim code never reads the wall
// clock: simTimeSeconds() is the sim clock.
//
// Usage per rendered frame:
//     loop.beginFrame(frameDeltaSeconds);
//     while (loop.shouldTick()) { world.tick(loop.tickDelta()); }
//     render(interpolate(previous, current, loop.alpha()));
class FixedLoop
{
public:
    explicit FixedLoop(double ticksPerSecond = 60.0, std::uint32_t maxTicksPerFrame = 8);

    // Feed one rendered frame's real delta. The backlog is clamped to
    // maxTicksPerFrame ticks so a long stall causes slow-motion, not a
    // catch-up spiral.
    void beginFrame(double frameDeltaSeconds);

    // True while a full tick interval is accumulated; consumes it and
    // advances the sim clock.
    [[nodiscard]] bool shouldTick();

    [[nodiscard]] double tickDelta() const { return m_tickDelta; }

    // Fraction [0,1) of the next tick already accumulated (valid once
    // shouldTick has returned false for the frame).
    [[nodiscard]] float alpha() const;

    [[nodiscard]] std::uint64_t tickCount() const { return m_tickCount; }

    [[nodiscard]] double simTimeSeconds() const { return static_cast<double>(m_tickCount) * m_tickDelta; }

private:
    double m_tickDelta = 1.0 / 60.0;
    double m_accumulator = 0.0;
    std::uint32_t m_maxTicksPerFrame = 8;
    std::uint64_t m_tickCount = 0;
};

} // namespace sol::sim
