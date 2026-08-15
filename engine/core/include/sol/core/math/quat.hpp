#pragma once

#include "sol/core/math/mat.hpp"
#include "sol/core/math/vec.hpp"

namespace sol::core {

// Unit quaternion for rotations. Identity is {0,0,0,1}.
struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    [[nodiscard]] static constexpr Quat identity() { return {}; }

    // Hamilton product: (this * rhs) applies rhs first, then this.
    [[nodiscard]] constexpr Quat operator*(Quat r) const
    {
        return {
            w * r.x + x * r.w + y * r.z - z * r.y,
            w * r.y - x * r.z + y * r.w + z * r.x,
            w * r.z + x * r.y - y * r.x + z * r.w,
            w * r.w - x * r.x - y * r.y - z * r.z,
        };
    }

    [[nodiscard]] constexpr bool operator==(const Quat&) const = default;
};

[[nodiscard]] constexpr Quat conjugate(Quat q)
{
    return {-q.x, -q.y, -q.z, q.w};
}

[[nodiscard]] constexpr float dot(Quat a, Quat b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

[[nodiscard]] inline float length(Quat q)
{
    return std::sqrt(dot(q, q));
}

[[nodiscard]] inline Quat normalize(Quat q)
{
    const float len = length(q);
    if (len <= 0.0f) {
        return Quat::identity();
    }
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Axis must be normalized.
[[nodiscard]] inline Quat fromAxisAngle(Vec3 axis, float angle)
{
    const float half = angle * 0.5f;
    const float s = std::sin(half);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

[[nodiscard]] constexpr Vec3 rotate(Quat q, Vec3 v)
{
    // v' = v + 2w(q_v x v) + 2(q_v x (q_v x v))
    const Vec3 qv = {q.x, q.y, q.z};
    const Vec3 t = cross(qv, v) * 2.0f;
    return v + t * q.w + cross(qv, t);
}

[[nodiscard]] constexpr Mat4 toMat4(Quat q)
{
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;

    Mat4 r = Mat4::identity();
    r.set(0, 0, 1.0f - 2.0f * (yy + zz));
    r.set(0, 1, 2.0f * (xy - wz));
    r.set(0, 2, 2.0f * (xz + wy));
    r.set(1, 0, 2.0f * (xy + wz));
    r.set(1, 1, 1.0f - 2.0f * (xx + zz));
    r.set(1, 2, 2.0f * (yz - wx));
    r.set(2, 0, 2.0f * (xz - wy));
    r.set(2, 1, 2.0f * (yz + wx));
    r.set(2, 2, 1.0f - 2.0f * (xx + yy));
    return r;
}

[[nodiscard]] inline Quat nlerp(Quat a, Quat b, float t)
{
    if (dot(a, b) < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    const Quat r = {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)};
    return normalize(r);
}

[[nodiscard]] inline Quat slerp(Quat a, Quat b, float t)
{
    float cosTheta = dot(a, b);
    if (cosTheta < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosTheta = -cosTheta;
    }
    if (cosTheta > 0.9995f) {
        return nlerp(a, b, t);
    }
    const float theta = std::acos(cosTheta);
    const float sinTheta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sinTheta;
    const float wb = std::sin(t * theta) / sinTheta;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

} // namespace sol::core
