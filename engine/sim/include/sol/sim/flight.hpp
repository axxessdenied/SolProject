#pragma once

#include "sol/core/math/math.hpp"

namespace sol::sim {

// Newtonian 6-DoF ship flight with layered assists (engine plan 2.7):
// raw force/torque integration underneath; assist-on flies the ship toward the
// commanded body-space velocity / turn rate so it handles "atmospherically"
// while staying momentum-aware. Deterministic: pure function of state,
// tuning, input, and dt.
//
// Body axes follow the engine convention (scalar.hpp): +X right, +Y up,
// -Z forward. Angular input/velocity are body-space rates about those axes
// (positive pitch = nose up, positive yaw = nose left, positive roll = right
// wing down).

struct ShipTuning
{
    // Max linear acceleration (m/s^2) per body axis; reverse/lateral/vertical
    // are usually weaker than main drive.
    float forwardAccel = 60.0f;
    float reverseAccel = 40.0f;
    float lateralAccel = 30.0f;
    float verticalAccel = 30.0f;

    // Assist-on soft speed cap (m/s) and commanded turn rates (rad/s).
    float maxSpeed = 220.0f;
    core::Vec3 maxTurnRate = {1.6f, 1.2f, 2.6f}; // pitch, yaw, roll

    // Max angular acceleration (rad/s^2) per body axis.
    core::Vec3 angularAccel = {6.0f, 4.5f, 9.0f};

    // Boost (held): scales linear acceleration and the assist cap.
    float boostAccelScale = 3.0f;
    float boostSpeedScale = 1.75f;

    // Cruise (superlight throttle mode, GDD 4): scales the assist cap and
    // linear acceleration so system-scale distances are coverable. Only
    // meaningful with assist on; ignored when assist is off.
    float cruiseSpeedScale = 25'000.0f; // 220 m/s -> 5,500 km/s
    float cruiseAccelScale = 12'000.0f;
};

struct ShipState
{
    core::DVec3 position;                              // sim space, meters
    core::DVec3 velocity;                              // sim space, m/s
    core::Quat orientation = core::Quat::identity();   // body -> sim space
    core::Vec3 angularVelocity;                        // body space, rad/s
};

struct FlightInput
{
    // Commanded thrust per body axis in [-1, 1]; z = -1 is full forward.
    core::Vec3 linear;
    // Commanded rotation rate per body axis in [-1, 1] (pitch, yaw, roll).
    core::Vec3 angular;
    bool assist = true;
    bool boost = false;
    bool cruise = false;
    // Weapon trigger; ignored by the flight model, consumed by the game's
    // weapon pass (one input struct serves player and NPC pilots alike).
    bool trigger = false;
};

// Advances one fixed sim tick of semi-implicit Euler. Orientation is
// re-normalized every step.
void stepShipFlight(ShipState& state, const ShipTuning& tuning, const FlightInput& input, double dt);

// The instantaneous linear acceleration (body space, m/s^2) a step with this
// state+input+dt applies; exposed so thruster effects and the HUD can mirror
// what the flight model actually does.
[[nodiscard]] core::Vec3 shipLinearAccelBody(const ShipState& state, const ShipTuning& tuning,
                                             const FlightInput& input, double dt);

} // namespace sol::sim
