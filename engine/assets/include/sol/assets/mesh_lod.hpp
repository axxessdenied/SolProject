#pragma once

// LOD chain generation and level selection (engine plan Phase 9 stage F).
//
// ⚑ The geometry this needs was already built and already tested at stage B -
// `mesh_edit`'s quadric `decimate` is the engine here and nothing in this file
// re-implements any of it. What stage F found missing was not an operation but
// a POLICY: which meshes deserve a chain, when a generated level is good
// enough to ship, and which level a frame should draw. That is what lives
// here, and it is headless and pure so all three answers can be asserted
// without a device.
//
// ⚑⚑ THE THREE ACCEPTANCE BANDS ARE EACH THE SHAPE OF A DEFECT MEASURED ON
// THE REAL ASSETS, NOT ROUND NUMBERS PICKED FOR NEATNESS:
//
//   - VOLUME, because a cube decimated to two triangles is still CLOSED, still
//     MANIFOLD and still border-free while enclosing nothing at all. Phase
//     16's invariants cannot tell a solid from a flat sliver; the signed
//     volume is the only thing that can, which is the third time stage E has
//     landed on that same fact.
//   - RADIUS, because a quadric's optimal position is not constrained to the
//     original hull, so a collapse pushes points OUTWARD - the station grows
//     +1.89% at a tenth of its triangles. `models.toml` already gives it
//     radius 100 against a mesh that measures 102, which the E1-E3 debt slice
//     recorded as the collision sphere sitting INSIDE the hull. An unbounded
//     LOD widens a defect this project has already measured.
//   - COOKED BYTES, because following `mesh_edit.hpp`'s own correct advice to
//     recompute normals after a collapse makes a level BIGGER than the mesh it
//     replaces: flat normals unshare every corner at three vertices per
//     triangle, and the station's half-triangle level came out at +49% of the
//     source's bytes. A level bigger than its source is not a level.
//
// ⚑ Every level is generated from LEVEL 0 rather than from the level above it,
// so error does not compound down the chain and the drifts below are all
// measured against the source the game actually ships.

#include "sol/assets/asset_loader.hpp"
#include "sol/assets/mesh_edit.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sol::assets {

// The cooked size of a mesh in bytes - header, then vertices, then indices,
// which is exactly what the cooker's `encodeMesh` writes and what
// `cooker.unit` already pins the layout of.
[[nodiscard]] std::size_t cookedMeshBytes(const MeshData& mesh);

// One generated level, with what it cost and how far it moved.
struct MeshLevel
{
    MeshData mesh;
    std::uint32_t triangles = 0;
    float boundingRadius = 0.0f;
    double volume = 0.0;
    std::size_t cookedBytes = 0;
    // Signed and relative, against LEVEL 0 - cumulative drift is what a viewer
    // sees, and a level that grew is as wrong as one that shrank.
    double volumeDrift = 0.0;
    double radiusDrift = 0.0;
};

struct LodOptions
{
    // ⚑ A mesh below this has nothing to give, and the measurement is stark: at
    // half its triangles the 1,068-triangle station loses 0.35% of its volume
    // and the 12-triangle cube loses 66.67%. A hand-built part carries no
    // redundancy, so refusing it is the correct answer and not a failure. On
    // the committed assets this floor admits station, gate and asteroid.
    std::uint32_t minimumSourceTriangles = 256;
    // Each level targets this fraction of LEVEL 0's triangle count.
    float ratio = 0.5f;
    std::uint32_t maxLevels = 2;
    // Fractions, absolute value, against level 0. Measured: these accept the
    // station's and the asteroid's two levels and reject the gate's second,
    // which loses 16.78% of its volume.
    double maxVolumeDrift = 0.10;
    double maxRadiusDrift = 0.01;
    // ⚑ Re-shading FLAT is what inflates a level past its source. Smooth costs
    // a crease at a shallow angle and keeps the corners shared.
    float smoothAngleDegrees = 40.0f;
    WeldOptions weld{};
};

struct LodChain
{
    // Levels 1..N. Level 0 is the caller's own source and is never in here -
    // the pipeline must not rewrite the bytes the game already loads.
    std::vector<MeshLevel> levels;
    // Why the chain stopped, always set and always in words an author can act
    // on. A chain that stops early is the normal case.
    std::string stopReason;
};

[[nodiscard]] LodChain buildLodChain(const MeshData& source, const LodOptions& options = {});

// ⚑⚑ THE POLICY IS IN PIXELS RATHER THAN METRES, AND THE CONTENT FORCES IT.
// This game's model radii run from 1 m (an asteroid, sized by its instance
// scale) to 106.7 m (a gate), and its camera covers four orders of magnitude of
// distance. A metre threshold would be one hand-tuned number per model AND
// wrong at another resolution; a pixel threshold is one number for every model
// at every resolution. It also disposes of the cockpit - a RenderInstance
// drawn at zero distance - with no special case at all, because a thing that
// fills the screen is never below any threshold.
//
// Level `i` takes over when the projected screen RADIUS falls below
// kLevelSwitchPixels[i - 1]. Descending, and the caller clamps to the levels
// that actually exist. For scale, at 720p and a 70 degree vertical FOV the
// focal length is 514 px: a station (100 m) crosses 96 px at about 535 m and
// 32 px at about 1.6 km, while a ship (8 m) is 4 px tall at a kilometre.
inline constexpr float kLevelSwitchPixels[] = {96.0f, 32.0f};

// Which level to draw. `levelCount` counts level 0, so 1 means "no chain" and
// the answer is always 0.
//
// ⚑ It takes PIXELS rather than a radius and a distance so that this header
// needs nothing from `sol::ui` - the projection lives in `pick.hpp` beside the
// focal length it shares with the target pick, and there is exactly one
// expression of "how big is this on screen" in the codebase.
[[nodiscard]] std::uint32_t selectMeshLevel(float screenRadiusPixels, std::uint32_t levelCount);

} // namespace sol::assets
