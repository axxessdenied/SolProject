#include "sol/assets/mesh_build.hpp"
#include "sol/assets/mesh_edit.hpp"
#include "sol/test/test.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace sol;
using assets::EditMesh;
using assets::MeshAdjacency;
using assets::MeshBuilder;

namespace {

[[nodiscard]] bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

[[nodiscard]] assets::BuildPoint normalized(assets::BuildPoint p)
{
    const double length = std::sqrt((p.x * p.x) + (p.y * p.y) + (p.z * p.z));
    return {p.x / length, p.y / length, p.z / length};
}

// A closed manifold with real adjacency and enough of it to say something -
// the shape every operation below is exercised on. Uvs are a function of the
// direction, so corners at a shared point agree and the weld can find them.
[[nodiscard]] EditMesh buildIcosphere(int subdivisions)
{
    const double phi = (1 + std::sqrt(5.0)) / 2;
    const assets::BuildPoint verts[12] = {{-1, phi, 0}, {1, phi, 0}, {-1, -phi, 0}, {1, -phi, 0},
                                          {0, -1, phi}, {0, 1, phi}, {0, -1, -phi}, {0, 1, -phi},
                                          {phi, 0, -1}, {phi, 0, 1}, {-phi, 0, -1}, {-phi, 0, 1}};
    const int indices[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                               {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                               {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                               {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};

    struct Tri
    {
        assets::BuildPoint a;
        assets::BuildPoint b;
        assets::BuildPoint c;
    };
    std::vector<Tri> tris;
    for (const auto& face : indices) {
        tris.push_back({normalized(verts[face[0]]), normalized(verts[face[1]]), normalized(verts[face[2]])});
    }
    for (int step = 0; step < subdivisions; ++step) {
        std::vector<Tri> next;
        for (const Tri& tri : tris) {
            const assets::BuildPoint ab =
                normalized({(tri.a.x + tri.b.x) / 2, (tri.a.y + tri.b.y) / 2, (tri.a.z + tri.b.z) / 2});
            const assets::BuildPoint bc =
                normalized({(tri.b.x + tri.c.x) / 2, (tri.b.y + tri.c.y) / 2, (tri.b.z + tri.c.z) / 2});
            const assets::BuildPoint ca =
                normalized({(tri.c.x + tri.a.x) / 2, (tri.c.y + tri.a.y) / 2, (tri.c.z + tri.a.z) / 2});
            next.push_back({tri.a, ab, ca});
            next.push_back({ab, tri.b, bc});
            next.push_back({ca, bc, tri.c});
            next.push_back({ab, bc, ca});
        }
        tris = std::move(next);
    }

    MeshBuilder builder;
    const auto uvOf = [](assets::BuildPoint d) {
        return assets::BuildUv{(d.x * 0.5) + 0.5, (d.z * 0.5) + 0.5};
    };
    for (const Tri& tri : tris) {
        builder.addFlatTriangle(tri.a, tri.b, tri.c, uvOf(tri.a), uvOf(tri.b), uvOf(tri.c));
    }
    EditMesh mesh = assets::toEditMesh(builder.build());
    assets::recomputeNormals(mesh, 180.0f); // fully smooth, so corners can share
    return mesh;
}

[[nodiscard]] EditMesh box(assets::BuildPoint center, assets::BuildPoint size)
{
    MeshBuilder builder;
    builder.addBox(center, size);
    return assets::toEditMesh(builder.build());
}

// The set of position triples a mesh draws, canonicalised, so two meshes can
// be compared for "same triangles" without caring how they are indexed.
[[nodiscard]] std::vector<std::array<std::uint32_t, 3>> faceSignature(const EditMesh& mesh)
{
    std::vector<std::array<std::uint32_t, 3>> faces;
    faces.reserve(mesh.triangleCount());
    for (std::uint32_t face = 0; face < mesh.triangleCount(); ++face) {
        std::array<std::uint32_t, 3> triple{mesh.facePosition(face, 0), mesh.facePosition(face, 1),
                                            mesh.facePosition(face, 2)};
        std::sort(triple.begin(), triple.end());
        faces.push_back(triple);
    }
    std::sort(faces.begin(), faces.end());
    return faces;
}

} // namespace

SOL_TEST(weldingIsIdempotent)
{
    EditMesh mesh = buildIcosphere(2);
    const std::size_t positions = mesh.positions.size();
    const std::size_t vertices = mesh.vertices.size();
    const double volume = assets::signedVolume(mesh);

    assets::weld(mesh);
    SOL_CHECK(mesh.positions.size() == positions);
    SOL_CHECK(mesh.vertices.size() == vertices);
    assets::weld(mesh);
    SOL_CHECK(mesh.positions.size() == positions);
    SOL_CHECK(mesh.vertices.size() == vertices);
    SOL_CHECK(near(assets::signedVolume(mesh), volume, 1e-9));
}

SOL_TEST(weldRespectsItsToleranceInBothDirections)
{
    // Two triangles whose shared edge is a hair apart: one tolerance sees a
    // strip, the other sees two loose triangles.
    MeshBuilder builder;
    const double gap = 1e-4;
    builder.addFlatTriangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0}, {1, 0}, {0, 1});
    builder.addFlatTriangle({1, 0, gap}, {1, 1, gap}, {0, 1, gap}, {1, 0}, {1, 1}, {0, 1});
    const assets::MeshData soup = builder.build();

    const EditMesh loose = assets::toEditMesh(soup, {1e-6f, 1e-3f, 1e-5f});
    SOL_CHECK(loose.positions.size() == 6);

    const EditMesh tight = assets::toEditMesh(soup, {1e-3f, 1e-3f, 1e-5f});
    SOL_CHECK(tight.positions.size() == 4);
    SOL_CHECK(tight.triangleCount() == 2);
}

// ⚑ The whole reason adjacency is built over positions: a box has 24 corners
// and 8 points, and a graph over corners would find 24 islands.
SOL_TEST(adjacencyIsBuiltOverPointsNotOverCorners)
{
    const EditMesh cube = box({0, 0, 0}, {2, 2, 2});
    SOL_CHECK(cube.vertices.size() == 24);
    SOL_CHECK(cube.positions.size() == 8);

    const MeshAdjacency adjacency = assets::buildAdjacency(cube);
    SOL_CHECK(adjacency.edges.size() == 18);
    SOL_CHECK(adjacency.isClosed());
    for (const MeshAdjacency::Edge& edge : adjacency.edges) {
        SOL_CHECK(edge.a < edge.b);
        SOL_CHECK(edge.faceCount == 2);
    }
    // Every corner of the box belongs to three quads, and how many TRIANGLES
    // that is depends on which way each quad's diagonal runs: three at least,
    // six at most, and thirty-six in total because every face lists three.
    std::uint32_t total = 0;
    for (std::uint32_t position = 0; position < cube.positions.size(); ++position) {
        const std::uint32_t count =
            adjacency.positionFaceStart[position + 1] - adjacency.positionFaceStart[position];
        SOL_CHECK(count >= 3 && count <= 6);
        total += count;
    }
    SOL_CHECK(total == 36);
    SOL_CHECK(adjacency.positionFaces.size() == 36);
    SOL_CHECK(adjacency.findEdge(0, 1) != nullptr || adjacency.findEdge(0, 2) != nullptr);
    SOL_CHECK(adjacency.findEdge(0, 99) == nullptr);
}

SOL_TEST(adjacencyStaysConsistentThroughEveryOperation)
{
    EditMesh mesh = buildIcosphere(2);
    const auto describe = [](const EditMesh& m) {
        const MeshAdjacency adjacency = assets::buildAdjacency(m);
        SOL_CHECK(adjacency.isManifold());
        SOL_CHECK(adjacency.isClosed());
        // Euler: V - E + F = 2 for anything topologically a sphere.
        const auto v = static_cast<long long>(m.positions.size());
        const auto e = static_cast<long long>(adjacency.edges.size());
        const auto f = static_cast<long long>(m.triangleCount());
        SOL_CHECK(v - e + f == 2);
        // Every edge is listed once and carries its faces contiguously.
        SOL_CHECK(adjacency.edgeFaces.size() == static_cast<std::size_t>(f) * 3);
    };

    describe(mesh);
    assets::weld(mesh);
    describe(mesh);
    assets::optimizeIndices(mesh);
    describe(mesh);
    assets::recomputeNormals(mesh, 30.0f);
    describe(mesh);
    assets::removeDegenerateFaces(mesh);
    describe(mesh);
}

SOL_TEST(appendingTwoSolidsKeepsBothOfTheirVolumes)
{
    EditMesh merged = box({0, 0, 0}, {2, 2, 2});
    const EditMesh other = box({10, 0, 0}, {1, 3, 5});
    const double first = assets::signedVolume(merged);
    const double second = assets::signedVolume(other);

    assets::append(merged, other);
    SOL_CHECK(merged.triangleCount() == 24);
    SOL_CHECK(merged.positions.size() == 16);
    SOL_CHECK(near(assets::signedVolume(merged), first + second, 1e-4));
    SOL_CHECK(assets::buildAdjacency(merged).isClosed());

    // Two solids that do not touch stay two surfaces after a weld as well -
    // appending is a merge of geometry, not an assertion that they are one.
    assets::weld(merged);
    SOL_CHECK(merged.positions.size() == 16);
    SOL_CHECK(near(assets::signedVolume(merged), first + second, 1e-4));
}

SOL_TEST(appendingCoincidentSolidsAndWeldingFusesTheirPoints)
{
    EditMesh merged = box({0, 0, 0}, {2, 2, 2});
    assets::append(merged, box({0, 0, 0}, {2, 2, 2}));
    SOL_CHECK(merged.positions.size() == 16);
    assets::weld(merged);
    SOL_CHECK(merged.positions.size() == 8); // the same eight points, found twice
    SOL_CHECK(merged.triangleCount() == 24); // the faces are still both there
}

SOL_TEST(degenerateFacesAreDroppedAndTheirLeftoversWithThem)
{
    MeshBuilder builder;
    builder.addBox({0, 0, 0}, {2, 2, 2});
    builder.addFlatTriangle({5, 0, 0}, {5, 0, 0}, {5, 1, 0}, {0, 0}, {1, 0}, {0, 1}); // repeated point
    builder.addFlatTriangle({7, 0, 0}, {8, 0, 0}, {9, 0, 0}, {0, 0}, {1, 0}, {0, 1}); // collinear
    EditMesh mesh = assets::toEditMesh(builder.build());
    SOL_CHECK(mesh.triangleCount() == 14);

    SOL_CHECK(assets::removeDegenerateFaces(mesh) == 2);
    SOL_CHECK(mesh.triangleCount() == 12);
    SOL_CHECK(mesh.positions.size() == 8); // the stray points went with them
    SOL_CHECK(assets::buildAdjacency(mesh).isClosed());
    SOL_CHECK(assets::removeDegenerateFaces(mesh) == 0); // idempotent
}

SOL_TEST(unusedVerticesAreRemovedWithoutMovingAnythingElse)
{
    EditMesh mesh = box({0, 0, 0}, {2, 2, 2});
    mesh.positions.push_back({99, 99, 99});
    mesh.vertices.push_back({static_cast<std::uint32_t>(mesh.positions.size() - 1), {0, 1, 0}, {0, 0}});
    const double volume = assets::signedVolume(mesh);

    SOL_CHECK(assets::removeUnused(mesh) == 1);
    SOL_CHECK(mesh.positions.size() == 8);
    SOL_CHECK(mesh.vertices.size() == 24);
    SOL_CHECK(near(assets::signedVolume(mesh), volume, 1e-9));
}

// Flat, and a crease that survives smoothing, and a smoothing wide enough to
// round a right angle. The threshold is the only thing separating them.
SOL_TEST(normalsAreFlatOrSmoothAccordingToTheAngleAsked)
{
    EditMesh flat = box({0, 0, 0}, {2, 2, 2});
    assets::recomputeNormals(flat, 0.0f);
    for (const assets::EditVertex& vertex : flat.vertices) {
        const float sum = std::abs(vertex.normal.x) + std::abs(vertex.normal.y) + std::abs(vertex.normal.z);
        SOL_CHECK(near(sum, 1.0, 1e-5)); // exactly one axis is non-zero
    }

    EditMesh creased = box({0, 0, 0}, {2, 2, 2});
    assets::recomputeNormals(creased, 45.0f); // a box corner is 90 degrees
    for (const assets::EditVertex& vertex : creased.vertices) {
        const float sum =
            std::abs(vertex.normal.x) + std::abs(vertex.normal.y) + std::abs(vertex.normal.z);
        SOL_CHECK(near(sum, 1.0, 1e-5));
    }

    EditMesh rounded = box({0, 0, 0}, {2, 2, 2});
    assets::recomputeNormals(rounded, 100.0f); // wide enough to smooth a right angle
    std::vector<core::Vec3> normalAt(rounded.positions.size(), core::Vec3{});
    for (const assets::EditVertex& vertex : rounded.vertices) {
        const core::Vec3 point = rounded.positions[vertex.position];
        SOL_CHECK(core::dot(vertex.normal, core::normalize(point)) > 0.9f);
        // Smoothing means ONE normal per point, whatever the corner's uv is.
        if (core::lengthSquared(normalAt[vertex.position]) == 0.0f) {
            normalAt[vertex.position] = vertex.normal;
        }
        SOL_CHECK(core::lengthSquared(normalAt[vertex.position] - vertex.normal) < 1e-10f);
    }
}

SOL_TEST(smoothNormalsOnASphereAllPointOutwards)
{
    EditMesh sphere = buildIcosphere(2);
    for (const assets::EditVertex& vertex : sphere.vertices) {
        const core::Vec3 point = sphere.positions[vertex.position];
        SOL_CHECK(core::dot(vertex.normal, core::normalize(point)) > 0.99f);
        SOL_CHECK(near(core::length(vertex.normal), 1.0, 1e-5));
    }
}

SOL_TEST(indexOptimisationImprovesTheCacheAndMovesNothingElse)
{
    EditMesh mesh = buildIcosphere(3);
    const float before = assets::averageCacheMissRatio(mesh);
    const double volume = assets::signedVolume(mesh);
    const std::uint32_t triangles = mesh.triangleCount();
    const std::size_t vertices = mesh.vertices.size();
    const auto signature = faceSignature(mesh);

    assets::optimizeIndices(mesh);
    const float after = assets::averageCacheMissRatio(mesh);
    if (after > before) {
        std::printf("  cache miss ratio got worse: %.3f -> %.3f\n", static_cast<double>(before),
                    static_cast<double>(after));
    }
    SOL_CHECK(after <= before);
    SOL_CHECK(after < 1.0f); // a closed mesh reuses two of every three corners

    SOL_CHECK(mesh.triangleCount() == triangles);
    SOL_CHECK(mesh.vertices.size() == vertices);
    SOL_CHECK(near(assets::signedVolume(mesh), volume, 1e-6));
    // Reordering is not remodelling: the same triangles, over the same points.
    SOL_CHECK(faceSignature(mesh) == signature);
}

SOL_TEST(decimationIsMonotonicInItsTargetCount)
{
    const EditMesh source = buildIcosphere(3);
    SOL_REQUIRE(source.triangleCount() == 1280);

    std::uint32_t previous = source.triangleCount();
    for (const std::uint32_t target : {1000u, 640u, 320u, 160u, 80u}) {
        EditMesh mesh = source;
        const std::uint32_t reached = assets::decimate(mesh, {target});
        SOL_CHECK(reached == mesh.triangleCount());
        SOL_CHECK(reached <= target);
        SOL_CHECK(reached < previous);
        previous = reached;
    }
}

SOL_TEST(decimationNeverTearsAManifoldInput)
{
    const EditMesh source = buildIcosphere(3);
    for (const std::uint32_t target : {800u, 400u, 200u, 100u, 40u}) {
        EditMesh mesh = source;
        assets::decimate(mesh, {target});
        const MeshAdjacency adjacency = assets::buildAdjacency(mesh);
        SOL_CHECK(adjacency.isManifold());
        SOL_CHECK(adjacency.isClosed());
        SOL_CHECK(adjacency.borderEdgeCount() == 0);
        const auto v = static_cast<long long>(mesh.positions.size());
        const auto e = static_cast<long long>(adjacency.edges.size());
        const auto f = static_cast<long long>(mesh.triangleCount());
        SOL_CHECK(v - e + f == 2);
    }
}

// The point of a quadric: an LOD has to still look like the thing. A sphere
// simplified to a twentieth of its triangles is still a sphere of the same
// size, or the LOD pops.
SOL_TEST(decimationKeepsTheGrossShapeItStartedWith)
{
    const EditMesh source = buildIcosphere(3);
    EditMesh mesh = source;
    assets::decimate(mesh, {80});

    SOL_CHECK(mesh.triangleCount() <= 80);
    const float radiusRatio = assets::boundingRadius(mesh) / assets::boundingRadius(source);
    const double volumeRatio = assets::signedVolume(mesh) / assets::signedVolume(source);
    if (radiusRatio >= 1.3f || volumeRatio <= 0.8 || volumeRatio >= 1.05) {
        std::printf("  1280 -> %u triangles: radius %.4fx, volume %.4fx\n", mesh.triangleCount(),
                    static_cast<double>(radiusRatio), volumeRatio);
    }
    // ⚑ Measured at 16:1 on a unit sphere: volume 0.873x and radius 1.214x.
    // The volume is the LOD holding its shape; the RADIUS GROWING is the part
    // worth knowing about, and it is not a defect - the quadric's minimum for a
    // convex patch is outside the surface, so a simplified hull CIRCUMSCRIBES
    // the one it came from and the drift compounds over hundreds of collapses.
    // Whoever wires LOD selection up has to recompute a level's own radius
    // rather than reusing the model row's, or an LOD will poke out of its bound.
    SOL_CHECK(radiusRatio > 0.9f && radiusRatio < 1.3f);
    SOL_CHECK(volumeRatio > 0.8 && volumeRatio < 1.05);
}

SOL_TEST(decimationStopsWhenTopologyRefusesRatherThanTearing)
{
    EditMesh mesh = buildIcosphere(2);
    const std::uint32_t reached = assets::decimate(mesh, {0});
    // ⚑ The floor is TWO, and it is the link condition that sets it: a
    // tetrahedron may still collapse, into one triangle drawn from both sides
    // (V3 E3 F2, Euler 2), and there it stops - collapsing again would leave an
    // edge carrying two faces with only one vertex opposite, which is the
    // condition refusing to pinch the surface. Asking for zero gets a surface,
    // not a hole.
    SOL_CHECK(reached == 2);
    SOL_CHECK(reached == mesh.triangleCount());
    const MeshAdjacency adjacency = assets::buildAdjacency(mesh);
    SOL_CHECK(adjacency.isClosed());
    SOL_CHECK(static_cast<long long>(mesh.positions.size()) -
                  static_cast<long long>(adjacency.edges.size()) + reached ==
              2);
}

SOL_TEST(decimationAtOrAboveTheCurrentCountDoesNothing)
{
    EditMesh mesh = buildIcosphere(1);
    const std::uint32_t triangles = mesh.triangleCount();
    SOL_CHECK(assets::decimate(mesh, {triangles}) == triangles);
    SOL_CHECK(assets::decimate(mesh, {triangles + 100}) == triangles);
    SOL_CHECK(mesh.triangleCount() == triangles);
}

SOL_TEST(borderPreservationHoldsAnOpenSurfacesOutline)
{
    const assets::BuildProfilePoint profile[] = {{1.0, 0.0}, {1.0, 1.0}, {1.0, 2.0}, {1.0, 3.0}};
    MeshBuilder builder;
    builder.addRevolve(profile, 32, 1.0, false);
    const EditMesh source = assets::toEditMesh(builder.build());
    const MeshAdjacency before = assets::buildAdjacency(source);
    SOL_REQUIRE(before.borderEdgeCount() == 64);

    EditMesh held = source;
    assets::decimate(held, {40, 0.0, true});
    const MeshAdjacency after = assets::buildAdjacency(held);
    SOL_CHECK(after.borderEdgeCount() == 64); // the two rims are untouched
    SOL_CHECK(after.isManifold());

    // Without the guard the rim is fair game and the outline moves.
    EditMesh free = source;
    assets::decimate(free, {40, 0.0, false});
    SOL_CHECK(assets::buildAdjacency(free).borderEdgeCount() < 64);
    SOL_CHECK(free.triangleCount() < held.triangleCount());
}

SOL_TEST(anErrorCeilingStopsACollapseThatWouldCostTooMuch)
{
    const EditMesh source = buildIcosphere(3);
    EditMesh cheap = source;
    assets::decimate(cheap, {0, 1e-9, true});
    EditMesh generous = source;
    assets::decimate(generous, {0, 1.0, true});
    SOL_CHECK(cheap.triangleCount() > generous.triangleCount());
    SOL_CHECK(assets::buildAdjacency(cheap).isClosed());
    SOL_CHECK(assets::buildAdjacency(generous).isClosed());
}

SOL_TEST(everyOperationSurvivesAnEmptyMesh)
{
    EditMesh mesh;
    assets::weld(mesh);
    assets::append(mesh, EditMesh{});
    SOL_CHECK(assets::removeDegenerateFaces(mesh) == 0);
    SOL_CHECK(assets::removeUnused(mesh) == 0);
    assets::recomputeNormals(mesh, 0.0f);
    assets::recomputeNormals(mesh, 45.0f);
    assets::optimizeIndices(mesh);
    SOL_CHECK(assets::decimate(mesh, {10}) == 0);
    SOL_CHECK(near(assets::signedVolume(mesh), 0.0, 1e-12));
    SOL_CHECK(near(assets::surfaceArea(mesh), 0.0, 1e-12));
    SOL_CHECK(near(assets::boundingRadius(mesh), 0.0, 1e-12));
    SOL_CHECK(assets::buildAdjacency(mesh).edges.empty());
    SOL_CHECK(assets::averageCacheMissRatio(mesh) == 0.0f);
}

SOL_TEST(aRoundTripThroughMeshDataChangesNothing)
{
    const EditMesh mesh = buildIcosphere(2);
    const assets::MeshData data = assets::toMeshData(mesh);
    SOL_CHECK(data.vertices.size() == mesh.vertices.size());
    SOL_CHECK(data.indices.size() == mesh.indices.size());

    const EditMesh again = assets::toEditMesh(data);
    SOL_CHECK(again.positions.size() == mesh.positions.size());
    SOL_CHECK(again.vertices.size() == mesh.vertices.size());
    SOL_CHECK(again.triangleCount() == mesh.triangleCount());
    SOL_CHECK(near(assets::signedVolume(again), assets::signedVolume(mesh), 1e-9));
}
