#include "sol/sim/flight.hpp"

#include "sol/test/test.hpp"

#include <cmath>

namespace {

using sol::core::DVec3;
using sol::core::Quat;
using sol::core::Vec3;
using sol::sim::FlightInput;
using sol::sim::ShipState;
using sol::sim::ShipTuning;
using sol::sim::stepShipFlight;

constexpr double kDt = 1.0 / 60.0;

void step(ShipState& state, const ShipTuning& tuning, const FlightInput& input, int ticks)
{
    for (int i = 0; i < ticks; ++i) {
        stepShipFlight(state, tuning, input, kDt);
    }
}

[[nodiscard]] double speed(const ShipState& state)
{
    return length(state.velocity);
}

} // namespace

SOL_TEST(flight_assistOnReachesAndHoldsSpeedCap)
{
    ShipTuning tuning;
    ShipState state;
    FlightInput input;
    input.linear = {0.0f, 0.0f, -1.0f}; // full forward

    step(state, tuning, input, 60 * 30); // 30 sim-seconds
    SOL_CHECK(std::abs(speed(state) - tuning.maxSpeed) < 0.5);

    // The cap must hold, not oscillate past it.
    for (int i = 0; i < 600; ++i) {
        stepShipFlight(state, tuning, input, kDt);
        SOL_CHECK(speed(state) <= tuning.maxSpeed + 0.5);
    }

    // Forward is -Z for an identity orientation.
    SOL_CHECK(state.velocity.z < 0.0);
    SOL_CHECK(std::abs(state.velocity.x) < 1e-3);
    SOL_CHECK(std::abs(state.velocity.y) < 1e-3);
}

SOL_TEST(flight_assistOnDampsToRestWithoutInput)
{
    ShipTuning tuning;
    ShipState state;
    state.velocity = {150.0, -40.0, 60.0};
    state.angularVelocity = {1.0f, -0.5f, 2.0f};

    step(state, tuning, FlightInput{}, 60 * 30);
    SOL_CHECK(speed(state) < 1e-3);
    SOL_CHECK(length(state.angularVelocity) < 1e-4f);
}

SOL_TEST(flight_assistOffIsRawNewtonian)
{
    ShipTuning tuning;
    ShipState state;
    FlightInput input;
    input.assist = false;
    input.linear = {0.0f, 0.0f, -1.0f};

    // v = a*t exactly (semi-implicit Euler with constant accel), no cap.
    step(state, tuning, input, 60 * 10);
    const double expected = static_cast<double>(tuning.forwardAccel) * 10.0;
    SOL_CHECK(std::abs(speed(state) - expected) < expected * 1e-6);
    SOL_CHECK(speed(state) > tuning.maxSpeed); // 600 m/s: the cap does not apply

    // Coasting: nothing damps.
    const DVec3 coastingVelocity = state.velocity;
    step(state, tuning, FlightInput{.assist = false}, 600);
    SOL_CHECK(state.velocity == coastingVelocity);
}

SOL_TEST(flight_assistOnTurnRateConvergesAndStops)
{
    ShipTuning tuning;
    ShipState state;
    FlightInput input;
    input.angular = {1.0f, 0.0f, 0.0f}; // full pitch up

    step(state, tuning, input, 60 * 5);
    SOL_CHECK(std::abs(state.angularVelocity.x - tuning.maxTurnRate.x) < 1e-3f);

    step(state, tuning, FlightInput{}, 60 * 5);
    SOL_CHECK(length(state.angularVelocity) < 1e-4f);
}

SOL_TEST(flight_yawRotatesForwardVector)
{
    ShipTuning tuning;
    ShipState state;
    // Drive the integrator directly: hold exactly quarter-turn-per-second yaw
    // for one second, bypassing the assist controller.
    state.angularVelocity = {0.0f, sol::core::kHalfPi, 0.0f};
    FlightInput input;
    input.assist = false;

    step(state, tuning, input, 60);

    // Positive yaw for a quarter turn takes forward (-Z) to left (-X).
    const Vec3 forward = rotate(state.orientation, {0.0f, 0.0f, -1.0f});
    SOL_CHECK(std::abs(forward.x + 1.0f) < 1e-2f);
    SOL_CHECK(std::abs(forward.y) < 1e-3f);
    SOL_CHECK(std::abs(forward.z) < 1e-2f);
    SOL_CHECK(std::abs(length(state.orientation) - 1.0f) < 1e-5f);
}

SOL_TEST(flight_cruiseScalesAssistCap)
{
    ShipTuning tuning;
    ShipState state;
    FlightInput input;
    input.linear = {0.0f, 0.0f, -1.0f};
    input.cruise = true;

    step(state, tuning, input, 60 * 60); // one sim-minute
    const double cap = static_cast<double>(tuning.maxSpeed) * tuning.cruiseSpeedScale;
    SOL_CHECK(speed(state) > cap * 0.5);
    SOL_CHECK(speed(state) <= cap * 1.001);

    // Dropping out of cruise re-applies the normal cap; the cruise drive
    // brakes back into the normal envelope within seconds, not hours.
    input.cruise = false;
    step(state, tuning, input, 60 * 30);
    SOL_CHECK(speed(state) <= tuning.maxSpeed + 1.0);
}

SOL_TEST(flight_boostRaisesCapWhileHeld)
{
    ShipTuning tuning;
    ShipState state;
    FlightInput input;
    input.linear = {0.0f, 0.0f, -1.0f};
    input.boost = true;

    step(state, tuning, input, 60 * 30);
    const double boostCap = static_cast<double>(tuning.maxSpeed) * tuning.boostSpeedScale;
    SOL_CHECK(std::abs(speed(state) - boostCap) < 1.0);

    input.boost = false;
    step(state, tuning, input, 60 * 30);
    SOL_CHECK(std::abs(speed(state) - tuning.maxSpeed) < 1.0);
}

SOL_TEST(flight_deterministicAcrossRuns)
{
    ShipTuning tuning;
    ShipState a;
    ShipState b;
    FlightInput input;
    input.linear = {0.3f, -0.2f, -1.0f};
    input.angular = {0.4f, 0.9f, -0.1f};

    step(a, tuning, input, 600);
    step(b, tuning, input, 600);
    SOL_CHECK(a.position == b.position);
    SOL_CHECK(a.velocity == b.velocity);
    SOL_CHECK(a.orientation == b.orientation);
    SOL_CHECK(a.angularVelocity == b.angularVelocity);
}

SOL_TEST(flight_orientationStaysNormalizedUnderTumble)
{
    ShipTuning tuning;
    ShipState state;
    FlightInput input;
    input.assist = false;
    input.angular = {1.0f, 1.0f, 1.0f};

    for (int i = 0; i < 60 * 60; ++i) {
        stepShipFlight(state, tuning, input, kDt);
    }
    SOL_CHECK(std::abs(length(state.orientation) - 1.0f) < 1e-4f);
}
