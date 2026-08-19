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
