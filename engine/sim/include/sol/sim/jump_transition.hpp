#pragma once

// Jump transition timing (engine plan Phase 8v). How long a jump takes to fly,
// what the starfield is doing at each moment of it, and — the part that
// actually has to be right — exactly when the world underneath is allowed to
// change.
//
// Pure and header-only so the sequence can be asserted headlessly, the same
// reason sol/sim/docking.hpp holds the berth geometry: "the swap happened
// twice" and "the swap never happened" are both bugs you want a test to find
// rather than a drive.
//
// This owns PHASE AND TIME ONLY. It draws nothing, knows no entities, and does
// not perform the system change itself — it reports the one moment the caller
// should, and the caller confirms. That split is what lets the game defer the
// actual load to end of tick, which it must: loadSystem mid-tick invalidates
// the collision scratch, and the death respawn has deferred it for that reason
// since Phase 8l (space_world.hpp, m_pendingRespawnSystem).

#include "sol/core/math/math.hpp"

#include <cstdint>

namespace sol::sim {

// Short, and tuned on evidence rather than argued about up front — the answer
// the user gave when asked how long this should be. These are the two numbers
// to move after flying it, in the tradition of kAssistSeconds (8l) and
// kHailRange (8s).
inline constexpr double kJumpTunnelSeconds = 1.8;
inline constexpr double kJumpArriveSeconds = 0.9;

// How much brighter the starfield reads at the peak of the tunnel. Multiplies
// the scene's kSkyIntensity rather than replacing it, so the sky's rest
// brightness stays a single fact defined in one place.
inline constexpr double kJumpSkyPeak = 3.0;

// Did this tick's motion carry the ship THROUGH a gate's opening?
//
// Phase 8w, replacing 8v's proximity sphere. The sphere had no notion of
// direction, so anything that came near enough was taken — and because a gate
// is solid (a Cube model of base radius 1 at scale 70, plus an 8 m hull, so the
// ship stops dead at 78 m) "near enough" had to be 110 m, which is a wide net
// to cast on a player who was only flying past. Phase 8w makes the gate
// non-solid and asks the honest question instead.
//
// ⚑ The direction requirement is not a rule here, it is the shape of the test:
// a segment that never changes sides of the plane cannot cross it, so flying
// past, alongside, or stopping short can never trigger a jump. That is what
// "you have to get right up on it" buys, and it is why no tuning constant is
// needed to make accidental jumps rare — they are impossible.
//
// `axis` is the gate's facing (unit). It is DERIVED, never stored: generation
// places every gate at hub + bearing * gateDistance, so the axis is
// normalize(gate - hub), the lane the gate serves. A gate works in BOTH
// directions, because nothing here cares about the sign of the crossing.
[[nodiscard]] inline bool crossedAperture(const core::DVec3& from, const core::DVec3& to,
                                          const core::DVec3& gate, const core::DVec3& axis,
                                          double frameRadius)
{
    const double before = dot(from - gate, axis);
    const double after = dot(to - gate, axis);
    if (before == after) {
        return false; // travelling parallel to the plane: never crosses it
    }
    if ((before > 0.0 && after > 0.0) || (before < 0.0 && after < 0.0)) {
        return false; // stayed on one side, however close it got
    }
    // Signs differ, so the crossing is on the segment: find it and ask whether
    // it went through the opening rather than past the frame's edge.
    const double t = before / (before - after);
    const core::DVec3 hit = from + (to - from) * t;
    return length(hit - gate) <= frameRadius;
}

enum class JumpPhase
{
    Idle,
    Tunnel, // the lane: streaks build to full, and the world has not changed yet
    Arrive, // the far side: streaks fall away, the destination is already loaded
};

struct JumpTransitionParams
{
    double tunnelSeconds = kJumpTunnelSeconds;
    double arriveSeconds = kJumpArriveSeconds;
};

// A jump in flight.
//
// The swap sits on the Tunnel -> Arrive boundary, which is the instant warp()
// reaches 1.0 and the starfield is a smear. That is the whole trick: the pop
// still happens exactly as it always did, at a moment there is nothing legible
// on screen to pop.
class JumpTransition
{
public:
    JumpTransition() = default;
    explicit JumpTransition(const JumpTransitionParams& params) : m_params(params) {}

