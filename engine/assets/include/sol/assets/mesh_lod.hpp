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
// focal length is 514 px: a station (100 m) crosses 20 px at about 2.6 km and
// 10 px at about 5.1 km, while a ship (8 m) is 4 px tall at a kilometre.
//
// ⚑⚑ THESE TWO NUMBERS ARE MEASURED, AND THE FIRST VERSION OF THEM WAS NOT.
// Stage F shipped {96, 32} with fourteen lines above defending the UNIT and
// nothing at all defending the VALUES, and playtest session 13 reported the
// switch popping - which it does, unmissably, because 96 px of RADIUS is 192
// px across, 27% of the height of a 720-high viewport.
//
// Phase 17 measured it: pin a level, capture, pin the next, capture, count the
// pixels that differ, at a ladder of ranges with the ship dead still. Both
// switches are a clean square law in the projected radius, with the noise
// floor of Lyrioa's own traffic at zero below r = 96:
//
//     changed pixels ~= 0.26 * R^2   for level 0 -> 1
//     changed pixels ~= 0.37 * R^2   for level 1 -> 2
//
// ⚑ THERE IS NO KNEE IN THAT CURVE, so there is no threshold to read off it -
// the decimation error is scale-invariant in PROPORTIONAL terms and only ever
// becomes invisible in ABSOLUTE ones. The ceiling used is ~100 changed pixels,
// anchored on the 88-228 px that ordinary ambient traffic moves between two
// captures of the same scene: a switch smaller than what the world is already
// doing on its own is not a switch anybody can catch. That gives 20 px and
// 10 px, where the events are ~104 and ~37 pixels against ~3,138 and ~379 at
// the old values - roughly 30x and 10x smaller.
//
// ⚑ THE SECOND SWITCH IS 1.4x AS DISRUPTIVE AS THE FIRST at the same screen
// size, which is why the pair is not a fixed ratio: matching the criterion
// rather than the spacing is what puts them at 2:1 instead of the old 3:1.
//
// ⚑ AND THE REASON THIS COSTS NOTHING TO GET WRONG IN THE CONSERVATIVE
// DIRECTION: stage F bought no frame time and never claimed to. The whole
// model catalog is 2,298 triangles and the frame is vsync-bound, so there is
// no budget on the other side of this trade to defend - every pixel of
// earliness was pure cost. Switch as LATE as still switches at all. If a
// future content load makes triangles matter, re-measure with `sol.lod_pin`
// rather than reasoning about it; that lever exists for exactly this.
inline constexpr float kLevelSwitchPixels[] = {20.0f, 10.0f};

// ⚑⚑ HOW FAR A LEVEL MUST CLIMB BACK BEFORE IT IS GIVEN UP (Phase 18), as a
// fraction of the threshold it fell past. Selection is otherwise a pure
// function of the current frame, so a radius parked on a threshold re-decides
// every frame and can answer differently each time - a shimmer while holding
// station, distinct from the pop while approaching that Phase 17 fixed.
//
// ⚑ THE MARGIN IS SPENT ON ONE SIDE ONLY. Dropping detail still happens at
// exactly the measured threshold, so `kLevelSwitchPixels` keeps the pixel
// budget behind it; only the return trip pays. The cost of that asymmetry is
// stated rather than hidden: the up-switch lands at 22 px rather than 20,
// where the event is ~126 changed pixels instead of ~104 - still far under the
// 88-228 px of ambient motion the thresholds were anchored against.
//
// ⚑ A RATIO rather than a pixel count, for the same reason the policy is in
// pixels at all: it means one number for every model at every resolution.
inline constexpr float kLevelSwitchHysteresis = 0.10f;

// Passed as `previousLevel` when there is no history - a first sight, or a
// drawable with no identity to remember (the cockpit is pushed by the game
// layer and has no entity behind it). Selection then answers statelessly.
inline constexpr std::uint32_t kNoPreviousLevel = 0xFFFFFFFFu;

// Which level to draw. `levelCount` counts level 0, so 1 means "no chain" and
// the answer is always 0.
//
// ⚑ It takes PIXELS rather than a radius and a distance so that this header
// needs nothing from `sol::ui` - the projection lives in `pick.hpp` beside the
// focal length it shares with the target pick, and there is exactly one
// expression of "how big is this on screen" in the codebase.
//
// ⚑ AND IT TAKES THE PREVIOUS LEVEL RATHER THAN KEEPING IT, so the rule stays
// a function of its arguments and stays assertable without a device. The
// renderer owns the memory because only the renderer knows which instance is
// which; the POLICY stays here, where `geometry.unit` can reach it. A
// `previousLevel` that is out of range for this chain is ignored rather than
// trusted - a model whose chain shrank under a re-cook must not be indexed
// through a level it no longer has.
[[nodiscard]] std::uint32_t selectMeshLevel(float screenRadiusPixels, std::uint32_t levelCount,
                                            std::uint32_t previousLevel = kNoPreviousLevel);

} // namespace sol::assets
