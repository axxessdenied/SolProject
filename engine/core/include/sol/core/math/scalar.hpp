#pragma once

// Sol Engine math conventions (decided once, never revisited - see engine plan 2.2):
// - Right-handed coordinate system: +X right, +Y up, -Z forward.
// - Column-major matrices, column vectors: transformed = M * v.
// - Angles are radians everywhere.
// - Projection: reversed-Z with infinite far plane, depth range [0,1]
//   (clear depth to 0.0, depth compare GREATER_OR_EQUAL).
// - Clip-space Y flip is handled by a negative viewport height in the
//   renderer; matrices stay Y-up.
// - Simulation-space positions are double precision (DVec3); rendering is
//   camera-relative in float.

#include <cmath>

namespace sol::core {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 0.5f * kPi;
inline constexpr double kPiD = 3.14159265358979323846;

[[nodiscard]] constexpr float radians(float degrees)
{
    return degrees * (kPi / 180.0f);
}

[[nodiscard]] constexpr float degrees(float radians)
{
    return radians * (180.0f / kPi);
}

template <typename T>
[[nodiscard]] constexpr T clamp(T value, T low, T high)
{
    return value < low ? low : (value > high ? high : value);
}

[[nodiscard]] constexpr float saturate(float value)
{
    return clamp(value, 0.0f, 1.0f);
}

template <typename T>
[[nodiscard]] constexpr T lerp(T a, T b, float t)
{
    return a + (b - a) * t;
}

[[nodiscard]] inline bool nearlyEqual(float a, float b, float epsilon = 1e-5f)
{
    return std::fabs(a - b) <= epsilon;
}

[[nodiscard]] inline bool nearlyEqual(double a, double b, double epsilon = 1e-12)
{
    return std::fabs(a - b) <= epsilon;
}

} // namespace sol::core
