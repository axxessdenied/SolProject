/// Guards the A3 orbit library, as FramesSelfCheck guards the frame library and
/// HarnessSelfCheck guards the harness.
///
/// Emits no measurement report. Its gate is that every property the library claims in its
/// headers actually holds, which is the precondition for any A3 number being evidence at all.
/// A2's handoff recorded that three reporting defects were caught by reading output rather
/// than by a test; this file is where that lesson is spent, so the properties are asserted
/// against closed-form values and analytic identities rather than against previous output.
///
/// Gravitational parameters come from the pinned ADR 0008 kernel rather than from literals, so
/// a check here exercises the same constants the scenarios do.

#include "Sol/Proto/Frames/ReferenceData.h"

#include "Sol/Proto/Orbit/BodySystem.h"
#include "Sol/Proto/Orbit/CampaignClock.h"
#include "Sol/Proto/Orbit/ConicEphemeris.h"
#include "Sol/Proto/Orbit/HybridPropagator.h"
#include "Sol/Proto/Orbit/Integrator.h"
#include "Sol/Proto/Orbit/KeplerPropagator.h"
#include "Sol/Proto/Orbit/OrbitalElements.h"
#include "Sol/Proto/Orbit/Stumpff.h"
#include "Sol/Proto/Orbit/TwoBody.h"

#include "Sol/Proto/Harness/Check.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using sol::proto::CheckContext;
using namespace sol::proto::frames;
using namespace sol::proto::orbit;

constexpr double kPi = 3.1415926535897932384626433832795;

[[nodiscard]] bool bitsEqual(double a, double b) noexcept
{
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] bool statesBitIdentical(const TwoBodyState& a, const TwoBodyState& b) noexcept
{
    return bitsEqual(a.position.x, b.position.x) && bitsEqual(a.position.y, b.position.y)
        && bitsEqual(a.position.z, b.position.z) && bitsEqual(a.velocity.x, b.velocity.x)
        && bitsEqual(a.velocity.y, b.velocity.y) && bitsEqual(a.velocity.z, b.velocity.z);
}

// ------------------------------------------------------------------------------------------
// Stumpff functions
// ------------------------------------------------------------------------------------------

void checkStumpff(CheckContext& checks)
{
    // The values at the origin are the leading series terms, and both branches must agree with
    // them exactly rather than approximately, since a step of zero must be a no-op.
    checks.check(stumpffC(0.0) == 0.5, "C(0) is exactly 1/2");
    checks.check(stumpffS(0.0) == 1.0 / 6.0, "S(0) is exactly 1/6");

    // Closed-form values at z = pi^2, where sqrt(z) = pi makes both functions exact rationals
    // in pi: C = (1 - cos pi)/pi^2 = 2/pi^2 and S = (pi - sin pi)/pi^3 = 1/pi^2.
    const double zPiSquared = kPi * kPi;
    checks.check(std::abs(stumpffC(zPiSquared) - 2.0 / (kPi * kPi)) < 1.0e-15,
                 "C(pi^2) equals 2/pi^2");
    checks.check(std::abs(stumpffS(zPiSquared) - 1.0 / (kPi * kPi)) < 1.0e-15,
                 "S(pi^2) equals 1/pi^2");

    // The series and closed forms must agree at the switchover. This is the check that would
    // catch a threshold moved to where neither form is accurate -- the failure the header's
    // choice of 1.0 is reasoned about but, until here, only asserted.
    //
    // The comparison has to be between the two *forms at one argument*, not between one form at
    // two arguments. Sampling either side of the threshold and differencing measures the
    // function's own slope -- dC/dz is about -1/24 near the origin, so a 2e-6 gap in z produces
    // an 8e-8 difference that has nothing to do with which branch ran. That is what the first
    // version of this check actually measured, and it failed for exactly that reason.
    const auto closedFormC = [](double z) {
        return z > 0.0 ? (1.0 - std::cos(std::sqrt(z))) / z
                       : (std::cosh(std::sqrt(-z)) - 1.0) / (-z);
    };
    const auto closedFormS = [](double z) {
        if (z > 0.0) {
            const double root = std::sqrt(z);
            return (root - std::sin(root)) / (root * root * root);
        }
        const double root = std::sqrt(-z);
        return (std::sinh(root) - root) / (root * root * root);
    };

    // Just inside the threshold, where stumpffC and stumpffS take the series branch.
    const double justBelow = 0.999999;
    checks.check(std::abs(stumpffC(justBelow) - closedFormC(justBelow)) < 1.0e-15,
                 "the C series agrees with the closed form at the switchover");
    checks.check(std::abs(stumpffS(justBelow) - closedFormS(justBelow)) < 1.0e-15,
                 "the S series agrees with the closed form at the switchover");
    checks.check(std::abs(stumpffC(-justBelow) - closedFormC(-justBelow)) < 1.0e-15,
                 "the C series agrees with the closed form on the hyperbolic side");
    checks.check(std::abs(stumpffS(-justBelow) - closedFormS(-justBelow)) < 1.0e-15,
                 "the S series agrees with the closed form on the hyperbolic side");

    // Just outside, where the closed-form branch runs. Trivially equal unless the branch
    // condition itself is wrong, which is the point: it pins which side of the threshold each
    // form is used on.
    const double justAbove = 1.000001;
    checks.check(stumpffC(justAbove) == closedFormC(justAbove),
                 "C uses the closed form above the switchover");
    checks.check(stumpffS(justAbove) == closedFormS(justAbove),
                 "S uses the closed form above the switchover");

    // An independent hyperbolic value, where cosh and sinh are computable by hand.
    const double zHyperbolic = -4.0;
    const double root = 2.0; // sqrt(-z)
    checks.check(std::abs(stumpffC(zHyperbolic) - (std::cosh(root) - 1.0) / 4.0) < 1.0e-15,
                 "C matches its hyperbolic closed form at z = -4");
    checks.check(std::abs(stumpffS(zHyperbolic) - (std::sinh(root) - root) / 8.0) < 1.0e-15,
                 "S matches its hyperbolic closed form at z = -4");

    // Both functions are positive and decreasing on the elliptic side over the range a bounded
    // orbit visits. A sign error in the series would show here and nowhere else.
    checks.check(stumpffC(0.5) > 0.0 && stumpffC(0.5) < 0.5, "C(0.5) lies in (0, 1/2)");
    checks.check(stumpffS(0.5) > 0.0 && stumpffS(0.5) < 1.0 / 6.0, "S(0.5) lies in (0, 1/6)");
}

