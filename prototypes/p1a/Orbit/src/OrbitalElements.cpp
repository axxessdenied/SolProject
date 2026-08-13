#include "Sol/Proto/Orbit/OrbitalElements.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sol::proto::orbit {
namespace {

using frames::Vec3;

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 6.283185307179586476925286766559;

/// acos with the argument clamped to its valid domain.
///
/// The cosines below are ratios of quantities computed from the same vectors, so rounding can
/// put them a few ULP outside [-1, 1] -- most reliably at exactly the degenerate cases this
/// code is trying to classify. Unclamped, that produces a NaN angle from a perfectly ordinary
/// orbit.
[[nodiscard]] double safeAcos(double cosine) noexcept
{
    return std::acos(std::clamp(cosine, -1.0, 1.0));
}

/// Resolves an angle recovered by acos into [0, 2pi) using the quadrant test @p isSecondHalf.
///
/// acos only ever returns [0, pi], so every classical angle needs a companion sign test to
/// recover the other half turn. Naming the operation keeps the six such tests below from being
/// six opportunities to get the sign backwards.
[[nodiscard]] double resolveQuadrant(double angle, bool isSecondHalf) noexcept
{
    return isSecondHalf ? kTwoPi - angle : angle;
}

/// Rotates a perifocal (PQW) vector into the inertial axes the state is expressed on.
[[nodiscard]] Vec3 perifocalToInertial(const Vec3& perifocal, double raan, double inclination,
                                       double argumentOfPeriapsis) noexcept
{
    const double cosRaan = std::cos(raan);
    const double sinRaan = std::sin(raan);
    const double cosInclination = std::cos(inclination);
    const double sinInclination = std::sin(inclination);
    const double cosArgp = std::cos(argumentOfPeriapsis);
    const double sinArgp = std::sin(argumentOfPeriapsis);

    const double m00 = cosRaan * cosArgp - sinRaan * sinArgp * cosInclination;
    const double m01 = -cosRaan * sinArgp - sinRaan * cosArgp * cosInclination;
    const double m02 = sinRaan * sinInclination;
    const double m10 = sinRaan * cosArgp + cosRaan * sinArgp * cosInclination;
    const double m11 = -sinRaan * sinArgp + cosRaan * cosArgp * cosInclination;
    const double m12 = -cosRaan * sinInclination;
    const double m20 = sinArgp * sinInclination;
    const double m21 = cosArgp * sinInclination;
    const double m22 = cosInclination;

    return Vec3{m00 * perifocal.x + m01 * perifocal.y + m02 * perifocal.z,
                m10 * perifocal.x + m11 * perifocal.y + m12 * perifocal.z,
                m20 * perifocal.x + m21 * perifocal.y + m22 * perifocal.z};
}

} // namespace

std::string_view orbitShapeName(OrbitShape shape) noexcept
{
    switch (shape) {
    case OrbitShape::General:             return "general";
    case OrbitShape::CircularInclined:    return "circular-inclined";
    case OrbitShape::EccentricEquatorial: return "eccentric-equatorial";
    case OrbitShape::CircularEquatorial:  return "circular-equatorial";
    case OrbitShape::Degenerate:          return "degenerate";
    }
    return "unknown";
}

