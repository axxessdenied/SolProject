#include "Sol/Proto/Orbit/Integrator.h"

#include <cmath>

namespace sol::proto::orbit {
namespace {

using frames::Vec3;

/// Classical fourth-order Runge-Kutta on the first-order system y' = (v, a(r)).
[[nodiscard]] TwoBodyState stepRungeKutta4(const TwoBodyState& state, double mu,
                                           double h) noexcept
{
    const Vec3& r0 = state.position;
    const Vec3& v0 = state.velocity;

    const Vec3 k1Position = v0;
    const Vec3 k1Velocity = gravitationalAcceleration(r0, mu);

    const Vec3 k2Position = v0 + k1Velocity * (0.5 * h);
    const Vec3 k2Velocity = gravitationalAcceleration(r0 + k1Position * (0.5 * h), mu);

    const Vec3 k3Position = v0 + k2Velocity * (0.5 * h);
    const Vec3 k3Velocity = gravitationalAcceleration(r0 + k2Position * (0.5 * h), mu);

    const Vec3 k4Position = v0 + k3Velocity * h;
    const Vec3 k4Velocity = gravitationalAcceleration(r0 + k3Position * h, mu);

    const double sixth = h / 6.0;
    TwoBodyState result;
    result.position =
        r0 + (k1Position + k2Position * 2.0 + k3Position * 2.0 + k4Position) * sixth;
    result.velocity =
        v0 + (k1Velocity + k2Velocity * 2.0 + k3Velocity * 2.0 + k4Velocity) * sixth;
    return result;
}

/// Velocity Verlet: one kick-drift-kick step, second order and symplectic.
[[nodiscard]] TwoBodyState stepVelocityVerlet(const TwoBodyState& state, double mu,
                                              double h) noexcept
{
    const Vec3 initialAcceleration = gravitationalAcceleration(state.position, mu);

    TwoBodyState result;
    result.position =
        state.position + state.velocity * h + initialAcceleration * (0.5 * h * h);

    const Vec3 finalAcceleration = gravitationalAcceleration(result.position, mu);
    result.velocity = state.velocity + (initialAcceleration + finalAcceleration) * (0.5 * h);
    return result;
}

/// Fourth-order symplectic composition (Forest-Ruth / Yoshida), three leapfrog stages.
///
/// The coefficients are the unique solution that cancels the third-order error term of a
/// symmetric three-stage composition. w0 is negative, which is not a mistake: a fourth-order
/// symplectic composition provably requires a backward stage, and that is the price of holding
/// both properties at once.
[[nodiscard]] TwoBodyState stepYoshida4(const TwoBodyState& state, double mu, double h) noexcept
{
    // 2^(1/3), written as a computed constant rather than a decimal literal so the value is
    // exactly the correctly-rounded cube root of two on this toolchain rather than however many
    // digits happened to get typed.
    static const double kCubeRootOfTwo = std::cbrt(2.0);
    static const double kW1 = 1.0 / (2.0 - kCubeRootOfTwo);
    static const double kW0 = -kCubeRootOfTwo / (2.0 - kCubeRootOfTwo);

    static const double kDrift0 = 0.5 * kW1;
    static const double kDrift1 = 0.5 * (kW0 + kW1);
    static const double kDrift2 = kDrift1;
    static const double kDrift3 = kDrift0;
    static const double kKick0 = kW1;
    static const double kKick1 = kW0;
    static const double kKick2 = kW1;

    Vec3 position = state.position;
    Vec3 velocity = state.velocity;

    position = position + velocity * (kDrift0 * h);
    velocity = velocity + gravitationalAcceleration(position, mu) * (kKick0 * h);
    position = position + velocity * (kDrift1 * h);
    velocity = velocity + gravitationalAcceleration(position, mu) * (kKick1 * h);
    position = position + velocity * (kDrift2 * h);
    velocity = velocity + gravitationalAcceleration(position, mu) * (kKick2 * h);
    position = position + velocity * (kDrift3 * h);

    return TwoBodyState{position, velocity};
}

} // namespace

std::string_view integratorName(IntegratorKind kind) noexcept
{
    switch (kind) {
    case IntegratorKind::RungeKutta4:    return "runge-kutta-4";
    case IntegratorKind::VelocityVerlet: return "velocity-verlet";
    case IntegratorKind::Yoshida4:       return "yoshida-4-symplectic";
    }
    return "unknown";
}

bool isSymplectic(IntegratorKind kind) noexcept
{
    switch (kind) {
    case IntegratorKind::RungeKutta4:    return false;
    case IntegratorKind::VelocityVerlet: return true;
    case IntegratorKind::Yoshida4:       return true;
    }
    return false;
}

int accelerationEvaluationsPerStep(IntegratorKind kind) noexcept
{
    switch (kind) {
    case IntegratorKind::RungeKutta4:    return 4;
    case IntegratorKind::VelocityVerlet: return 2;
    case IntegratorKind::Yoshida4:       return 3;
    }
    return 0;
}

TwoBodyState integrateStep(IntegratorKind kind, const TwoBodyState& state,
                           double gravitationalParameter, double stepSeconds) noexcept
{
    switch (kind) {
    case IntegratorKind::RungeKutta4:
        return stepRungeKutta4(state, gravitationalParameter, stepSeconds);
    case IntegratorKind::VelocityVerlet:
        return stepVelocityVerlet(state, gravitationalParameter, stepSeconds);
    case IntegratorKind::Yoshida4:
        return stepYoshida4(state, gravitationalParameter, stepSeconds);
    }
    return state;
}

} // namespace sol::proto::orbit
