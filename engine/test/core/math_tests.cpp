#include "sol/core/math/math.hpp"

#include "sol/test/test.hpp"

using namespace sol::core;

namespace {

bool nearlyEqual(const Mat4& a, const Mat4& b, float epsilon = 1e-4f)
{
    for (int i = 0; i < 16; ++i) {
        if (!sol::core::nearlyEqual(a.m[i], b.m[i], epsilon)) {
            return false;
        }
    }
    return true;
}

bool nearlyEqualQ(Quat a, Quat b, float epsilon = 1e-5f)
{
    // q and -q are the same rotation
    if (dot(a, b) < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    return sol::core::nearlyEqual(a.x, b.x, epsilon) && sol::core::nearlyEqual(a.y, b.y, epsilon) &&
           sol::core::nearlyEqual(a.z, b.z, epsilon) && sol::core::nearlyEqual(a.w, b.w, epsilon);
}

} // namespace

// --- scalar ---

SOL_TEST(scalarConversionsAndClamp)
{
    SOL_CHECK(nearlyEqual(radians(180.0f), kPi));
    SOL_CHECK(nearlyEqual(degrees(kHalfPi), 90.0f));
    SOL_CHECK(clamp(5, 0, 3) == 3);
    SOL_CHECK(clamp(-1.0f, 0.0f, 3.0f) == 0.0f);
    SOL_CHECK(saturate(1.5f) == 1.0f);
    SOL_CHECK(nearlyEqual(lerp(2.0f, 4.0f, 0.5f), 3.0f));
}

// --- Vec3 ---

SOL_TEST(vec3BasicOps)
{
    const Vec3 a = {1.0f, 2.0f, 3.0f};
    const Vec3 b = {4.0f, -5.0f, 6.0f};
    SOL_CHECK((a + b) == Vec3{5.0f, -3.0f, 9.0f});
    SOL_CHECK((a - b) == Vec3{-3.0f, 7.0f, -3.0f});
    SOL_CHECK((a * 2.0f) == Vec3{2.0f, 4.0f, 6.0f});
    SOL_CHECK(nearlyEqual(dot(a, b), 4.0f - 10.0f + 18.0f));
}

SOL_TEST(vec3CrossIsRightHanded)
{
    const Vec3 x = {1.0f, 0.0f, 0.0f};
    const Vec3 y = {0.0f, 1.0f, 0.0f};
    const Vec3 z = {0.0f, 0.0f, 1.0f};
    SOL_CHECK(nearlyEqual(cross(x, y), z));
    SOL_CHECK(nearlyEqual(cross(y, z), x));
    SOL_CHECK(nearlyEqual(cross(z, x), y));
}

SOL_TEST(vec3NormalizeAndLength)
{
    const Vec3 v = {3.0f, 0.0f, 4.0f};
    SOL_CHECK(nearlyEqual(length(v), 5.0f));
    SOL_CHECK(nearlyEqual(length(normalize(v)), 1.0f));
    SOL_CHECK(normalize(Vec3{}) == Vec3{}); // zero-safe
}

SOL_TEST(dvec3PrecisionAtLargeCoordinates)
{
    // The reason sim space is double: a 1 cm offset 100 million km from origin
    // must survive. In float it would vanish entirely.
    const DVec3 base = {1.0e11, -1.0e11, 5.0e10};
    const DVec3 offset = {0.01, 0.01, 0.01};
    const DVec3 moved = base + offset;
    const Vec3 cameraRelative = (moved - base).toVec3();
    // Double ULP at 1e11 is ~1.5e-5, so the 1 cm offset survives to within micrometers.
    SOL_CHECK(nearlyEqual(cameraRelative, Vec3{0.01f, 0.01f, 0.01f}, 1e-4f));
    SOL_CHECK(nearlyEqual(length(DVec3{3.0e10, 0.0, 4.0e10}), 5.0e10, 1.0));
}

// --- Mat4 ---

SOL_TEST(mat4IdentityIsMultiplicativeNeutral)
{
    Mat4 a = Mat4::identity();
    a.set(0, 3, 7.0f);
    a.set(2, 1, -3.0f);
    SOL_CHECK(nearlyEqual(a * Mat4::identity(), a));
    SOL_CHECK(nearlyEqual(Mat4::identity() * a, a));
}

SOL_TEST(mat4TranslationMovesPointsNotDirections)
{
    const Mat4 t = translation({1.0f, 2.0f, 3.0f});
    SOL_CHECK(nearlyEqual(transformPoint(t, {1.0f, 1.0f, 1.0f}), Vec3{2.0f, 3.0f, 4.0f}));
    SOL_CHECK(nearlyEqual(transformDirection(t, {1.0f, 1.0f, 1.0f}), Vec3{1.0f, 1.0f, 1.0f}));
}

SOL_TEST(mat4RotationYIsRightHanded)
{
    // +90 degrees about +Y takes +X to -Z (right-hand rule).
    const Mat4 r = rotationY(kHalfPi);
    SOL_CHECK(nearlyEqual(transformDirection(r, {1.0f, 0.0f, 0.0f}), Vec3{0.0f, 0.0f, -1.0f}));
    SOL_CHECK(nearlyEqual(transformDirection(r, {0.0f, 0.0f, -1.0f}), Vec3{-1.0f, 0.0f, 0.0f}));
}

SOL_TEST(mat4CompositionAppliesRightToLeft)
{
    // M = T * R: rotate first, then translate.
    const Mat4 m = translation({10.0f, 0.0f, 0.0f}) * rotationY(kHalfPi);
    SOL_CHECK(nearlyEqual(transformPoint(m, {1.0f, 0.0f, 0.0f}), Vec3{10.0f, 0.0f, -1.0f}));
}

SOL_TEST(mat4TransposeRoundTrips)
{
    Mat4 a;
    for (int i = 0; i < 16; ++i) {
        a.m[i] = static_cast<float>(i * i - 3);
    }
    SOL_CHECK(transpose(transpose(a)) == a);
    SOL_CHECK(a.at(1, 2) == transpose(a).at(2, 1));
}

SOL_TEST(mat4InverseRoundTripsForTRS)
{
    const Mat4 m = translation({4.0f, -2.0f, 9.0f}) * rotationZ(0.7f) * rotationX(-1.2f) *
                   scale({2.0f, 3.0f, 0.5f});
    SOL_CHECK(nearlyEqual(m * inverse(m), Mat4::identity(), 1e-3f));
    SOL_CHECK(nearlyEqual(inverse(m) * m, Mat4::identity(), 1e-3f));
    SOL_CHECK(nearlyEqual(inverse(Mat4::identity()), Mat4::identity()));
}

SOL_TEST(mat4InverseOfSingularReturnsIdentity)
{
    SOL_CHECK(nearlyEqual(inverse(Mat4{}), Mat4::identity()));
}

SOL_TEST(mat4LookAtBasics)
{
    // Camera at +5Z looking at origin: origin lands 5 units down -Z in view space.
    const Mat4 view = lookAt({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    SOL_CHECK(nearlyEqual(transformPoint(view, {0.0f, 0.0f, 0.0f}), Vec3{0.0f, 0.0f, -5.0f}));
    // A point right of the target stays right (+X) in view space.
    SOL_CHECK(nearlyEqual(transformPoint(view, {1.0f, 0.0f, 0.0f}), Vec3{1.0f, 0.0f, -5.0f}));
    // The camera position maps to the view-space origin.
    SOL_CHECK(nearlyEqual(transformPoint(view, {0.0f, 0.0f, 5.0f}), Vec3{0.0f, 0.0f, 0.0f}));
}

SOL_TEST(mat4ReversedZProjectionDepthRange)
{
    const float nearZ = 0.1f;
    const Mat4 proj = perspectiveInfiniteReversedZ(radians(60.0f), 16.0f / 9.0f, nearZ);

    // Point on the near plane -> depth 1.
    const Vec4 nearClip = proj * Vec4{0.0f, 0.0f, -nearZ, 1.0f};
    SOL_CHECK(nearlyEqual(nearClip.z / nearClip.w, 1.0f));

    // Very distant point -> depth approaches 0.
    const Vec4 farClip = proj * Vec4{0.0f, 0.0f, -1.0e7f, 1.0f};
    SOL_CHECK(farClip.z / farClip.w < 1e-4f);
    SOL_CHECK(farClip.z / farClip.w >= 0.0f);

    // View-space center stays centered.
    SOL_CHECK(nearlyEqual(nearClip.x, 0.0f));
    SOL_CHECK(nearlyEqual(nearClip.y, 0.0f));
}

// --- Quat ---

SOL_TEST(quatIdentityDoesNothing)
{
    const Vec3 v = {1.0f, -2.0f, 3.0f};
    SOL_CHECK(nearlyEqual(rotate(Quat::identity(), v), v));
}

SOL_TEST(quatAxisAngleMatchesMatrixRotation)
{
    const Quat q = fromAxisAngle({0.0f, 1.0f, 0.0f}, kHalfPi);
    SOL_CHECK(nearlyEqual(rotate(q, {1.0f, 0.0f, 0.0f}), Vec3{0.0f, 0.0f, -1.0f}));
    // toMat4 must agree with direct quaternion rotation.
    const Vec3 v = {0.3f, -0.7f, 1.1f};
    SOL_CHECK(nearlyEqual(transformDirection(toMat4(q), v), rotate(q, v)));
}

SOL_TEST(quatCompositionAppliesRightFirst)
{
    const Quat yaw90 = fromAxisAngle({0.0f, 1.0f, 0.0f}, kHalfPi);
    const Quat pitch90 = fromAxisAngle({1.0f, 0.0f, 0.0f}, kHalfPi);
    // (pitch * yaw): yaw first. +X --yaw--> -Z; then +90 about +X (RH: +Y->+Z,
    // +Z->-Y) takes -Z to +Y.
    const Vec3 result = rotate(pitch90 * yaw90, {1.0f, 0.0f, 0.0f});
    SOL_CHECK(nearlyEqual(result, Vec3{0.0f, 1.0f, 0.0f}));
}

SOL_TEST(quatConjugateInverts)
{
    const Quat q = normalize(Quat{0.2f, -0.4f, 0.1f, 0.8f});
    const Vec3 v = {1.0f, 2.0f, 3.0f};
    SOL_CHECK(nearlyEqual(rotate(conjugate(q), rotate(q, v)), v));
    SOL_CHECK(nearlyEqualQ(q * conjugate(q), Quat::identity()));
}

SOL_TEST(quatSlerpInterpolatesAngle)
{
    const Quat a = Quat::identity();
    const Quat b = fromAxisAngle({0.0f, 1.0f, 0.0f}, kHalfPi);
    SOL_CHECK(nearlyEqualQ(slerp(a, b, 0.0f), a));
    SOL_CHECK(nearlyEqualQ(slerp(a, b, 1.0f), b));
    const Quat half = slerp(a, b, 0.5f);
    const Quat expected = fromAxisAngle({0.0f, 1.0f, 0.0f}, kHalfPi * 0.5f);
    SOL_CHECK(nearlyEqualQ(half, expected, 1e-4f));
    // nlerp agrees with slerp at endpoints and stays normalized.
    SOL_CHECK(nearlyEqual(length(nlerp(a, b, 0.37f)), 1.0f));
}