OrbitalElements elementsFromState(const TwoBodyState& state,
                                  double gravitationalParameter) noexcept
{
    OrbitalElements elements;

    const Vec3& positionVector = state.position;
    const Vec3& velocityVector = state.velocity;
    const double r = frames::length(positionVector);
    const double v = frames::length(velocityVector);

    const Vec3 angularMomentum = frames::cross(positionVector, velocityVector);
    const double h = frames::length(angularMomentum);

    if (r == 0.0 || h == 0.0 || gravitationalParameter <= 0.0) {
        // No orbital plane exists. Reported rather than approximated: a radial trajectory is a
        // real physical state that ADR 0011's conic coast simply cannot represent, and the
        // eligibility rules depend on being told so.
        elements.shape = OrbitShape::Degenerate;
        return elements;
    }

    // The node vector, z_hat x h. Its third component is identically zero by construction, so
    // it is written out rather than formed through a general cross product.
    const Vec3 node{-angularMomentum.y, angularMomentum.x, 0.0};
    const double nodeMagnitude = frames::length(node);

    const double radialVelocity = frames::dot(positionVector, velocityVector);
    const Vec3 eccentricityVector =
        (positionVector * (v * v - gravitationalParameter / r) - velocityVector * radialVelocity)
        * (1.0 / gravitationalParameter);
    const double e = frames::length(eccentricityVector);

    elements.eccentricity = e;
    elements.semiLatusRectum = h * h / gravitationalParameter;
    elements.inclination = safeAcos(angularMomentum.z / h);

    // Semi-major axis from the energy rather than from p and e, because the p/(1-e^2) form
    // divides by a quantity that goes to zero exactly at the parabolic case.
    const double energy = 0.5 * v * v - gravitationalParameter / r;
    if (std::abs(energy) > 0.0 && std::abs(e - 1.0) > kCircularEccentricityThreshold) {
        elements.semiMajorAxis = -gravitationalParameter / (2.0 * energy);
    } else {
        elements.semiMajorAxis = std::numeric_limits<double>::infinity();
    }

    const bool isCircular = e < kCircularEccentricityThreshold;
    const bool isEquatorial = elements.inclination < kEquatorialInclinationThreshold
                           || (kPi - elements.inclination) < kEquatorialInclinationThreshold;

    // True longitude is defined for every non-degenerate orbit, so it is computed
    // unconditionally and serves as the fallback angle for the doubly degenerate case.
    elements.trueLongitude =
        resolveQuadrant(safeAcos(positionVector.x / r), positionVector.y < 0.0);

    if (!isEquatorial) {
        elements.rightAscensionOfAscendingNode =
            resolveQuadrant(safeAcos(node.x / nodeMagnitude), node.y < 0.0);
        elements.argumentOfLatitude =
            resolveQuadrant(safeAcos(frames::dot(node, positionVector) / (nodeMagnitude * r)),
                            positionVector.z < 0.0);
    }

    if (!isCircular) {
        elements.trueAnomaly =
            resolveQuadrant(safeAcos(frames::dot(eccentricityVector, positionVector) / (e * r)),
                            radialVelocity < 0.0);
        elements.longitudeOfPeriapsis =
            resolveQuadrant(safeAcos(eccentricityVector.x / e), eccentricityVector.y < 0.0);
        if (!isEquatorial) {
            elements.argumentOfPeriapsis = resolveQuadrant(
                safeAcos(frames::dot(node, eccentricityVector) / (nodeMagnitude * e)),
                eccentricityVector.z < 0.0);
        }
    }

    if (isCircular && isEquatorial) {
        elements.shape = OrbitShape::CircularEquatorial;
    } else if (isCircular) {
        elements.shape = OrbitShape::CircularInclined;
    } else if (isEquatorial) {
        elements.shape = OrbitShape::EccentricEquatorial;
    } else {
        elements.shape = OrbitShape::General;
    }

    elements.periapsisRadius = elements.semiLatusRectum / (1.0 + e);
    if (e < 1.0 - kCircularEccentricityThreshold) {
        elements.apoapsisRadius = elements.semiLatusRectum / (1.0 - e);
        elements.periodSeconds = kTwoPi
                               * std::sqrt(elements.semiMajorAxis * elements.semiMajorAxis
                                           * elements.semiMajorAxis / gravitationalParameter);
    } else {
        // An open orbit has no apoapsis and no period. Infinity is the honest value for both,
        // and it propagates loudly if anything downstream treats it as a number.
        elements.apoapsisRadius = std::numeric_limits<double>::infinity();
        elements.periodSeconds = std::numeric_limits<double>::infinity();
    }

    return elements;
}

TwoBodyState stateFromElements(const OrbitalElements& elements,
                               double gravitationalParameter) noexcept
{
    TwoBodyState state;
    if (elements.shape == OrbitShape::Degenerate || elements.semiLatusRectum <= 0.0
        || gravitationalParameter <= 0.0) {
        return state;
    }

    // Each degenerate shape substitutes the angle that is actually defined for it. This is the
    // inverse of the classification above, and the two must agree: whichever angle
    // elementsFromState left undefined is the one that must not be read back here.
    double raan = elements.rightAscensionOfAscendingNode;
    double argumentOfPeriapsis = elements.argumentOfPeriapsis;
    double trueAnomaly = elements.trueAnomaly;

    switch (elements.shape) {
    case OrbitShape::CircularEquatorial:
        raan = 0.0;
        argumentOfPeriapsis = 0.0;
        trueAnomaly = elements.trueLongitude;
        break;
    case OrbitShape::CircularInclined:
        argumentOfPeriapsis = 0.0;
        trueAnomaly = elements.argumentOfLatitude;
        break;
    case OrbitShape::EccentricEquatorial:
        raan = 0.0;
        argumentOfPeriapsis = elements.longitudeOfPeriapsis;
        break;
    case OrbitShape::General:
    case OrbitShape::Degenerate:
        break;
    }

    const double p = elements.semiLatusRectum;
    const double e = elements.eccentricity;
    const double cosTrueAnomaly = std::cos(trueAnomaly);
    const double sinTrueAnomaly = std::sin(trueAnomaly);
    const double r = p / (1.0 + e * cosTrueAnomaly);
    const double velocityScale = std::sqrt(gravitationalParameter / p);

    const Vec3 positionPerifocal{r * cosTrueAnomaly, r * sinTrueAnomaly, 0.0};
    const Vec3 velocityPerifocal{-velocityScale * sinTrueAnomaly,
                                 velocityScale * (e + cosTrueAnomaly), 0.0};

    state.position =
        perifocalToInertial(positionPerifocal, raan, elements.inclination, argumentOfPeriapsis);
    state.velocity =
        perifocalToInertial(velocityPerifocal, raan, elements.inclination, argumentOfPeriapsis);
    return state;
}

} // namespace sol::proto::orbit
