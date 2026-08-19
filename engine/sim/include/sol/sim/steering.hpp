#pragma once

// NPC steering behaviors (engine plan Phase 6): pure functions from ship
// state + a geometric goal to a FlightInput, built on the assist flight
// model (angular input commands turn rates; linear input commands body
// velocity). Lua state machines pick the behavior; these fly it.

#include "sol/sim/flight.hpp"

#include <cstdint>
#include <span>

namespace sol::sim {

inline constexpr std::uint32_t kNoAvoidHandle = 0xffff'ffffu;

struct AvoidanceSphere
{
    core::DVec3 position;
    double radius = 0.0;
    // The caller's own handle for this thing (the game passes an entity
    // index), carried through untouched — the trick predation.hpp's
    // PreyCandidate already uses, so nothing in here has to know what an
    // entity is. It exists because a ship must not avoid ITSELF, and a hunter
    // must not avoid the ship it is attacking (engine plan Phase 8y).
    std::uint32_t handle = kNoAvoidHandle;
};

// The fastest this ship may be going with `distance` of room ahead of it, on
// the two-regime profile steerTravel already arrives with: above the normal
// envelope the cruise drive brakes, below it the thrusters do.
//
// ⚑ Extracted rather than written (Phase 8y). steerTravel has computed exactly
// this since Phase 6 to arrive at a destination; the whole of 8y's speed rule
// is that same curve asked a second question — "what can I stop before?" —
// about an obstacle instead of about the target. Two callers of one function
// cannot drift into two subtly different deceleration curves, which is what a
// second hand-written profile would have become.
[[nodiscard]] double brakingSpeedLimit(const ShipTuning& tuning, double distance);

// How much room a ship leaves between its own skin and anything it passes.
// Wider than any hull in the game (the 4x freighter is 32 m) and the same
// +100 m avoidObstacles has always added to its danger radius, so the two
// halves of "do not hit that" agree about how close is too close.
inline constexpr double kPathClearance = 100.0;

// How far along `from -> to` this ship first touches something, or a negative
// value when the whole segment is clear. `clearance` is added to every sphere
// (the ship has a hull, and wants room besides), and `ignore` drops one sphere
// by handle — which is always the querying ship itself, since a ship that
// avoids its own body never moves again.
//
// ⚑ This is the query the game has never had. Avoidance is a lateral nudge
// (avoidObstacles below) and at cruise speed a nudge cannot act: a shuttle
// cruises at 5.5e6 m/s, so one 145 Hz tick covers ~38 km while a station is
// 130 m across and a ship is 8 — the obstacle is crossed hundreds of times
// over inside the tick that would have steered around it. The only thing that
// answers a blocked path at that speed is not being at that speed, so this
// reports a DISTANCE and the caller turns it into a speed limit.
[[nodiscard]] double pathBlockedAt(const core::DVec3& from, const core::DVec3& to,
                                   double clearance, std::span<const AvoidanceSphere> obstacles,
                                   std::uint32_t ignore = kNoAvoidHandle);

// Core primitive: point the nose at aimPoint while commanding the given
// world-space velocity (clamped per body axis by the assist envelope).
[[nodiscard]] FlightInput steerAimAndMove(const ShipState& state, const ShipTuning& tuning,
                                          const core::DVec3& aimPoint,
                                          const core::DVec3& desiredVelocity);

// Chase a moving target, settling at desiredRange (meters).
[[nodiscard]] FlightInput steerPursue(const ShipState& state, const ShipTuning& tuning,
                                      const core::DVec3& targetPosition,
                                      const core::DVec3& targetVelocity, double desiredRange);

// Long-haul travel (player autopilot): fly to targetPosition and arrive
// velocity-matched at arrivalRange meters from it. Commands the cruise drive
// while the remaining distance affords braking (a two-regime speed profile:
// the cruise drive sheds speed down to the normal envelope, maneuvering
// thrusters finish the stop — mirroring the flight model's interruptible
// cruise), and holds sub-cruise speed until the nose is roughly on target.
// Obstacles deflect the sub-cruise approach only; at cruise speeds a tick
// crosses them entirely.
// `selfHandle` is the caller's handle for the ship being flown, so the path
// query below can skip its own body (Phase 8y). Leaving it unset is loud
// rather than quiet: a ship that finds itself in the way parks immediately.
[[nodiscard]] FlightInput steerTravel(const ShipState& state, const ShipTuning& tuning,
                                      const core::DVec3& targetPosition,
                                      const core::DVec3& targetVelocity, double arrivalRange,
                                      std::span<const AvoidanceSphere> obstacles = {},
                                      std::uint32_t selfHandle = kNoAvoidHandle);

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
