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

#include <array>
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

/// Whether a forward/up pair defines a view basis at all.
///
/// Two ways it does not: either vector being zero, or the two being parallel. In both cases the
/// side axis is the cross product of parallel vectors — zero — and the roll of the camera about
/// its own view direction is genuinely undefined rather than merely hard to compute.
///
/// This is checked rather than papered over. Substituting a fallback up axis would produce a
/// perfectly plausible image at an arbitrary roll, and for a renderer whose whole purpose this
/// increment is measurement, an arbitrary roll is a silently wrong answer: a centroid measured
/// in a rotated frame is still a number. @ref cameraRelativeView carries this as a
/// precondition, and `Renderer` refuses the frame instead of guessing.
///
/// The threshold is on the sine of the angle between the normalised vectors, so it is an
/// angular tolerance rather than a magnitude one: 1e-6 is about 0.2 arc-seconds.
inline bool isViewBasisWellFormed(const Vec3d& forward, const Vec3d& worldUp)
{
    const double forwardLength = length(forward);
    const double upLength = length(worldUp);
    if (!(forwardLength > 0.0) || !(upLength > 0.0)) {
        return false;
    }
    return length(cross(forward * (1.0 / forwardLength), worldUp * (1.0 / upLength))) > 1.0e-6;
}

/// A view matrix with the camera at the origin.
///
/// There is deliberately no translation term. The camera *is* the origin, so geometry arrives
/// already relative to it. A conventional view matrix would translate by the camera's world
/// position — reintroducing, in `float`, exactly the large magnitude the frame model exists to
/// keep out of the GPU.
///
/// @pre @ref isViewBasisWellFormed holds for @p forward and @p worldUp. With parallel or zero
/// inputs `normalise` returns a zero vector by design and the basis collapses silently, which is
/// why the caller checks first rather than this function returning something defensible.
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

/// Conventional perspective projection: near maps to 0, far maps to 1, finite far plane.
///
/// **This exists only as the depth gate's negative control.** It is the arrangement the
/// reversed-Z infinite projection replaced, and it is here so that the gate can be shown to
/// fail rather than only to pass. A depth test that cannot fail measures nothing — the same
/// lesson the jitter gate's sub-pixel control taught.
///
/// Its failure mode is the point: depth precision is spent at the far plane, where a float's
/// exponent gives almost nothing back, so two distinct distances at long range collapse to the
/// same stored value. That is exactly the "depth collapse" the P1b threshold names.
inline Mat4f conventionalPerspective(
    double verticalFovRadians,
    double aspect,
    double nearPlaneMetres,
    double farPlaneMetres)
{
    const double focal = 1.0 / std::tan(verticalFovRadians * 0.5);
    const double range = nearPlaneMetres - farPlaneMetres;

    Mat4f projection;
    projection.m[0][0] = static_cast<float>(focal / aspect);
    projection.m[1][1] = static_cast<float>(-focal);
    projection.m[2][2] = static_cast<float>(farPlaneMetres / range);
    projection.m[2][3] = -1.0F;
    projection.m[3][2] = static_cast<float>(farPlaneMetres * nearPlaneMetres / range);
    return projection;
}

/// A view frustum as inward-facing half-spaces, in the space its matrix consumes.
///
/// Because the view matrix is camera-relative and carries no translation, the planes extracted
/// from a view-projection come out in **camera-relative world space** — the same space terrain
/// vertices are emitted in, so a node's centre needs only the camera subtracted before testing.
struct Frustum {
    struct Plane {
        /// Unit-length, pointing into the frustum. `dot(normal, p) + distance >= 0` is inside.
        Vec3d normal;
        double distance = 0.0;
    };

    /// Five planes under the reversed-Z infinite projection, six under the conventional control.
    std::array<Plane, 6> planes{};
    std::uint32_t count = 0;
};

/// Gribb-Hartmann plane extraction, skipping planes the projection does not actually impose.
///
/// The clip conditions are `-w <= x,y <= w` and `0 <= z <= w`, and each becomes a row combination
/// of the matrix. The **infinite far plane shows up as a degenerate row**: under reversed Z the
/// `z >= 0` condition reduces to `near >= 0`, whose xyz part is exactly zero, because there is no
/// far clip to impose. Dropping zero-length normals is therefore not defensive coding against a
/// case that cannot happen — it is how the infinite projection's missing plane is handled, and it
/// is also what lets the same function serve the conventional control, where that row is a real
/// far plane and six planes come back instead of five.
inline Frustum extractFrustum(const Mat4f& viewProjection)
{
    const auto row = [&viewProjection](int r) {
        return std::array<double, 4>{
            static_cast<double>(viewProjection.m[0][r]),
            static_cast<double>(viewProjection.m[1][r]),
            static_cast<double>(viewProjection.m[2][r]),
            static_cast<double>(viewProjection.m[3][r]),
        };
    };
    const std::array<double, 4> r0 = row(0);
    const std::array<double, 4> r1 = row(1);
    const std::array<double, 4> r2 = row(2);
    const std::array<double, 4> r3 = row(3);

    const auto combine = [](const std::array<double, 4>& a,
                            const std::array<double, 4>& b,
                            double sign) {
        return std::array<double, 4>{
            a[0] + (sign * b[0]),
            a[1] + (sign * b[1]),
            a[2] + (sign * b[2]),
            a[3] + (sign * b[3]),
        };
    };

    const std::array<std::array<double, 4>, 6> candidates{
        combine(r3, r0, 1.0),   // left:   w + x >= 0
        combine(r3, r0, -1.0),  // right:  w - x >= 0
        combine(r3, r1, 1.0),   // bottom: w + y >= 0
        combine(r3, r1, -1.0),  // top:    w - y >= 0
        r2,                     // z >= 0: degenerate under the infinite projection
        combine(r3, r2, -1.0),  // w - z >= 0: the near plane under reversed Z
    };

    Frustum frustum;
    for (const std::array<double, 4>& candidate : candidates) {
        const Vec3d normal{candidate[0], candidate[1], candidate[2]};
        const double magnitude = length(normal);
        if (!(magnitude > 0.0)) {
            continue;
        }
        const double scale = 1.0 / magnitude;
        frustum.planes[frustum.count] = Frustum::Plane{normal * scale, candidate[3] * scale};
        ++frustum.count;
    }
    return frustum;
}

/// Whether a sphere lies wholly outside the frustum, and is therefore certainly invisible.
///
/// Conservative in the direction that matters: it rejects only when the sphere is entirely
/// beyond a plane, so a sphere straddling one is kept. Combined with a bounding radius that
/// overestimates a node's extent, nothing that could contribute a pixel is ever removed — which
/// is the property that makes culling safe to add while the LOD gate's pop detector cannot fire.
[[nodiscard]] inline bool sphereOutsideFrustum(
    const Frustum& frustum,
    const Vec3d& centre,
    double radius)
{
    for (std::uint32_t i = 0; i < frustum.count; ++i) {
        if (dot(frustum.planes[i].normal, centre) + frustum.planes[i].distance < -radius) {
            return true;
        }
    }
    return false;
}

} // namespace sol::render::detail
