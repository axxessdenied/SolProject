#pragma once

// Weapon math (engine plan Phase 6): the projectile intercept (lead)
// solution and a swept hit query. Firing state and projectile entities live
// game-side; this is the deterministic math under them.

#include "sol/core/math/math.hpp"

namespace sol::sim {

// First-intercept fire direction for a projectile of the given speed
// (relative to the shooter, inheriting shooter velocity) against a target
// with constant velocity. False when no intercept exists (target outruns the
// projectile); outDirection is then the direct line to the target.
[[nodiscard]] bool computeInterceptDirection(const core::DVec3& shooterPosition,
                                             const core::DVec3& shooterVelocity,
                                             const core::DVec3& targetPosition,
                                             const core::DVec3& targetVelocity,
                                             double projectileSpeed, core::DVec3& outDirection);

// Earliest t in [0,1] where the segment from->to enters the sphere; false on
// a miss. A segment starting inside reports t = 0.
[[nodiscard]] bool segmentHitsSphere(const core::DVec3& from, const core::DVec3& to,
                                     const core::DVec3& center, double radius, double& outT);

} // namespace sol::sim
