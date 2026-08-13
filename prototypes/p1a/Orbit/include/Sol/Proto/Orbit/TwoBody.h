#pragma once

#include "Sol/Proto/Frames/Vec3.h"

#include <cstdint>
#include <string>

namespace sol::proto::orbit {

/// A position and velocity about one gravitating body, in metres and metres per second.
///
/// The state is expressed relative to the central body's centre, on inertial ICRF-parallel
/// axes -- FrameId::EarthIcrf for a geocentric orbit. It carries no frame tag and no epoch of
/// its own, deliberately: this is the *interior* arithmetic type of the orbit library, in the
/// same sense that A2's Vec3 is the interior type of the frame library. Everything that
/// crosses this library's API boundary is paired with the body it is about and the campaign
/// instant it is valid at, and a bare TwoBodyState passed between subsystems is exactly the
/// mistake those pairings prevent.
///
/// Under ADR 0011 exactly one body gravitates at any instant, so "the central body" is always
/// singular and always known. That is what makes this two-vector type sufficient rather than a
/// simplification: it is the complete authoritative state of a coasting craft in the accepted
/// model.
struct TwoBodyState {
    frames::Vec3 position{};
    frames::Vec3 velocity{};
};

/// One gravitating body in the patched-conic hierarchy.
///
/// Every field is sourced or derived rather than invented. The gravitational parameter comes
/// from the pinned gm_de440 kernel, the surface radius from the pinned planetary-constants
/// kernel, and the sphere-of-influence radius from ADR 0011's Laplace formulation applied to
/// those two. The atmosphere limit is the one number A3 chooses rather than derives; see
/// BodySystem.h for why it cannot be derived from the handoff tolerance.
struct GravitationalBody {
    /// NAIF integer body code, e.g. 399 for Earth.
    int naifId{0};
    std::string name;
    /// GM, in m^3/s^2.
    double gravitationalParameter{0.0};
    /// Equatorial radius, in metres: the surface reference for altitude and the below-surface
    /// eligibility test.
    ///
    /// Equatorial and not mean, which for Earth differ by 7.1 km. The equatorial radius is the
    /// larger, so a below-surface test against it fires no later than the true surface does at
    /// any latitude -- the conservative direction for a gate that means "the craft has already
    /// arrived". ADR 0011's 140 km atmosphere limit is likewise an altitude above this radius,
    /// and says so.
    double surfaceRadiusMetres{0.0};
    /// Radius of the Laplace sphere of influence, in metres. Infinite for the hierarchy root,
    /// which by construction is not inside anything.
    double sphereOfInfluenceRadiusMetres{0.0};
    /// Radius, not altitude, above which ADR 0011 declares the propagation drag-free and the
    /// analytical coast eligible. Zero for an airless body.
    double atmosphereLimitRadiusMetres{0.0};
};

/// Gravitational acceleration at @p position, in m/s^2.
///
/// The ADR 0011 integrand in its entirety: one inverse-square term about one body. No J2, no
/// third body, no drag, no radiation pressure. A reader looking for the missing terms should
/// read ADR 0011 rather than assume this is a simplification of something larger.
[[nodiscard]] frames::Vec3 gravitationalAcceleration(const frames::Vec3& position,
                                                     double gravitationalParameter) noexcept;

/// Specific orbital energy, in J/kg: v^2/2 - mu/r.
///
/// Conserved exactly by the analytical coast and approximately by a numerical integrator, which
/// makes its drift the cheapest available measure of integrator quality. A3 reports it for every
/// integrator candidate.
[[nodiscard]] double specificEnergy(const TwoBodyState& state,
                                    double gravitationalParameter) noexcept;

/// Specific angular momentum vector, in m^2/s: r x v.
///
/// Conserved exactly under any central force, including by a numerical integrator up to
/// rounding, so its drift isolates rounding from truncation error in a way the energy does not.
[[nodiscard]] frames::Vec3 specificAngularMomentum(const TwoBodyState& state) noexcept;

/// Distance from the central body's centre, in metres.
[[nodiscard]] double radius(const TwoBodyState& state) noexcept;

/// Speed relative to the central body, in m/s.
[[nodiscard]] double speed(const TwoBodyState& state) noexcept;

/// Circular orbital speed at radius @p radiusMetres, in m/s.
[[nodiscard]] double circularSpeed(double radiusMetres, double gravitationalParameter) noexcept;

/// Period of a circular orbit at radius @p radiusMetres, in seconds.
[[nodiscard]] double circularPeriodSeconds(double radiusMetres,
                                           double gravitationalParameter) noexcept;

} // namespace sol::proto::orbit
