#include <sol/sim/jump_transition.hpp>

#include <sol/test/test.hpp>

using sol::sim::JumpPhase;
using sol::sim::JumpTransition;
using sol::sim::JumpTransitionParams;
using sol::sim::kJumpArriveSeconds;
using sol::sim::kJumpSkyPeak;
using sol::sim::kJumpTunnelSeconds;

namespace {

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
    const double steps[] = {1.0 / 144.0, 1.0 / 60.0, 1.0 / 30.0, kJumpTunnelSeconds,
                            kJumpTunnelSeconds / 2.0};
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
