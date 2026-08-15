#pragma once

#include "sol/core/math/scalar.hpp"

namespace sol::core {

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    [[nodiscard]] constexpr Vec2 operator+(Vec2 rhs) const { return {x + rhs.x, y + rhs.y}; }
    [[nodiscard]] constexpr Vec2 operator-(Vec2 rhs) const { return {x - rhs.x, y - rhs.y}; }
    [[nodiscard]] constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    [[nodiscard]] constexpr Vec2 operator-() const { return {-x, -y}; }
    [[nodiscard]] constexpr bool operator==(const Vec2&) const = default;
};

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    [[nodiscard]] constexpr Vec3 operator+(Vec3 rhs) const { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
    [[nodiscard]] constexpr Vec3 operator-(Vec3 rhs) const { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
    [[nodiscard]] constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    [[nodiscard]] constexpr Vec3 operator*(Vec3 rhs) const { return {x * rhs.x, y * rhs.y, z * rhs.z}; }
    [[nodiscard]] constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    [[nodiscard]] constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    [[nodiscard]] constexpr bool operator==(const Vec3&) const = default;

    constexpr Vec3& operator+=(Vec3 rhs)
    {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }
    constexpr Vec3& operator-=(Vec3 rhs)
    {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        return *this;
    }
    constexpr Vec3& operator*=(float s)
    {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
};

struct Vec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    [[nodiscard]] constexpr Vec4 operator+(Vec4 rhs) const
    {
        return {x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w};
    }
    [[nodiscard]] constexpr Vec4 operator-(Vec4 rhs) const
    {
        return {x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w};
    }
    [[nodiscard]] constexpr Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    [[nodiscard]] constexpr bool operator==(const Vec4&) const = default;

    [[nodiscard]] constexpr Vec3 xyz() const { return {x, y, z}; }
};

// Simulation-space position vector (see conventions in scalar.hpp).
struct DVec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    [[nodiscard]] constexpr DVec3 operator+(DVec3 rhs) const { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
    [[nodiscard]] constexpr DVec3 operator-(DVec3 rhs) const { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
    [[nodiscard]] constexpr DVec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    [[nodiscard]] constexpr DVec3 operator-() const { return {-x, -y, -z}; }
    [[nodiscard]] constexpr bool operator==(const DVec3&) const = default;

    constexpr DVec3& operator+=(DVec3 rhs)
    {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }

    // Camera-relative demotion to render space; only valid for SMALL differences.
    [[nodiscard]] constexpr Vec3 toVec3() const
    {
        return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    }
};

[[nodiscard]] constexpr Vec3 toVec3(DVec3 v)
{
    return v.toVec3();
}

[[nodiscard]] constexpr DVec3 toDVec3(Vec3 v)
{
    return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
}

// --- Vec2 ---

[[nodiscard]] constexpr float dot(Vec2 a, Vec2 b)
{
    return a.x * b.x + a.y * b.y;
}

[[nodiscard]] inline float length(Vec2 v)
{
    return std::sqrt(dot(v, v));
}

// --- Vec3 ---

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] constexpr float lengthSquared(Vec3 v)
{
    return dot(v, v);
}

[[nodiscard]] inline float length(Vec3 v)
{
    return std::sqrt(dot(v, v));
}

[[nodiscard]] inline Vec3 normalize(Vec3 v)
{
    const float len = length(v);
    return len > 0.0f ? v * (1.0f / len) : Vec3{};
}

[[nodiscard]] constexpr Vec3 lerp(Vec3 a, Vec3 b, float t)
{
    return a + (b - a) * t;
}

// --- Vec4 ---

[[nodiscard]] constexpr float dot(Vec4 a, Vec4 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// --- DVec3 ---

[[nodiscard]] constexpr double dot(DVec3 a, DVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] inline double length(DVec3 v)
{
    return std::sqrt(dot(v, v));
}

[[nodiscard]] inline DVec3 normalize(DVec3 v)
{
    const double len = length(v);
    return len > 0.0 ? v * (1.0 / len) : DVec3{};
}

[[nodiscard]] inline bool nearlyEqual(Vec3 a, Vec3 b, float epsilon = 1e-5f)
{
    return nearlyEqual(a.x, b.x, epsilon) && nearlyEqual(a.y, b.y, epsilon) &&
           nearlyEqual(a.z, b.z, epsilon);
}

} // namespace sol::core
