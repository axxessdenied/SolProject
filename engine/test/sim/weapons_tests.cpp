#include <cmath>

#include <sol/sim/weapons.hpp>
#include <sol/test/test.hpp>

using sol::core::DVec3;
using sol::sim::computeInterceptDirection;
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
