#include "Sol/Proto/Frames/FrameTransform.h"

#include <algorithm>
#include <cmath>

namespace sol::proto::frames {
namespace {

/// Degrees per century converted to radians per second, for the pole terms.
constexpr double kRadiansPerSecondPerDegreePerCentury =
    0.017453292519943295769236907684886 / (36525.0 * 86400.0);

/// Degrees per day converted to radians per second, for the prime-meridian term.
constexpr double kRadiansPerSecondPerDegreePerDay =
    0.017453292519943295769236907684886 / 86400.0;

} // namespace

Mat3 rotationX(double angleRadians) noexcept
{
    const double s = std::sin(angleRadians);
    const double c = std::cos(angleRadians);
    return Mat3{{{1.0, 0.0, 0.0}, {0.0, c, s}, {0.0, -s, c}}};
}

Mat3 rotationZ(double angleRadians) noexcept
{
    const double s = std::sin(angleRadians);
    const double c = std::cos(angleRadians);
    return Mat3{{{c, s, 0.0}, {-s, c, 0.0}, {0.0, 0.0, 1.0}}};
}

Mat3 rotationXDerivative(double angleRadians) noexcept
{
    const double s = std::sin(angleRadians);
    const double c = std::cos(angleRadians);
    return Mat3{{{0.0, 0.0, 0.0}, {0.0, -s, c}, {0.0, -c, -s}}};
}

Mat3 rotationZDerivative(double angleRadians) noexcept
{
    const double s = std::sin(angleRadians);
    const double c = std::cos(angleRadians);
    return Mat3{{{-s, c, 0.0}, {-c, -s, 0.0}, {0.0, 0.0, 0.0}}};
}

double orthonormalityError(const Mat3& a) noexcept
{
    const Mat3 product = a * transpose(a);
    double worst = 0.0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const double expected = (row == column) ? 1.0 : 0.0;
            worst = std::max(worst, std::abs(product.m[row][column] - expected));
        }
    }
    return worst;
}

StateVector toChild(const FrameTransform& transform, const StateVector& parentState,
                    FrameId childFrame) noexcept
{
    const Vec3 offset = parentState.position.metres() - transform.originInParent;
    const Vec3 relativeVelocity =
        parentState.velocity.metresPerSecond() - transform.originVelocityInParent;

    StateVector result;
    result.position = PositionMetres::fromMetres(transform.rotation * offset);
    result.velocity = VelocityMetresPerSecond::fromMetresPerSecond(
        transform.rotation * relativeVelocity + transform.rotationRate * offset);
    result.frame = childFrame;
    result.epoch = parentState.epoch;
    return result;
}

StateVector toParent(const FrameTransform& transform, const StateVector& childState,
                     FrameId parentFrame) noexcept
{
    const Vec3 offset = applyTranspose(transform.rotation, childState.position.metres());
    const Vec3 relativeVelocity = applyTranspose(
        transform.rotation,
        childState.velocity.metresPerSecond() - transform.rotationRate * offset);

    StateVector result;
    result.position = PositionMetres::fromMetres(offset + transform.originInParent);
    result.velocity = VelocityMetresPerSecond::fromMetresPerSecond(
        relativeVelocity + transform.originVelocityInParent);
    result.frame = parentFrame;
    result.epoch = childState.epoch;
    return result;
}

FrameTransform compose(const FrameTransform& parentToMiddle,
                       const FrameTransform& middleToChild) noexcept
{
    // r_child = M.R (P.R (r - P.t) - M.t) = (M.R P.R) (r - (P.t + P.R^T M.t))
    FrameTransform result;
    result.rotation = middleToChild.rotation * parentToMiddle.rotation;
    result.rotationRate = middleToChild.rotationRate * parentToMiddle.rotation
                        + middleToChild.rotation * parentToMiddle.rotationRate;

    result.originInParent = parentToMiddle.originInParent
                          + applyTranspose(parentToMiddle.rotation, middleToChild.originInParent);
    result.originVelocityInParent =
        parentToMiddle.originVelocityInParent
        + applyTranspose(parentToMiddle.rotationRate, middleToChild.originInParent)
        + applyTranspose(parentToMiddle.rotation, middleToChild.originVelocityInParent);
    return result;
}

