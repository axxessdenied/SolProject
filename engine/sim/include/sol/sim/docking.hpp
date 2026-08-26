#pragma once

// Docking berth geometry (engine plan Phase 8r). Where a station's ports are,
// how close and how slow you have to be to be in one, and — the part that
// actually constrains the numbers — how far they must sit from the station so
// that flying to one is not flying into it.
//
// Pure and header-only so the geometry can be asserted headlessly, the same
// reason sol/ui/map_projection.hpp holds the map's fog rule: a berth that
// overlaps the hull is a bug you want a test to find, not a drive.
//
// Berths are DERIVED, never stored. StationSpec carries a position and that is
// enough to answer every question below, so nothing here is generated, cached
// or serialised.

#include "sol/core/math/math.hpp"

#include <cmath>
#include <cstdint>

namespace sol::sim {

// How many berths a station has. Uniform for now: a count only means something
// once berths can be occupied, and nothing in the sim docks an NPC.
inline constexpr std::uint32_t kBerthCount = 4;

// The three numbers that have to agree, and why they are what they are.
//
// The game draws a station as a torus of major radius 90 m and tube 12 m, so
// its outer edge is at 102 m. Around that sit two spheres: a 100 m collision
// sphere the player physically bounces off, and a 130 m avoidance sphere that
// NPC steering and the autopilot both dodge. A berth on the ring would be
// inside both — the autopilot would steer away from the place it was told to
// go, and the player would be shoved off the pad by the hull.
//
// So the ring of berths sits at 200 m and a berth is captured within 60 m of
// its centre, which puts the capture sphere between 140 m and 260 m out. The
// 10 m of daylight between 140 and the 130 m avoidance sphere is the whole
// point of these two numbers, and docking_tests asserts it rather than
// trusting this comment.
inline constexpr double kBerthRingRadius = 200.0;
inline constexpr double kBerthCaptureRadius = 60.0;

// Arrive slow or fly through. The shuttle tops out at 220 m/s and the
// freighter at 120, so this is a real gate and not a formality.
inline constexpr double kBerthApproachSpeed = 50.0;

// Berth `berth` of a station centred at `station`, evenly spaced around its
// equator. The habitat ring lies in XZ (the station's masts and solar panels
// are what run along Y), so berths sit level with it and read as ports off the
// ring rather than as points above the roof.
[[nodiscard]] inline core::DVec3 berthPoint(const core::DVec3& station, std::uint32_t berth)
{
    const double angle =
        6.283185307179586 * static_cast<double>(berth % kBerthCount) / static_cast<double>(kBerthCount);
    return station + core::DVec3{std::cos(angle) * kBerthRingRadius, 0.0, std::sin(angle) * kBerthRingRadius};
}

// Parked: inside the capture sphere and slow enough to be stopping rather than
// passing through. Both halves matter — distance alone would dock a ship that
// crossed the berth at cruise speed.
[[nodiscard]] inline bool
inBerth(const core::DVec3& ship, double speedMetersPerSecond, const core::DVec3& berth)
{
    return length(ship - berth) <= kBerthCaptureRadius && speedMetersPerSecond <= kBerthApproachSpeed;
}

} // namespace sol::sim