// ------------------------------------------------------------------------------------------
// Kepler propagation
// ------------------------------------------------------------------------------------------

/// A circular orbit of radius @p radiusMetres in the reference plane.
[[nodiscard]] TwoBodyState circularOrbit(double radiusMetres, double mu) noexcept
{
    TwoBodyState state;
    state.position = Vec3{radiusMetres, 0.0, 0.0};
    state.velocity = Vec3{0.0, circularSpeed(radiusMetres, mu), 0.0};
    return state;
}

void checkKepler(CheckContext& checks, double muEarth, double earthRadius)
{
    const double orbitRadius = earthRadius + 200.0e3;
    const TwoBodyState initial = circularOrbit(orbitRadius, muEarth);
    const double period = circularPeriodSeconds(orbitRadius, muEarth);

    // A zero-length step must return the input untouched, bit for bit. Anything else means the
    // solver perturbs a state merely by being asked about it, which would make every handoff
    // in the increment lossy for no reason.
    const KeplerPropagation zeroStep = propagateKepler(initial, muEarth, 0.0);
    checks.check(statesBitIdentical(zeroStep.state, initial),
                 "a zero-duration Kepler propagation is bit-identical to its input");
    checks.check(zeroStep.converged, "the zero-duration propagation reports convergence");

    // A full period returns to the starting state. The tolerance is in metres against an
    // orbit radius of 6.6e6 m, so this is a relative accuracy of about 1e-13.
    const KeplerPropagation fullPeriod = propagateKepler(initial, muEarth, period);
    checks.check(fullPeriod.converged, "a one-period propagation converges");
    const double returnError = distance(fullPeriod.state.position, initial.position);
    checks.check(returnError < 1.0e-6,
                 "propagating a circular orbit by one period returns to the start within 1 um");

    // Half a period is the antipode, at the same radius and opposite velocity.
    const KeplerPropagation halfPeriod = propagateKepler(initial, muEarth, 0.5 * period);
    checks.check(halfPeriod.converged, "a half-period propagation converges");
    checks.check(std::abs(radius(halfPeriod.state) - orbitRadius) < 1.0e-6,
                 "the half-period state is at the same radius");
    checks.check(distance(halfPeriod.state.position, -initial.position) < 1.0e-6,
                 "the half-period state is antipodal");

    // Forward then backward is the identity, to within rounding. This is the property the
    // handoff machinery relies on when it has to place a transition at an instant it has
    // already stepped past.
    const KeplerPropagation forward = propagateKepler(initial, muEarth, 1234.5);
    const KeplerPropagation back = propagateKepler(forward.state, muEarth, -1234.5);
    checks.check(distance(back.state.position, initial.position) < 1.0e-6,
                 "forward then backward Kepler propagation recovers the position within 1 um");
    checks.check(distance(back.state.velocity, initial.velocity) < 1.0e-9,
                 "forward then backward Kepler propagation recovers the velocity within 1 nm/s");

    // Conserved quantities are conserved by construction in a conic evaluation, so a
    // measurable drift here means the f-and-g coefficients disagree with each other.
    const double energyBefore = specificEnergy(initial, muEarth);
    const double energyAfter = specificEnergy(forward.state, muEarth);
    checks.check(std::abs((energyAfter - energyBefore) / energyBefore) < 1.0e-14,
                 "Kepler propagation conserves specific energy to 1e-14 relative");
    const double momentumBefore = length(specificAngularMomentum(initial));
    const double momentumAfter = length(specificAngularMomentum(forward.state));
    checks.check(std::abs((momentumAfter - momentumBefore) / momentumBefore) < 1.0e-14,
                 "Kepler propagation conserves specific angular momentum to 1e-14 relative");

    // An elliptical orbit, so the circular case is not the only one exercised. Periapsis at
    // 200 km, apoapsis near geostationary radius.
    TwoBodyState elliptical;
    elliptical.position = Vec3{orbitRadius, 0.0, 0.0};
    elliptical.velocity = Vec3{0.0, 10.15e3, 0.0};
    const OrbitalElements ellipticalElements = elementsFromState(elliptical, muEarth);
    checks.check(ellipticalElements.eccentricity > 0.5 && ellipticalElements.eccentricity < 0.8,
                 "the elliptical test case is genuinely eccentric");
    const KeplerPropagation ellipticalPeriod =
        propagateKepler(elliptical, muEarth, ellipticalElements.periodSeconds);
    checks.check(distance(ellipticalPeriod.state.position, elliptical.position) < 1.0e-3,
                 "an eccentric orbit returns to its start after one period within 1 mm");

    // A hyperbolic arc, which is what every sphere-of-influence escape is. Escape speed at
    // this radius is sqrt(2 mu / r); 1.3x that is comfortably hyperbolic.
    TwoBodyState hyperbolic;
    hyperbolic.position = Vec3{orbitRadius, 0.0, 0.0};
    hyperbolic.velocity = Vec3{0.0, 1.3 * std::sqrt(2.0 * muEarth / orbitRadius), 0.0};
    const KeplerPropagation hyperbolicForward = propagateKepler(hyperbolic, muEarth, 3600.0);
    checks.check(hyperbolicForward.converged, "a hyperbolic propagation converges");
    checks.check(hyperbolicForward.stumpffArgument < 0.0,
                 "the hyperbolic case is solved with a negative Stumpff argument");
    checks.check(radius(hyperbolicForward.state) > orbitRadius,
                 "the hyperbolic arc recedes from the central body");
    const KeplerPropagation hyperbolicBack =
        propagateKepler(hyperbolicForward.state, muEarth, -3600.0);
    checks.check(distance(hyperbolicBack.state.position, hyperbolic.position) < 1.0e-3,
                 "a hyperbolic arc round-trips within 1 mm");

    // A degenerate input must be refused rather than answered. The eligibility rules depend on
    // the solver reporting failure instead of producing a plausible vector.
    TwoBodyState degenerate;
    const KeplerPropagation degenerateResult = propagateKepler(degenerate, muEarth, 100.0);
    checks.check(!degenerateResult.converged,
                 "a zero-radius state is refused rather than propagated");
}

