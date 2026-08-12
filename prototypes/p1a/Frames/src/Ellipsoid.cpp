#include "Sol/Proto/Frames/Ellipsoid.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace sol::proto::frames {

Ellipsoid::Ellipsoid(std::string name, double equatorialRadiusMetres, double polarRadiusMetres)
    : m_name{std::move(name)}
    , m_equatorialRadius{equatorialRadiusMetres}
    , m_polarRadius{polarRadiusMetres}
{
    if (!(m_equatorialRadius > 0.0) || !(m_polarRadius > 0.0)
        || m_polarRadius > m_equatorialRadius) {
        throw std::runtime_error("Ellipsoid: '" + m_name + "' has non-physical radii");
    }
    m_flattening = (m_equatorialRadius - m_polarRadius) / m_equatorialRadius;
    m_eccentricitySquared = m_flattening * (2.0 - m_flattening);
}

Ellipsoid Ellipsoid::fromRadiiKilometres(std::string name, double equatorialKm, double polarKm)
{
    return Ellipsoid{std::move(name), equatorialKm * 1000.0, polarKm * 1000.0};
}

Ellipsoid Ellipsoid::wgs84()
{
    // Defining constants of WGS84: the semi-major axis in metres and the inverse flattening.
    // The semi-minor axis is derived, never quoted, because quoting a rounded b alongside a
    // and 1/f is how the three drift out of agreement.
    constexpr double kSemiMajor = 6378137.0;
    constexpr double kInverseFlattening = 298.257223563;
    const double semiMinor = kSemiMajor * (1.0 - 1.0 / kInverseFlattening);
    return Ellipsoid{"WGS84", kSemiMajor, semiMinor};
}

PositionMetres Ellipsoid::toBodyFixed(const Geodetic& geodetic) const noexcept
{
    const double sinLatitude = std::sin(geodetic.latitude.radians());
    const double cosLatitude = std::cos(geodetic.latitude.radians());
    const double sinLongitude = std::sin(geodetic.longitude.radians());
    const double cosLongitude = std::cos(geodetic.longitude.radians());

    // Radius of curvature in the prime vertical.
    const double primeVertical =
        m_equatorialRadius / std::sqrt(1.0 - m_eccentricitySquared * sinLatitude * sinLatitude);

    const double xy = (primeVertical + geodetic.heightMetres) * cosLatitude;
    return PositionMetres::fromMetres(
        Vec3{xy * cosLongitude,
             xy * sinLongitude,
             (primeVertical * (1.0 - m_eccentricitySquared) + geodetic.heightMetres)
                 * sinLatitude});
}

Geodetic Ellipsoid::fromBodyFixed(const PositionMetres& bodyFixed, int* iterationsUsed) const noexcept
{
    const Vec3& r = bodyFixed.metres();
    const double distanceFromAxis = std::hypot(r.x, r.y);

    Geodetic result{};
    result.longitude = Radians::fromRadians(std::atan2(r.y, r.x));

    // Newton iteration on geodetic latitude. Seeded with the geocentric latitude, which is
    // within 0.2 degrees of the geodetic value everywhere on Earth.
    double latitude = std::atan2(r.z, distanceFromAxis * (1.0 - m_eccentricitySquared));
    int iteration = 0;
    for (; iteration < 16; ++iteration) {
        const double sinLatitude = std::sin(latitude);
        const double primeVertical =
            m_equatorialRadius / std::sqrt(1.0 - m_eccentricitySquared * sinLatitude * sinLatitude);
        const double next = std::atan2(
            r.z + m_eccentricitySquared * primeVertical * sinLatitude, distanceFromAxis);
        const double delta = next - latitude;
        latitude = next;
        // One radian of latitude is about 6.4e6 m, so 1e-16 rad is well under a nanometre.
        if (std::abs(delta) < 1e-16) {
            ++iteration;
            break;
        }
    }
    if (iterationsUsed != nullptr) {
        *iterationsUsed = iteration;
    }

    const double sinLatitude = std::sin(latitude);
    const double cosLatitude = std::cos(latitude);
    const double primeVertical =
        m_equatorialRadius / std::sqrt(1.0 - m_eccentricitySquared * sinLatitude * sinLatitude);

    result.latitude = Radians::fromRadians(latitude);

    // Two height formulas, selected by latitude. The equatorial form divides by cos(lat) and
    // the polar form by sin(lat); each is ill-conditioned exactly where the other is stable.
    // The anchor sits at 28 degrees, so the equatorial branch is the one A2 exercises, but a
    // frame library that silently loses precision near the poles is a defect waiting for the
    // first polar-orbit scenario.
    if (std::abs(cosLatitude) > 0.5) {
        result.heightMetres = distanceFromAxis / cosLatitude - primeVertical;
    } else {
        result.heightMetres =
            r.z / sinLatitude - primeVertical * (1.0 - m_eccentricitySquared);
    }

    return result;
}

Vec3 Ellipsoid::eastAxis(const Geodetic& geodetic) const noexcept
{
    const double sinLongitude = std::sin(geodetic.longitude.radians());
    const double cosLongitude = std::cos(geodetic.longitude.radians());
    return Vec3{-sinLongitude, cosLongitude, 0.0};
}

Vec3 Ellipsoid::northAxis(const Geodetic& geodetic) const noexcept
{
    const double sinLatitude = std::sin(geodetic.latitude.radians());
    const double cosLatitude = std::cos(geodetic.latitude.radians());
    const double sinLongitude = std::sin(geodetic.longitude.radians());
    const double cosLongitude = std::cos(geodetic.longitude.radians());
    return Vec3{-sinLatitude * cosLongitude, -sinLatitude * sinLongitude, cosLatitude};
}

Vec3 Ellipsoid::upAxis(const Geodetic& geodetic) const noexcept
{
    const double sinLatitude = std::sin(geodetic.latitude.radians());
    const double cosLatitude = std::cos(geodetic.latitude.radians());
    const double sinLongitude = std::sin(geodetic.longitude.radians());
    const double cosLongitude = std::cos(geodetic.longitude.radians());
    return Vec3{cosLatitude * cosLongitude, cosLatitude * sinLongitude, sinLatitude};
}

} // namespace sol::proto::frames
