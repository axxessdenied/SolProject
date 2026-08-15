#include <sol/sim/steering.hpp>

#include <sol/test/test.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

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
    SOL_CHECK(aimError(state, target) < 0.1);      // nose on target
    SOL_CHECK(length(state.velocity) < 20.0);      // hovering, not orbiting wildly
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

    SOL_CHECK(cruised);                                   // the leg used the cruise drive
    SOL_CHECK(topSpeed > tuning.maxSpeed * 10.0);         // genuinely superluminal-ish leg
    const double distance = length(target - state.position);
    SOL_CHECK(distance < arrivalRange * 1.25);            // stopped at the doorstep
    SOL_CHECK(distance > 100.0);                          // not inside the target
    SOL_CHECK(length(state.velocity) < 20.0);             // and actually stopped
    SOL_CHECK(aimError(state, target) < 0.15);            // nose on the target
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
