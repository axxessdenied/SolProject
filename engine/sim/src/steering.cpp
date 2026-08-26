#include "sol/sim/steering.hpp"

#include "sol/sim/weapons.hpp" // segmentHitsSphere: what is in the way, and how far

#include <cmath>

namespace sol::sim {

namespace {

using core::DVec3;
using core::Vec3;

constexpr float kTurnGain = 3.0f; // commanded rate per unit of aim offset

[[nodiscard]] DVec3 clampLength(DVec3 v, double maxLength)
{
    const double len = length(v);
    return len > maxLength && len > 0.0 ? v * (maxLength / len) : v;
}

} // namespace

FlightInput steerAimAndMove(const ShipState& state,
                            const ShipTuning& tuning,
                            const DVec3& aimPoint,
                            const DVec3& desiredVelocity)
{
    FlightInput input;

    const DVec3 toAim = aimPoint - state.position;
    if (dot(toAim, toAim) > 1.0e-9) {
        const Vec3 aimBody = rotate(conjugate(state.orientation), toVec3(normalize(toAim)));
        if (aimBody.z < 0.0f) {
            // Ahead: proportional pitch/yaw toward the boresight.
            input.angular.x = core::clamp(kTurnGain * aimBody.y, -1.0f, 1.0f);
            input.angular.y = core::clamp(kTurnGain * -aimBody.x, -1.0f, 1.0f);
        } else {
            // Behind: full-rate turn on whichever axes bring it around.
            input.angular.x = aimBody.y >= 0.0f ? 1.0f : -1.0f;
            input.angular.y = aimBody.x <= 0.0f ? 1.0f : -1.0f;
        }
    }

    // Assist semantics: linear input commands a body-frame velocity fraction.
    const Vec3 velocityBody =
        rotate(conjugate(state.orientation),
               toVec3(clampLength(desiredVelocity, static_cast<double>(tuning.maxSpeed))));
    if (tuning.maxSpeed > 0.0f) {
        input.linear = {
            core::clamp(velocityBody.x / tuning.maxSpeed, -1.0f, 1.0f),
            core::clamp(velocityBody.y / tuning.maxSpeed, -1.0f, 1.0f),
            core::clamp(velocityBody.z / tuning.maxSpeed, -1.0f, 1.0f),
        };
    }
    return input;
}

FlightInput steerPursue(const ShipState& state,
                        const ShipTuning& tuning,
                        const DVec3& targetPosition,
                        const DVec3& targetVelocity,
                        double desiredRange)
{
    const DVec3 toTarget = targetPosition - state.position;
    const double distance = length(toTarget);
    const DVec3 direction = distance > 1.0e-6 ? toTarget * (1.0 / distance) : DVec3{0.0, 0.0, -1.0};

    // Close (or back off) at a rate proportional to the range error; the
    // clamp inside steerAimAndMove keeps it within the assist envelope.
    const double closingSpeed = (distance - desiredRange) * 0.5;
    const DVec3 desiredVelocity = targetVelocity + direction * closingSpeed;
    return steerAimAndMove(state, tuning, targetPosition, desiredVelocity);
}

double brakingSpeedLimit(const ShipTuning& tuning, double distance)
{
    if (!(distance > 0.0)) {
        return 0.0;
    }
    // Half the available deceleration (margin for the controller lag), two
    // regimes to mirror flight.cpp's cruise exit: above the normal envelope
    // the cruise drive brakes (reverseAccel * cruiseAccelScale), below it the
    // thrusters do.
    const double envelopeSpeed = static_cast<double>(tuning.maxSpeed) * 2.0;
    const double normalBrake = 0.5 * tuning.reverseAccel;
    if (!(normalBrake > 0.0)) {
        return 0.0; // a ship that cannot brake may not choose to go fast
    }
    const double cruiseBrake = normalBrake * tuning.cruiseAccelScale;
    const double envelopeDistance = envelopeSpeed * envelopeSpeed / (2.0 * normalBrake);
    if (distance <= envelopeDistance) {
        return std::sqrt(2.0 * normalBrake * distance);
    }
    return std::sqrt(envelopeSpeed * envelopeSpeed + 2.0 * cruiseBrake * (distance - envelopeDistance));
}

double pathBlockedAt(const DVec3& from,
                     const DVec3& to,
                     double clearance,
                     std::span<const AvoidanceSphere> obstacles,
                     std::uint32_t ignore)
{
    const DVec3 lane = to - from;
    const double laneLength = core::length(lane);
    if (!(laneLength > 0.0)) {
        return -1.0;
    }
    double nearest = -1.0;
    for (const AvoidanceSphere& obstacle : obstacles) {
        if (obstacle.handle == ignore && ignore != kNoAvoidHandle) {
            continue;
        }
        double t = 0.0;
        if (!segmentHitsSphere(
                from, to, obstacle.position, obstacle.radius + (clearance > 0.0 ? clearance : 0.0), t)) {
            continue;
        }
        const double distance = t * laneLength;
        if (nearest < 0.0 || distance < nearest) {
            nearest = distance;
        }
    }
    return nearest;
}

FlightInput steerTravel(const ShipState& state,
                        const ShipTuning& tuning,
                        const DVec3& targetPosition,
                        const DVec3& targetVelocity,
                        double arrivalRange,
                        std::span<const AvoidanceSphere> obstacles,
                        std::uint32_t selfHandle)
{
    const DVec3 toTarget = targetPosition - state.position;
    const double distance = length(toTarget);
    const double remaining = distance - arrivalRange;
    if (remaining <= 0.0) {
        // Inside the arrival bubble: match the target's velocity, nose on it.
        return steerAimAndMove(state, tuning, targetPosition, targetVelocity);
    }
    const DVec3 direction = toTarget * (1.0 / distance);

    // Braking-limited speed profile (brakingSpeedLimit), which is the same
    // curve Phase 8y asks about an obstacle.
    double desiredSpeed = brakingSpeedLimit(tuning, remaining);
    const double cruiseMaxSpeed = static_cast<double>(tuning.maxSpeed) * tuning.cruiseSpeedScale;
    desiredSpeed = std::min(desiredSpeed, cruiseMaxSpeed);

    // ⚑ And the same question about whatever is in the way (Phase 8y). A
    // blocked path is answered with SPEED, never with steering: at cruise the
    // ship crosses a station or another hull entirely within one tick, so the
    // lateral nudge below has no time to act. Capping here means the cruise
    // gate a few lines down turns itself off — the refusal falls out of the
    // limit rather than being a second rule that could disagree with it — and
    // once sub-cruise, avoidObstacles gets its chance to steer around.
    const double blockedAt =
        pathBlockedAt(state.position, targetPosition, kPathClearance, obstacles, selfHandle);
    if (blockedAt >= 0.0) {
        desiredSpeed = std::min(desiredSpeed, brakingSpeedLimit(tuning, blockedAt));
    }

    // Cruise only once the nose is roughly on target; until then close at
    // normal speed while the turn completes.
    bool cruise = desiredSpeed > static_cast<double>(tuning.maxSpeed);
    if (cruise && aimError(state, targetPosition) > 0.35) {
        cruise = false;
        desiredSpeed = static_cast<double>(tuning.maxSpeed);
    }

    DVec3 desiredVelocity = targetVelocity + direction * desiredSpeed;
    if (!cruise) {
        // The ship's own sphere needs no exclusion here: avoidObstacles skips
        // anything it is already inside, and a ship is always inside itself.
        avoidObstacles(desiredVelocity, state, obstacles, 6.0);
    }

    FlightInput input = steerAimAndMove(state, tuning, targetPosition, desiredVelocity);
    if (cruise) {
        // Re-derive the velocity command against the cruise envelope;
        // steerAimAndMove clamps to the normal one.
        const Vec3 velocityBody =
            rotate(conjugate(state.orientation), toVec3(clampLength(desiredVelocity, cruiseMaxSpeed)));
        const float scale = tuning.maxSpeed * tuning.cruiseSpeedScale;
        input.linear = {
            core::clamp(velocityBody.x / scale, -1.0f, 1.0f),
            core::clamp(velocityBody.y / scale, -1.0f, 1.0f),
            core::clamp(velocityBody.z / scale, -1.0f, 1.0f),
        };
        input.cruise = true;
    }
    return input;
}

FlightInput
steerEvade(const ShipState& state, const ShipTuning& tuning, const DVec3& threatPosition, double weavePhase)
{
    const DVec3 fromThreat = state.position - threatPosition;
    const double distance = length(fromThreat);
    const Vec3 away = distance > 1.0e-6 ? toVec3(fromThreat * (1.0 / distance)) : Vec3{0.0f, 0.0f, -1.0f};

    // Weave on an axis perpendicular to the escape line.
    Vec3 perpendicular = cross(away, Vec3{0.0f, 1.0f, 0.0f});
    if (lengthSquared(perpendicular) < 1.0e-6f) {
        perpendicular = cross(away, Vec3{1.0f, 0.0f, 0.0f});
    }
    perpendicular = normalize(perpendicular);

    const Vec3 escape = normalize(away + perpendicular * (0.6f * static_cast<float>(std::sin(weavePhase))));
    const DVec3 escapeD = core::toDVec3(escape);
    const DVec3 desiredVelocity = escapeD * static_cast<double>(tuning.maxSpeed);

    FlightInput input = steerAimAndMove(state, tuning, state.position + escapeD * 2'000.0, desiredVelocity);
    input.boost = distance < 600.0;
    return input;
}

FlightInput steerFormation(const ShipState& state,
                           const ShipTuning& tuning,
                           const DVec3& anchorPosition,
                           const DVec3& anchorVelocity,
                           const DVec3& worldOffset)
{
    const DVec3 slot = anchorPosition + worldOffset;
    const DVec3 desiredVelocity = anchorVelocity + (slot - state.position) * 0.5;
    // Look where the formation is going once settled, at the slot until then.
    const DVec3 aimPoint = slot + anchorVelocity * 2.0;
    return steerAimAndMove(state, tuning, aimPoint, desiredVelocity);
}

void avoidObstacles(DVec3& desiredVelocity,
                    const ShipState& state,
                    std::span<const AvoidanceSphere> obstacles,
                    double lookaheadSeconds)
{
    const DVec3 velocity = state.velocity;
    const double speedSquared = dot(velocity, velocity);
    const double strength = length(desiredVelocity);
    if (strength <= 0.0) {
        return;
    }

    for (const AvoidanceSphere& obstacle : obstacles) {
        const DVec3 toObstacle = obstacle.position - state.position;
        double closestTime = 0.0;
        if (speedSquared > 1.0e-9) {
            closestTime = core::clamp(dot(toObstacle, velocity) / speedSquared, 0.0, lookaheadSeconds);
        }
        const DVec3 closestPoint = velocity * closestTime - toObstacle; // from center to path
        const double clearance = length(closestPoint);
        const double dangerRadius = obstacle.radius * 1.5 + 100.0;
        if (clearance >= dangerRadius || length(toObstacle) <= obstacle.radius) {
            continue;
        }
        const DVec3 pushDirection = clearance > 1.0e-6 ? closestPoint * (1.0 / clearance)
                                                       : DVec3{0.0, 1.0, 0.0}; // dead-center: pick a side
        const double urgency = 1.0 - clearance / dangerRadius;
        desiredVelocity += pushDirection * (strength * urgency * 1.5);
    }
}

double aimError(const ShipState& state, const DVec3& point)
{
    const DVec3 toPoint = point - state.position;
    if (dot(toPoint, toPoint) < 1.0e-9) {
        return 0.0;
    }
    const Vec3 forward = rotate(state.orientation, Vec3{0.0f, 0.0f, -1.0f});
    const DVec3 forwardD = core::toDVec3(forward);
    const double cosine = dot(forwardD, normalize(toPoint));
    return std::acos(core::clamp(cosine, -1.0, 1.0));
}

} // namespace sol::sim