// ------------------------------------------------------------------------------------------
// Orbital elements
// ------------------------------------------------------------------------------------------

void checkElements(CheckContext& checks, double muEarth, double earthRadius)
{
    const double orbitRadius = earthRadius + 200.0e3;

    // The A3 reference case is circular and equatorial, which is the doubly degenerate corner
    // of the classical set. It must be reported as such rather than given a fabricated
    // periapsis.
    const TwoBodyState circular = circularOrbit(orbitRadius, muEarth);
    const OrbitalElements circularElements = elementsFromState(circular, muEarth);
    checks.check(circularElements.shape == OrbitShape::CircularEquatorial,
                 "a circular equatorial orbit is classified as such");
    checks.check(circularElements.eccentricity < kCircularEccentricityThreshold,
                 "the circular case has eccentricity below the classification threshold");
    checks.check(std::abs(circularElements.semiMajorAxis - orbitRadius) < 1.0e-6,
                 "the circular case recovers its radius as the semi-major axis");
    checks.check(std::abs(circularElements.periodSeconds
                          - circularPeriodSeconds(orbitRadius, muEarth))
                     < 1.0e-6,
                 "the element period agrees with the closed-form circular period");
    checks.check(circularElements.argumentOfPeriapsis == 0.0
                     && circularElements.trueAnomaly == 0.0,
                 "the undefined angles of a circular equatorial orbit are left at zero rather "
                 "than fabricated");

    // An inclined circular orbit: node defined, periapsis not.
    TwoBodyState inclined;
    inclined.position = Vec3{orbitRadius, 0.0, 0.0};
    const double speedCircular = circularSpeed(orbitRadius, muEarth);
    inclined.velocity = Vec3{0.0, speedCircular * std::cos(0.9), speedCircular * std::sin(0.9)};
    const OrbitalElements inclinedElements = elementsFromState(inclined, muEarth);
    checks.check(inclinedElements.shape == OrbitShape::CircularInclined,
                 "a circular inclined orbit is classified as such");
    checks.check(std::abs(inclinedElements.inclination - 0.9) < 1.0e-12,
                 "the inclination is recovered from the velocity tilt");

    // A general orbit round-trips through the element conversion.
    TwoBodyState general;
    general.position = Vec3{orbitRadius * 0.9, orbitRadius * 0.4, orbitRadius * 0.2};
    general.velocity = Vec3{-2.0e3, 6.0e3, 2.5e3};
    const OrbitalElements generalElements = elementsFromState(general, muEarth);
    checks.check(generalElements.shape == OrbitShape::General,
                 "an eccentric inclined orbit is classified as general");
    const TwoBodyState rebuilt = stateFromElements(generalElements, muEarth);
    checks.check(distance(rebuilt.position, general.position) < 1.0e-6,
                 "a general orbit round-trips through elements within 1 um");
    checks.check(distance(rebuilt.velocity, general.velocity) < 1.0e-9,
                 "a general orbit round-trips its velocity within 1 nm/s");

    // The degenerate cases must round-trip too, through their substitute angles. This is the
    // check that a substitution written in one direction was also written in the other.
    const TwoBodyState circularRebuilt = stateFromElements(circularElements, muEarth);
    checks.check(distance(circularRebuilt.position, circular.position) < 1.0e-6,
                 "a circular equatorial orbit round-trips through its true longitude");
    const TwoBodyState inclinedRebuilt = stateFromElements(inclinedElements, muEarth);
    checks.check(distance(inclinedRebuilt.position, inclined.position) < 1.0e-6,
                 "a circular inclined orbit round-trips through its argument of latitude");

    // Periapsis and apoapsis bracket the current radius for a bound orbit.
    checks.check(generalElements.periapsisRadius <= radius(general)
                     && radius(general) <= generalElements.apoapsisRadius,
                 "the current radius lies between periapsis and apoapsis");

    // A radial trajectory has no orbital plane and must be reported as degenerate.
    TwoBodyState radial;
    radial.position = Vec3{orbitRadius, 0.0, 0.0};
    radial.velocity = Vec3{1.0e3, 0.0, 0.0};
    checks.check(elementsFromState(radial, muEarth).shape == OrbitShape::Degenerate,
                 "a radial trajectory is reported as degenerate rather than given elements");
}

