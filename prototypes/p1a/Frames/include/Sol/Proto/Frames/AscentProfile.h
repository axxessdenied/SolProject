#pragma once

#include "Sol/Proto/Frames/FrameGraph.h"
#include "Sol/Proto/Frames/Units.h"

namespace sol::proto::frames {

/// A closed-form surface-to-200-km ascent path, expressed in the launch-site frame.
///
/// This is a *kinematic* profile, not a dynamics solution. Nothing here integrates a force,
/// consumes propellant, or knows what a rocket is. Position and velocity come from one
/// analytic curve and its exact derivative, which makes every sampled state internally
/// consistent to the last bit.
///
/// That is the correct instrument for A2. The increment measures what a frame conversion does
/// to a state, so the state has to be exact input; a numerically integrated trajectory would
/// mix integrator error into a conversion-error measurement and make the result unattributable.
/// Trajectory integration is increment A3's subject, under ADR 0011.
///
/// The curve is chosen so its boundary conditions are physically sane rather than arbitrary:
/// vertical rise and downrange speed both start at zero at liftoff, vertical speed returns to
/// zero at insertion, and downrange speed reaches circular velocity there. Between those ends
/// it is a smooth interpolation with no claim to being any particular vehicle's trajectory.
class AscentProfile {
public:
    /// @param durationSeconds       Ascent duration.
    /// @param finalAltitudeMetres   Altitude at insertion, above the reference ellipsoid.
    /// @param finalDownrangeSpeed   Horizontal speed at insertion, relative to the rotating
    ///                              launch site frame, in m/s.
    /// @param azimuthFromNorth      Launch azimuth. ADR 0008's anchor is a coastal easterly
    ///                              site, so 90 degrees flies due east and gains the most from
    ///                              Earth's rotation.
    AscentProfile(double durationSeconds, double finalAltitudeMetres, double finalDownrangeSpeed,
                  Radians azimuthFromNorth) noexcept;

    /// The vehicle's state at @p secondsSinceLiftoff, in launch-site east-north-up coordinates.
    [[nodiscard]] VehicleState sample(double secondsSinceLiftoff) const noexcept;

    [[nodiscard]] double durationSeconds() const noexcept { return m_duration; }
    [[nodiscard]] double finalAltitudeMetres() const noexcept { return m_finalAltitude; }

    /// Altitude above the launch site's horizon plane at @p secondsSinceLiftoff.
    ///
    /// Not the same thing as geodetic altitude: the topocentric frame is a plane tangent at
    /// the anchor, so a vehicle 1300 km downrange sits well below its own ENU "up" coordinate
    /// relative to the ellipsoid. A2 reports both, because conflating them is a mistake that
    /// hides inside a plausible-looking number.
    [[nodiscard]] double planarAltitudeMetres(double secondsSinceLiftoff) const noexcept;

private:
    double m_duration{0.0};
    double m_finalAltitude{0.0};
    double m_finalDownrangeSpeed{0.0};
    double m_downrangeScale{0.0};
    double m_eastFraction{0.0};
    double m_northFraction{0.0};
};

} // namespace sol::proto::frames
