#include <cmath>

#include <sol/sim/docking.hpp>
#include <sol/test/test.hpp>

using sol::core::DVec3;
using sol::sim::berthPoint;
using sol::sim::inBerth;
using sol::sim::kBerthApproachSpeed;
using sol::sim::kBerthCaptureRadius;
using sol::sim::kBerthCount;
using sol::sim::kBerthRingRadius;

namespace {

// What the game puts around a station, restated here so the test fails if
// either side moves: a 100 m collision sphere the player bounces off and a
// 130 m avoidance sphere the autopilot and NPC steering both dodge
// (space_world.hpp kStationRadiusMeters, space_world.cpp modelBaseRadius).
constexpr double kHullRadius = 100.0;
constexpr double kAvoidanceRadius = 130.0;

} // namespace

SOL_TEST(docking_berth_capture_sphere_clears_the_avoidance_sphere)
{
    // The load-bearing assertion of the whole geometry. If the capture sphere
    // reached inside the avoidance sphere, autopilot would be steering away
    // from the point it was told to arrive at, and the player would be pushed
    // off the pad by the hull it is attached to.
    SOL_CHECK(kBerthRingRadius - kBerthCaptureRadius > kAvoidanceRadius);
    SOL_CHECK(kBerthRingRadius - kBerthCaptureRadius > kHullRadius);

    const DVec3 station{1.0e8, -4.0e7, 2.5e8};
    for (std::uint32_t berth = 0; berth < kBerthCount; ++berth) {
        const DVec3 point = berthPoint(station, berth);
        const double nearestApproach = length(point - station) - kBerthCaptureRadius;
        SOL_CHECK(nearestApproach > kAvoidanceRadius);
    }
}

SOL_TEST(docking_berths_ring_the_station_evenly)
{
    const DVec3 station{0.0, 0.0, 0.0};
    for (std::uint32_t berth = 0; berth < kBerthCount; ++berth) {
        const DVec3 point = berthPoint(station, berth);
        // On the ring, and level with the habitat ring rather than above it.
        SOL_CHECK(std::fabs(length(point - station) - kBerthRingRadius) < 1.0e-9);
        SOL_CHECK(std::fabs(point.y) < 1.0e-9);
    }
    // Evenly spaced: no two berths share a point, and adjacent ones are the
    // same distance apart the whole way round.
    const double firstGap = length(berthPoint(station, 1) - berthPoint(station, 0));
    SOL_CHECK(firstGap > 2.0 * kBerthCaptureRadius); // and they do not overlap
    for (std::uint32_t berth = 0; berth < kBerthCount; ++berth) {
        const double gap = length(berthPoint(station, berth + 1) - berthPoint(station, berth));
        SOL_CHECK(std::fabs(gap - firstGap) < 1.0e-6);
    }
}

SOL_TEST(docking_berth_index_wraps_and_is_station_relative)
{
    const DVec3 origin{};
    const DVec3 elsewhere{-9.0e7, 3.0e6, 1.0e8};
    for (std::uint32_t berth = 0; berth < kBerthCount; ++berth) {
        // Wrapping is what keeps a stale berth index from indexing off the
        // ring; berth N and berth N + count are the same port.
        SOL_CHECK(length(berthPoint(origin, berth) - berthPoint(origin, berth + kBerthCount)) < 1.0e-9);
        // The same berth of a station far from the origin is the same offset.
        const DVec3 near = berthPoint(origin, berth);
        const DVec3 far = berthPoint(elsewhere, berth);
        SOL_CHECK(length((far - elsewhere) - near) < 1.0e-6);
    }
}

SOL_TEST(docking_in_berth_needs_both_distance_and_speed)
{
    const DVec3 station{5.0e7, 0.0, 5.0e7};
    const DVec3 berth = berthPoint(station, 2);

    SOL_CHECK(inBerth(berth, 0.0, berth));
    SOL_CHECK(inBerth(berth, kBerthApproachSpeed, berth)); // the gate is inclusive

    // Parked in the middle of it but crossing at cruise: not docked. Distance
    // alone would let a ship dock by flying through the port at full speed.
    SOL_CHECK(!inBerth(berth, kBerthApproachSpeed + 1.0, berth));

    // Stationary, but outside the capture sphere.
    const DVec3 justOutside = berth + DVec3{kBerthCaptureRadius + 0.5, 0.0, 0.0};
    SOL_CHECK(!inBerth(justOutside, 0.0, berth));
    const DVec3 justInside = berth + DVec3{kBerthCaptureRadius - 0.5, 0.0, 0.0};
    SOL_CHECK(inBerth(justInside, 0.0, berth));

    // And a berth does not capture a ship sitting on the station's hull, which
    // is the failure the ring radius exists to prevent.
    SOL_CHECK(!inBerth(station, 0.0, berth));
}
