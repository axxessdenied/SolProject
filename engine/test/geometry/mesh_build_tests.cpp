#include "sol/assets/mesh_build.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/test/test.hpp"

#include <cmath>
#include <vector>

using namespace sol;
using assets::MeshBuilder;

namespace {

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] assets::EditMesh welded(const MeshBuilder& builder)
{
    return assets::toEditMesh(builder.build());
}

[[nodiscard]] bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

} // namespace

SOL_TEST(boxIsAClosedSolidOfItsOwnVolume)
{
    MeshBuilder builder;
    builder.addBox({1, 2, 3}, {2, 4, 6});
    SOL_CHECK(builder.vertexCount() == 24);
    SOL_CHECK(builder.triangleCount() == 12);

    const assets::EditMesh mesh = welded(builder);
    SOL_CHECK(mesh.positions.size() == 8);
    const assets::MeshAdjacency adjacency = assets::buildAdjacency(mesh);
    SOL_CHECK(adjacency.isClosed());
    // Positive volume is the winding test: an inside-out box measures negative.
    SOL_CHECK(near(assets::signedVolume(mesh), 48.0, 1e-3));
    SOL_CHECK(near(assets::surfaceArea(mesh), (2 * ((2 * 4) + (4 * 6) + (2 * 6))), 1e-3));

    const assets::MeshBounds box = assets::bounds(mesh);
    SOL_CHECK(near(box.min.x, 0.0, 1e-6) && near(box.max.x, 2.0, 1e-6));
    SOL_CHECK(near(box.min.y, 0.0, 1e-6) && near(box.max.y, 4.0, 1e-6));
    SOL_CHECK(near(box.min.z, 0.0, 1e-6) && near(box.max.z, 6.0, 1e-6));
}

// The reason `tile` exists: a 2 m dash and a 200 m hull wearing one 256 px
// panel are the same stretch at 0..1, and only one of them is ever seen close.
SOL_TEST(tilingSizesUvsFromTheSurfaceRatherThanStretchingThem)
{
    MeshBuilder stretched;
    stretched.addBox({0, 0, 0}, {2, 4, 6});
    MeshBuilder tiled;
    tiled.addBox({0, 0, 0}, {2, 4, 6}, 0.5);

    const assets::MeshData a = stretched.build();
    const assets::MeshData b = tiled.build();
    SOL_REQUIRE(a.vertices.size() == b.vertices.size());

    float stretchedMax = 0.0f;
    float tiledMax = 0.0f;
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        stretchedMax = std::max(stretchedMax, std::max(a.vertices[i].uv[0], a.vertices[i].uv[1]));
        tiledMax = std::max(tiledMax, std::max(b.vertices[i].uv[0], b.vertices[i].uv[1]));
        // Same geometry either way - only the uvs move.
        SOL_CHECK(a.vertices[i].position[0] == b.vertices[i].position[0]);
    }
    SOL_CHECK(near(stretchedMax, 1.0, 1e-6));
    SOL_CHECK(near(tiledMax, 3.0, 1e-6)); // the 6 m face at half a repeat per meter
}

SOL_TEST(beamRunsBetweenItsEndsWithTheSectionItWasGiven)
{
    MeshBuilder builder;
    builder.addBeam({0, 0, 0}, {10, 0, 0}, 2, 4);
    const assets::EditMesh mesh = welded(builder);
    SOL_CHECK(mesh.positions.size() == 8);
    SOL_CHECK(assets::buildAdjacency(mesh).isClosed());
    SOL_CHECK(near(assets::signedVolume(mesh), 10.0 * 2.0 * 4.0, 1e-3));

    const assets::MeshBounds box = assets::bounds(mesh);
    SOL_CHECK(near(box.min.x, 0.0, 1e-5) && near(box.max.x, 10.0, 1e-5));
    SOL_CHECK(near(box.max.y - box.min.y, 4.0, 1e-5));
    SOL_CHECK(near(box.max.z - box.min.z, 2.0, 1e-5));
}

// A vertical beam is the case the reference axis exists for: cross(d, up) is
// zero when the run IS up, and the frame would collapse.
SOL_TEST(aVerticalBeamStillGetsAUsableFrame)
{
    MeshBuilder builder;
    builder.addBeam({0, -5, 0}, {0, 5, 0}, 2, 2);
    const assets::EditMesh mesh = welded(builder);
    SOL_CHECK(mesh.positions.size() == 8);
    SOL_CHECK(assets::buildAdjacency(mesh).isClosed());
    SOL_CHECK(near(assets::signedVolume(mesh), 10.0 * 2.0 * 2.0, 1e-3));
}

SOL_TEST(aBeamWithNoLengthEmitsNothing)
{
    MeshBuilder builder;
    builder.addBeam({1, 1, 1}, {1, 1, 1}, 2, 2);
    SOL_CHECK(builder.vertexCount() == 0);
    SOL_CHECK(builder.triangleCount() == 0);
}

