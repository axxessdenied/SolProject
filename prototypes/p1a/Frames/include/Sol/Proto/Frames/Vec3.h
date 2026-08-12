#pragma once

#include <cmath>

namespace sol::proto::frames {

/// Three raw double components carrying neither a unit nor a reference frame.
///
/// Vec3 is deliberately dimensionless, frameless storage. Everything that crosses this
/// library's API boundary is wrapped in a unit type (Units.h) and tagged with a frame
/// (FrameId.h); Vec3 exists only so those wrappers can share one arithmetic implementation.
///
/// Passing a bare Vec3 between subsystems is exactly the mistake the wrappers prevent, so
/// treat it as an implementation detail of the layer you are writing.
struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

[[nodiscard]] constexpr Vec3 operator+(const Vec3& a, const Vec3& b) noexcept
{
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] constexpr Vec3 operator-(const Vec3& a, const Vec3& b) noexcept
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] constexpr Vec3 operator-(const Vec3& a) noexcept
{
    return Vec3{-a.x, -a.y, -a.z};
}

[[nodiscard]] constexpr Vec3 operator*(const Vec3& a, double s) noexcept
{
    return Vec3{a.x * s, a.y * s, a.z * s};
}

[[nodiscard]] constexpr Vec3 operator*(double s, const Vec3& a) noexcept
{
    return a * s;
}

[[nodiscard]] constexpr double dot(const Vec3& a, const Vec3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3 cross(const Vec3& a, const Vec3& b) noexcept
{
    return Vec3{a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}

/// Euclidean length. Uses std::hypot rather than sqrt(dot(v, v)) so that a component near
/// the top or bottom of the double range does not overflow or flush the intermediate sum.
/// At Solar-System magnitudes the naive form is safe, but this library is also asked to
/// measure differences down to nanometres, and a single implementation that is correct at
/// both ends removes a class of scale-dependent surprise.
[[nodiscard]] inline double length(const Vec3& v) noexcept
{
    return std::hypot(std::hypot(v.x, v.y), v.z);
}

/// Length of (a - b). Written as one call because the difference of two large, nearly equal
/// vectors is the quantity this library exists to measure, and naming it keeps every error
/// metric in the increment computed the same way.
[[nodiscard]] inline double distance(const Vec3& a, const Vec3& b) noexcept
{
    return length(a - b);
}

/// Returns v scaled to unit length. Returns v unchanged when its length is zero, since the
/// callers here build orthonormal bases from analytically non-degenerate inputs and a silent
/// NaN would be far harder to trace than a visibly unnormalised axis.
[[nodiscard]] inline Vec3 normalized(const Vec3& v) noexcept
{
    const double len = length(v);
    return len == 0.0 ? v : v * (1.0 / len);
}

} // namespace sol::proto::frames