// ------------------------------------------------------------------------------------------
// Integrators
// ------------------------------------------------------------------------------------------

void checkIntegrators(CheckContext& checks, double muEarth, double earthRadius)
{
    const double orbitRadius = earthRadius + 200.0e3;
    const TwoBodyState initial = circularOrbit(orbitRadius, muEarth);

    for (const IntegratorKind kind : kIntegratorCandidates) {
        const std::string name{integratorName(kind)};

        // A single small step must agree with the exact conic to far better than the step
        // itself moves the craft, or the integrator is not solving the same problem.
        const double step = 0.5;
        const TwoBodyState numerical = integrateStep(kind, initial, muEarth, step);
        const TwoBodyState exact = propagateKepler(initial, muEarth, step).state;
        const double travelled = distance(exact.position, initial.position);
        const double stepError = distance(numerical.position, exact.position);
        checks.check(stepError < 1.0e-6 * travelled,
                     name + ": one 0.5 s step agrees with the exact conic to 1e-6 of the "
                            "distance travelled");

        // Convergence order. Halving the step must reduce the error over a fixed interval by
        // 2^order.
        //
        // The step sizes are deliberately coarse. Measuring convergence needs truncation error
        // to dominate, and the Kepler reference this compares against carries its own rounding
        // of order 1e-13 relative -- about 7e-7 m at this radius. At one-second steps RK4's
        // truncation error over 100 s is *below* that floor, so the ratio measured is the ratio
        // of two noise samples and means nothing. That is what the first version of this check
        // did, and it failed for that reason rather than because RK4 is not fourth order.
        // Sixteen- and eight-second steps over 800 s put every candidate's error four or more
        // orders of magnitude above the floor.
        const double interval = 800.0;
        const auto errorAtStep = [&](double h) {
            TwoBodyState state = initial;
            const auto stepCount = static_cast<int>(interval / h);
            for (int i = 0; i < stepCount; ++i) {
                state = integrateStep(kind, state, muEarth, h);
            }
            const TwoBodyState reference = propagateKepler(initial, muEarth, interval).state;
            return distance(state.position, reference.position);
        };
        const double coarseError = errorAtStep(16.0);
        const double fineError = errorAtStep(8.0);
        const double observedRatio = coarseError / fineError;
        const double expectedRatio = kind == IntegratorKind::VelocityVerlet ? 4.0 : 16.0;
        // A wide band, deliberately. The point is to distinguish second order from fourth, not
        // to measure the constant.
        checks.check(observedRatio > 0.5 * expectedRatio && observedRatio < 2.0 * expectedRatio,
                     name + ": halving the step changes the error by the ratio its order "
                            "predicts (expected near " + std::to_string(expectedRatio)
                         + ", observed " + std::to_string(observedRatio) + ")");

        // Angular momentum is conserved under any central force, so every integrator must hold
        // it to near rounding regardless of order. An integrator that loses it has a bug in the
        // acceleration direction rather than a truncation error.
        TwoBodyState state = initial;
        for (int i = 0; i < 200; ++i) {
            state = integrateStep(kind, state, muEarth, 1.0);
        }
        const double momentumBefore = length(specificAngularMomentum(initial));
        const double momentumAfter = length(specificAngularMomentum(state));
        checks.check(std::abs((momentumAfter - momentumBefore) / momentumBefore) < 1.0e-12,
                     name + ": conserves specific angular momentum over 200 steps");

        checks.check(accelerationEvaluationsPerStep(kind) > 0,
                     name + ": reports a nonzero acceleration cost per step");
    }

    // The symplectic claim, which is the reason three candidates exist. Over a long run the
    // symplectic integrators' energy error must stay bounded while RK4's accumulates. Measured
    // as the spread of the energy error rather than its endpoint, because a secular error can
    // pass through zero.
    const double period = circularPeriodSeconds(orbitRadius, muEarth);
    const double initialEnergy = specificEnergy(initial, muEarth);
    const auto energyDriftOverOrbits = [&](IntegratorKind kind, int orbits) {
        TwoBodyState state = initial;
        const double step = 4.0;
        const auto stepCount = static_cast<long long>(period * orbits / step);
        double worst = 0.0;
        for (long long i = 0; i < stepCount; ++i) {
            state = integrateStep(kind, state, muEarth, step);
            const double relative =
                std::abs((specificEnergy(state, muEarth) - initialEnergy) / initialEnergy);
            worst = std::max(worst, relative);
        }
        return worst;
    };
    const double verletDriftShort = energyDriftOverOrbits(IntegratorKind::VelocityVerlet, 1);
    const double verletDriftLong = energyDriftOverOrbits(IntegratorKind::VelocityVerlet, 8);
    // Bounded means eight orbits are not eight times worse than one. A factor of two of slack
    // covers the oscillation's phase.
    checks.check(verletDriftLong < 2.0 * verletDriftShort,
                 "velocity Verlet's energy error is bounded rather than accumulating over "
                 "eight orbits");

    const double yoshidaDriftShort = energyDriftOverOrbits(IntegratorKind::Yoshida4, 1);
    const double yoshidaDriftLong = energyDriftOverOrbits(IntegratorKind::Yoshida4, 8);
    checks.check(yoshidaDriftLong < 2.0 * yoshidaDriftShort,
                 "Yoshida 4 energy error is bounded rather than accumulating over eight orbits");

    checks.check(isSymplectic(IntegratorKind::VelocityVerlet)
                     && isSymplectic(IntegratorKind::Yoshida4)
                     && !isSymplectic(IntegratorKind::RungeKutta4),
                 "the symplectic flag matches the integrators' construction");
}

