#include <algorithm>
#include <cmath>
#include <vector>

#include <sol/sim/steering.hpp>
#include <sol/test/test.hpp>

using sol::core::DVec3;
using sol::core::Quat;
using sol::sim::aimError;
using sol::sim::AvoidanceSphere;
using sol::sim::avoidObstacles;
using sol::sim::FlightInput;
using sol::sim::ShipState;
using sol::sim::ShipTuning;
using sol::sim::steerAimAndMove;
using sol::sim::steerEvade;
using sol::sim::steerFormation;
using sol::sim::steerPursue;
using sol::sim::steerTravel;
using sol::sim::stepShipFlight;

namespace {

constexpr double kDt = 1.0 / 60.0;

// Runs a steering closed loop for the given seconds.
template <typename SteerFn>
void simulate(ShipState& state, const ShipTuning& tuning, double seconds, SteerFn steer)
{
    const int ticks = static_cast<int>(seconds / kDt);
    for (int i = 0; i < ticks; ++i) {
        stepShipFlight(state, tuning, steer(state, i * kDt), kDt);
    }
}

} // namespace

SOL_TEST(steering_aim_signs)
{
    const ShipTuning tuning;
    ShipState state; // identity orientation, nose -Z

    // Target above: pitch up (+x); target left (-x world): yaw left (+y).
    FlightInput input = steerAimAndMove(state, tuning, {0.0, 100.0, -100.0}, {});
    SOL_CHECK(input.angular.x > 0.0f);
    input = steerAimAndMove(state, tuning, {-100.0, 0.0, -100.0}, {});
    SOL_CHECK(input.angular.y > 0.0f);
    // Target dead behind: saturated turn.
    input = steerAimAndMove(state, tuning, {0.0, 10.0, 100.0}, {});
    SOL_CHECK(input.angular.x == 1.0f);

    // Velocity command: forward desired velocity maps to negative body z.
    input = steerAimAndMove(state, tuning, {0.0, 0.0, -100.0}, {0.0, 0.0, -110.0});
    SOL_CHECK(std::abs(input.linear.z + 110.0f / tuning.maxSpeed) < 1e-5f);
    // And clamps at the envelope.
    input = steerAimAndMove(state, tuning, {0.0, 0.0, -100.0}, {0.0, 0.0, -10'000.0});
    SOL_CHECK(input.linear.z == -1.0f);
}

SOL_TEST(steering_pursue_settles_at_range)
{
    const ShipTuning tuning;
    ShipState state;
    state.position = {0.0, 0.0, 5'000.0}; // target 5 km ahead of the nose
    const DVec3 target{0.0, 0.0, 0.0};

    simulate(state, tuning, 90.0, [&](const ShipState& s, double) {
        return steerPursue(s, tuning, target, {}, 300.0);
    });

    const double distance = length(target - state.position);
    SOL_CHECK(std::abs(distance - 300.0) < 60.0); // settled near desired range
    SOL_CHECK(aimError(state, target) < 0.1);     // nose on target
    SOL_CHECK(length(state.velocity) < 20.0);     // hovering, not orbiting wildly
}

SOL_TEST(steering_pursue_offset_target_gets_aimed_at)
{
    const ShipTuning tuning;
    ShipState state; // target off to the side and behind
    const DVec3 target{2'000.0, 800.0, 3'000.0};

    simulate(state, tuning, 60.0, [&](const ShipState& s, double) {
        return steerPursue(s, tuning, target, {}, 250.0);
    });
    SOL_CHECK(aimError(state, target) < 0.15);
    SOL_CHECK(std::abs(length(target - state.position) - 250.0) < 60.0);
}

SOL_TEST(steering_evade_opens_distance)
{
    const ShipTuning tuning;
    ShipState state;
    const DVec3 threat{0.0, 0.0, -400.0}; // dead ahead

    const double before = length(state.position - threat);
    simulate(state, tuning, 15.0, [&](const ShipState& s, double t) {
        return steerEvade(s, tuning, threat, t * 3.0);
    });
    const double after = length(state.position - threat);
    SOL_CHECK(after > before + 1'000.0);
}

SOL_TEST(steering_formation_holds_slot_on_moving_anchor)
{
    const ShipTuning tuning;
    ShipState state;
    state.position = {500.0, -200.0, 400.0}; // starts out of position

    const DVec3 anchorStart{0.0, 0.0, 0.0};
    const DVec3 anchorVelocity{40.0, 0.0, -60.0};
    const DVec3 offset{80.0, 0.0, 60.0};

    simulate(state, tuning, 60.0, [&](const ShipState& s, double t) {
        const DVec3 anchor = anchorStart + anchorVelocity * t;
        return steerFormation(s, tuning, anchor, anchorVelocity, offset);
    });

    const DVec3 anchorEnd = anchorStart + anchorVelocity * 60.0;
    SOL_CHECK(length(state.position - (anchorEnd + offset)) < 40.0);
    SOL_CHECK(length(state.velocity - anchorVelocity) < 10.0);
}

SOL_TEST(steering_travel_phases)
{
    const ShipTuning tuning;
    ShipState state; // identity orientation, nose -Z

    // Far and aligned: cruise drive engaged, full-scale forward command.
    FlightInput input = steerTravel(state, tuning, {0.0, 0.0, -500'000.0}, {}, 1'500.0);
    SOL_CHECK(input.cruise);
    SOL_CHECK(input.linear.z < 0.0f);

    // Far but facing away: no cruise until the nose comes around.
    input = steerTravel(state, tuning, {0.0, 0.0, 500'000.0}, {}, 1'500.0);
    SOL_CHECK(!input.cruise);

    // Just outside the arrival bubble: sub-envelope approach speed.
    input = steerTravel(state, tuning, {0.0, 0.0, -1'600.0}, {}, 1'500.0);
    SOL_CHECK(!input.cruise);
    SOL_CHECK(input.linear.z < 0.0f);
    SOL_CHECK(input.linear.z > -0.5f); // sqrt(2*20*100) ~ 63 m/s of 220

    // Inside the bubble with a static target: command a full stop.
    input = steerTravel(state, tuning, {0.0, 0.0, -1'000.0}, {}, 1'500.0);
    SOL_CHECK(!input.cruise);
    SOL_CHECK(input.linear == sol::core::Vec3{});
}

SOL_TEST(steering_travel_arrives_from_cruise_range)
{
    const ShipTuning tuning;
    ShipState state;
    state.position = {0.0, 0.0, 500'000.0}; // 500 km out, target ahead of the nose
    const DVec3 target{0.0, 0.0, 0.0};
    const double arrivalRange = 1'500.0;

    bool cruised = false;
    double topSpeed = 0.0;
    simulate(state, tuning, 120.0, [&](const ShipState& s, double) {
        const FlightInput input = steerTravel(s, tuning, target, {}, arrivalRange);
        cruised = cruised || input.cruise;
        topSpeed = std::max(topSpeed, length(s.velocity));
        return input;
    });

    SOL_CHECK(cruised);                           // the leg used the cruise drive
    SOL_CHECK(topSpeed > tuning.maxSpeed * 10.0); // genuinely superluminal-ish leg
    const double distance = length(target - state.position);
    SOL_CHECK(distance < arrivalRange * 1.25); // stopped at the doorstep
    SOL_CHECK(distance > 100.0);               // not inside the target
    SOL_CHECK(length(state.velocity) < 20.0);  // and actually stopped
    SOL_CHECK(aimError(state, target) < 0.15); // nose on the target
}

SOL_TEST(steering_avoidance_deflects_collision_course)
{
    ShipState state;
    state.velocity = {0.0, 0.0, -200.0}; // heading straight at the obstacle
    DVec3 desired{0.0, 0.0, -200.0};
    const std::vector<AvoidanceSphere> obstacles = {{{0.0, 0.0, -1'500.0}, 150.0}};

    avoidObstacles(desired, state, obstacles, 10.0);
    // Lateral push appeared; forward intent retained.
    SOL_CHECK(std::sqrt(desired.x * desired.x + desired.y * desired.y) > 50.0);
    SOL_CHECK(desired.z < -100.0);

    // A clear path is untouched.
    DVec3 clear{0.0, 0.0, 200.0}; // moving away
    ShipState away;
    away.velocity = {0.0, 0.0, 200.0};
    avoidObstacles(clear, away, obstacles, 10.0);
    SOL_CHECK(clear == DVec3{0.0, 0.0, 200.0});
}

SOL_TEST(steering_path_query_reports_the_first_thing_in_the_way)
{
    // The query the game never had (Phase 8y). Avoidance is a lateral nudge
    // and at cruise a nudge cannot act: a shuttle covers ~38 km per tick while
    // a station is 130 m across, so the obstacle is crossed entirely inside
    // the tick that would have steered around it. This answers a DISTANCE
    // instead, which the caller turns into a speed.
    using sol::sim::kNoAvoidHandle;
    using sol::sim::pathBlockedAt;
    const DVec3 from{0.0, 0.0, 0.0};
    const DVec3 to{0.0, 0.0, -10'000.0};

    // Dead ahead: blocked at its own near edge, clearance included.
    const std::vector<AvoidanceSphere> ahead = {{{0.0, 0.0, -5'000.0}, 100.0, 7}};
    const double blocked = pathBlockedAt(from, to, 50.0, ahead);
    SOL_CHECK(std::abs(blocked - 4'850.0) < 1.0); // 5000 - (100 + 50)

    // Beside the path, behind the ship, and past the end of the segment: all
    // clear. The last is what stops a distant obstacle slowing a ship that
    // will have turned long before reaching it.
    const std::vector<AvoidanceSphere> beside = {{{2'000.0, 0.0, -5'000.0}, 100.0, 7}};
    SOL_CHECK(pathBlockedAt(from, to, 50.0, beside) < 0.0);
    const std::vector<AvoidanceSphere> behind = {{{0.0, 0.0, 5'000.0}, 100.0, 7}};
    SOL_CHECK(pathBlockedAt(from, to, 50.0, behind) < 0.0);
    const std::vector<AvoidanceSphere> beyond = {{{0.0, 0.0, -20'000.0}, 100.0, 7}};
    SOL_CHECK(pathBlockedAt(from, to, 50.0, beyond) < 0.0);

    // Nearest wins, whatever order they are listed in.
    const std::vector<AvoidanceSphere> two = {{{0.0, 0.0, -8'000.0}, 100.0, 7},
                                              {{0.0, 0.0, -3'000.0}, 100.0, 8}};
    SOL_CHECK(std::abs(pathBlockedAt(from, to, 50.0, two) - 2'850.0) < 1.0);

    // ⚑ The handle drops exactly one sphere: a ship must not avoid its own
    // body, and one that did would find itself blocked at zero and never move
    // again. Dropping the near one leaves the far one still blocking.
    SOL_CHECK(std::abs(pathBlockedAt(from, to, 50.0, two, 8) - 7'850.0) < 1.0);
    SOL_CHECK(pathBlockedAt(from, to, 50.0, ahead, 7) < 0.0);
    // And kNoAvoidHandle drops nothing, even against spheres that carry it.
    const std::vector<AvoidanceSphere> unhandled = {{{0.0, 0.0, -5'000.0}, 100.0}};
    SOL_CHECK(pathBlockedAt(from, to, 50.0, unhandled, kNoAvoidHandle) > 0.0);

    // Degenerate inputs answer rather than divide: no obstacles, and a segment
    // of no length at all.
    SOL_CHECK(pathBlockedAt(from, to, 50.0, {}) < 0.0);
    SOL_CHECK(pathBlockedAt(from, from, 50.0, ahead) < 0.0);
    // Already touching reports zero rather than a negative "clear".
    const std::vector<AvoidanceSphere> onTop = {{{0.0, 0.0, -10.0}, 100.0, 7}};
    SOL_CHECK(pathBlockedAt(from, to, 50.0, onTop) == 0.0);
}

SOL_TEST(steering_travel_does_not_fly_through_what_is_in_its_lane)
{
    // ⚑ The whole point of the phase, flown rather than inspected. Before this
    // a hunter crossing a system had NOTHING in its obstacle list but stations
    // and planets, so it flew its lane at cruise and hit whatever was on it -
    // measured in Phase 8x as a raider ramming its prey, and the player, at
    // 10^6 m/s. The rule is answered with speed rather than steering, so the
    // assertion is a distance never closed rather than a flag.
    using sol::sim::brakingSpeedLimit;
    const ShipTuning tuning;
    const DVec3 target{0.0, 0.0, -6.0e8}; // a real trade leg away
    const DVec3 blockerAt{0.0, 0.0, -3.0e6};
    const std::vector<AvoidanceSphere> blocker = {{blockerAt, 130.0, 3}};

    // ⚑ Measured on the SWEPT segment, not on the sampled positions, and the
    // difference is the phase in miniature: at cruise a tick covers tens of
    // kilometres, so a ship jumps clean over a 130 m station between two
    // samples and a per-tick proximity check sees nothing at all. The
    // collision pass sweeps for exactly this reason.
    const auto flyClosest = [&](std::span<const AvoidanceSphere> spheres,
                                std::uint32_t self,
                                ShipState& out) {
        ShipState state;
        double closest = 1.0e30;
        const int ticks = static_cast<int>(40.0 / kDt);
        for (int i = 0; i < ticks; ++i) {
            const DVec3 before = state.position;
            stepShipFlight(state, tuning, steerTravel(state, tuning, target, {}, 250.0, spheres, self), kDt);
            const DVec3 lane = state.position - before;
            const double laneLength = length(lane);
            double t = 0.0;
            if (laneLength > 0.0) {
                t = std::clamp(dot(blockerAt - before, lane) / (laneLength * laneLength), 0.0, 1.0);
            }
            closest = std::min(closest, length(blockerAt - (before + lane * t)));
        }
        out = state;
        return closest;
    };

    // The negative control first, and it is the bug itself: told about
    // nothing, the ship flies straight through the obstacle's position.
    ShipState blind;
    SOL_CHECK(flyClosest({}, sol::sim::kNoAvoidHandle, blind) < 130.0);
    SOL_CHECK(blind.position.z < blockerAt.z); // and out the far side

    // Told about it, the same flight never touches it.
    ShipState seeing;
    SOL_CHECK(flyClosest(blocker, 9, seeing) > 130.0);
    // And it is still going somewhere: a ship that answered "blocked" by
    // stopping forever would pass the check above and be useless.
    SOL_CHECK(seeing.position.z < -1.0e6);

    // Claiming the blocker as its own body puts it back in the blind case,
    // which is what the handle is for and why it must be passed.
    ShipState confused;
    SOL_CHECK(flyClosest(blocker, 3, confused) < 130.0);

    // The curve itself: monotonic, zero at zero, and never negative.
    SOL_CHECK(brakingSpeedLimit(tuning, 0.0) == 0.0);
    SOL_CHECK(brakingSpeedLimit(tuning, -5.0) == 0.0);
    double previous = -1.0;
    for (int i = 0; i <= 40; ++i) {
        const double limit = brakingSpeedLimit(tuning, static_cast<double>(i) * 500.0);
        SOL_CHECK(limit >= previous);
        previous = limit;
    }
    // A ship with no reverse thrust may not choose to go fast.
    ShipTuning stuck = tuning;
    stuck.reverseAccel = 0.0f;
    SOL_CHECK(brakingSpeedLimit(stuck, 10'000.0) == 0.0);
}
