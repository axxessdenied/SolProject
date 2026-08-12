#include "Sol/Proto/Frames/AscentProfile.h"

#include <cmath>

namespace sol::proto::frames {

AscentProfile::AscentProfile(double durationSeconds, double finalAltitudeMetres,
                             double finalDownrangeSpeed, Radians azimuthFromNorth) noexcept
    : m_duration{durationSeconds}
    , m_finalAltitude{finalAltitudeMetres}
    , m_finalDownrangeSpeed{finalDownrangeSpeed}
{
    // Downrange distance follows d(u) = D u^3, so d'(0) = 0 and d'(1) = 3D/T. Choosing D from
    // the required insertion speed is what ties the curve's shape to a physical end condition
    // instead of to a number picked for looking right.
    m_downrangeScale = m_finalDownrangeSpeed * m_duration / 3.0;
    m_eastFraction = std::sin(azimuthFromNorth.radians());
    m_northFraction = std::cos(azimuthFromNorth.radians());
}

VehicleState AscentProfile::sample(double secondsSinceLiftoff) const noexcept
{
    const double u = secondsSinceLiftoff / m_duration;

    // Altitude: smoothstep, so vertical speed is zero at both liftoff and insertion.
    const double altitude = m_finalAltitude * u * u * (3.0 - 2.0 * u);
    const double altitudeRate = m_finalAltitude * 6.0 * u * (1.0 - u) / m_duration;

    const double downrange = m_downrangeScale * u * u * u;
    const double downrangeRate = 3.0 * m_downrangeScale * u * u / m_duration;

    VehicleState state;
    state.positionInEnu = Vec3{downrange * m_eastFraction, downrange * m_northFraction, altitude};
    state.velocityInEnu =
        Vec3{downrangeRate * m_eastFraction, downrangeRate * m_northFraction, altitudeRate};
    return state;
}

double AscentProfile::planarAltitudeMetres(double secondsSinceLiftoff) const noexcept
{
    return sample(secondsSinceLiftoff).positionInEnu.z;
}

} // namespace sol::proto::frames
