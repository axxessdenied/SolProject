/// @file
/// The minimum linear algebra the renderer needs, kept internal.
///
/// Not a public engine math module. Selecting one is a later decision; putting these in
/// `sol::render::detail` keeps that decision open instead of pre-empting it with whatever B1
/// happened to need first.
///
/// The division of precision is the load-bearing part. World-space quantities are `double`,
/// because P1a increment A2 measured that a `float` cannot carry Solar System coordinates at
/// all. Everything handed to the GPU is `float` and camera-relative, because once the camera
/// is the origin the magnitudes are small enough that `float` has room to spare. The
/// conversion happens in exactly one place — @ref cameraRelative — so there is one line to
/// audit when the jitter gate is measured.

#pragma once

#include "Sol/Render/WorldVec3.h"

#include <cmath>
#include <cstdint>

namespace sol::render::detail {

/// World-space vectors are the public @ref WorldVec3; only the operations are internal.
using Vec3d = WorldVec3;

inline Vec3d operator+(const Vec3d& a, const Vec3d& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3d operator-(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3d operator*(const Vec3d& v, double s) { return {v.x * s, v.y * s, v.z * s}; }

inline double dot(const Vec3d& a, const Vec3d& b)
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline Vec3d cross(const Vec3d& a, const Vec3d& b)
{
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

inline double length(const Vec3d& v)
{
    return std::sqrt(dot(v, v));
}

inline Vec3d normalise(const Vec3d& v)
{
    const double magnitude = length(v);
    return magnitude > 0.0 ? v * (1.0 / magnitude) : Vec3d{};
}

/// A float 3-vector destined for a vertex buffer or push constant.
struct Vec3f {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

/// The one place a world-space quantity becomes a GPU-space one.
///
/// Subtracting in `double` and only then narrowing is what keeps the result accurate: the
/// difference is small even when both operands are astronomical, so the narrowing loses
/// nothing that matters. Narrowing first and subtracting afterwards would destroy the result,
/// and is the mistake this function exists to make impossible to write by accident.
inline Vec3f cameraRelative(const Vec3d& worldPosition, const Vec3d& cameraPosition)
{
    const Vec3d relative = worldPosition - cameraPosition;
    return Vec3f{
        static_cast<float>(relative.x),
        static_cast<float>(relative.y),
        static_cast<float>(relative.z),
    };
}

/// Column-major 4x4, matching Vulkan's expectation for a `mat4` push constant.
struct Mat4f {
    /// column[c][r]
    float m[4][4]{};

    static Mat4f identity()
    {
        Mat4f result;
        for (int i = 0; i < 4; ++i) {
            result.m[i][i] = 1.0F;
        }
        return result;
    }
};

inline Mat4f multiply(const Mat4f& a, const Mat4f& b)
{
    Mat4f result;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k][row] * b.m[column][k];
            }
            result.m[column][row] = sum;
        }
    }
    return result;
}

/// A view matrix with the camera at the origin.
///
/// There is deliberately no translation term. The camera *is* the origin, so geometry arrives
/// already relative to it. A conventional view matrix would translate by the camera's world
/// position — reintroducing, in `float`, exactly the large magnitude the frame model exists to
/// keep out of the GPU.
inline Mat4f cameraRelativeView(const Vec3d& forward, const Vec3d& worldUp)
{
    const Vec3d f = normalise(forward);
    const Vec3d s = normalise(cross(f, worldUp));
    const Vec3d u = cross(s, f);

    Mat4f view = Mat4f::identity();
    view.m[0][0] = static_cast<float>(s.x);
    view.m[1][0] = static_cast<float>(s.y);
    view.m[2][0] = static_cast<float>(s.z);
    view.m[0][1] = static_cast<float>(u.x);
    view.m[1][1] = static_cast<float>(u.y);
    view.m[2][1] = static_cast<float>(u.z);
    view.m[0][2] = static_cast<float>(-f.x);
    view.m[1][2] = static_cast<float>(-f.y);
    view.m[2][2] = static_cast<float>(-f.z);
    return view;
}

/// Reversed-Z infinite perspective projection for Vulkan clip space.
///
/// Three properties, each of which the depth gate depends on:
///
/// - **Reversed Z.** Near maps to 1 and far to 0. Floating-point depth has its precision
///   concentrated near zero, and a conventional projection spends that precision at the far
///   plane where nothing needs it. Reversing puts it at the near plane where everything does.
///   This is what makes a surface-to-orbit depth range survive in one buffer, and it is why
///   the requirement set demands a *float* depth format rather than a normalised one.
/// - **Infinite far plane.** There is no far clip. Choosing a far distance for a scene
///   containing both a launch pad and a planet is a choice between clipping the planet and
///   destroying near precision; with reversed Z the infinite form costs nothing.
/// - **Y flip.** Vulkan's clip space has +Y downward, unlike OpenGL's. Baking the flip here
///   keeps it out of every shader.
///
/// @param verticalFovRadians vertical field of view
/// @param aspect             width / height
/// @param nearPlaneMetres    distance to the near plane; the only depth-precision knob left
inline Mat4f reversedZInfinitePerspective(
    double verticalFovRadians,
    double aspect,
    double nearPlaneMetres)
{
    const double focal = 1.0 / std::tan(verticalFovRadians * 0.5);

    Mat4f projection;
    projection.m[0][0] = static_cast<float>(focal / aspect);
    // Negated: Vulkan clip space points +Y down.
    projection.m[1][1] = static_cast<float>(-focal);
    projection.m[2][2] = 0.0F;
    projection.m[2][3] = -1.0F;
    projection.m[3][2] = static_cast<float>(nearPlaneMetres);
    return projection;
}

} // namespace sol::render::detail
