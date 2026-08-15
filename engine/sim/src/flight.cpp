#include "sol/sim/flight.hpp"

namespace sol::sim {

namespace {

using core::Quat;
using core::Vec3;

// Per-axis linear acceleration limits for the current input direction.
[[nodiscard]] Vec3 accelLimits(const ShipTuning& tuning, Vec3 direction)
{
    return {
        tuning.lateralAccel,
        tuning.verticalAccel,
        direction.z < 0.0f ? tuning.forwardAccel : tuning.reverseAccel,
    };
}

[[nodiscard]] Vec3 clampPerAxis(Vec3 v, Vec3 limit)
{
    return {
        core::clamp(v.x, -limit.x, limit.x),
        core::clamp(v.y, -limit.y, limit.y),
        core::clamp(v.z, -limit.z, limit.z),
    };
}

// Acceleration that moves `current` toward `target` at most `limit` per axis,
// reaching it exactly when within reach this tick (dt-independent controller).
[[nodiscard]] Vec3 rateMatch(Vec3 current, Vec3 target, Vec3 limit, float dt)
{
    const Vec3 needed = (target - current) / dt;
    return clampPerAxis(needed, limit);
}

} // namespace

Vec3 shipLinearAccelBody(const ShipState& state, const ShipTuning& tuning, const FlightInput& input,
                         double dt)
{
    const float dtf = static_cast<float>(dt);
    if (!(dtf > 0.0f)) {
        return {};
    }
    const Vec3 command = clampPerAxis(input.linear, {1.0f, 1.0f, 1.0f});

    float accelScale = input.boost ? tuning.boostAccelScale : 1.0f;
    float speedScale = input.boost ? tuning.boostSpeedScale : 1.0f;
    if (input.assist && input.cruise) {
        accelScale = tuning.cruiseAccelScale;
        speedScale = tuning.cruiseSpeedScale;
    }

    if (!input.assist) {
        // Raw Newtonian: thrust is thrust; nothing damps, nothing caps.
        return command * (accelLimits(tuning, command) * accelScale);
    }

    // Assist: chase the commanded body-space velocity. Unused axes command
    // zero, which is the counter-thrust damping; the cap comes from the
    // commanded velocity never exceeding maxSpeed.
    const Vec3 velocityBody = rotate(conjugate(state.orientation), toVec3(state.velocity));
    const Vec3 targetVelocity = command * (tuning.maxSpeed * speedScale);

    // Cruise exit ("interruptible", GDD 4): while far above the normal
    // envelope, brake with the cruise drive, not the maneuvering thrusters —
    // otherwise shedding cruise speed would take hours.
    const float envelope = tuning.maxSpeed * speedScale * 2.0f;
    if (!input.cruise && length(velocityBody) > envelope) {
        accelScale = tuning.cruiseAccelScale;
    }

    const Vec3 limits = accelLimits(tuning, command) * accelScale;
    return rateMatch(velocityBody, targetVelocity, limits, dtf);
}

void stepShipFlight(ShipState& state, const ShipTuning& tuning, const FlightInput& input, double dt)
{
    const float dtf = static_cast<float>(dt);
    if (!(dtf > 0.0f)) {
        return;
    }

    // --- Angular ---
    const Vec3 angularCommand = clampPerAxis(input.angular, {1.0f, 1.0f, 1.0f});
    Vec3 angularAcceleration;
    if (input.assist) {
        const Vec3 targetRate = angularCommand * tuning.maxTurnRate;
        angularAcceleration = rateMatch(state.angularVelocity, targetRate, tuning.angularAccel, dtf);
    } else {
        angularAcceleration = angularCommand * tuning.angularAccel;
    }
    state.angularVelocity += angularAcceleration * dtf;

    // Integrate orientation: dq/dt = 0.5 * q * (omega_body, 0).
    const Vec3 omega = state.angularVelocity;
    const Quat omegaQuat = {omega.x, omega.y, omega.z, 0.0f};
    const Quat rate = state.orientation * omegaQuat;
    state.orientation = normalize(Quat{
        state.orientation.x + 0.5f * rate.x * dtf,
        state.orientation.y + 0.5f * rate.y * dtf,
        state.orientation.z + 0.5f * rate.z * dtf,
        state.orientation.w + 0.5f * rate.w * dtf,
    });

    // --- Linear (uses the post-integration orientation: semi-implicit) ---
    const Vec3 accelBody = shipLinearAccelBody(state, tuning, input, dt);
    const Vec3 accelWorld = rotate(state.orientation, accelBody);
    state.velocity += core::toDVec3(accelWorld) * dt;
    state.position += state.velocity * dt;
}

} // namespace sol::sim
