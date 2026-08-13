#include "Sol/Proto/Orbit/KeplerPropagator.h"

#include "Sol/Proto/Orbit/Stumpff.h"

#include <cmath>
#include <limits>

namespace sol::proto::orbit {
namespace {

using frames::Vec3;

/// Relative convergence tolerance on the universal anomaly.
///
/// Chosen against what a double can actually deliver rather than against a round number. For a
/// 200 km orbit chi runs to a few thousand m^(1/2), where one ULP is about 5e-13 in absolute
/// terms, so a relative tolerance of 1e-13 is roughly two ULP: tight enough that the residual
/// is at the arithmetic floor, loose enough that the iteration is not asked to beat it.
constexpr double kRelativeTolerance = 1.0e-13;

/// Hard iteration bound. Newton on the universal Kepler equation converges quadratically from
/// the standard initial guesses and reaches tolerance in single digits for every case A3
/// exercises. This exists so a pathological input terminates rather than hangs, and the
/// converged flag reports that it did.
constexpr int kMaxIterations = 64;

/// |alpha * r0| below this is treated as parabolic for the purpose of choosing an initial
/// guess. alpha * r0 is dimensionless -- it is 1 for a circular orbit and 0 for a parabolic one
/// -- so this is a scale-free test, which a bare threshold on alpha would not be.
constexpr double kParabolicThreshold = 1.0e-6;

[[nodiscard]] double signOf(double value) noexcept
{
    return value < 0.0 ? -1.0 : 1.0;
}

/// Initial guess for chi, by conic type.
///
/// The three branches are Vallado's standard guesses. Branching here is safe in a way that
/// branching inside the iteration would not be: a poor guess costs iterations, whereas a
/// mis-chosen solution branch costs correctness. The iteration that follows is identical for
/// every conic.
[[nodiscard]] double initialGuess(double alpha, double r0, double sigma0, double sqrtMu,
                                  double elapsedSeconds, const Vec3& position,
                                  const Vec3& velocity, double gravitationalParameter) noexcept
{
    const double alphaR0 = alpha * r0;

    if (alphaR0 > kParabolicThreshold) {
        // Elliptic. chi is approximately sqrt(mu) * dt / a, exact for a circular orbit.
        return sqrtMu * elapsedSeconds * alpha;
    }

    if (alphaR0 < -kParabolicThreshold) {
        // Hyperbolic. The logarithmic form inverts the hyperbolic Kepler equation directly and
        // lands close enough that Newton converges without a safeguarded bracket.
        const double a = 1.0 / alpha; // negative
        const double sign = signOf(elapsedSeconds);
        const double numerator = -2.0 * gravitationalParameter * alpha * elapsedSeconds;
        const double denominator =
            sigma0 * sqrtMu + sign * std::sqrt(-gravitationalParameter * a) * (1.0 - r0 * alpha);
        // A zero or negative argument to the logarithm means the guess formula is outside its
        // domain, which happens only for inputs the eligibility rules already reject. Falling
        // back to the elliptic form keeps the solver total.
        const double ratio = numerator / denominator;
        if (!(ratio > 0.0)) {
            return sqrtMu * elapsedSeconds * alpha;
        }
        return sign * std::sqrt(-a) * std::log(ratio);
    }

    // Parabolic, or close enough that neither other guess is well conditioned. Barker's
    // equation solved in closed form through the standard trigonometric substitution.
    const Vec3 h = frames::cross(position, velocity);
    const double hMagnitude = frames::length(h);
    const double p = hMagnitude * hMagnitude / gravitationalParameter;
    const double s = 0.5 * std::atan(1.0 / (3.0 * std::sqrt(gravitationalParameter / (p * p * p))
                                            * elapsedSeconds));
    const double w = std::atan(std::cbrt(std::tan(s)));
    return std::sqrt(p) * 2.0 / std::tan(2.0 * w);
}

} // namespace

KeplerPropagation propagateKepler(const TwoBodyState& state, double gravitationalParameter,
                                  double elapsedSeconds) noexcept
{
    KeplerPropagation result;

    const Vec3& r0Vector = state.position;
    const Vec3& v0Vector = state.velocity;
    const double r0 = frames::length(r0Vector);
    const double v0 = frames::length(v0Vector);

    if (r0 == 0.0 || gravitationalParameter <= 0.0) {
        // A degenerate conic. The eligibility rules in HybridPropagator.h reject these before a
        // coast begins, so reaching here is a programming error; returning the input unchanged
        // with converged = false makes it visible without producing NaNs that would contaminate
        // every statistic downstream.
        result.state = state;
        return result;
    }

    const double sqrtMu = std::sqrt(gravitationalParameter);
    // alpha = 1/a, from the vis-viva relation. Positive elliptic, zero parabolic, negative
    // hyperbolic -- and never explicitly branched on outside the initial guess.
    const double alpha = 2.0 / r0 - v0 * v0 / gravitationalParameter;
    // sigma0 = r . v / sqrt(mu), the radial-velocity term of the universal Kepler equation.
    const double sigma0 = frames::dot(r0Vector, v0Vector) / sqrtMu;

    double chi = initialGuess(alpha, r0, sigma0, sqrtMu, elapsedSeconds, r0Vector, v0Vector,
                              gravitationalParameter);

    double z = 0.0;
    double c = 0.5;
    double s = 1.0 / 6.0;
    double r = r0;
    double previousAbsoluteStep = std::numeric_limits<double>::infinity();

    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        z = chi * chi * alpha;
        c = stumpffC(z);
        s = stumpffS(z);

        // r is both the radius at the trial chi and, not coincidentally, dF/dchi. Using it for
        // both is not a shortcut: the derivative of the universal Kepler equation with respect
        // to chi *is* the radius, which is what makes this Newton step cheap.
        r = chi * chi * c + sigma0 * chi * (1.0 - z * s) + r0 * (1.0 - z * c);

        const double f = sigma0 * chi * chi * c + (1.0 - r0 * alpha) * chi * chi * chi * s
                       + r0 * chi - sqrtMu * elapsedSeconds;

        if (r == 0.0) {
            break;
        }

        const double step = -f / r;
        chi += step;
        result.iterations = iteration + 1;

        const double absoluteStep = std::abs(step);
        const double scale = std::abs(chi);
        result.finalRelativeStep = scale == 0.0 ? absoluteStep : absoluteStep / scale;

        if (absoluteStep == 0.0 || result.finalRelativeStep <= kRelativeTolerance) {
            result.converged = true;
            break;
        }
        // The step no longer shrinking means the iteration has hit the rounding floor. Continuing
        // would oscillate in the last bit forever, and -- more to the point for ADR 0010 -- would
        // make the iteration count depend on which bit it happened to land on. Stopping here is
        // what keeps the propagation reproducible.
        if (iteration >= 3 && absoluteStep >= previousAbsoluteStep) {
            result.converged = result.finalRelativeStep <= kRelativeTolerance;
            break;
        }
        previousAbsoluteStep = absoluteStep;
    }

    // Recompute the Stumpff terms at the accepted chi so f and g are consistent with the value
    // actually used, rather than with the trial value of the last iteration.
    z = chi * chi * alpha;
    c = stumpffC(z);
    s = stumpffS(z);

    const double f = 1.0 - chi * chi * c / r0;
    const double g = elapsedSeconds - chi * chi * chi * s / sqrtMu;

    const Vec3 newPosition = r0Vector * f + v0Vector * g;
    const double newRadius = frames::length(newPosition);

    // The Lagrange rate coefficients. gdot uses the new radius and fdot uses both, which is why
    // the position is formed first.
    const double fDot = newRadius == 0.0 ? 0.0
                                         : sqrtMu / (newRadius * r0) * chi * (z * s - 1.0);
    const double gDot = newRadius == 0.0 ? 1.0 : 1.0 - chi * chi * c / newRadius;

    result.state.position = newPosition;
    result.state.velocity = r0Vector * fDot + v0Vector * gDot;
    result.universalAnomaly = chi;
    result.stumpffArgument = z;
    return result;
}

} // namespace sol::proto::orbit