SOL_TEST(torusIsClosedAndMeasuresWhatPappusSaysItShould)
{
    MeshBuilder builder;
    builder.addTorus(10, 2, 64, 32, 1);
    const assets::EditMesh mesh = welded(builder);
    const assets::MeshAdjacency adjacency = assets::buildAdjacency(mesh);
    SOL_CHECK(adjacency.isManifold());
    SOL_CHECK(adjacency.isClosed());

    // A segmented torus is inscribed, so it comes in under the analytic figure
    // and closes on it as the segment count rises - within 1% at 64x32.
    const double analytic = 2 * kPi * kPi * 10.0 * 2.0 * 2.0;
    const double measured = assets::signedVolume(mesh);
    SOL_CHECK(measured > 0.0);
    SOL_CHECK(measured < analytic);
    SOL_CHECK(measured > analytic * 0.99);

    for (const assets::EditVertex& vertex : mesh.vertices) {
        SOL_CHECK(near(core::length(vertex.normal), 1.0, 1e-5));
    }
}

SOL_TEST(flatTriangleTakesItsNormalFromItsWinding)
{
    MeshBuilder builder;
    builder.addFlatTriangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0}, {1, 0}, {0, 1});
    const assets::MeshData mesh = builder.build();
    SOL_REQUIRE(mesh.vertices.size() == 3);
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        SOL_CHECK(near(vertex.normal[0], 0.0, 1e-6));
        SOL_CHECK(near(vertex.normal[1], 0.0, 1e-6));
        SOL_CHECK(near(vertex.normal[2], 1.0, 1e-6));
    }
}

SOL_TEST(revolveWithCapsIsAClosedCylinder)
{
    const assets::BuildProfilePoint profile[] = {{1.0, 0.0}, {1.0, 4.0}};
    MeshBuilder builder;
    builder.addRevolve(profile, 96, 1.0, true);

    const assets::EditMesh mesh = welded(builder);
    const assets::MeshAdjacency adjacency = assets::buildAdjacency(mesh);
    SOL_CHECK(adjacency.isManifold());
    SOL_CHECK(adjacency.isClosed());

    const double analytic = kPi * 1.0 * 1.0 * 4.0;
    const double measured = assets::signedVolume(mesh);
    SOL_CHECK(measured > 0.0); // outward winding
    SOL_CHECK(measured < analytic);
    SOL_CHECK(measured > analytic * 0.99);
}

SOL_TEST(revolveWithoutCapsLeavesExactlyTwoBorderLoops)
{
    const assets::BuildProfilePoint profile[] = {{1.0, 0.0}, {1.0, 4.0}};
    MeshBuilder builder;
    builder.addRevolve(profile, 24, 1.0, false);

    const assets::EditMesh mesh = welded(builder);
    const assets::MeshAdjacency adjacency = assets::buildAdjacency(mesh);
    SOL_CHECK(adjacency.isManifold());
    SOL_CHECK(!adjacency.isClosed());
    SOL_CHECK(adjacency.borderEdgeCount() == 48); // one ring at each end
}

SOL_TEST(extrudeOfARectangularOutlineIsTheSameSolidAsABeam)
{
    const assets::BuildProfilePoint outline[] = {{-1, -2}, {1, -2}, {1, 2}, {-1, 2}};
    MeshBuilder extruded;
    extruded.addExtrude(outline, {0, 0, 0}, {10, 0, 0});
    MeshBuilder beam;
    beam.addBeam({0, 0, 0}, {10, 0, 0}, 2, 4);

    const assets::EditMesh a = welded(extruded);
    const assets::EditMesh b = welded(beam);
    // Eight corners plus the two fan centres an extruded cap is closed with -
    // a beam caps with a quad and needs neither.
    SOL_CHECK(a.positions.size() == b.positions.size() + 2);
    SOL_CHECK(assets::buildAdjacency(a).isClosed());
    SOL_CHECK(near(assets::signedVolume(a), assets::signedVolume(b), 1e-3));
    SOL_CHECK(near(assets::surfaceArea(a), assets::surfaceArea(b), 1e-3));
}

SOL_TEST(degenerateSweepsRefuseToEmitRatherThanEmittingRubbish)
{
    const assets::BuildProfilePoint two[] = {{1, 0}, {1, 1}};
    MeshBuilder builder;
    builder.addRevolve(two, 2, 1.0, true); // fewer than three segments
    SOL_CHECK(builder.vertexCount() == 0);

    const assets::BuildProfilePoint line[] = {{0, 0}, {1, 0}};
    builder.addExtrude(line, {0, 0, 0}, {0, 0, 1}); // fewer than three outline points
    SOL_CHECK(builder.vertexCount() == 0);

    const assets::BuildProfilePoint square[] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    builder.addExtrude(square, {0, 0, 0}, {0, 0, 0}); // no run
    SOL_CHECK(builder.vertexCount() == 0);
}

