#pragma once

#include "sol/core/math/vec.hpp"

namespace sol::core {

// Column-major 4x4: m[column * 4 + row]. Column vectors: transformed = M * v.
struct Mat4
{
    float m[16] = {};

    [[nodiscard]] static constexpr Mat4 identity()
    {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    [[nodiscard]] constexpr float at(int row, int column) const { return m[column * 4 + row]; }

    constexpr void set(int row, int column, float value) { m[column * 4 + row] = value; }

    [[nodiscard]] constexpr Vec4 column(int c) const
    {
        return {m[c * 4 + 0], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]};
    }

    [[nodiscard]] constexpr Mat4 operator*(const Mat4& rhs) const
    {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += at(row, k) * rhs.at(k, c);
                }
                r.set(row, c, sum);
            }
        }
        return r;
    }

    [[nodiscard]] constexpr Vec4 operator*(Vec4 v) const
    {
        return {
            m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
            m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
            m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w,
        };
    }

    [[nodiscard]] constexpr bool operator==(const Mat4&) const = default;
};

[[nodiscard]] constexpr Vec3 transformPoint(const Mat4& m, Vec3 p)
{
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return r.xyz();
}

[[nodiscard]] constexpr Vec3 transformDirection(const Mat4& m, Vec3 d)
{
    const Vec4 r = m * Vec4{d.x, d.y, d.z, 0.0f};
    return r.xyz();
}

[[nodiscard]] constexpr Mat4 transpose(const Mat4& a)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            r.set(c, row, a.at(row, c));
        }
    }
    return r;
}

