#include "sol/sim/weapons.hpp"

#include <cmath>

namespace sol::sim {

bool computeInterceptDirection(const core::DVec3& shooterPosition,
                               const core::DVec3& shooterVelocity,
                               const core::DVec3& targetPosition,
                               const core::DVec3& targetVelocity, double projectileSpeed,
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

bool segmentHitsSphere(const core::DVec3& from, const core::DVec3& to, const core::DVec3& center,
                       double radius, double& outT)
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