SOL_TEST(clearReturnsABuilderToEmpty)
{
    MeshBuilder builder;
    builder.addBox({0, 0, 0}, {1, 1, 1});
    SOL_CHECK(builder.vertexCount() == 24);
    builder.clear();
    SOL_CHECK(builder.vertexCount() == 0);
    SOL_CHECK(builder.triangleCount() == 0);
    SOL_CHECK(builder.build().vertices.empty());
}

// ⚑ Stage E needs this: a drag happens in the frame the mesh is built in and
// the number it has to write is in the part's own frame.
//
// ⚑ NOT BUILT FROM 90 DEGREES, which round-trips by luck - the gotcha this repo
// paid for is that 30 degrees comes back as 29.999999999999996 and 90, 45 and
// 35 survive exactly. A test that used the safe angles would pass over a broken
// inverse.
SOL_TEST(buildTransformInverseRoundTripsARotatedNonUniformScale)
{
    const assets::BuildTransform transform =
        assets::BuildTransform::fromTrs({10.0, -2.5, 4.0}, {0.35, 0.7, -0.2}, {2.0, 0.5, 1.5});
    assets::BuildTransform inverse;
    SOL_REQUIRE(transform.inverse(inverse));

    const assets::BuildPoint samples[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-3.25, 7.5, 0.125}};
    for (const assets::BuildPoint& p : samples) {
        const assets::BuildPoint there = transform.transformPoint(p);
        const assets::BuildPoint back = inverse.transformPoint(there);
        SOL_CHECK(near(back.x, p.x, 1e-12));
        SOL_CHECK(near(back.y, p.y, 1e-12));
        SOL_CHECK(near(back.z, p.z, 1e-12));
    }

    // Composing the two gives the identity, which is the same claim made
    // against the operator the part tree actually uses.
    const assets::BuildTransform composed = transform * inverse;
    SOL_CHECK(near(composed.x.x, 1.0, 1e-12));
    SOL_CHECK(near(composed.y.y, 1.0, 1e-12));
    SOL_CHECK(near(composed.z.z, 1.0, 1e-12));
    SOL_CHECK(near(composed.translation.x, 0.0, 1e-12));
    SOL_CHECK(near(composed.translation.y, 0.0, 1e-12));
    SOL_CHECK(near(composed.translation.z, 0.0, 1e-12));
}

// ⚑ A mirroring transform has a NEGATIVE determinant, and this is where the
// adjugate trap lives: the normal matrix a few lines away is the adjugate
// WITHOUT the division, so it carries that sign. A true inverse divides, and a
// test that only ever used a right-handed transform would never tell the two
// apart.
SOL_TEST(buildTransformInverseSurvivesAMirroringTransform)
{
    const assets::BuildTransform mirrored =
        assets::BuildTransform::fromTrs({1.0, 2.0, 3.0}, {0.0, 0.35, 0.0}, {-2.0, 1.0, 1.0});
    SOL_CHECK(mirrored.determinant() < 0.0);

    assets::BuildTransform inverse;
    SOL_REQUIRE(mirrored.inverse(inverse));
    const assets::BuildPoint p{4.0, -1.5, 0.25};
    const assets::BuildPoint back = inverse.transformPoint(mirrored.transformPoint(p));
    SOL_CHECK(near(back.x, p.x, 1e-12));
    SOL_CHECK(near(back.y, p.y, 1e-12));
    SOL_CHECK(near(back.z, p.z, 1e-12));
}

// A part scaled flat has no frame to write a point back into, and saying so is
// the difference between a refused edit and a number derived from a division
// by zero.
SOL_TEST(buildTransformInverseRefusesASingularTransform)
{
    const assets::BuildTransform flattened =
        assets::BuildTransform::fromTrs({0, 0, 0}, {0, 0, 0}, {1.0, 0.0, 1.0});
    assets::BuildTransform inverse;
    SOL_CHECK(!flattened.inverse(inverse));
}

// The identity is its own inverse, and it matters that this is EXACT rather
// than near: an untransformed part is the common case, and a drag on one must
// write back the number the author typed, not that number plus an epsilon.
SOL_TEST(buildTransformInverseOfTheIdentityIsExactlyTheIdentity)
{
    assets::BuildTransform inverse;
    SOL_REQUIRE(assets::BuildTransform{}.inverse(inverse));
    SOL_CHECK(inverse.isIdentity());

    const assets::BuildPoint p{-2.6, -1.3, -1.0};
    const assets::BuildPoint back = inverse.transformPoint(p);
    SOL_CHECK(back.x == p.x);
    SOL_CHECK(back.y == p.y);
    SOL_CHECK(back.z == p.z);
}
