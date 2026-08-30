#include <cmath>

#include <sol/sim/weapons.hpp>
#include <sol/test/test.hpp>

using sol::core::DVec3;
using sol::sim::computeInterceptDirection;
using sol::sim::layWithinArc;
using sol::sim::segmentHitsSphere;

SOL_TEST(weapons_intercept_stationary_target_is_direct)
{
    DVec3 direction;
    SOL_CHECK(computeInterceptDirection({}, {}, {1000.0, 0.0, 0.0}, {}, 900.0, direction));
    SOL_CHECK(std::abs(direction.x - 1.0) < 1e-9);
}

SOL_TEST(weapons_intercept_leads_crossing_target)
{
    // Target 1000 m ahead crossing at 100 m/s; projectile 500 m/s. The
    // intercept point must actually be reached by both at the same time.
    DVec3 direction;
    const DVec3 targetPosition{0.0, 0.0, -1000.0};
    const DVec3 targetVelocity{100.0, 0.0, 0.0};
    SOL_CHECK(computeInterceptDirection({}, {}, targetPosition, targetVelocity, 500.0, direction));
    SOL_CHECK(direction.x > 0.0); // leads into the target's path

    // Verify the solution: find t where projectile and target coincide.
    // |d*500*t| == |targetPos + targetVel*t|
    const double t = 1000.0 / std::abs(direction.z * 500.0 / 1.0); // z closes at 500*|dz|
    const DVec3 projectileAt = direction * (500.0 * t);
    const DVec3 targetAt = targetPosition + targetVelocity * t;
    SOL_CHECK(length(projectileAt - targetAt) < 1.0);
}

SOL_TEST(weapons_intercept_shooter_velocity_inherited)
{
    // Shooter and target flying identically: shot behaves as if both were
    // stationary - direct line, guaranteed hit.
    DVec3 direction;
    const DVec3 velocity{0.0, 0.0, -200.0};
    SOL_CHECK(computeInterceptDirection({}, velocity, {0.0, 100.0, 0.0}, velocity, 400.0, direction));
    SOL_CHECK(std::abs(direction.y - 1.0) < 1e-9);
}

SOL_TEST(weapons_intercept_impossible_falls_back_to_direct)
{
    // Target receding at twice the projectile speed.
    DVec3 direction;
    SOL_CHECK(!computeInterceptDirection({}, {}, {0.0, 0.0, -1000.0}, {0.0, 0.0, -800.0}, 400.0, direction));
    SOL_CHECK(std::abs(direction.z + 1.0) < 1e-9); // direct line fallback
}

SOL_TEST(weapons_segment_sphere)
{
    double t = -1.0;
    // Straight through: enters at 40% of the way.
    SOL_CHECK(segmentHitsSphere({0.0, 0.0, 0.0}, {100.0, 0.0, 0.0}, {50.0, 0.0, 0.0}, 10.0, t));
    SOL_CHECK(std::abs(t - 0.4) < 1e-9);
    // Grazing miss.
    SOL_CHECK(!segmentHitsSphere({0.0, 11.0, 0.0}, {100.0, 11.0, 0.0}, {50.0, 0.0, 0.0}, 10.0, t));
    // Stops short of the sphere.
    SOL_CHECK(!segmentHitsSphere({0.0, 0.0, 0.0}, {30.0, 0.0, 0.0}, {50.0, 0.0, 0.0}, 10.0, t));
    // Starting inside reports t = 0.
    SOL_CHECK(segmentHitsSphere({48.0, 0.0, 0.0}, {100.0, 0.0, 0.0}, {50.0, 0.0, 0.0}, 10.0, t));
    SOL_CHECK(t == 0.0);
}

// --- Traverse (engine plan Phase 31 stage C2) -------------------------------
//
// The rest direction below is always -Z (a ship's nose) so that the angles in
// each case read off the page.

namespace {

[[nodiscard]] double angleBetween(const DVec3& a, const DVec3& b)
{
    return std::acos(sol::core::clamp(dot(normalize(a), normalize(b)), -1.0, 1.0)) *
           (180.0 / sol::core::kPiD);
}

constexpr DVec3 kNose{0.0, 0.0, -1.0};

} // namespace

