#include <sol/sim/collision.hpp>

#include <sol/test/test.hpp>

#include <cmath>
#include <vector>

using sol::core::DVec3;
using sol::sim::CollisionBody;
using sol::sim::Contact;
using sol::sim::resolveCollisions;

namespace {

// A ship that moved from 'from' to 'to' this tick with matching velocity.
CollisionBody mover(DVec3 from, DVec3 to, double radius, double inverseMass, double dt = 1.0 / 60.0)
{
    return {
        .previousPosition = from,
        .position = to,
        .velocity = (to - from) * (1.0 / dt),
        .radius = radius,
        .inverseMass = inverseMass,
    };
}

CollisionBody fixed(DVec3 at, double radius)
{
    return {.previousPosition = at, .position = at, .velocity = {}, .radius = radius,
            .inverseMass = 0.0};
}

} // namespace

SOL_TEST(collision_fast_mover_does_not_tunnel)
{
    // 91 km in one tick (cruise speed) from 50 km altitude: the naive overlap
    // test would land the ship 41 km inside the planet.
    const double startAltitude = 6.371e6 + 8.0 + 5.0e4;
    std::vector<CollisionBody> bodies = {
        mover({0.0, 0.0, startAltitude}, {0.0, 0.0, startAltitude - 9.1e4}, 8.0, 1.0),
        fixed({}, 6.371e6),
    };
    std::vector<Contact> contacts;
    resolveCollisions(bodies, 0.2, contacts);

    SOL_CHECK(contacts.size() == 1);
    const double altitude = length(bodies[0].position) - 6.371e6 - 8.0;
    SOL_CHECK(altitude >= 0.0);
    SOL_CHECK(altitude < 1.0); // parked at the surface, not bounced into space...
    SOL_CHECK(bodies[0].velocity.z >= 0.0); // ...and no longer moving inward
    SOL_CHECK(bodies[1].position == DVec3{}); // immovable stayed put
}

SOL_TEST(collision_miss_reports_nothing)
{
    std::vector<CollisionBody> bodies = {
        mover({-1000.0, 30.0, 0.0}, {1000.0, 30.0, 0.0}, 5.0, 1.0), // passes 30 m above
        fixed({}, 20.0),
    };
    std::vector<Contact> contacts;
    resolveCollisions(bodies, 0.2, contacts);
    SOL_CHECK(contacts.empty());
    SOL_CHECK(bodies[0].position == DVec3{1000.0, 30.0, 0.0});
}

SOL_TEST(collision_head_on_equal_masses)
{
    // Two equal ships (combined radius 10) starting 12 m apart, each closing
    // at 100 m/s: they meet mid-tick; restitution 0 kills the closing
    // velocity symmetrically.
    const double dt = 1.0 / 60.0;
    std::vector<CollisionBody> bodies = {
        mover({-6.0, 0.0, 0.0}, {-6.0 + 100.0 * dt, 0.0, 0.0}, 5.0, 1.0, dt),
        mover({6.0, 0.0, 0.0}, {6.0 - 100.0 * dt, 0.0, 0.0}, 5.0, 1.0, dt),
    };
    std::vector<Contact> contacts;
    resolveCollisions(bodies, 0.0, contacts);

    SOL_CHECK(contacts.size() == 1);
    if (contacts.empty()) {
        return;
    }
    SOL_CHECK(std::abs(contacts[0].impactSpeed - 200.0) < 1.0e-6);
    // Momentum conserved (was zero) and closing motion gone.
    SOL_CHECK(std::abs(bodies[0].velocity.x + bodies[1].velocity.x) < 1.0e-9);
    SOL_CHECK(std::abs(bodies[0].velocity.x) < 1.0e-9);
    // Separated to at least touching.
    SOL_CHECK(length(bodies[0].position - bodies[1].position) >= 10.0);
}

SOL_TEST(collision_restitution_bounces)
{
    const double dt = 1.0 / 60.0;
    std::vector<CollisionBody> bodies = {
        mover({0.0, 12.0, 0.0}, {0.0, 12.0 - 60.0 * dt, 0.0}, 2.0, 1.0, dt),
        fixed({}, 10.0),
    };
    std::vector<Contact> contacts;
    resolveCollisions(bodies, 0.5, contacts);

    SOL_CHECK(contacts.size() == 1);
    SOL_CHECK(std::abs(bodies[0].velocity.y - 30.0) < 1.0e-6); // half the speed, outward
}

SOL_TEST(collision_initial_overlap_depenetrates)
{
    std::vector<CollisionBody> bodies = {
        {.previousPosition = {1.0, 0.0, 0.0}, .position = {1.0, 0.0, 0.0}, .velocity = {},
         .radius = 5.0, .inverseMass = 1.0},
        fixed({}, 5.0),
    };
    std::vector<Contact> contacts;
    resolveCollisions(bodies, 0.2, contacts);

    SOL_CHECK(contacts.size() == 1);
    SOL_CHECK(length(bodies[0].position - bodies[1].position) >= 10.0);
    if (contacts.empty()) {
        return;
    }
    SOL_CHECK(contacts[0].impactSpeed == 0.0); // overlap, not an impact
}

SOL_TEST(collision_unequal_masses_share_impulse)
{
    // Light interceptor rams a heavy freighter at rest; heavy one barely moves.
    const double dt = 1.0 / 60.0;
    std::vector<CollisionBody> bodies = {
        mover({-10.0, 0.0, 0.0}, {-10.0 + 120.0 * dt, 0.0, 0.0}, 4.0, 1.0, dt), // m = 1
        mover({5.0, 0.0, 0.0}, {5.0, 0.0, 0.0}, 10.0, 0.05, dt),                // m = 20
    };
    std::vector<Contact> contacts;
    resolveCollisions(bodies, 0.0, contacts);

    SOL_CHECK(contacts.size() == 1);
    // Momentum: 120 = v1 + 20*v2, and equal post-impact velocities at e=0.
    SOL_CHECK(std::abs(bodies[0].velocity.x - bodies[1].velocity.x) < 1.0e-6);
    SOL_CHECK(std::abs(bodies[0].velocity.x + 20.0 * bodies[1].velocity.x - 120.0) < 1.0e-6);
}