[[nodiscard]] constexpr Mat4 translation(Vec3 t)
{
    Mat4 r = Mat4::identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

[[nodiscard]] constexpr Mat4 scale(Vec3 s)
{
    Mat4 r;
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    r.m[15] = 1.0f;
    return r;
}

[[nodiscard]] inline Mat4 rotationX(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 r = Mat4::identity();
    r.set(1, 1, c);
    r.set(2, 1, s);
    r.set(1, 2, -s);
    r.set(2, 2, c);
    return r;
}

[[nodiscard]] inline Mat4 rotationY(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 r = Mat4::identity();
    r.set(0, 0, c);
    r.set(2, 0, -s);
    r.set(0, 2, s);
    r.set(2, 2, c);
    return r;
}

[[nodiscard]] inline Mat4 rotationZ(float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    Mat4 r = Mat4::identity();
    r.set(0, 0, c);
    r.set(1, 0, s);
    r.set(0, 1, -s);
    r.set(1, 1, c);
    return r;
}

// General 4x4 inverse via the adjugate. Returns identity for singular input.
[[nodiscard]] constexpr Mat4 inverse(const Mat4& a)
{
    const float* s = a.m;
    Mat4 inv;

    inv.m[0] = s[5] * s[10] * s[15] - s[5] * s[11] * s[14] - s[9] * s[6] * s[15] + s[9] * s[7] * s[14] +
               s[13] * s[6] * s[11] - s[13] * s[7] * s[10];
    inv.m[4] = -s[4] * s[10] * s[15] + s[4] * s[11] * s[14] + s[8] * s[6] * s[15] - s[8] * s[7] * s[14] -
               s[12] * s[6] * s[11] + s[12] * s[7] * s[10];
    inv.m[8] = s[4] * s[9] * s[15] - s[4] * s[11] * s[13] - s[8] * s[5] * s[15] + s[8] * s[7] * s[13] +
               s[12] * s[5] * s[11] - s[12] * s[7] * s[9];
    inv.m[12] = -s[4] * s[9] * s[14] + s[4] * s[10] * s[13] + s[8] * s[5] * s[14] - s[8] * s[6] * s[13] -
                s[12] * s[5] * s[10] + s[12] * s[6] * s[9];
    inv.m[1] = -s[1] * s[10] * s[15] + s[1] * s[11] * s[14] + s[9] * s[2] * s[15] - s[9] * s[3] * s[14] -
               s[13] * s[2] * s[11] + s[13] * s[3] * s[10];
    inv.m[5] = s[0] * s[10] * s[15] - s[0] * s[11] * s[14] - s[8] * s[2] * s[15] + s[8] * s[3] * s[14] +
               s[12] * s[2] * s[11] - s[12] * s[3] * s[10];
    inv.m[9] = -s[0] * s[9] * s[15] + s[0] * s[11] * s[13] + s[8] * s[1] * s[15] - s[8] * s[3] * s[13] -
               s[12] * s[1] * s[11] + s[12] * s[3] * s[9];
    inv.m[13] = s[0] * s[9] * s[14] - s[0] * s[10] * s[13] - s[8] * s[1] * s[14] + s[8] * s[2] * s[13] +
                s[12] * s[1] * s[10] - s[12] * s[2] * s[9];
    inv.m[2] = s[1] * s[6] * s[15] - s[1] * s[7] * s[14] - s[5] * s[2] * s[15] + s[5] * s[3] * s[14] +
               s[13] * s[2] * s[7] - s[13] * s[3] * s[6];
    inv.m[6] = -s[0] * s[6] * s[15] + s[0] * s[7] * s[14] + s[4] * s[2] * s[15] - s[4] * s[3] * s[14] -
               s[12] * s[2] * s[7] + s[12] * s[3] * s[6];
    inv.m[10] = s[0] * s[5] * s[15] - s[0] * s[7] * s[13] - s[4] * s[1] * s[15] + s[4] * s[3] * s[13] +
                s[12] * s[1] * s[7] - s[12] * s[3] * s[5];
    inv.m[14] = -s[0] * s[5] * s[14] + s[0] * s[6] * s[13] + s[4] * s[1] * s[14] - s[4] * s[2] * s[13] -
                s[12] * s[1] * s[6] + s[12] * s[2] * s[5];
    inv.m[3] = -s[1] * s[6] * s[11] + s[1] * s[7] * s[10] + s[5] * s[2] * s[11] - s[5] * s[3] * s[10] -
               s[9] * s[2] * s[7] + s[9] * s[3] * s[6];
    inv.m[7] = s[0] * s[6] * s[11] - s[0] * s[7] * s[10] - s[4] * s[2] * s[11] + s[4] * s[3] * s[10] +
               s[8] * s[2] * s[7] - s[8] * s[3] * s[6];
    inv.m[11] = -s[0] * s[5] * s[11] + s[0] * s[7] * s[9] + s[4] * s[1] * s[11] - s[4] * s[3] * s[9] -
                s[8] * s[1] * s[7] + s[8] * s[3] * s[5];
    inv.m[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] + s[4] * s[2] * s[9] +
                s[8] * s[1] * s[6] - s[8] * s[2] * s[5];

    const float det = s[0] * inv.m[0] + s[1] * inv.m[4] + s[2] * inv.m[8] + s[3] * inv.m[12];
    if (det == 0.0f) {
        return Mat4::identity();
    }
    const float invDet = 1.0f / det;
    Mat4 r;
    for (int i = 0; i < 16; ++i) {
        r.m[i] = inv.m[i] * invDet;
    }
    return r;
}

// Right-handed view matrix: camera at eye, looking at target, -Z forward in view space.
[[nodiscard]] inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    const Vec3 f = normalize(target - eye); // forward
    const Vec3 r = normalize(cross(f, up)); // right
    const Vec3 u = cross(r, f);             // corrected up

    Mat4 view = Mat4::identity();
    view.set(0, 0, r.x);
    view.set(0, 1, r.y);
    view.set(0, 2, r.z);
    view.set(1, 0, u.x);
    view.set(1, 1, u.y);
    view.set(1, 2, u.z);
    view.set(2, 0, -f.x);
    view.set(2, 1, -f.y);
    view.set(2, 2, -f.z);
    view.set(0, 3, -dot(r, eye));
    view.set(1, 3, -dot(u, eye));
    view.set(2, 3, dot(f, eye));
    return view;
}

// Reversed-Z, infinite-far, right-handed perspective; depth in [0,1] with
// near plane mapping to depth 1 and infinity to depth 0 (see scalar.hpp).
// The renderer's negative viewport height supplies the Vulkan Y flip.
[[nodiscard]] inline Mat4 perspectiveInfiniteReversedZ(float fovYRadians, float aspect, float nearZ)
{
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 r;
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = 0.0f;
    r.m[11] = -1.0f;
    r.m[14] = nearZ;
    return r;
}

} // namespace sol::core
