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

// ⚑ How close you have to get to a gate for it to take you — and the one
// number in this item that flying it had to correct.
//
// The obvious choice is the gate's own drawn frame, kGateRadiusMeters = 70 m.
// That is exactly wrong, because the gate is SOLID: it is a Cube model of base
// radius 1 at scale 70, so it carries a 70 m collision sphere, and the player's
// hull is another 8 m. The ship physically stops at 78 m and cannot be moved a
// metre closer — six seconds of full manual thrust does nothing. Capturing at
// the frame radius means capturing inside the one volume the ship is excluded
// from, so the jump could never fire.
//
// 110 m therefore sits comfortably outside the 78 m contact distance, with
// 32 m of daylight, and jump_transition_tests asserts that margin rather than
// trusting this comment. It is the same rule Phase 8r's berths landed on
// (capture must not overlap the sphere the ship is pushed out of), arrived at
// from the opposite direction.
inline constexpr double kGateCaptureRadius = 110.0;

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
