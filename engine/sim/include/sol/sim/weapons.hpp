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
                                             double projectileSpeed,
                                             core::DVec3& outDirection);

// Lay a gun on a direction, within the traverse its mount allows (engine plan
// Phase 31 stage C2). Pure geometry: the caller supplies both directions in
// the same frame and gets back the one the gun actually points along.
//
// `rest` is where the mount points when nothing has laid it - its authored
// `aim`, rotated into the world. `sought` is where the gunner wants it.
//
// ⚑ `arcDegrees` IS THE FULL CONE ANGLE CENTRED ON `rest`, not a half-angle.
// A `270` turret therefore reaches 135 degrees either side of its aim, which
// is what makes a dorsal ring able to fire forward, aft and to both beams and
// blind only through the hull it is bolted to. `0` is a gun bolted down and
// `360` is a gun with no stop at all.
//
// Returns whether `sought` was INSIDE the ring. `outBearing` is set either
// way, and on a refusal it is the direction on the STOP nearest what was
// sought: a ring that cannot reach still turns as far as it goes, which is
// both what a real mount does and what leaves the gun already round the right
// way the moment the target crosses into its arc.
[[nodiscard]] bool
layWithinArc(const core::DVec3& rest, const core::DVec3& sought, double arcDegrees, core::DVec3& outBearing);

// Earliest t in [0,1] where the segment from->to enters the sphere; false on
// a miss. A segment starting inside reports t = 0.
[[nodiscard]] bool segmentHitsSphere(
    const core::DVec3& from, const core::DVec3& to, const core::DVec3& center, double radius, double& outT);

} // namespace sol::sim
