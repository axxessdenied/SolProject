#pragma once

#include "Sol/Proto/Frames/Vec3.h"

namespace sol::proto::frames {

/// Row-major 3x3 matrix of doubles.
///
/// Used for two distinct things, and the distinction matters when reading the code:
///   - a *rotation* R that takes coordinates expressed in a parent frame's axes into a
///     child frame's axes, so `r_child = R * r_parent`;
///   - the time derivative Rdot of such a rotation, which is not itself a rotation and must
///     never be orthonormalised or inverted by transpose.
///
/// Quaternions were considered and rejected for A2. The measurements here include the
/// orthogonality drift of composed rotations, and a matrix makes that drift directly
/// observable rather than hiding it behind a renormalising quaternion product.
struct Mat3 {
    /// m[row][column].
    double m[3][3]{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
};

[[nodiscard]] constexpr Mat3 identityMat3() noexcept
{
    return Mat3{};
}

/// The all-zero matrix.
///
/// Needed because Mat3's default is the *identity*, which is the right default for a rotation
/// and exactly the wrong one for a rotation rate. A default-constructed rate of identity would
/// inject a spurious 1 rad/s into every velocity transform, so rate members are initialised
/// from this rather than left to default.
[[nodiscard]] constexpr Mat3 zeroMat3() noexcept
{
    return Mat3{{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}}};
}

[[nodiscard]] constexpr Vec3 operator*(const Mat3& a, const Vec3& v) noexcept
{
    return Vec3{a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
                a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
                a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}

[[nodiscard]] constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept
{
    Mat3 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.m[row][column] = a.m[row][0] * b.m[0][column]
                                  + a.m[row][1] * b.m[1][column]
                                  + a.m[row][2] * b.m[2][column];
        }
    }
    return result;
}

/// Scales every element. Meaningful for a rotation *derivative*, which is what the frame
/// transforms carry; scaling a rotation itself produces a matrix that is no longer one.
[[nodiscard]] constexpr Mat3 operator*(const Mat3& a, double s) noexcept
{
    Mat3 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.m[row][column] = a.m[row][column] * s;
        }
    }
    return result;
}

/// Element-wise sum. Same caveat as the scalar product: this is derivative arithmetic.
[[nodiscard]] constexpr Mat3 operator+(const Mat3& a, const Mat3& b) noexcept
{
    Mat3 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.m[row][column] = a.m[row][column] + b.m[row][column];
        }
    }
    return result;
}

[[nodiscard]] constexpr Mat3 transpose(const Mat3& a) noexcept
{
    Mat3 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.m[row][column] = a.m[column][row];
        }
    }
    return result;
}

/// Applies the transpose of @p a to @p v, that is, the inverse rotation when @p a is
/// orthonormal. Named rather than written as `transpose(a) * v` so the call sites that
/// depend on orthonormality are greppable.
[[nodiscard]] constexpr Vec3 applyTranspose(const Mat3& a, const Vec3& v) noexcept
{
    return Vec3{a.m[0][0] * v.x + a.m[1][0] * v.y + a.m[2][0] * v.z,
                a.m[0][1] * v.x + a.m[1][1] * v.y + a.m[2][1] * v.z,
                a.m[0][2] * v.x + a.m[1][2] * v.y + a.m[2][2] * v.z};
}

/// Frame rotation about x by @p angleRadians: the matrix taking coordinates of a fixed
/// vector from the original axes into axes rotated by +angle about x.
[[nodiscard]] Mat3 rotationX(double angleRadians) noexcept;

/// Frame rotation about z by @p angleRadians. See rotationX for the convention.
[[nodiscard]] Mat3 rotationZ(double angleRadians) noexcept;

/// Derivative of rotationX with respect to its angle.
[[nodiscard]] Mat3 rotationXDerivative(double angleRadians) noexcept;

/// Derivative of rotationZ with respect to its angle.
[[nodiscard]] Mat3 rotationZDerivative(double angleRadians) noexcept;

/// Largest absolute deviation of `a * transpose(a)` from the identity.
///
/// Zero for an exactly orthonormal matrix. This is the metric the increment reports for
/// rotation drift under repeated composition; it is unitless, so it can be compared across
/// chains of different depth.
[[nodiscard]] double orthonormalityError(const Mat3& a) noexcept;

} // namespace sol::proto::frames
