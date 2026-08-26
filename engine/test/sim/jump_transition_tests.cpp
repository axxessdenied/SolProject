#include <sol/sim/jump_transition.hpp>
#include <sol/test/test.hpp>

using sol::core::DVec3;
using sol::sim::crossedAperture;
using sol::sim::JumpPhase;
using sol::sim::JumpTransition;
using sol::sim::JumpTransitionParams;
using sol::sim::kJumpArriveSeconds;
using sol::sim::kJumpSkyPeak;
using sol::sim::kJumpTunnelSeconds;

namespace {

// What the game puts at a gate, restated here so the test fails if either side
// moves. A gate is a Cube model (modelBaseRadius 1.0) drawn at RenderShape
// scale 70, and collision radius is base * scale.x — so the gate is a SOLID
// 70 m sphere. The player's hull is the Ship model at base 8.0, scale 1.
constexpr double kGateFrameRadius = 70.0;
constexpr double kPlayerHullRadius = 8.0;

// Runs a whole transition in `step`-sized advances, consuming the swap the way
// the game does, and reports how many times it was offered. The count is the
// point: a jump that swapped twice would load the destination on top of itself,
// and one that swapped never would leave the player where they started.
struct RunResult
{
    int swaps = 0;
    int frames = 0;
    double maxWarp = 0.0;
    double minWarp = 1.0;
    bool warpLeftRange = false;
    bool sawTunnel = false;
    bool sawArrive = false;
    bool arriveBeforeSwap = false;
};

RunResult run(JumpTransition& jump, double step)
{
    RunResult result;
    // Bounded so a transition that never finishes fails as a hang in the
    // assertion below rather than as a hung test binary.
    while (jump.active() && result.frames < 100000) {
        jump.advance(step);
        ++result.frames;
        if (jump.phase() == JumpPhase::Tunnel) {
            result.sawTunnel = true;
        }
        if (jump.phase() == JumpPhase::Arrive) {
            result.sawArrive = true;
        }
        const double warp = jump.warp();
        if (warp < 0.0 || warp > 1.0) {
            result.warpLeftRange = true;
        }
        result.maxWarp = warp > result.maxWarp ? warp : result.maxWarp;
        result.minWarp = warp < result.minWarp ? warp : result.minWarp;
        if (jump.swapDue()) {
            // The world may only change on the far side of the lane.
            if (jump.phase() != JumpPhase::Arrive) {
                result.arriveBeforeSwap = true;
            }
            ++result.swaps;
            jump.noteSwapped();
        }
    }
    return result;
}

} // namespace

SOL_TEST(jump_transition_runs_its_phases_in_order_and_returns_to_rest)
{
    JumpTransition jump;
    SOL_CHECK(!jump.active());
    SOL_CHECK(jump.phase() == JumpPhase::Idle);
    SOL_CHECK(jump.warp() == 0.0);

    SOL_CHECK(jump.begin(7));
    SOL_CHECK(jump.active());
    SOL_CHECK(jump.phase() == JumpPhase::Tunnel);
    SOL_CHECK(jump.destination() == 7);

    const RunResult result = run(jump, 1.0 / 60.0);
    SOL_CHECK(result.sawTunnel);
    SOL_CHECK(result.sawArrive);
    SOL_CHECK(!result.arriveBeforeSwap);
    SOL_CHECK(jump.phase() == JumpPhase::Idle);
    SOL_CHECK(!jump.active());

    // Roughly the advertised length: the two constants plus at most one frame
    // of overshoot. Loose on purpose — this pins the shape, not the tuning.
    const double seconds = static_cast<double>(result.frames) / 60.0;
    SOL_CHECK(seconds >= kJumpTunnelSeconds + kJumpArriveSeconds);
    SOL_CHECK(seconds <= kJumpTunnelSeconds + kJumpArriveSeconds + 0.05);
}

SOL_TEST(jump_transition_offers_the_swap_exactly_once)
{
    // At a range of frame rates, including ones that land a frame boundary
    // exactly on the swap point.
    const double steps[] = {
        1.0 / 144.0, 1.0 / 60.0, 1.0 / 30.0, kJumpTunnelSeconds, kJumpTunnelSeconds / 2.0};
    for (const double step : steps) {
        JumpTransition jump;
        SOL_CHECK(jump.begin(3));
        const RunResult result = run(jump, step);
        SOL_CHECK(result.swaps == 1);
        SOL_CHECK(!result.arriveBeforeSwap);
        SOL_CHECK(jump.phase() == JumpPhase::Idle);
    }
}