// ------------------------------------------------------------------------------------------
// Body hierarchy and spheres of influence
// ------------------------------------------------------------------------------------------

void checkBodySystem(CheckContext& checks, const BodySystem& system)
{
    checks.check(system.isRoot(kNaifSun), "the Sun is the root of the gravitational hierarchy");
    checks.check(system.primaryOf(kNaifEarth) == kNaifSun, "Earth's primary is the Sun");
    checks.check(system.primaryOf(kNaifMoon) == kNaifEarth, "the Moon's primary is Earth");
    checks.check(!system.isRoot(kNaifEarth) && !system.isRoot(kNaifMoon),
                 "only one body is the root");

    // Published Laplace radii for comparison: Earth's sphere of influence is about 9.24e8 m and
    // the Moon's about 6.6e7 m. The bands are wide because the value here is derived from the
    // pinned fixtures rather than copied, and the two differ slightly in which mass is used for
    // Earth -- this library uses the Earth-Moon system's mass, since that is what follows the
    // heliocentric conic. Narrow enough to catch an exponent or unit error, which is what a
    // sanity band is for.
    const double earthSoi = system.sphereOfInfluenceRadius(kNaifEarth);
    const double moonSoi = system.sphereOfInfluenceRadius(kNaifMoon);
    checks.check(earthSoi > 9.0e8 && earthSoi < 9.5e8,
                 "Earth's Laplace sphere of influence lands near the published 9.24e8 m");
    checks.check(moonSoi > 6.4e7 && moonSoi < 6.8e7,
                 "the Moon's Laplace sphere of influence lands near the published 6.6e7 m");
    checks.check(moonSoi < earthSoi, "the Moon's sphere of influence nests inside Earth's");

    checks.check(std::isinf(system.sphereOfInfluenceRadius(kNaifSun)),
                 "the root body's sphere of influence is unbounded");

    // The Moon is about 1/81 of Earth's mass, so its share of the Earth-Moon system is near
    // 0.0121.
    checks.check(system.moonMassFraction() > 0.0120 && system.moonMassFraction() < 0.0123,
                 "the Moon's mass fraction of the Earth-Moon system is near 0.0121");

    const GravitationalBody& earth = system.body(kNaifEarth);
    checks.check(std::abs(earth.atmosphereLimitRadiusMetres - earth.meanRadiusMetres
                          - kEarthAtmosphereLimitAltitudeMetres)
                     < 1.0e-6,
                 "Earth's atmosphere limit is its equatorial radius plus the chosen altitude");
    checks.check(system.body(kNaifMoon).atmosphereLimitRadiusMetres == 0.0,
                 "the Moon is airless, so its atmosphere limit is exactly zero");

    // The Moon must sit well inside Earth's sphere of influence, or the hierarchy is incoherent.
    checks.check(system.semiMajorAxisAboutPrimary(kNaifMoon) < earthSoi,
                 "the Moon's orbit lies inside Earth's sphere of influence");
}

// ------------------------------------------------------------------------------------------
// Conic ephemeris
// ------------------------------------------------------------------------------------------