    // Starts a jump to `destination`. Refuses while one is already running --
    // you cannot jump out of a jump, and returning false lets the caller say so
    // rather than silently retargeting a transition in flight.
    bool begin(std::uint32_t destination)
    {
        if (m_phase != JumpPhase::Idle) {
            return false;
        }
        m_phase = JumpPhase::Tunnel;
        m_elapsed = 0.0;
        m_destination = destination;
        m_swapPending = false;
        return true;
    }

    void advance(double deltaSeconds)
    {
        if (m_phase == JumpPhase::Idle) {
            return;
        }
        m_elapsed += deltaSeconds;
        if (m_phase == JumpPhase::Tunnel && m_elapsed >= m_params.tunnelSeconds) {
            // Carry the remainder rather than dropping it, so a long frame does
            // not silently stretch the arrival by however much it overshot.
            m_elapsed -= m_params.tunnelSeconds;
            m_phase = JumpPhase::Arrive;
            m_swapPending = true;
        }
        // The transition does not end while the caller still owes us the swap.
        // One long frame can step clean over the tunnel AND the arrival, and
        // finishing here would leave the player in the system they jumped out
        // of, with nothing on screen to say so.
        if (m_phase == JumpPhase::Arrive && !m_swapPending
            && m_elapsed >= m_params.arriveSeconds) {
            clear();
        }
    }

    // True on exactly the one advance that crossed the swap point, and stays
    // true until noteSwapped() consumes it. Read-and-confirm rather than a bare
    // edge, because the caller defers the actual load to end of tick and the
    // flag has to survive the gap.
    [[nodiscard]] bool swapDue() const { return m_swapPending; }

    void noteSwapped() { m_swapPending = false; }

    void clear()
    {
        m_phase = JumpPhase::Idle;
        m_elapsed = 0.0;
        m_destination = 0;
        m_swapPending = false;
    }

    [[nodiscard]] JumpPhase phase() const { return m_phase; }
    [[nodiscard]] bool active() const { return m_phase != JumpPhase::Idle; }
    [[nodiscard]] std::uint32_t destination() const { return m_destination; }
    [[nodiscard]] double elapsed() const { return m_elapsed; }

    // 0 at rest, 1 at the swap. Accelerating into the lane and decelerating out
    // of it, so the two halves meet at full stretch without a visible corner.
    [[nodiscard]] double warp() const
    {
        switch (m_phase) {
        case JumpPhase::Idle: break;
        case JumpPhase::Tunnel: {
            const double t = progress(m_params.tunnelSeconds);
            return t * t;
        }
        case JumpPhase::Arrive: {
            const double t = 1.0 - progress(m_params.arriveSeconds);
            return t * t;
        }
        }
        return 0.0;
    }

    // Multiplier on the scene's resting sky intensity. Driven off warp() rather
    // than tracked separately: one curve, so the brightness can never disagree
    // with the streaks it is supposed to be part of.
    [[nodiscard]] double skyScale() const { return 1.0 + warp() * (kJumpSkyPeak - 1.0); }

private:
    // A zero-length phase is complete the moment it starts. Guarding here is
    // what keeps a params struct of all zeroes degrading into an immediate swap
    // instead of dividing by it.
    [[nodiscard]] double progress(double duration) const
    {
        if (duration <= 0.0) {
            return 1.0;
        }
        return core::clamp(m_elapsed / duration, 0.0, 1.0);
    }

    JumpTransitionParams m_params{};
    JumpPhase m_phase = JumpPhase::Idle;
    double m_elapsed = 0.0;
    std::uint32_t m_destination = 0;
    bool m_swapPending = false;
};

} // namespace sol::sim
