#pragma once

#include "Sol/Proto/Frames/Units.h"
#include "Sol/Proto/Frames/Vec3.h"

#include <string>

namespace sol::proto::frames {

/// A geodetic coordinate on a named reference ellipsoid.
///
/// ADR 0008 fixes the P1 launch anchor as 28.0 degrees north, 80.5 degrees west, 5 m above the
/// reference ellipsoid. A height is meaningless without the surface it is measured from, and A2
/// measured that two defensible ellipsoids move the same anchor 0.403 m -- 400 times the
/// position budget. ADR 0008 consequently requires the datum to travel with the coordinate, and
/// this type is where that requirement is enforced rather than remembered.
struct Geodetic {
    Radians latitude{};
    /// East-positive longitude, matching ADR 0008's `-80.5` convention.
    Radians longitude{};
    /// Height above the reference ellipsoid, in metres.
    double heightMetres{0.0};
};

/// A biaxial reference ellipsoid.
class Ellipsoid {
public:
    /// @param name             Identifier recorded in evidence, e.g. "IAU/pck00011 Earth".
    /// @param equatorialRadius Semi-major axis, in metres.
    /// @param polarRadius      Semi-minor axis, in metres.
    Ellipsoid(std::string name, double equatorialRadiusMetres, double polarRadiusMetres);

    /// The IAU Earth ellipsoid built from a pinned planetary-constants kernel's BODY399_RADII.
    /// This is the shape A2 treats as authoritative, because it is the value that arrives with
    /// a checksum from the source ADR 0008 names.
    [[nodiscard]] static Ellipsoid fromRadiiKilometres(std::string name,
                                                       double equatorialKm,
                                                       double polarKm);

    /// WGS84, for comparison only.
    ///
    /// Present so the increment can measure what the datum choice costs, not because the
    /// project has adopted it. Its constants are definitions of the WGS84 system, so they are
    /// written here rather than read from a kernel; no NAIF generic kernel supplies them.
    [[nodiscard]] static Ellipsoid wgs84();

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] double equatorialRadiusMetres() const noexcept { return m_equatorialRadius; }
    [[nodiscard]] double polarRadiusMetres() const noexcept { return m_polarRadius; }
    [[nodiscard]] double flattening() const noexcept { return m_flattening; }
    [[nodiscard]] double inverseFlattening() const noexcept { return 1.0 / m_flattening; }
    [[nodiscard]] double firstEccentricitySquared() const noexcept { return m_eccentricitySquared; }

    /// Geodetic coordinate to a body-fixed cartesian vector, in metres.
    [[nodiscard]] PositionMetres toBodyFixed(const Geodetic& geodetic) const noexcept;

    /// Body-fixed cartesian vector back to a geodetic coordinate.
    ///
    /// Solved by Newton iteration on the parametric latitude rather than by Bowring's
    /// single-step approximation. Bowring is accurate to roughly a millimetre at terrestrial
    /// altitudes -- which is the entire A2 position budget, so it would consume the tolerance
    /// this conversion is being measured against. The iteration converges to the double
    /// rounding limit in a handful of steps and its cost is irrelevant here, because the
    /// inverse runs at reporting boundaries, not per frame conversion.
    ///
    /// @param iterationsUsed  Optional out-parameter recording the iteration count, so the
    ///                        evidence can state convergence behaviour rather than assert it.
    [[nodiscard]] Geodetic fromBodyFixed(const PositionMetres& bodyFixed,
                                         int* iterationsUsed = nullptr) const noexcept;

    /// The east-north-up orthonormal basis at @p geodetic, as rows of a rotation that takes
    /// body-fixed coordinates into topocentric coordinates.
    ///
    /// Depends only on latitude and longitude: the basis is the same at any height along the
    /// ellipsoid normal, which is why a vehicle climbing to 200 km does not rotate its local
    /// horizon frame.
    [[nodiscard]] Vec3 eastAxis(const Geodetic& geodetic) const noexcept;
    [[nodiscard]] Vec3 northAxis(const Geodetic& geodetic) const noexcept;
    [[nodiscard]] Vec3 upAxis(const Geodetic& geodetic) const noexcept;

private:
    std::string m_name;
    double m_equatorialRadius{0.0};
    double m_polarRadius{0.0};
    double m_flattening{0.0};
    double m_eccentricitySquared{0.0};
};

} // namespace sol::proto::frames
