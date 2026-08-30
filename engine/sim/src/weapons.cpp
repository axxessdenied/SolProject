#include "sol/sim/weapons.hpp"

#include "sol/core/math/scalar.hpp"

#include <cmath>

namespace sol::sim {

bool computeInterceptDirection(const core::DVec3& shooterPosition,
                               const core::DVec3& shooterVelocity,
                               const core::DVec3& targetPosition,
                               const core::DVec3& targetVelocity,
                               double projectileSpeed,
                               core::DVec3& outDirection)
{
    const core::DVec3 relativePosition = targetPosition - shooterPosition;
    const core::DVec3 relativeVelocity = targetVelocity - shooterVelocity;

    // Solve |p + v t| = s t for the earliest positive t.
    const double a = dot(relativeVelocity, relativeVelocity) - projectileSpeed * projectileSpeed;
    const double b = 2.0 * dot(relativePosition, relativeVelocity);
    const double c = dot(relativePosition, relativePosition);

    double interceptTime = -1.0;
    if (std::abs(a) < 1.0e-9) {
        // Relative speed matches projectile speed: linear equation.
        if (std::abs(b) > 1.0e-12) {
            interceptTime = -c / b;
        }
    } else {
        const double discriminant = b * b - 4.0 * a * c;
        if (discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            const double t1 = (-b - root) / (2.0 * a);
            const double t2 = (-b + root) / (2.0 * a);
            const double tMin = t1 < t2 ? t1 : t2;
            const double tMax = t1 < t2 ? t2 : t1;
            interceptTime = tMin > 0.0 ? tMin : tMax;
        }
    }

    if (interceptTime <= 0.0) {
        outDirection = normalize(relativePosition);
        return false;
    }
    outDirection = normalize(relativePosition + relativeVelocity * interceptTime);
    return true;
}

namespace {

// Any unit vector perpendicular to `v`. Only ever needed for the one
// degenerate case below - a gun laid exactly 180 degrees off its rest, where
// every plane through the two is as good as every other and the cross product
// that would normally pick one is zero.
[[nodiscard]] core::DVec3 anyPerpendicular(const core::DVec3& v)
{
    const core::DVec3 axis = std::abs(v.x) < 0.9 ? core::DVec3{1.0, 0.0, 0.0} : core::DVec3{0.0, 1.0, 0.0};
    return normalize(cross(v, axis));
}

} // namespace

bool layWithinArc(const core::DVec3& rest,
                  const core::DVec3& sought,
                  double arcDegrees,
                  core::DVec3& outBearing)
{
    const double restLength = length(rest);
    const core::DVec3 r = restLength > 0.0 ? rest * (1.0 / restLength) : core::DVec3{0.0, 0.0, -1.0};
    const double soughtLength = length(sought);
    const core::DVec3 s = soughtLength > 0.0 ? sought * (1.0 / soughtLength) : r;
    const double alignment = core::clamp(dot(r, s), -1.0, 1.0);

    // A gun with no ring points where it is bolted, full stop - and it "bears"
    // exactly when that is already where the gunner wanted it. Callers that
    // pass `rest` as the sought direction for such a gun therefore always get
    // true, which is what keeps a fixed nose gun firing on a held trigger with
    // no special case anywhere above this.
    const double halfArc = 0.5 * arcDegrees * (core::kPiD / 180.0);
    if (halfArc <= 0.0) {
        outBearing = r;
        return alignment >= 1.0 - 1.0e-9;
    }
    if (alignment >= std::cos(halfArc)) {
        outBearing = s;
        return true;
    }

    // On the stop, in the plane the two directions span: `rest` swung as far
    // round toward `sought` as the ring allows.
    core::DVec3 perpendicular = s - r * alignment;
    const double perpendicularLength = length(perpendicular);
    perpendicular =
        perpendicularLength > 1.0e-9 ? perpendicular * (1.0 / perpendicularLength) : anyPerpendicular(r);
    outBearing = r * std::cos(halfArc) + perpendicular * std::sin(halfArc);
    return false;
}

bool segmentHitsSphere(
    const core::DVec3& from, const core::DVec3& to, const core::DVec3& center, double radius, double& outT)
{
    const core::DVec3 start = from - center;
    if (dot(start, start) <= radius * radius) {
        outT = 0.0;
        return true;
    }
    const core::DVec3 direction = to - from;
    const double a = dot(direction, direction);
    if (a <= 0.0) {
        return false;
    }
    const double b = 2.0 * dot(start, direction);
    const double c = dot(start, start) - radius * radius;
    const double discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) {
        return false;
    }
    const double t = (-b - std::sqrt(discriminant)) / (2.0 * a);
    if (t < 0.0 || t > 1.0) {
        return false;
    }
    outT = t;
    return true;
}

} // namespace sol::sim
