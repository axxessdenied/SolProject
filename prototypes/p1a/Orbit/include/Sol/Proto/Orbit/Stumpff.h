#pragma once

namespace sol::proto::orbit {

/// The Stumpff functions C(z) and S(z), which carry the universal-variable Kepler solution
/// across every conic type without branching on the conic.
///
///     C(z) = (1 - cos sqrt(z)) / z              for z > 0
///          = (cosh sqrt(-z) - 1) / (-z)         for z < 0
///          = 1/2                                at z = 0
///
///     S(z) = (sqrt(z) - sin sqrt(z)) / z^(3/2)  for z > 0
///          = (sinh sqrt(-z) - sqrt(-z)) / (-z)^(3/2)  for z < 0
///          = 1/6                                at z = 0
///
/// The closed forms above are the definitions, and they are numerically unusable near z = 0:
/// both are a small difference of two nearly equal quantities divided by a small number, so
/// they lose most of their significant digits exactly where a near-parabolic or short-interval
/// propagation puts them. A 200 km circular orbit stepped at one second sits at |z| well under
/// 1e-3, which is squarely inside the bad region -- this is not a hypothetical corner.
///
/// Both functions are therefore evaluated from their power series near the origin, where the
/// series is not merely accurate but is the *better-conditioned* form: every term is computed
/// directly and summed, with no cancellation anywhere.
///
///     C(z) = sum_k (-z)^k / (2k + 2)!
///     S(z) = sum_k (-z)^k / (2k + 3)!
///
/// The switchover point is chosen so that both forms are accurate at the boundary rather than
/// where one is merely tolerable; see kSeriesThreshold in the implementation for the argument.

/// C(z), the second Stumpff function.
[[nodiscard]] double stumpffC(double z) noexcept;

/// S(z), the third Stumpff function.
[[nodiscard]] double stumpffS(double z) noexcept;

} // namespace sol::proto::orbit
