#pragma once

// What the last frame actually drew, per level (engine plan Phase 9 stage F).
//
// ⚑⚑ THIS EXISTS BECAUSE OF WHAT STAGE F CANNOT BE MEASURED BY. The whole
// model catalog is 2,298 triangles and the frame is vsync-bound, so no
// frame-rate reading will ever show that LOD selection works - the honest
// evidence is *which level each instance chose*, and that is a number rather
// than a picture. A drive can assert this; it cannot assert a screenshot.
//
// ⚑ It is a process-global for the same reason `sol::core::frameProfiler()` is:
// the console lives in `content.cpp`, which owns no renderer and should not
// learn about one to answer a question about the last frame. It is deliberately
// in its own header with no Vulkan in it, so reading the tally costs the
// console nothing but this file.
//
// Written once per frame by the draw, read by the overlay and by `sol.lods`.

#include <cstdint>

namespace game {

// One more than the number of switch thresholds the policy carries: a model
// can hold level 0 plus one level per threshold.
inline constexpr std::uint32_t kMaxDrawLevels = 3;

struct LodReport
{
    // Opaque meshes drawn at each level this frame.
    std::uint32_t drawn[kMaxDrawLevels] = {};
    // Levels loaded per model, summed over the catalog - so a report with
    // levels loaded but nothing drawn below 0 is a selection problem, and one
    // with no levels loaded at all is a cook problem. The two failures look
    // identical from the frame rate and completely different here.
    std::uint32_t levelsLoaded = 0;
    std::uint32_t modelsWithLevels = 0;
    // ⚑ The largest thing drawn THAT HAS A CHAIN, and the level it chose. The
    // first version reported the largest of anything and read 812.3 px in
    // every sample at every range, because the COCKPIT is a RenderInstance
    // about five metres from the eye carrying an 8 m radius - so it pins the
    // maximum forever and hides the one number the probe exists to show. An
    // instrument dominated by the single instance that can never switch is an
    // instrument that measures nothing.
    float largestChainedRadius = 0.0f;
    std::uint32_t largestChainedLevel = 0;
    // What the projection was against, so a predicted pixel size can be
    // checked rather than assumed - the drive's arithmetic needs the height it
    // actually rendered at, not the one the notes remember.
    float viewportHeight = 0.0f;
};

[[nodiscard]] LodReport& lodReport();

// ⚑⚑ THE LEVER PHASE 17 NEEDED AND STAGE F DID NOT HAVE. `lodReport` says
// which level was drawn; nothing could say which level to draw, so comparing
// level 0 against level 1 at a fixed camera meant editing a `constexpr` and
// rebuilding. That is too slow to sweep a ladder of ranges, and sweeping is
// how the thresholds get set by measurement rather than by taste.
//
// ⚑ It reaches only states the renderer already reaches - it moves the same
// selection the threshold moves - so 8u's rule (a dev lever that reaches a
// state the sim cannot is a second implementation) is satisfied rather than
// dodged. A pinned level is still clamped to the levels a model actually has.
//
// ⚑ It is PERMANENT, not scaffolding: it is the instrument for judging any
// future change to the policy, and stage F's own history is the argument -
// the first `sol.lods` measured nothing and had to be rebuilt into an
// instrument, which is exactly the cost this avoids paying a third time.
inline constexpr std::int32_t kLodPinAutomatic = -1;

[[nodiscard]] std::int32_t& lodPin();

} // namespace game