// ⚑ THE ARC IS THE FULL CONE, NOT A HALF-ANGLE, and this is the case that
// pins it. A 90-degree ring reaches 45 degrees either side: something 40
// degrees off is inside it and something 50 degrees off is not. Read `arc` as
// a half-angle instead — the obvious alternative, and the one the def files
// would silently agree with — and both of these flip.
SOL_TEST(weapons_arc_is_the_full_cone_centred_on_the_rest_direction)
{
    DVec3 bearing;
    const double inside = 40.0 * (sol::core::kPiD / 180.0);
    const double outside = 50.0 * (sol::core::kPiD / 180.0);
    SOL_CHECK(layWithinArc(kNose, {std::sin(inside), 0.0, -std::cos(inside)}, 90.0, bearing));
    SOL_CHECK(!layWithinArc(kNose, {std::sin(outside), 0.0, -std::cos(outside)}, 90.0, bearing));
}

// A gun that bears points exactly where it was asked to, not somewhere near
// it: a ring inside its limits adds nothing of its own.
SOL_TEST(weapons_a_gun_that_bears_points_exactly_where_it_was_laid)
{
    DVec3 bearing;
    const DVec3 sought = normalize(DVec3{1.0, 0.5, -1.0});
    SOL_CHECK(layWithinArc(kNose, sought, 270.0, bearing));
    SOL_CHECK(angleBetween(bearing, sought) < 1e-9);
}

// ⚑ A RING THAT CANNOT REACH STILL TURNS AS FAR AS IT GOES, and it turns
// TOWARD what it was laid on rather than back to its rest. That is what leaves
// a turret already round the right way the moment the target crosses into its
// arc; returning to rest — the tidier-looking alternative — would make every
// gun start its swing from scratch.
SOL_TEST(weapons_a_gun_that_cannot_bear_stops_on_its_limit_facing_the_target)
{
    DVec3 bearing;
    // 120 degrees off the nose, against a 90-degree ring that reaches 45.
    const double sought = 120.0 * (sol::core::kPiD / 180.0);
    SOL_CHECK(!layWithinArc(kNose, {std::sin(sought), 0.0, -std::cos(sought)}, 90.0, bearing));
    SOL_CHECK(std::abs(angleBetween(bearing, kNose) - 45.0) < 1e-6);
    SOL_CHECK(bearing.x > 0.0);            // swung the way the target lies, not the other
    SOL_CHECK(std::abs(bearing.y) < 1e-9); // and stayed in the plane the two span
}

// The two ends of the range an author can write. Zero is a gun bolted down:
// it points where it is aimed and bears only on that. 360 has no stop at all.
SOL_TEST(weapons_a_zero_arc_is_bolted_down_and_a_full_arc_has_no_stop)
{
    DVec3 bearing;
    SOL_CHECK(layWithinArc(kNose, kNose, 0.0, bearing));
    SOL_CHECK(angleBetween(bearing, kNose) < 1e-9);
    SOL_CHECK(!layWithinArc(kNose, {1.0, 0.0, 0.0}, 0.0, bearing));
    SOL_CHECK(angleBetween(bearing, kNose) < 1e-9); // and it did not budge

    const DVec3 astern{0.0, 0.0, 1.0};
    SOL_CHECK(layWithinArc(kNose, astern, 360.0, bearing));
    SOL_CHECK(angleBetween(bearing, astern) < 1e-9);
}

// ⚑ THE ONE DEGENERATE CASE: a gun laid EXACTLY astern of its own rest, where
// every plane through the two directions is as good as every other and the
// cross product that normally picks one is zero. It must still come out on the
// limit rather than as a zero vector, because the caller divides by nothing
// and fires down whatever it is handed.
SOL_TEST(weapons_a_gun_laid_exactly_opposite_its_rest_still_reaches_its_limit)
{
    DVec3 bearing;
    SOL_CHECK(!layWithinArc(kNose, {0.0, 0.0, 1.0}, 100.0, bearing));
    SOL_CHECK(std::abs(length(bearing) - 1.0) < 1e-9);
    SOL_CHECK(std::abs(angleBetween(bearing, kNose) - 50.0) < 1e-6);
}