SOL_TEST(jump_transition_survives_a_frame_that_steps_clean_over_the_whole_thing)
{
    // The case that a naive threshold gets wrong: one advance longer than the
    // entire transition. The swap must still be offered, and must still be
    // offered only once — dropping it here strands the player in the system
    // they jumped out of.
    JumpTransition jump;
    SOL_CHECK(jump.begin(11));
    jump.advance(1000.0);

    SOL_CHECK(jump.swapDue());
    SOL_CHECK(jump.phase() == JumpPhase::Arrive);
    SOL_CHECK(jump.destination() == 11);
    SOL_CHECK(jump.active()); // it does not finish while the swap is unconsumed

    jump.noteSwapped();
    SOL_CHECK(!jump.swapDue());

    jump.advance(0.0);
    SOL_CHECK(jump.phase() == JumpPhase::Idle);
    SOL_CHECK(!jump.active());
}

SOL_TEST(jump_transition_refuses_to_start_on_top_of_itself)
{
    JumpTransition jump;
    SOL_CHECK(jump.begin(2));
    SOL_CHECK(!jump.begin(5)); // you cannot jump out of a jump
    SOL_CHECK(jump.destination() == 2);

    jump.advance(kJumpTunnelSeconds + 0.001);
    SOL_CHECK(jump.phase() == JumpPhase::Arrive);
    SOL_CHECK(!jump.begin(5)); // nor out of the arrival
    SOL_CHECK(jump.destination() == 2);
}

SOL_TEST(jump_transition_clear_leaves_nothing_armed)
{
    // Mid-tunnel: what loading a save has to do, since loadSave does not go
    // through loadSystem and would otherwise resume inside a transition.
    JumpTransition jump;
    SOL_CHECK(jump.begin(4));
    jump.advance(kJumpTunnelSeconds * 0.5);
    jump.clear();
    SOL_CHECK(!jump.active());
    SOL_CHECK(!jump.swapDue());
    SOL_CHECK(jump.warp() == 0.0);
    jump.advance(1000.0);
    SOL_CHECK(!jump.swapDue()); // and clearing cannot leave a swap to fire later

    // Cleared across the swap point, with the swap still owed: the flag must go
    // with it, or the next tick loads a system nobody is travelling to.
    JumpTransition armed;
    SOL_CHECK(armed.begin(9));
    armed.advance(kJumpTunnelSeconds + 0.1);
    SOL_CHECK(armed.swapDue());
    armed.clear();
    SOL_CHECK(!armed.swapDue());
    SOL_CHECK(!armed.active());
}

SOL_TEST(jump_transition_warp_and_sky_stay_in_range_and_peak_at_the_swap)
{
    JumpTransition jump;
    SOL_CHECK(jump.begin(1));

    // Rest, both ends.
    SOL_CHECK(jump.warp() == 0.0);
    SOL_CHECK(jump.skyScale() == 1.0);

    // Full stretch exactly at the boundary, which is where the swap lands and
    // therefore the one moment the starfield must be unreadable.
    jump.advance(kJumpTunnelSeconds);
    SOL_CHECK(jump.swapDue());
    SOL_CHECK(jump.warp() > 0.99);
    SOL_CHECK(jump.skyScale() > kJumpSkyPeak - 0.05);
    jump.noteSwapped();

    const RunResult result = run(jump, 1.0 / 120.0);
    SOL_CHECK(!result.warpLeftRange);
    SOL_CHECK(result.maxWarp <= 1.0);
    SOL_CHECK(jump.warp() == 0.0);
    SOL_CHECK(jump.skyScale() == 1.0);

    // And over a whole run from cold, the curve is bounded the entire way.
    JumpTransition whole;
    SOL_CHECK(whole.begin(1));
    const RunResult full = run(whole, 1.0 / 120.0);
    SOL_CHECK(!full.warpLeftRange);
    SOL_CHECK(full.maxWarp > 0.9);  // it does reach full stretch
    SOL_CHECK(full.minWarp < 0.15); // and it does come back
}

