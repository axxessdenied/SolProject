#include "sol/sim/collision.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sol::sim {

namespace {

// How far past exact touching a resolved pair is pushed, so it does not
// re-collide from floating-point dust on the next tick.
//
// ⚑ The nudge has to be REPRESENTABLE at the coordinates the playfield
// actually uses, and a slack relative to the radius is not. A ship against a
// station is a combined 108 m, so 1e-9 of it is 1.08e-7 m — while the
// playfield hub is a planet orbiting at 4e10-4e11 m (`universe.cpp`), where
// one ULP of a double coordinate is 3.8e-6-3.1e-5 m. The old radius-relative
// slack was worth 0.004-0.03 of an ULP out there: it rounded away entirely,
// the pair came to rest EXACTLY touching, and the next tick read that as an
// overlap. Scaling to the position as well as to the radius is what makes the
// push survive the add.
//
// The radius term is kept as a floor so near-origin behaviour — every case
// the unit tests cover, and the reason they never saw this — is unchanged.
[[nodiscard]] double separationSlack(const core::DVec3& position, double combinedRadius)
{
    const double coordinate = std::max({std::abs(position.x), std::abs(position.y), std::abs(position.z)});
    // 32 ULP of the coordinate: still clear of rounding after the inverse-mass
    // split halves it and the normal spreads it across three components, and
    // only ~2 mm at 4e11 m — invisible against a 100 m station.
    constexpr double kSeparationUlps = 32.0;
    return std::max(combinedRadius * 1.0e-9,
                    kSeparationUlps * coordinate * std::numeric_limits<double>::epsilon());
}

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
        // ⚑ Already overlapping when the tick began, and the direction of the
        // motion decides what to do with it. Rewinding to the start of the
        // tick is right for motion that digs deeper; for motion that SEPARATES
        // it is catastrophic. A pilot flying out of a contact would have the
        // whole tick thrown away — every tick, in every direction — while the
        // depenetration below is then the only way out, and that push can be
        // lost to rounding at playfield coordinates (see `separationSlack`).
        // The two together are a ship welded to a station for good, which is
        // exactly what they did. Keep separating motion and let the
        // depenetration finish the job.
        impactTime = dot(sweep, startDelta) < 0.0 ? 0.0 : 1.0;
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
    // slide is dropped; invisible at 60 Hz). An impactTime of 1 is the
    // separating-overlap case above and leaves the tick's motion standing.
    a.position = a.previousPosition + (a.position - a.previousPosition) * impactTime;
    b.position = b.previousPosition + (b.position - b.previousPosition) * impactTime;

    core::DVec3 normal = a.position - b.position;
    const double distance = length(normal);
    normal = distance > 0.0 ? normal * (1.0 / distance) : core::DVec3{1.0, 0.0, 0.0};

    // Depenetrate to just past touching, split by inverse mass.
    const double penetration = combinedRadius + separationSlack(a.position, combinedRadius) - distance;
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
