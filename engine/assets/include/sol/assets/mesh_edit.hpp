#pragma once

// Editable mesh representation (engine plan Phase 9 stage B). `MeshData` is a
// GPU upload buffer - a flat run of unshared corners - and merging, moving a
// vertex, optimising and decimating all need something a buffer cannot give
// them: adjacency. This is that something, and it is deliberately headless and
// pure so the operations can be proved without a device.
//
// ⚑ The load-bearing decision: adjacency is built over POSITIONS, not over
// render vertices. A hard edge splits a corner without splitting the surface -
// a box has 8 points and 24 corners - so an operation that treated the corners
// as the graph would find 24 disconnected islands and tear the solid apart at
// every crease.

#include "sol/assets/asset_loader.hpp"
#include "sol/core/math/vec.hpp"

#include <cstdint>
#include <vector>

namespace sol::assets {

// Position tolerance decides TOPOLOGY; the other two only decide whether two
// corners standing at the same point may share one render vertex.
struct WeldOptions
{
    float positionTolerance = 1e-5f;
    float normalTolerance = 1e-3f;
    float uvTolerance = 1e-5f;
};

// A render vertex: one corner of one face, carrying whatever makes it distinct
// from another corner at the same point.
struct EditVertex
{
    std::uint32_t position = 0;
    core::Vec3 normal{};
    core::Vec2 uv{};
};

struct EditMesh
{
    std::vector<core::Vec3> positions;
    std::vector<EditVertex> vertices;
    std::vector<std::uint32_t> indices; // three per triangle, into `vertices`

    [[nodiscard]] std::uint32_t triangleCount() const
    {
        return static_cast<std::uint32_t>(indices.size() / 3);
    }

    [[nodiscard]] core::Vec3 pointOf(std::uint32_t vertex) const
    {
        return positions[vertices[vertex].position];
    }

    // The three positions of a face - the triple every topological operation
    // actually works on.
    [[nodiscard]] std::uint32_t facePosition(std::uint32_t face, std::uint32_t corner) const
    {
        return vertices[indices[(face * 3) + corner]].position;
    }
};

// Compressed adjacency over the position graph. Rebuilt rather than
// maintained: every operation that changes topology invalidates it, and a
// stale adjacency is the bug this whole representation exists to avoid.
struct MeshAdjacency
{
    struct Edge
    {
        std::uint32_t a = 0; // a < b
        std::uint32_t b = 0;
        std::uint32_t firstFace = 0; // into edgeFaces
        std::uint32_t faceCount = 0;
    };

    std::vector<Edge> edges; // sorted by (a, b)
    std::vector<std::uint32_t> edgeFaces;
    std::vector<std::uint32_t> positionFaceStart; // positions.size() + 1
    std::vector<std::uint32_t> positionFaces;

    // Manifold: no edge carries more than two faces. Closed: every edge
    // carries exactly two, so the surface has no border.
    [[nodiscard]] bool isManifold() const;
    [[nodiscard]] bool isClosed() const;
    [[nodiscard]] std::uint32_t borderEdgeCount() const;
    [[nodiscard]] const Edge* findEdge(std::uint32_t a, std::uint32_t b) const;
};

// Welds a soup into an edit mesh. Positions within tolerance become one point;
// corners agreeing on all three attributes become one render vertex.
[[nodiscard]] EditMesh toEditMesh(const MeshData& data, const WeldOptions& options = {});
[[nodiscard]] MeshData toMeshData(const EditMesh& mesh);

[[nodiscard]] MeshAdjacency buildAdjacency(const EditMesh& mesh);

// Re-welds in place. Idempotent: welding a welded mesh at the same tolerance
// changes nothing.
void weld(EditMesh& mesh, const WeldOptions& options = {});

// Appends one mesh onto another. Nothing is welded across the seam - two
// solids that touch stay two surfaces until `weld` is asked for, because
// merging geometry and merging topology are different intentions.
void append(EditMesh& mesh, const EditMesh& addition);

// Drops faces with a repeated position or an area at or below the epsilon,
// then anything left unreferenced. Returns the number of faces removed.
std::uint32_t removeDegenerateFaces(EditMesh& mesh, float areaEpsilon = 1e-12f);

// Drops render vertices and positions nothing refers to. Returns vertices removed.
std::uint32_t removeUnused(EditMesh& mesh);

// smoothAngleDegrees <= 0 gives per-face (flat) normals. Otherwise faces that
// share an edge and meet at a shallower angle share a normal, and a steeper
// crease splits the corner. Uvs are carried through untouched.
void recomputeNormals(EditMesh& mesh, float smoothAngleDegrees = 0.0f);

// Reorders triangles for the post-transform cache, then renumbers vertices in
// first-use order for the fetch cache. Geometry and topology are untouched -
// only the order they are visited in changes.
//
// ⚑ It keeps the BETTER of the two orders, measured. A greedy reorder cannot
// beat an order that is already good, and `MeshBuilder`'s output is: on the
// committed assets this pass made the cache worse on the gate and the station
// until it was made conditional. So it is safe to run on anything, and on
// authored geometry it is usually a no-op.
void optimizeIndices(EditMesh& mesh);

// Average cache misses per triangle under a FIFO cache of `cacheSize`; 3.0 is
// the worst case and ~0.6 is about the floor for a closed triangle mesh.
[[nodiscard]] float averageCacheMissRatio(const EditMesh& mesh, std::uint32_t cacheSize = 32);

struct DecimateOptions
{
    std::uint32_t targetTriangles = 0;
    // Refuse any collapse whose quadric error exceeds this; 0 means no ceiling.
    double maxError = 0.0;
    // Keep border edges where they are, so an open surface holds its outline.
    bool preserveBorders = true;
};

// Quadric-error edge collapse, the LOD generator's engine. Returns the
// triangle count actually reached, which can be above the target when the
// topology refuses further collapses.
//
// ⚑ Attributes are carried, not recomputed: a collapse moves a point, so the
// normals around it go stale. Call recomputeNormals afterwards if the result
// is going to be shaded. Keeping that separate is deliberate - decimation must
// not silently decide whether a hull is faceted or smooth.
std::uint32_t decimate(EditMesh& mesh, const DecimateOptions& options);

// Divergence-theorem volume, signed, so winding matters. A closed solid's
// volume is invariant under welding, appending and index optimisation, which
// makes it the cheap assertion that an operation moved nothing it shouldn't.
[[nodiscard]] double signedVolume(const EditMesh& mesh);
[[nodiscard]] double surfaceArea(const EditMesh& mesh);

struct MeshBounds
{
    core::Vec3 min{};
    core::Vec3 max{};
};

[[nodiscard]] MeshBounds bounds(const EditMesh& mesh);

// The radius a model def would carry: the furthest point from the origin the
// mesh is authored around.
[[nodiscard]] float boundingRadius(const EditMesh& mesh);

} // namespace sol::assets
