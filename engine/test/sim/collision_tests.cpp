#include <cmath>
#include <vector>

#include <sol/sim/collision.hpp>
#include <sol/test/test.hpp>

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
    return {.previousPosition = at, .position = at, .velocity = {}, .radius = radius, .inverseMass = 0.0};
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
    SOL_CHECK(altitude < 1.0);                // parked at the surface, not bounced into space...
    SOL_CHECK(bodies[0].velocity.z >= 0.0);   // ...and no longer moving inward
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
        {.previousPosition = {1.0, 0.0, 0.0},
         .position = {1.0, 0.0, 0.0},
         .velocity = {},
         .radius = 5.0,
         .inverseMass = 1.0},
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

// ⚑ Everything above this line runs within 6.4e6 m of the origin, where one
// ULP of a double coordinate is about 1e-9 m and the depenetration nudge —
// 1e-9 of the combined radius, so ~1.1e-7 m for a ship on a station — is
// worth nearly a thousand ULP. The game does not run there. A playfield hub
// is the system's middle planet (`universe.cpp`), orbiting at 4e10-4e11 m,
// and stations sit 1.0e8-4.0e8 m off that hub; out there one ULP is
// 3.8e-6-3.1e-5 m and the same nudge is worth 0.004 of one. It rounded away,
// the pair came to rest exactly touching, every later tick read that as an
// overlap, and the overlap branch discarded the whole tick's motion in every
// direction. A ship that bumped a station was stuck for good — found by a
// player, not by these tests, because coverage of a rule is not coverage of
// its instances.

namespace {

constexpr double kPlayfieldRestitution = 0.15; // space_world.cpp's kCollisionRestitution
constexpr double kShipRadius = 8.0;            // models.toml, ship
constexpr double kStationRadius = 100.0;       // models.toml, station

// A station out where the game actually puts one: an outer hub plus its
// offset. The magnitude is the point of the number.
constexpr DVec3 kPlayfieldStation{2.34e11, 1.25e11, 3.00e11};

// One tick of the game loop in space_world.cpp's order: remember where you
// were (sim.flight), integrate, resolve, write position and velocity back.
void tickAgainstStation(CollisionBody& ship, const CollisionBody& station, double dt)
{
    ship.previousPosition = ship.position;
    ship.position = ship.position + ship.velocity * dt;

    CollisionBody pair[2] = {ship, station};
    std::vector<Contact> contacts;
    resolveCollisions(pair, kPlayfieldRestitution, contacts);
    ship.position = pair[0].position;
    ship.velocity = pair[0].velocity;
}

// Sixteenths of a turn, tilted off the equator so no heading is axis-aligned.
DVec3 approachHeading(int sixteenth)
{
    constexpr double kTau = 6.283185307179586476925;
    const double angle = kTau * static_cast<double>(sixteenth) / 16.0;
    return normalize(DVec3{std::cos(angle), 0.31, std::sin(angle)});
}

} // namespace

SOL_TEST(collision_overlap_lets_a_body_slide_rather_than_pinning_it)
{
    // Overlapping when the tick began and moving TANGENTIALLY — scraping along
    // a hull. Rewinding to the start of the tick is right only for motion that
    // digs deeper; applied to motion that does not, it pins the body to the
    // spot it entered on. Near the origin, so this is about the branch and not
    // about precision.
    std::vector<CollisionBody> bodies = {
        mover({9.0, 0.0, 0.0}, {9.0, 3.0, 0.0}, 5.0, 1.0), // centres 9 m apart, combined 10
        fixed({}, 5.0),
    };
    std::vector<Contact> contacts;
    resolveCollisions(bodies, 0.0, contacts);

    SOL_CHECK(contacts.size() == 1);
    SOL_CHECK(bodies[0].position.y > 2.0);                              // slid along, not pinned at y = 0
    SOL_CHECK(length(bodies[0].position - bodies[1].position) >= 10.0); // and still pushed clear
}

SOL_TEST(collision_separation_survives_playfield_coordinates)
{
    // The nudge past exact touching has to be representable where the game
    // runs, or the pair rests at exactly the combined radius and the next tick
    // calls that an overlap. STRICTLY greater is the whole assertion.
    const double combined = kShipRadius + kStationRadius;
    const DVec3 approach = approachHeading(3);
    std::vector<CollisionBody> bodies = {
        mover(kPlayfieldStation - approach * (combined + 1.0),
              kPlayfieldStation - approach * (combined - 0.5),
              kShipRadius,
              1.0),
        fixed(kPlayfieldStation, kStationRadius),
    };
    std::vector<Contact> contacts;
    resolveCollisions(bodies, kPlayfieldRestitution, contacts);

    SOL_CHECK(contacts.size() == 1);
    SOL_CHECK(length(bodies[0].position - bodies[1].position) > combined);
}

SOL_TEST(collision_a_bumped_ship_can_always_thrust_away_at_playfield_range)
{
    // The player's bug, end to end: drift into a station, then hold full
    // reverse. Every heading has to come free. Sixteen of them, because the
    // failure depended on which way the ship happened to be pointing — about
    // half of all bumps trapped it.
    const double dt = 1.0 / 60.0;
    const double combined = kShipRadius + kStationRadius;
    const CollisionBody station = fixed(kPlayfieldStation, kStationRadius);

    for (int heading = 0; heading < 16; ++heading) {
        const DVec3 approach = approachHeading(heading);
        CollisionBody ship = fixed(kPlayfieldStation - approach * (combined + 30.0), kShipRadius);
        ship.inverseMass = 1.0;
        ship.velocity = approach * 5.0; // a gentle bump, which is the case that stuck

        for (int i = 0; i < 400; ++i) { // settle against the hull
            tickAgainstStation(ship, station, dt);
        }
        const DVec3 settled = ship.position;

        ship.velocity = approach * -60.0; // full reverse, held for a second
        for (int i = 0; i < 60; ++i) {
            tickAgainstStation(ship, station, dt);
        }
        // 60 m/s for a second is 60 m in open space; anything above a third of
        // that is unambiguously "the ship moved".
        SOL_CHECK(length(ship.position - settled) > 20.0);
    }
}
