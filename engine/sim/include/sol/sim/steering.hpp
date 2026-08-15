#pragma once

// NPC steering behaviors (engine plan Phase 6): pure functions from ship
// state + a geometric goal to a FlightInput, built on the assist flight
// model (angular input commands turn rates; linear input commands body
// velocity). Lua state machines pick the behavior; these fly it.

#include "sol/sim/flight.hpp"

#include <span>

namespace sol::sim {

struct AvoidanceSphere
{
    core::DVec3 position;
    double radius = 0.0;
};

// Core primitive: point the nose at aimPoint while commanding the given
// world-space velocity (clamped per body axis by the assist envelope).
[[nodiscard]] FlightInput steerAimAndMove(const ShipState& state, const ShipTuning& tuning,
                                          const core::DVec3& aimPoint,
                                          const core::DVec3& desiredVelocity);

// Chase a moving target, settling at desiredRange (meters).
[[nodiscard]] FlightInput steerPursue(const ShipState& state, const ShipTuning& tuning,
                                      const core::DVec3& targetPosition,
                                      const core::DVec3& targetVelocity, double desiredRange);

// Run from a threat, weaving; weavePhase advances with sim time (rad).
// Boosts while the threat is close.
[[nodiscard]] FlightInput steerEvade(const ShipState& state, const ShipTuning& tuning,
                                     const core::DVec3& threatPosition, double weavePhase);

// Hold a world-space offset from a moving anchor, matching its velocity.
[[nodiscard]] FlightInput steerFormation(const ShipState& state, const ShipTuning& tuning,
                                         const core::DVec3& anchorPosition,
                                         const core::DVec3& anchorVelocity,
                                         const core::DVec3& worldOffset);

// Deflects desiredVelocity away from any obstacle the current velocity would
// carry the ship near within lookahead seconds. Apply before steerAimAndMove.
void avoidObstacles(core::DVec3& desiredVelocity, const ShipState& state,
                    std::span<const AvoidanceSphere> obstacles, double lookaheadSeconds);

// Angle (radians) between the nose and the direction to point; weapon gate.
[[nodiscard]] double aimError(const ShipState& state, const core::DVec3& point);

} // namespace sol::sim
