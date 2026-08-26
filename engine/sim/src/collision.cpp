#include "sol/sim/collision.hpp"

#include <cmath>

namespace sol::sim {

namespace {

// Nudge past exact touching so a resolved pair doesn't re-collide from
// floating-point dust next tick.
constexpr double kSeparationSlack = 1.0 + 1.0e-9;

void resolvePair(CollisionBody& a,
                 CollisionBody& b,
                 std::uint32_t indexA,
                 std::uint32_t indexB,
                 double restitution,
                 std::vector<Contact>& outContacts)
{
    const double inverseMassSum = a.inverseMass + b.inverseMass;
    if (inverseMassSum <= 0.0) {
        return; // two immovables
    }

    const double combinedRadius = a.radius + b.radius;
    const core::DVec3 startDelta = a.previousPosition - b.previousPosition;
    const core::DVec3 sweep = (a.position - a.previousPosition) - (b.position - b.previousPosition);

    // Time of impact along the tick's relative motion, or 0 if already
    // overlapping at the start.
    double impactTime = -1.0;
    if (dot(startDelta, startDelta) < combinedRadius * combinedRadius) {
        impactTime = 0.0;
    } else {
        const double aq = dot(sweep, sweep);
        if (aq <= 0.0) {
            return; // no relative motion
        }
        const double bq = 2.0 * dot(startDelta, sweep);
        const double cq = dot(startDelta, startDelta) - combinedRadius * combinedRadius;
        const double discriminant = bq * bq - 4.0 * aq * cq;
        if (discriminant < 0.0) {
            return;
        }
        const double t = (-bq - std::sqrt(discriminant)) / (2.0 * aq);
        if (t < 0.0 || t > 1.0) {
            return;
        }
        impactTime = t;
    }

    // Place both bodies at the impact configuration (the remaining sub-tick
    // slide is dropped; invisible at 60 Hz).
    a.position = a.previousPosition + (a.position - a.previousPosition) * impactTime;
    b.position = b.previousPosition + (b.position - b.previousPosition) * impactTime;

    core::DVec3 normal = a.position - b.position;
    const double distance = length(normal);
    normal = distance > 0.0 ? normal * (1.0 / distance) : core::DVec3{1.0, 0.0, 0.0};

    // Depenetrate to exactly touching, split by inverse mass.
    const double penetration = combinedRadius * kSeparationSlack - distance;
    if (penetration > 0.0) {
        a.position += normal * (penetration * a.inverseMass / inverseMassSum);
        b.position += -normal * (penetration * b.inverseMass / inverseMassSum);
    }

    const double closingSpeed = -dot(a.velocity - b.velocity, normal);
    if (closingSpeed > 0.0) {
        const double impulse = (1.0 + restitution) * closingSpeed / inverseMassSum;
        a.velocity += normal * (impulse * a.inverseMass);
        b.velocity += -normal * (impulse * b.inverseMass);
    }

    outContacts.push_back(Contact{
        .bodyA = indexA,
        .bodyB = indexB,
        .normal = normal,
        .impactSpeed = closingSpeed > 0.0 ? closingSpeed : 0.0,
    });
}

} // namespace

void resolveCollisions(std::span<CollisionBody> bodies, double restitution, std::vector<Contact>& outContacts)
{
    for (std::size_t i = 0; i + 1 < bodies.size(); ++i) {
        for (std::size_t j = i + 1; j < bodies.size(); ++j) {
            resolvePair(bodies[i],
                        bodies[j],
                        static_cast<std::uint32_t>(i),
                        static_cast<std::uint32_t>(j),
                        restitution,
                        outContacts);
        }
    }
}

} // namespace sol::sim