void checkEphemeris(CheckContext& checks, const ReferenceData& data, const BodySystem& system,
                    const ConicEphemeris& ephemeris)
{
    const TdbEpoch anchor = ephemeris.anchorEpoch();
    checks.check(anchor.secondsPastJ2000() == data.fixtureEpoch().secondsPastJ2000(),
                 "the ephemeris is anchored at the fixtures' own epoch");

    // At the anchor the conic model must reproduce the fixtures it was built from. It cannot do
    // so bit for bit: Earth's position is reconstructed as barycentre minus the Moon's mass
    // share of the geocentric vector, and the Horizons barycentre fixture was formed with the
    // DE441 mass ratio rather than with gm_de440's. The residual is therefore a real
    // measurement of how well two pinned data products agree, and it is asserted loosely
    // enough to be a sanity bound rather than a restatement of the number.
    for (const int naifId : {kNaifSun, kNaifEarthMoonBarycentre, kNaifEarth, kNaifMoon}) {
        const TwoBodyState conic = ephemeris.barycentricState(naifId, anchor);
        const TwoBodyState linear = ephemeris.linearBarycentricState(naifId, anchor);
        const double positionResidual = distance(conic.position, linear.position);
        checks.check(positionResidual < 1.0,
                     "at the anchor epoch the conic ephemeris reproduces the fixture for NAIF "
                         + std::to_string(naifId) + " within 1 m (residual "
                         + std::to_string(positionResidual) + " m)");
    }

    // The conic and the linear model must diverge as the interval grows, or the replacement was
    // pointless. Over one day Earth's heliocentric path curves by tens of thousands of
    // kilometres away from its tangent.
    const TdbEpoch oneDayLater = anchor.advancedBy(Seconds::fromDays(1.0));
    const double divergence = distance(ephemeris.barycentricState(kNaifEarth, oneDayLater).position,
                                       ephemeris.linearBarycentricState(kNaifEarth, oneDayLater)
                                           .position);
    checks.check(divergence > 1.0e6,
                 "over one day the conic ephemeris departs from linear extrapolation by more "
                 "than 1000 km, which is why A3 replaced it");

    // The direct conic and the differenced barycentric path must agree, or the two access
    // routes disagree about where the Moon is.
    const TwoBodyState moonDirect = ephemeris.stateRelativeTo(kNaifMoon, kNaifEarth, oneDayLater);
    const TwoBodyState moonDifferenced =
        TwoBodyState{ephemeris.barycentricState(kNaifMoon, oneDayLater).position
                         - ephemeris.barycentricState(kNaifEarth, oneDayLater).position,
                     ephemeris.barycentricState(kNaifMoon, oneDayLater).velocity
                         - ephemeris.barycentricState(kNaifEarth, oneDayLater).velocity};
    checks.check(distance(moonDirect.position, moonDifferenced.position) < 1.0e-3,
                 "the direct and differenced routes to the Moon's geocentric position agree "
                 "within 1 mm");

    // The Moon's geocentric distance must stay in its real range over a month, which is the
    // cheapest check that the conic is the Moon's orbit and not something else.
    double minimumDistance = 1.0e30;
    double maximumDistance = 0.0;
    for (int hour = 0; hour < 24 * 30; ++hour) {
        const TdbEpoch when = anchor.advancedBy(Seconds::fromSeconds(hour * 3600.0));
        const double d = length(ephemeris.stateRelativeTo(kNaifMoon, kNaifEarth, when).position);
        minimumDistance = std::min(minimumDistance, d);
        maximumDistance = std::max(maximumDistance, d);
    }
    checks.check(minimumDistance > 3.5e8 && maximumDistance < 4.1e8,
                 "the Moon's geocentric distance stays within its real perigee/apogee range "
                 "over 30 days");

    // The frame snapshot must differ from A2's only in the celestial origins. The launch-site
    // boundary is built by the same A2 code, so it must be identical.
    VehicleState vehicle;
    const FrameGraphSnapshot a2Snapshot = buildSnapshot(data, oneDayLater, vehicle);
    const FrameGraphSnapshot a3Snapshot = ephemeris.buildSnapshot(data, oneDayLater, vehicle);
    const auto index = [](FrameId frame) { return static_cast<std::size_t>(frame); };
    checks.check(a2Snapshot.parentToFrame[index(FrameId::LaunchSiteEnu)].originInParent.x
                     == a3Snapshot.parentToFrame[index(FrameId::LaunchSiteEnu)].originInParent.x,
                 "the A3 snapshot leaves A2's launch-site boundary untouched");
    checks.check(a2Snapshot.parentToFrame[index(FrameId::EarthIcrf)].originInParent.x
                     != a3Snapshot.parentToFrame[index(FrameId::EarthIcrf)].originInParent.x,
                 "the A3 snapshot does change Earth's origin, which is the point of it");

    (void)system;
}

// ------------------------------------------------------------------------------------------
// Campaign clock
// ------------------------------------------------------------------------------------------

