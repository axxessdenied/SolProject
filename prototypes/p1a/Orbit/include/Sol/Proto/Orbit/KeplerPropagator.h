#pragma once

#include "Sol/Proto/Orbit/TwoBody.h"

namespace sol::proto::orbit {

/// What one universal-variable Kepler propagation did, not merely what it produced.
///
/// The solver is iterative, and an iterative step inside an "analytical" coast is exactly the
/// kind of detail that gets asserted away and later turns out to matter. A3 reports the
/// iteration count and the final residual for every propagation it measures, so the claim that
/// the coast is cheap and exact is a measurement rather than a definition.
struct KeplerPropagation {
    TwoBodyState state{};
    /// Newton iterations actually performed.
    int iterations{0};
    /// True when the iteration reached the relative tolerance. False means the reported state
    /// is the best the solver managed and must not be treated as a reference value.
    bool converged{false};
    /// Universal anomaly chi, in m^(1/2). Recorded because it is the quantity the iteration
    /// actually solves for, and a surprising value here explains a surprising state.
    double universalAnomaly{0.0};
    /// z = chi^2 / a. Negative for hyperbolic arcs, zero for parabolic, positive for elliptic.
    /// This is the argument the Stumpff functions are evaluated at, so it also says which side
    /// of their series threshold the propagation landed on.
    double stumpffArgument{0.0};
    /// |dchi| / |chi| at the final iteration.
    double finalRelativeStep{0.0};
};

/// Propagates @p state forward or backward by @p elapsedSeconds about a body of gravitational
/// parameter @p gravitationalParameter.
///
/// This is ADR 0011's analytical coast: a closed-form conic evaluation, exact in the accepted
/// model rather than approximate to it. The distinction matters for how A3's gates read. The
/// 100 m one-orbit gate does not compare an approximation against reality; it compares a
/// numerical integrator against the model that ADR 0011 declares authoritative, so error in
/// that comparison belongs entirely to the integrator.
///
/// The universal-variable formulation is used rather than a per-conic Kepler or Barker solve
/// because patched conics produce hyperbolic arcs at every sphere-of-influence escape and
/// near-parabolic ones at the marginal cases between escape and capture. A formulation that
/// branches on conic type has to decide which branch a marginal orbit is on, and that decision
/// is a discontinuity sitting exactly where the transition contract is most fragile. The
/// universal form has no such branch: eccentricity enters only through the sign of z.
///
/// Negative @p elapsedSeconds propagates backward and is exact in the same sense, which the
/// handoff machinery relies on when it has to place a transition at an instant it has already
/// stepped past.
[[nodiscard]] KeplerPropagation propagateKepler(const TwoBodyState& state,
                                                double gravitationalParameter,
                                                double elapsedSeconds) noexcept;

} // namespace sol::proto::orbit
