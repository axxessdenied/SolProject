#include "Sol/Proto/Orbit/TwoBody.h"

#include <cmath>

namespace sol::proto::orbit {

using frames::Vec3;

Vec3 gravitationalAcceleration(const Vec3& position, double gravitationalParameter) noexcept
{
    const double r = frames::length(position);
    if (r == 0.0) {
        // A craft exactly at the central body's centre has no defined gravity direction. The
        // callers here never produce that state -- the eligibility rules reject a degenerate
        // conic before an integrator ever sees one -- but returning zero rather than a NaN keeps
        // a programming error visible as a stationary craft instead of poisoning every
        // subsequent sample and every statistic computed over them.
        return Vec3{};
    }
    // Written as mu/r^3 * r rather than mu/r^2 * rhat so the normalisation and the inverse
    // square share one division. The two forms differ in the last bits, and the whole increment
    // is a determinism argument, so the form is fixed here and not restated at call sites.
    const double scale = -gravitationalParameter / (r * r * r);
    return position * scale;
}

double specificEnergy(const TwoBodyState& state, double gravitationalParameter) noexcept
{
    const double r = frames::length(state.position);
    const double v = frames::length(state.velocity);
    return 0.5 * v * v - gravitationalParameter / r;
}

Vec3 specificAngularMomentum(const TwoBodyState& state) noexcept
{
    return frames::cross(state.position, state.velocity);
}

double radius(const TwoBodyState& state) noexcept
{
    return frames::length(state.position);
}

double speed(const TwoBodyState& state) noexcept
{
    return frames::length(state.velocity);
}

double circularSpeed(double radiusMetres, double gravitationalParameter) noexcept
{
    return std::sqrt(gravitationalParameter / radiusMetres);
}

double circularPeriodSeconds(double radiusMetres, double gravitationalParameter) noexcept
{
    // 2 pi sqrt(r^3 / mu). Kepler's third law for the circular case.
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    return kTwoPi * std::sqrt(radiusMetres * radiusMetres * radiusMetres / gravitationalParameter);
}

} // namespace sol::proto::orbit