SOL_TEST(jump_transition_aperture_takes_a_ship_that_goes_through_it)
{
    const DVec3 gate{1.0e8, -4.0e7, 2.5e8};
    const DVec3 axis{0.0, 0.0, 1.0};

    // Straight through the middle.
    SOL_CHECK(crossedAperture(
        gate - DVec3{0.0, 0.0, 50.0}, gate + DVec3{0.0, 0.0, 50.0}, gate, axis, kGateFrameRadius));
    // Through the opening but off-centre: still through the doorway.
    const DVec3 offset{40.0, 20.0, 0.0}; // 44.7 m from the centre, inside 70
    SOL_CHECK(crossedAperture(gate + offset - DVec3{0.0, 0.0, 50.0},
                              gate + offset + DVec3{0.0, 0.0, 50.0},
                              gate,
                              axis,
                              kGateFrameRadius));
    // And the other way, because a gate is a doorway rather than a turnstile.
    SOL_CHECK(crossedAperture(
        gate + DVec3{0.0, 0.0, 50.0}, gate - DVec3{0.0, 0.0, 50.0}, gate, axis, kGateFrameRadius));
    // One tick of a fast ship stepping clean over the plane still counts.
    SOL_CHECK(crossedAperture(
        gate - DVec3{0.0, 0.0, 4000.0}, gate + DVec3{0.0, 0.0, 4000.0}, gate, axis, kGateFrameRadius));
}

SOL_TEST(jump_transition_aperture_refuses_everything_that_did_not_go_through)
{
    const DVec3 gate{-2.0e7, 8.0e6, -3.0e8};
    const DVec3 axis{1.0, 0.0, 0.0};

    // ⚑ The accidental-jump case, stated as a test: flying PAST the gate,
    // parallel to its plane and well inside the frame radius. Phase 8v's
    // proximity sphere took this one; an aperture cannot.
    SOL_CHECK(!crossedAperture(
        gate + DVec3{0.0, -500.0, 0.0}, gate + DVec3{0.0, 500.0, 0.0}, gate, axis, kGateFrameRadius));

    // Crossing the plane but outside the frame: past the edge, not through it.
    const DVec3 wide{0.0, 200.0, 0.0}; // 200 m off axis, frame is 70
    SOL_CHECK(!crossedAperture(gate + wide - DVec3{50.0, 0.0, 0.0},
                               gate + wide + DVec3{50.0, 0.0, 0.0},
                               gate,
                               axis,
                               kGateFrameRadius));

    // Approaching head-on and stopping short. Touching the threshold is not
    // passing through it, which is exactly the case autopilot has to beat.
    SOL_CHECK(!crossedAperture(
        gate - DVec3{900.0, 0.0, 0.0}, gate - DVec3{20.0, 0.0, 0.0}, gate, axis, kGateFrameRadius));

    // Sitting perfectly still at the gate's own position.
    SOL_CHECK(!crossedAperture(gate, gate, gate, axis, kGateFrameRadius));

    // Receding after having arrived on one side.
    SOL_CHECK(!crossedAperture(
        gate - DVec3{30.0, 0.0, 0.0}, gate - DVec3{600.0, 0.0, 0.0}, gate, axis, kGateFrameRadius));
}

SOL_TEST(jump_transition_aperture_is_a_real_doorway_the_ship_can_reach)
{
    // Phase 8v's lesson kept as a live assertion. A gate used to be solid: a
    // 70 m collision sphere plus an 8 m hull stopped the ship at 78 m, so an
    // opening the ship is supposed to pass through only means something if
    // nothing pushes it out first. Phase 8w makes gates non-blocking, and this
    // pins the arithmetic that made the old rule impossible.
    constexpr double kOldContactDistance = kGateFrameRadius + kPlayerHullRadius;
    SOL_CHECK(kOldContactDistance == 78.0);
    SOL_CHECK(kOldContactDistance > kGateFrameRadius); // i.e. it could never get in

    // The crossing point that matters is inside the frame, which is inside the
    // distance the ship used to stop at — so this test fails the day a gate
    // becomes solid again.
    const DVec3 gate{};
    const DVec3 axis{0.0, 1.0, 0.0};
    SOL_CHECK(crossedAperture(DVec3{0.0, -1.0, 0.0}, DVec3{0.0, 1.0, 0.0}, gate, axis, kGateFrameRadius));
}

SOL_TEST(jump_transition_zero_length_params_swap_immediately_rather_than_divide)
{
    // A degenerate tuning must not produce a NaN curve or a jump that never
    // lands. It should simply be over.
    JumpTransition jump{JumpTransitionParams{.tunnelSeconds = 0.0, .arriveSeconds = 0.0}};
    SOL_CHECK(jump.begin(6));
    SOL_CHECK(jump.warp() >= 0.0);
    SOL_CHECK(jump.warp() <= 1.0);

    jump.advance(1.0 / 60.0);
    SOL_CHECK(jump.swapDue());
    SOL_CHECK(jump.destination() == 6);
    jump.noteSwapped();
    jump.advance(1.0 / 60.0);
    SOL_CHECK(jump.phase() == JumpPhase::Idle);
}
