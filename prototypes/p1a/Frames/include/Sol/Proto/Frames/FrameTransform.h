#pragma once

#include "Sol/Proto/Frames/FrameId.h"
#include "Sol/Proto/Frames/Mat3.h"
#include "Sol/Proto/Frames/TimeScales.h"
#include "Sol/Proto/Frames/Units.h"
#include "Sol/Proto/Frames/Vec3.h"

namespace sol::proto::frames {

/// A position and velocity, tagged with the frame it is expressed in and the instant it is
/// valid at.
///
/// The epoch is part of the state, not an ambient parameter, because two of the frames in
/// this graph rotate. A state converted with the wrong epoch produces a plausible vector that
/// is silently hundreds of metres wrong, and A2 exists partly to make that class of error
/// impossible to commit quietly.
struct StateVector {
    PositionMetres position{};
    VelocityMetresPerSecond velocity{};
    FrameId frame{FrameId::SsbIcrf};
    TdbEpoch epoch{};
};

/// A rigid transform from a parent frame to a child frame at one instant.
///
/// Defines `r_child = rotation * (r_parent - originInParent)`, with the velocity following by
/// differentiation:
///
///     v_child = rotation * (v_parent - originVelocityInParent)
///             + rotationRate * (r_parent - originInParent)
///
/// `rotationRate` is dR/dt, not an angular-velocity vector. Carrying the matrix derivative
/// rather than an omega vector keeps the velocity transform exact for frames whose rotation
/// axis itself moves -- the IAU Earth pole drifts, slowly, and an omega-cross formulation
/// would quietly drop that term. The magnitude involved is tiny; the point is that the code
/// does not need the reader to know it is tiny.
struct FrameTransform {
    /// Position of the child frame's origin, expressed in the parent frame, in metres.
    Vec3 originInParent{};
    /// Velocity of the child frame's origin, expressed in the parent frame, in m/s.
    Vec3 originVelocityInParent{};
    /// Rotation taking parent-frame coordinates into child-frame coordinates.
    Mat3 rotation{};
    /// Time derivative of @c rotation, in 1/s. Explicitly zero-initialised: Mat3's own default
    /// is the identity, which would mean a spurious 1 rad/s on every unset boundary.
    Mat3 rotationRate{zeroMat3()};
};

/// Applies @p transform in the parent-to-child direction.
[[nodiscard]] StateVector toChild(const FrameTransform& transform, const StateVector& parentState,
                                  FrameId childFrame) noexcept;

/// Applies @p transform in the child-to-parent direction.
///
/// The exact algebraic inverse of toChild for an orthonormal rotation, not a numerically
/// refitted approximation of it.
[[nodiscard]] StateVector toParent(const FrameTransform& transform, const StateVector& childState,
                                   FrameId parentFrame) noexcept;

/// Composes two transforms so that applying the result equals applying @p parentToMiddle and
/// then @p middleToChild.
///
/// Used only by the flat model, which needs each frame's transform expressed directly against
/// the root. It is separated out precisely so the evidence can attribute the flat model's
/// error to composition rather than to conversion.
[[nodiscard]] FrameTransform compose(const FrameTransform& parentToMiddle,
                                     const FrameTransform& middleToChild) noexcept;

/// Earth's IAU prime-meridian angle at @p epoch, in degrees, from BODY399_PM coefficients.
///
/// @param reduceWholeTurns  When true, whole turns per day are removed from the linear rate
///                          before it is multiplied by the day count.
///
/// The reduction is not a micro-optimisation, it is a precision requirement, and A2 measures
/// the difference. Earth's IAU rate is 360.9856235 degrees per day, so twenty-six years past
/// J2000 the unreduced angle is about 3.4e6 degrees. One ULP at that magnitude is 7.6e-10
/// degrees, which is 85 micrometres of arc at Earth's surface -- 8.5% of A2's entire
/// millimetre position budget, spent on nothing but the representation of an angle that is
/// about to be reduced modulo 360 anyway.
///
/// Splitting the rate into 360 degrees per day plus a 0.9856235 remainder discards the whole
/// turns before they are ever formed. The subtraction 360.9856235 - 360 is exact in binary,
/// and integer turns are exactly invisible to sine and cosine, so the reduction is lossless
/// as well as cheaper.
[[nodiscard]] double earthPrimeMeridianDegrees(TdbEpoch epoch, const double primeMeridian[3],
                                               bool reduceWholeTurns) noexcept;

/// Earth's IAU body-fixed orientation at @p epoch, built from a pinned planetary-constants
/// kernel.
///
/// @param poleRightAscension  BODY399_POLE_RA coefficients, in degrees and degrees/century.
/// @param poleDeclination     BODY399_POLE_DEC coefficients.
/// @param primeMeridian       BODY399_PM coefficients, in degrees and degrees/day.
///
/// This is the IAU_EARTH definition, and it is *not* an accurate Earth orientation model: it
/// omits nutation, polar motion, and UT1-UTC, so it can differ from ITRF by tens of metres at
/// the surface. That is acceptable and deliberate for A2, which measures the numerical
/// behaviour of a rotating-frame boundary, not the astronomical accuracy of Earth's attitude.
/// The production Earth orientation model is an open decision in docs/architecture.md, and
/// this function must not be mistaken for closing it.
[[nodiscard]] FrameTransform earthBodyFixedTransform(TdbEpoch epoch,
                                                     const double poleRightAscension[3],
                                                     const double poleDeclination[3],
                                                     const double primeMeridian[3]) noexcept;

} // namespace sol::proto::frames