double earthPrimeMeridianDegrees(TdbEpoch epoch, const double primeMeridian[3],
                                 bool reduceWholeTurns) noexcept
{
    const double days = epoch.secondsPastJ2000() / Seconds::kSecondsPerDay;

    double linear = 0.0;
    if (reduceWholeTurns) {
        // Whole turns per day are removed before they are ever multiplied by the day count.
        // The subtraction is exact in binary: both operands share an exponent, so no bits of
        // the remainder are lost.
        const double wholeTurnsPerDay = std::floor(primeMeridian[1] / 360.0);
        const double residualRate = primeMeridian[1] - 360.0 * wholeTurnsPerDay;

        const double wholeDays = std::floor(days);
        const double dayFraction = days - wholeDays;
        linear = residualRate * wholeDays + primeMeridian[1] * dayFraction;
    } else {
        linear = primeMeridian[1] * days;
    }

    double angle = primeMeridian[0] + linear;
    if (primeMeridian[2] != 0.0) {
        // Earth's quadratic term is exactly zero in pck00011. A body whose term is not would
        // reintroduce the large-magnitude problem the linear reduction just removed, and would
        // need the same treatment before this function could be trusted for it.
        angle += primeMeridian[2] * days * days;
    }
    return std::fmod(angle, 360.0);
}

FrameTransform earthBodyFixedTransform(TdbEpoch epoch, const double poleRightAscension[3],
                                       const double poleDeclination[3],
                                       const double primeMeridian[3]) noexcept
{
    const double centuries = epoch.secondsPastJ2000() / (Seconds::kSecondsPerDay * 36525.0);

    const double rightAscensionDegrees = poleRightAscension[0]
                                       + poleRightAscension[1] * centuries
                                       + poleRightAscension[2] * centuries * centuries;
    const double declinationDegrees = poleDeclination[0]
                                    + poleDeclination[1] * centuries
                                    + poleDeclination[2] * centuries * centuries;
    const double primeMeridianDegrees = earthPrimeMeridianDegrees(epoch, primeMeridian, true);

    // The IAU body-fixed rotation: line up with the node, tip to the pole, then spin to the
    // prime meridian.
    const double alpha = Radians::fromDegrees(rightAscensionDegrees + 90.0).radians();
    const double polarTilt = Radians::fromDegrees(90.0 - declinationDegrees).radians();
    const double spin = Radians::fromDegrees(primeMeridianDegrees).radians();

    const Mat3 nodeRotation = rotationZ(alpha);
    const Mat3 tiltRotation = rotationX(polarTilt);
    const Mat3 spinRotation = rotationZ(spin);

    // Angle rates in rad/s. The pole terms are of order 1e-12 rad/s and the spin term is
    // 7.29e-5 rad/s; all three are carried because dropping the small ones is an accuracy
    // claim this increment has no reason to make.
    const double alphaRate = (poleRightAscension[1] + 2.0 * poleRightAscension[2] * centuries)
                           * kRadiansPerSecondPerDegreePerCentury;
    const double tiltRate = -(poleDeclination[1] + 2.0 * poleDeclination[2] * centuries)
                          * kRadiansPerSecondPerDegreePerCentury;
    const double spinRate = (primeMeridian[1]
                             + 2.0 * primeMeridian[2] * epoch.secondsPastJ2000()
                                   / Seconds::kSecondsPerDay)
                          * kRadiansPerSecondPerDegreePerDay;

    FrameTransform transform;
    transform.originInParent = Vec3{};
    transform.originVelocityInParent = Vec3{};
    transform.rotation = spinRotation * tiltRotation * nodeRotation;
    transform.rotationRate =
        (rotationZDerivative(spin) * spinRate) * tiltRotation * nodeRotation
        + spinRotation * (rotationXDerivative(polarTilt) * tiltRate) * nodeRotation
        + spinRotation * tiltRotation * (rotationZDerivative(alpha) * alphaRate);
    return transform;
}

} // namespace sol::proto::frames
