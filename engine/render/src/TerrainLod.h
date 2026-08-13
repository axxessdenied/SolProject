/// @file
/// Cube-sphere terrain with continuous distance-based level of detail.
///
/// A real-scale planet cannot be one mesh: Earth's surface at one-metre resolution is on the
/// order of 10^14 vertices. The renderer must therefore choose, every frame, which parts of the
/// surface to represent finely and which coarsely — and the P1b LOD gate is about whether that
/// choice is *visible*. A scheme that swaps meshes abruptly pops; the gate forbids popping.
///
/// The scheme here is CDLOD (continuous distance-dependent LOD):
///
///   - The sphere is six quadtrees, one per cube face, projected onto the sphere.
///   - A node subdivides when the camera is inside that level's range, so detail follows the
///     camera rather than a fixed tessellation.
///   - **Every vertex carries both its own position and the position it would occupy at the
///     next coarser level**, and a per-patch morph factor blends between them. As the camera
///     approaches a transition the fine mesh continuously *becomes* the coarse one, so the
///     switch happens when the two are already identical. That is what makes transitions
///     unpoppable rather than merely fast.
///
/// The morph is the load-bearing part and the thing the gate actually tests. It can be
/// disabled, which is the LOD gate's negative control: without it the same traversal must
/// produce detectable pops, and a detector that cannot see them is not measuring anything.

#pragma once

#include "RenderMath.h"

#include <cstdint>
#include <vector>

namespace sol::render::detail {

/// One terrain vertex: where it is, and where it would be one level coarser.
///
/// Carrying both is what allows the blend to happen in the vertex shader. Computing the coarse
/// position there instead would mean re-deriving the parent grid's sampling, which is exactly
/// the kind of duplicated arithmetic that drifts and reintroduces a seam.
struct TerrainVertex {
    /// Position **relative to the camera**, in metres.
    ///
    /// Camera-relative and not planet-relative, and the difference is not cosmetic. A
    /// planet-relative position is around 6.4e6 m, where a `float` quantises to half a metre;
    /// subtracting the camera in the shader would then be catastrophic cancellation of two
    /// large nearly-equal floats, and the terrain would visibly quantise near the viewer. The
    /// subtraction happens on the CPU in `double`, which is the same rule the rest of the
    /// renderer follows and the reason terrain is rebuilt as the camera moves.
    Vec3f position;
    /// The same vertex evaluated on the next coarser grid, also camera-relative.
    Vec3f coarsePosition;

    /// Terrain height normalised to roughly 0..1, for shading.
    ///
    /// Shading must be a smooth function of the surface, never of the LOD level. Colouring by
    /// level would paint a visible seam at exactly the transitions the gate measures, and the
    /// detector would then be measuring the debug colouring rather than the geometry.
    float heightUnit = 0.0F;
    /// The coarse grid's height, blended alongside the position so shading morphs too.
    float coarseHeightUnit = 0.0F;
};

/// A drawable block of terrain.
struct TerrainPatch {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::int32_t vertexOffset = 0;
    /// 0 = use the fine position, 1 = fully morphed to the coarse position.
    float morph = 0.0F;
    /// Quadtree depth, recorded so the harness can see how detail is distributed.
    std::uint32_t level = 0;
};

/// What one frame's terrain selection produced.
struct TerrainFrame {
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TerrainPatch> patches;

    /// How many quadtree nodes were visited and how many were emitted. Recorded because a
    /// selection that thrashes between levels shows up here before it shows up as a pop.
    std::uint32_t nodesVisited = 0;
};

/// Planet shape and detail configuration.
struct TerrainConfig {
    /// Planet radius in metres. Earth by default (ADR 0008 reference ellipsoid, equatorial).
    double radiusMetres = 6378136.6;
    /// Peak-to-trough height variation, metres. Terrain must have relief or the LOD gate is
    /// vacuous: a perfectly smooth sphere looks identical at every level and cannot pop.
    double reliefMetres = 8000.0;
    /// Grid resolution per patch edge. Vertex count per patch is (grid + 1)^2.
    ///
    /// Eight rather than sixteen. Terrain is regenerated on the CPU every frame, so vertex
    /// count is the dominant cost; and a coarser grid makes the difference between adjacent
    /// LOD levels *larger*, which is the property the LOD gate needs to be able to detect.
    std::uint32_t gridResolution = 8;
    /// Deepest quadtree level. Sets the finest ground resolution.
    std::uint32_t maxLevel = 10;
    /// Distance, in multiples of a node's world size, within which that node subdivides.
    /// Larger means detail persists further out, at more triangles.
    double subdivisionFactor = 2.5;
    /// Fraction of a level's range over which the morph runs. The blend must complete before
    /// the switch, so this cannot be zero without reintroducing a pop.
    double morphBand = 0.35;

    /// Disables morphing. **Negative control only** — this is the configuration the LOD gate
    /// must be able to detect as popping.
    bool morphEnabled = true;
};

/// Deterministic terrain height above the reference radius, in metres.
///
/// Procedural rather than authored: ADR 0012 assigns parametric shapes to procedural
/// generation, and an authored heightfield would drag an asset pipeline into an increment that
/// gates on precision and continuity rather than on content. Deterministic because the LOD gate
/// compares consecutive frames, and terrain that differed between evaluations would register as
/// popping that the LOD scheme did not cause.
[[nodiscard]] double terrainHeight(const Vec3d& unitDirection, const TerrainConfig& config);

/// Selects and builds the terrain for one camera position.
///
/// @param config      planet shape and detail settings
/// @param cameraPlanetRelative  camera position relative to the planet centre, metres
/// @param out         reused between frames so the allocation does not churn per frame
void buildTerrain(
    const TerrainConfig& config,
    const Vec3d& cameraPlanetRelative,
    TerrainFrame& out);

} // namespace sol::render::detail
