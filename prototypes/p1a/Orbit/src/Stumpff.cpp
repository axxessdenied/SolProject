#include "Sol/Proto/Orbit/Stumpff.h"

#include <cmath>

namespace sol::proto::orbit {
namespace {

/// Below this |z| the series form is used instead of the closed form.
///
/// The choice is bounded from both sides. Above it, the closed form must not have lost
/// precision: at |z| = 1 the numerator of C is 1 - cos(1) = 0.4597, which is the same order as
/// its operands, so no significant cancellation has occurred. Below it, the series must
/// converge quickly: at |z| = 1 the terms are 1/(2k+2)!, which fall below the double epsilon by
/// k = 8. Both hold comfortably at 1.0, and the two forms agree there to within a few ULP,
/// which the self-check asserts rather than assumes.
constexpr double kSeriesThreshold = 1.0;

/// Sums a Stumpff series with the given starting factorial.
///
/// @param firstFactorialArgument  2 for C (whose leading term is 1/2!), 3 for S (1/3!).
///
/// Terms are accumulated in ascending order of k, which is the order that adds the largest
/// term first and every subsequent one to a running total it cannot perturb much. Summation
/// order is fixed here rather than left to a compiler or a caller because ADR 0010 asserts
/// bit-exactness on the same build, and floating-point addition is not associative.
[[nodiscard]] double stumpffSeries(double z, int firstFactorialArgument) noexcept
{
    double term = 1.0;
    // Build the leading 1/n! by direct multiplication rather than by calling a factorial, so the
    // divisions are the same ones the recurrence below performs.
    for (int i = 2; i <= firstFactorialArgument; ++i) {
        term /= static_cast<double>(i);
    }

    double sum = term;
    int nextFactorialArgument = firstFactorialArgument + 1;

    // Each term is the previous one times -z / ((n+1)(n+2)), which advances the factorial by two
    // and the power of -z by one. Twelve terms is far past convergence at |z| <= 1 -- the
    // eighth is already below the double epsilon relative to the sum -- and the loop still
    // exits on the term rather than on the count, so the bound is a guard and not the
    // termination condition.
    for (int iteration = 0; iteration < 12; ++iteration) {
        const double denominator = static_cast<double>(nextFactorialArgument)
                                 * static_cast<double>(nextFactorialArgument + 1);
        term *= -z / denominator;
        const double previousSum = sum;
        sum += term;
        if (sum == previousSum) {
            break;
        }
        nextFactorialArgument += 2;
    }
    return sum;
}

} // namespace

double stumpffC(double z) noexcept
{
    if (std::abs(z) < kSeriesThreshold) {
        return stumpffSeries(z, 2);
    }
    if (z > 0.0) {
        const double root = std::sqrt(z);
        return (1.0 - std::cos(root)) / z;
    }
    const double root = std::sqrt(-z);
    return (std::cosh(root) - 1.0) / (-z);
}

double stumpffS(double z) noexcept
{
    if (std::abs(z) < kSeriesThreshold) {
        return stumpffSeries(z, 3);
    }
    if (z > 0.0) {
        const double root = std::sqrt(z);
        return (root - std::sin(root)) / (root * root * root);
    }
    const double root = std::sqrt(-z);
    return (std::sinh(root) - root) / (root * root * root);
}

} // namespace sol::proto::orbit