void checkCampaignClock(CheckContext& checks, const ReferenceData& data)
{
    const CampaignDuration oneSecond = CampaignDuration::fromNanoseconds(1'000'000'000);
    const CampaignDuration oneHour = oneSecond * 3600;
    checks.check(oneHour.nanoseconds() == 3'600'000'000'000LL,
                 "an hour of campaign time is an exact integer nanosecond count");
    checks.check(oneHour.seconds() == 3600.0,
                 "the nanosecond count converts to seconds exactly at hour scale");

    // The property the warp-equivalence gate depends on: reaching an instant by many small
    // increments and by one large one must land on exactly the same integer.
    CampaignInstant stepped;
    for (int i = 0; i < 3600; ++i) {
        stepped = stepped + oneSecond;
    }
    const CampaignInstant jumped = CampaignInstant{} + oneHour;
    checks.check(stepped == jumped,
                 "3600 one-second increments reach exactly the same instant as one hour");

    // A double clock would not manage that. Demonstrated rather than asserted, because the
    // whole reason ADR 0010 requires integer accumulation is that the failure is invisible
    // until it is measured.
    double doubleClock = 0.0;
    for (int i = 0; i < 3600; ++i) {
        doubleClock += 0.1;
    }
    checks.check(doubleClock != 360.0,
                 "the negative control holds: 3600 accumulations of 0.1 s in a double do not "
                 "reach 360 s exactly");

    const TdbEpoch campaignEpoch =
        data.timeScales().utcToTdb(UtcDateTime{2026, 1, 1, 0, 0, 0.0});
    const TdbEpoch oneHourIn = toTdb(jumped, campaignEpoch);
    checks.check(std::abs(oneHourIn.secondsPastJ2000() - campaignEpoch.secondsPastJ2000() - 3600.0)
                     < 1.0e-9,
                 "converting a campaign instant to TDB advances the ephemeris scale by the same "
                 "interval");
}

// ------------------------------------------------------------------------------------------
// Hybrid propagator
// ------------------------------------------------------------------------------------------

/// A craft in a circular orbit at @p altitudeMetres above Earth's equatorial radius.
[[nodiscard]] CraftState circularCraft(const BodySystem& system, double altitudeMetres,
                                       CampaignInstant instant)
{
    const GravitationalBody& earth = system.body(kNaifEarth);
    const double r = earth.meanRadiusMetres + altitudeMetres;

    CraftState craft;
    craft.state.position = Vec3{r, 0.0, 0.0};
    craft.state.velocity = Vec3{0.0, circularSpeed(r, earth.gravitationalParameter), 0.0};
    craft.centralBodyNaifId = kNaifEarth;
    craft.instant = instant;
    craft.regime = PropagationRegime::LocalNumerical;
    return craft;
}

void checkHybridPropagator(CheckContext& checks, const BodySystem& system,
                           const ConicEphemeris& ephemeris, const ReferenceData& data)
{
    HybridPropagator::Settings settings;
    settings.campaignEpochTdb = data.timeScales().utcToTdb(UtcDateTime{2026, 1, 1, 0, 0, 0.0});
    settings.integrator = IntegratorKind::Yoshida4;
    const HybridPropagator propagator{system, ephemeris, settings};

    const CraftState orbiting = circularCraft(system, 200.0e3, CampaignInstant{});

    // --- eligibility -------------------------------------------------------------------
    checks.check(propagator.coastEligibility(orbiting).has_value(),
                 "a 200 km circular orbit is eligible for analytical coast");

    CraftState thrusting = orbiting;
    thrusting.thrustActive = true;
    const auto thrustResult = propagator.coastEligibility(thrusting);
    checks.check(!thrustResult.has_value()
                     && thrustResult.error() == HandoffRejection::ThrustActive,
                 "a thrusting craft is refused the analytical coast, explicitly");

    const CraftState lowFlying = circularCraft(system, 100.0e3, CampaignInstant{});
    const auto atmosphereResult = propagator.coastEligibility(lowFlying);
    checks.check(!atmosphereResult.has_value()
                     && atmosphereResult.error() == HandoffRejection::InsideAtmosphere,
                 "a craft below the atmosphere limit is refused the analytical coast, "
                 "explicitly");

    CraftState radialFall = orbiting;
    radialFall.state.velocity = Vec3{-100.0, 0.0, 0.0};
    const auto degenerateResult = propagator.coastEligibility(radialFall);
    checks.check(!degenerateResult.has_value()
                     && degenerateResult.error() == HandoffRejection::DegenerateConic,
                 "a radial trajectory is refused the analytical coast, explicitly");

    CraftState farAway = orbiting;
    farAway.state.position = Vec3{2.0e9, 0.0, 0.0};
    const auto outsideResult = propagator.coastEligibility(farAway);
    checks.check(!outsideResult.has_value()
                     && outsideResult.error()
                            == HandoffRejection::OutsideCentralBodySphereOfInfluence,
                 "a craft outside its claimed sphere of influence is refused, explicitly");

    CraftState underground = orbiting;
    underground.state.position = Vec3{1.0e6, 0.0, 0.0};
    const auto belowResult = propagator.coastEligibility(underground);
    checks.check(!belowResult.has_value()
                     && belowResult.error() == HandoffRejection::BelowSurface,
                 "a craft below the surface is refused, explicitly");

    CraftState unknownBody = orbiting;
    unknownBody.centralBodyNaifId = 599;
    const auto unknownResult = propagator.coastEligibility(unknownBody);
    checks.check(!unknownResult.has_value()
                     && unknownResult.error() == HandoffRejection::UnknownCentralBody,
                 "an unknown central body is refused, explicitly");

    // --- handoff ------------------------------------------------------------------------
    const auto coasting = propagator.beginCoast(orbiting);
    checks.check(coasting.has_value(), "beginCoast succeeds for an eligible craft");
    checks.check(statesBitIdentical(coasting.value().state, orbiting.state),
                 "beginning a coast does not alter the state by even one bit");
    checks.check(coasting.value().anchor.centralBodyNaifId == kNaifEarth
                     && coasting.value().anchor.instant == orbiting.instant,
                 "the coast anchor records the instant and body it was taken at");

    checks.check(!propagator.beginCoast(coasting.value()).has_value(),
                 "beginning a coast twice is refused");

    const auto returned = propagator.endCoast(coasting.value());
    checks.check(returned.has_value(), "endCoast succeeds for a coasting craft");
    checks.check(statesBitIdentical(returned.value().state, orbiting.state),
                 "a coast begun and immediately ended returns a bit-identical state");
    checks.check(!propagator.endCoast(orbiting).has_value(),
                 "ending a coast that never began is refused");

    // Evaluating a coast at its own anchor instant must be the identity. This is the property
    // that makes the regime-change discontinuity structurally zero rather than merely small.
    const auto atAnchor = propagator.evaluateCoastAt(coasting.value(), orbiting.instant);
    checks.check(atAnchor.has_value()
                     && statesBitIdentical(atAnchor.value().state, orbiting.state),
                 "evaluating a coast at its anchor instant is the exact identity");

    // Cycling in and out of the coast must not accumulate error, which is the hybrid analogue
    // of A2's iterated conversion drift.
    CraftState cycling = orbiting;
    for (int i = 0; i < 1000; ++i) {
        cycling = propagator.beginCoast(cycling).value();
        cycling = propagator.endCoast(cycling).value();
    }
    checks.check(statesBitIdentical(cycling.state, orbiting.state),
                 "1000 coast begin/end cycles leave the state bit-identical");

    // --- rebase ------------------------------------------------------------------------
    const auto toSun = propagator.rebaseTo(orbiting, kNaifSun);
    checks.check(toSun.has_value(), "rebasing to the Sun succeeds");
    checks.check(toSun.value().centralBodyNaifId == kNaifSun,
                 "the rebased craft records its new central body");
    checks.check(radius(toSun.value().state) > 1.4e11,
                 "an Earth-orbiting craft expressed heliocentrically is about 1 AU out");
    const auto backToEarth = propagator.rebaseTo(toSun.value(), kNaifEarth);
    checks.check(backToEarth.has_value(), "rebasing back to Earth succeeds");
    const double rebaseError = distance(backToEarth.value().state.position, orbiting.state.position);
    checks.check(rebaseError < 1.0,
                 "a rebase round trip through the Sun preserves position within 1 m (error "
                     + std::to_string(rebaseError) + " m)");

    // --- advance ------------------------------------------------------------------------
    const double period = circularPeriodSeconds(
        radius(orbiting.state), system.body(kNaifEarth).gravitationalParameter);
    const CampaignInstant oneOrbitLater =
        orbiting.instant + CampaignDuration::fromSecondsRounded(period);

    const AdvanceResult coastedOrbit = propagator.advanceTo(
        propagator.beginCoast(orbiting).value(), oneOrbitLater,
        CampaignDuration::fromSecondsRounded(60.0));
    checks.check(coastedOrbit.craft.instant == oneOrbitLater,
                 "an advance reaches exactly its target instant");
    checks.check(coastedOrbit.events.empty(),
                 "coasting one 200 km orbit crosses no sphere-of-influence boundary");
    checks.check(coastedOrbit.integratorSteps == 0,
                 "a coasting advance performs no integrator steps");
    checks.check(coastedOrbit.coastEvaluationFailures == 0,
                 "no coast evaluation was refused");
    checks.check(distance(coastedOrbit.craft.state.position, orbiting.state.position) < 1.0,
                 "a coasted orbit returns to its starting position within 1 m");

    // Warp invariance of the anchored coast: the tick size must not change the answer at all.
    const AdvanceResult fineTick =
        propagator.advanceTo(propagator.beginCoast(orbiting).value(), oneOrbitLater,
                             CampaignDuration::fromSecondsRounded(1.0));
    const AdvanceResult coarseTick =
        propagator.advanceTo(propagator.beginCoast(orbiting).value(), oneOrbitLater,
                             CampaignDuration::fromSecondsRounded(1000.0));
    checks.check(statesBitIdentical(fineTick.craft.state, coarseTick.craft.state),
                 "an anchored coast reaches a bit-identical state at 1 s and 1000 s ticks");

    // The stepped mode must not be warp-invariant, or the two modes are not actually different
    // and the architectural comparison A3 reports would be empty.
    HybridPropagator::Settings steppedSettings = settings;
    steppedSettings.coastEvaluation = CoastEvaluation::SteppedFromPrevious;
    const HybridPropagator steppedPropagator{system, ephemeris, steppedSettings};
    const AdvanceResult steppedFine =
        steppedPropagator.advanceTo(steppedPropagator.beginCoast(orbiting).value(), oneOrbitLater,
                                    CampaignDuration::fromSecondsRounded(1.0));
    const AdvanceResult steppedCoarse =
        steppedPropagator.advanceTo(steppedPropagator.beginCoast(orbiting).value(), oneOrbitLater,
                                    CampaignDuration::fromSecondsRounded(1000.0));
    checks.check(!statesBitIdentical(steppedFine.craft.state, steppedCoarse.craft.state),
                 "a stepped coast does not reach a bit-identical state at 1 s and 1000 s ticks, "
                 "which is the difference between the two modes");

    // --- determinism --------------------------------------------------------------------
    const AdvanceResult repeat =
        propagator.advanceTo(propagator.beginCoast(orbiting).value(), oneOrbitLater,
                             CampaignDuration::fromSecondsRounded(60.0));
    checks.check(statesBitIdentical(repeat.craft.state, coastedOrbit.craft.state),
                 "repeating an advance with identical inputs is bit-identical");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        CheckContext checks;

        const std::filesystem::path fixtureRoot = parseFixtureRoot(argc, argv);
        const ReferenceData data = ReferenceData::loadFromDirectory(fixtureRoot);

        // NAIF 399 is Earth. Taken from the pinned kernel so this file contains no
        // gravitational constant of its own.
        const double muEarth = data.gravitationalParameter(399);
        const double earthRadius = data.earthEllipsoid().equatorialRadiusMetres();

        checks.check(muEarth > 3.9e14 && muEarth < 4.0e14,
                     "Earth's gravitational parameter from the pinned kernel is near 3.986e14 "
                     "m^3/s^2");

        checkStumpff(checks);
        checkKepler(checks, muEarth, earthRadius);
        checkElements(checks, muEarth, earthRadius);
        checkIntegrators(checks, muEarth, earthRadius);

        const BodySystem system = BodySystem::fromReferenceData(data);
        const ConicEphemeris ephemeris = ConicEphemeris::fromReferenceData(data, system);

        checkBodySystem(checks, system);
        checkEphemeris(checks, data, system, ephemeris);
        checkCampaignClock(checks, data);
        checkHybridPropagator(checks, system, ephemeris, data);

        return checks.summarize("OrbitSelfCheck");
    } catch (const std::exception& error) {
        std::cerr << "OrbitSelfCheck: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
