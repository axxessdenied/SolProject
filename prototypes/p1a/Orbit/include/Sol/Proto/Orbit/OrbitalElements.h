#pragma once

#include "Sol/Proto/Orbit/TwoBody.h"

#include <cstdint>
#include <string_view>

namespace sol::proto::orbit {

/// Which classical elements are actually defined for a given orbit.
///
/// The classical set is singular at zero eccentricity and zero inclination: a circular orbit
/// has no periapsis, so the argument of periapsis is undefined, and an equatorial orbit has no
/// ascending node, so the right ascension of the ascending node is undefined. Both cases are
/// *exactly* what A3 measures -- the reference case is a 200 km circular orbit -- so this is
/// not a corner to be handled defensively but the main path.
///
/// The library reports the degeneracy rather than silently substituting zero, because a
/// reported argument of periapsis of 0.0 for a circular orbit is a number that looks like a
/// measurement and is not one.
enum class OrbitShape : std::uint8_t {
    /// Eccentric and inclined. Every classical element is defined.
    General,
    /// Circular within tolerance, inclined. Argument of periapsis and true anomaly are
    /// undefined; argumentOfLatitude replaces their sum.
    CircularInclined,
    /// Eccentric, equatorial within tolerance. Node and argument of periapsis are undefined;
    /// longitudeOfPeriapsis replaces their sum.
    EccentricEquatorial,
    /// Circular and equatorial. Only trueLongitude is defined.
    CircularEquatorial,
    /// Angular momentum is zero: a radial trajectory with no orbital plane. No conic element
    /// is defined, and ADR 0011's analytical coast cannot represent it.
    Degenerate,
};

[[nodiscard]] std::string_view orbitShapeName(OrbitShape shape) noexcept;

/// Classical orbital elements, in SI units and radians.
///
/// Angles are reported in radians because that is the unit every interior in this library
/// works in; degrees appear only in evidence reports, at the named conversion in Units.h.
struct OrbitalElements {
    OrbitShape shape{OrbitShape::Degenerate};

    /// Semi-major axis, in metres. Negative for a hyperbolic orbit. Infinite for parabolic,
    /// which is why semiLatusRectum is also reported: it is finite for every conic.
    double semiMajorAxis{0.0};
    /// Semi-latus rectum p = h^2/mu, in metres. Finite and well defined for every conic
    /// including the parabolic case, so it is the safe shape parameter to compare against.
    double semiLatusRectum{0.0};
    double eccentricity{0.0};
    /// Inclination, in radians, measured from the reference plane of the axes the state is
    /// expressed on. For a state on ICRF axes this is inclination to the ICRF equator, which
    /// is not the same as inclination to Earth's true equator of date.
    double inclination{0.0};
    /// Right ascension of the ascending node, in radians. Undefined for equatorial shapes.
    double rightAscensionOfAscendingNode{0.0};
    /// Argument of periapsis, in radians. Undefined for circular shapes.
    double argumentOfPeriapsis{0.0};
    /// True anomaly, in radians. Undefined for circular shapes.
    double trueAnomaly{0.0};

    /// Argument of latitude, in radians: the substitute for argp + nu when the orbit is
    /// circular but inclined.
    double argumentOfLatitude{0.0};
    /// Longitude of periapsis, in radians: the substitute for raan + argp when the orbit is
    /// eccentric but equatorial.
    double longitudeOfPeriapsis{0.0};
    /// True longitude, in radians: the substitute for raan + argp + nu when the orbit is both
    /// circular and equatorial. Defined for every non-degenerate orbit.
    double trueLongitude{0.0};

    /// Periapsis and apoapsis radii, in metres. Apoapsis is infinite for parabolic and
    /// negative-by-convention for hyperbolic orbits, which do not have one.
    double periapsisRadius{0.0};
    double apoapsisRadius{0.0};

    /// Orbital period, in seconds. Infinite for a non-elliptical orbit.
    double periodSeconds{0.0};
};

/// Eccentricity below which an orbit is reported as circular.
///
/// This is a *classification* threshold, not an accuracy claim, and it is deliberately loose.
/// A numerically integrated circular orbit picks up an eccentricity of order the integrator's
/// relative error -- 1e-10 or so -- purely from truncation, and reporting that as an eccentric
/// orbit with a meaningful argument of periapsis would be worse than useless. Anything above
/// this genuinely has a periapsis worth naming.
inline constexpr double kCircularEccentricityThreshold = 1.0e-8;

/// Inclination in radians below which an orbit is reported as equatorial. Same reasoning.
inline constexpr double kEquatorialInclinationThreshold = 1.0e-8;

/// Derives classical elements from a Cartesian state.
///
/// The state's axes define the reference plane and the reference direction: inclination and
/// node are measured against the z axis and x axis of whatever frame the state is expressed
/// in. Passing a body-fixed state here produces plausible numbers that mean nothing, which is
/// why the frame pairing lives at this library's API boundary and not in TwoBodyState.
[[nodiscard]] OrbitalElements elementsFromState(const TwoBodyState& state,
                                                double gravitationalParameter) noexcept;

/// Rebuilds a Cartesian state from classical elements.
///
/// Exists so the element conversion can be round-tripped rather than merely inspected. An
/// element set that cannot reproduce the state it came from is wrong in a way that reading the
/// numbers will not reveal, and A3's reference-orbit result is reported in elements.
[[nodiscard]] TwoBodyState stateFromElements(const OrbitalElements& elements,
                                             double gravitationalParameter) noexcept;

} // namespace sol::proto::orbit
