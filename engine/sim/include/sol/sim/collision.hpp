#pragma once

// Sphere collision for the flight sim (engine plan Phase 6): swept
// continuous tests so nothing tunnels at cruise speeds (5,500 km/s is ~91 km
// per 60 Hz tick), impulse response with restitution, and contact reporting
// for the damage model to consume. Deliberately array-based and ECS-free:
// the game builds the body list each tick and writes results back.

#include "sol/core/math/math.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace sol::sim {

struct CollisionBody
{
    core::DVec3 previousPosition; // start of tick (statics: == position)
    core::DVec3 position;         // end of tick; adjusted on contact
    core::DVec3 velocity;         // m/s; adjusted on contact
    double radius = 0.0;          // meters
    double inverseMass = 0.0;     // 0 = immovable (stations, planets)
};

struct Contact
{
    std::uint32_t bodyA = 0;
    std::uint32_t bodyB = 0;
    core::DVec3 normal;       // unit, from B toward A
    double impactSpeed = 0.0; // closing speed along the normal at impact, m/s
};

// Resolves every pair once (greedy, earliest-time-of-impact within the pair):
// sweeps relative motion over the tick, places touching bodies at the impact
// configuration, and applies a normal impulse with the given restitution.
// Appends one Contact per resolved impact. O(n^2) pairs - fine at encounter
// scale (1-20 ships + a handful of statics).
void resolveCollisions(std::span<CollisionBody> bodies,
                       double restitution,
                       std::vector<Contact>& outContacts);

} // namespace sol::sim
