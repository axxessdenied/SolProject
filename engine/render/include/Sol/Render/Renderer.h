/// @file
/// The presenting renderer: device, swapchain, depth buffer, and frame loop.
///
/// No Vulkan type crosses this interface (ADR 0002). The window is handed over as the
/// operating system's own handles, so `sol::platform` and `sol::render` share a seam without
/// sharing a library.

#pragma once

#include "Sol/Render/DeviceCapabilities.h"
#include "Sol/Render/WorldVec3.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sol::render {

class VulkanInstance;

/// The window the renderer presents to, in OS terms.
struct SurfaceTarget {
    /// `HWND` on Windows.
    void* nativeWindow = nullptr;
    /// `HINSTANCE` on Windows.
    void* nativeInstance = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// Which physical device to use, when the default policy is not what a measurement needs.
///
/// The default — every field empty — is the production policy: prefer a discrete GPU, and
/// accept the first otherwise-acceptable device when there is none. Constraining it exists for
/// the P1b [reference-hardware evidence
/// plan](../../SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md), which
/// requires every gating threshold to be measured on **both** available devices under their own
/// names. No shipping code path has a reason to set it.
///
/// @warning **A constraint that matches nothing is an error, never a fallback.** A harness that
/// asked for the integrated GPU and silently received the discrete one would publish its
/// numbers under the wrong device name — which is the exact failure the evidence plan exists to
/// prevent, and it would be invisible in the output because every other field of the report
/// would be internally consistent. @ref Renderer::create fails with a diagnostic naming what was
/// requested and every device that was enumerated.
struct DeviceSelection {
    /// Case-insensitive substring of the device name. Empty imposes no name constraint.
    std::string nameContains;

    /// Restrict to one device category. Absent imposes no kind constraint.
    std::optional<DeviceKind> kind;

    /// True when this imposes no constraint at all, which selects the production policy.
    [[nodiscard]] bool unconstrained() const
    {
        return nameContains.empty() && !kind.has_value();
    }
};

/// Renders a selection as the diagnostic prints it, e.g. `kind=IntegratedGpu, name contains "UHD"`.
std::string toString(const DeviceSelection& selection);

/// Camera pose in the authoritative world frame.
///
/// Position is `double` and world-space; the renderer subtracts it before anything reaches the
/// GPU. @ref nearPlaneMetres is the only depth-precision control, because the projection has
/// no far plane — see the reversed-Z rationale in the implementation.
/// How depth is distributed across the view frustum.
enum class ProjectionMode : std::uint8_t {
    /// The production arrangement: near maps to 1, far to 0, no far plane.
    ReversedZInfinite,
    /// Near maps to 0, far to 1, finite far plane.
    ///
    /// **Negative control only.** This is what reversed-Z replaced, and it exists so the depth
    /// gate can be demonstrated to fail rather than only to pass. Never select it for anything
    /// that produces a result about the renderer's real behaviour.
    ConventionalFinite,
};

struct CameraState {
    WorldVec3 position;
    WorldVec3 forward{0.0, 0.0, -1.0};
    WorldVec3 up{0.0, 1.0, 0.0};
    double verticalFovRadians = 1.0472; // 60 degrees
    double nearPlaneMetres = 0.1;

    ProjectionMode projection = ProjectionMode::ReversedZInfinite;
    /// Used only by @ref ProjectionMode::ConventionalFinite.
    double farPlaneMetres = 1.0e7;
};

/// One object in the reference scene.
///
/// A unit cube scaled by @ref radiusMetres and placed at @ref worldPosition. Crude on purpose:
/// B1 gates on depth behaviour and precision, not on content, and authored geometry would add
/// an asset pipeline dependency this increment has no reason to take on.
struct SceneObject {
    WorldVec3 worldPosition;
    double radiusMetres = 1.0;
    float colour[3]{1.0F, 1.0F, 1.0F};

    /// Render with a smooth radial intensity profile instead of flat shading.
    ///
    /// Set this for the screen-space jitter gate's reference marker. A hard-edged marker
    /// cannot be centroided to sub-pixel accuracy without antialiasing — coverage is binary,
    /// so the centroid quantises to about a pixel and a 0.25-pixel gate becomes unmeasurable.
    /// A smooth profile carries the sub-pixel information in intensity, where a luminance
    /// weighted centroid can recover it.
    bool smoothMarker = false;
};

/// Named scalable quality tiers.
///
/// ADR 0002 requires "a conservative required baseline for older hardware and optional visual
/// features for stronger GPUs", and the P1b plan lists "representative scalable quality
/// settings, including a conservative low tier" as a B1 deliverable.
///
/// @par The knob that cannot scale down
/// The intuitive quality control is @ref TerrainSettings::subdivisionFactor — "detail persists
/// further out, at more triangles" — and it is **unavailable as a low-tier lever**. It has a
/// hard validity floor at roughly 2.8, because a per-vertex morph needs its band wide compared
/// with a patch or the factor sweeps 0 to 1 across a single patch and distorts it. The default
/// is 3.0, so the entire downward range is about 7% before the scheme stops being
/// well-conditioned. Measured at 0.6, the morph produced *more* popping than no morphing.
///
/// So every tier scales on @ref TerrainSettings::maxLevel and @ref TerrainSettings::gridResolution
/// instead, and **`subdivisionFactor` is 3.0 at all three**. It cannot go down, and raising it for
/// the high tier was measured and rejected — see @ref TerrainSettings::withQuality. This is a
/// property of the CDLOD scheme rather than a choice, and it is recorded here because "just lower
/// the subdivision factor" is the first thing anyone will try.
///
/// @par Every tier is gate-validated
/// The LOD continuity threshold is defined "at the recorded quality setting", so a tier is only
/// usable once `render.lod-gate` has been run at it. All three below have been, on the RTX 4060,
/// Release, 2026-08-14; the figures are in `evidence/p1b/B1/Index.md`.
enum class QualityTier : std::uint8_t {
    /// The conservative baseline ADR 0002 requires. Coarsest terrain, fewest vertices per patch.
    Low,
    /// The tier every other B1 gate result is measured at, and the shipped default.
    Medium,
    /// Optional detail for stronger GPUs: two more quadtree levels, same patch grid.
    High,
};

std::string_view toString(QualityTier tier);

/// Planet terrain settings, or absent for no terrain.
struct TerrainSettings {
    /// Planet centre in the authoritative world frame.
    WorldVec3 centre;
    double radiusMetres = 6378136.6;
    double reliefMetres = 8000.0;
    std::uint32_t maxLevel = 12;

    /// How close the camera must be, in multiples of a patch's world size, before that patch
    /// subdivides. **This is the quality setting**, and the LOD gate is defined "at the
    /// recorded quality setting" rather than in the abstract.
    ///
    /// Larger values tessellate more finely for the same view and cost more triangles. Values
    /// large enough make LOD transitions sub-pixel, at which point nothing can pop and the
    /// gate passes for a reason that has nothing to do with the LOD scheme — a pass worth
    /// distrusting. A representative setting keeps the geometric error near the perceptual
    /// limit, which is where a shipping renderer would put it for performance and where
    /// morphing is actually load-bearing.
    ///
    /// **There is a hard lower bound of roughly 2.8, and this default sits just above it.** The
    /// morph band is at most this many patch-widths, and a per-vertex morph needs the band wide
    /// compared with a patch or the factor sweeps 0 to 1 across a single patch and distorts it.
    /// Measured: at 0.6 the per-vertex morph produced *more* popping than no morphing at all.
    /// The default was 2.5 — below the floor — which handed every caller an ill-conditioned
    /// configuration; it is 3.0 to match the value the LOD gate is measured at.
    double subdivisionFactor = 3.0;

    /// Grid resolution per patch edge; vertices per patch are `(gridResolution + 1)^2`.
    ///
    /// A quality lever that scales cost quadratically without touching the LOD scheme's
    /// conditioning, which is what makes it usable for the low tier where `subdivisionFactor`
    /// is not. Morphing is per-vertex, so a coarser grid changes how much geometry each patch
    /// carries, not whether the morph is continuous across a transition.
    std::uint32_t gridResolution = 8;

    /// Continuous LOD morphing. Disabling it is the LOD gate's negative control: the same
    /// traverse must then produce detectable popping, and a detector that cannot see it is not
    /// measuring anything.
    bool morphEnabled = true;

    /// The settings for @p tier, leaving @ref centre, @ref radiusMetres and @ref reliefMetres
    /// at their current values — those describe the world, not the quality level.
    ///
    /// @note This returns settings; it does not validate them against the LOD gate. A tier is
    /// only usable once the gate has been run at it, because the LOD continuity threshold is
    /// defined "at the recorded quality setting" and a tier nobody measured is a setting nobody
    /// can claim.
    [[nodiscard]] TerrainSettings withQuality(QualityTier tier) const;
};

/// What one presented frame did, for the performance and jitter reports.
struct FrameStats {
    /// Frames presented since creation.
    std::uint64_t frameIndex = 0;
    /// True when the swapchain was rebuilt this frame, which invalidates timing for it.
    bool swapchainRebuilt = false;

    /// True only when this call actually rendered and presented an image.
    ///
    /// False for every path that returns without drawing — a minimised window most of all,
    /// which @ref Renderer::renderFrame documents as a normal state rather than an error. The
    /// distinction has to be visible to the caller: the readback buffers keep their previous
    /// contents, so a measurement harness that cannot tell a skipped frame from a rendered one
    /// will read the last frame's pixels again and score the duplicate as a sample.
    bool presented = false;

    /// Terrain patches drawn and quadtree nodes visited this frame. A selection that thrashes
    /// between levels shows up in these counts before it shows up as a visible pop.
    std::uint32_t terrainPatches = 0;
    std::uint32_t terrainNodesVisited = 0;
    std::uint32_t terrainVertices = 0;

    /// Bytes currently allocated through the renderer's device allocator.
    ///
    /// **This one cannot vary under the current design**, and saying so is the point. Every
    /// allocation happens once during creation and capacity is fixed, so a flat line here is
    /// arithmetic rather than an observation — which is why the earlier "memory is bounded"
    /// claim was withdrawn. The three fields below exist so the measurement has instruments that
    /// *can* move.
    std::uint64_t deviceAllocatedBytes = 0;

    /// Bytes the allocator has actually reserved from Vulkan, across all heaps.
    ///
    /// Distinct from @ref deviceAllocatedBytes: VMA suballocates from larger blocks, so a leak
    /// that fits inside existing blocks moves the allocation total while a leak that exhausts
    /// them moves this one. A working set that grows shows up in one or the other.
    std::uint64_t deviceBlockBytes = 0;
    /// Live allocations and blocks held by the allocator. Integer counts, so a per-frame leak of
    /// even one object is visible here long before the byte totals drift.
    std::uint32_t deviceAllocationCount = 0;
    std::uint32_t deviceBlockCount = 0;
};

/// A presented frame's pixels, read back from the swapchain image.
struct CapturedFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Row-major, 4 bytes per pixel, in R, G, B, A order regardless of the surface's own
    /// channel order. Empty when the frame was skipped.
    std::vector<std::uint8_t> rgba;

    /// Row-major depth values, one per pixel, exactly as the depth buffer holds them.
    ///
    /// Under the reversed-Z infinite projection these are `nearPlaneMetres / distance`, so a
    /// depth value converts back to a distance analytically. That is what lets the depth gate
    /// be a measurement rather than an inspection.
    std::vector<float> depth;

    [[nodiscard]] bool empty() const { return rgba.empty(); }

    /// Depth at one pixel, or a negative value when out of range.
    [[nodiscard]] float depthAt(std::uint32_t x, std::uint32_t y) const
    {
        if (x >= width || y >= height) {
            return -1.0F;
        }
        return depth[static_cast<std::size_t>(y) * width + x];
    }
};

/// A device, swapchain, and frame loop presenting to one window.
///
/// Move-only. Not thread-safe; a moved-from renderer may only be destroyed or assigned to.
class Renderer {
public:
    /// Selects a device, creates the swapchain, and prepares the frame loop.
    ///
    /// @param instance  must outlive the renderer.
    /// @param target    the window to present to.
    /// @param selection which device to use; the default is the production policy. See
    ///                  @ref DeviceSelection for why a request that matches nothing fails
    ///                  rather than falling back.
    ///
    /// Fails with an actionable diagnostic when no device meets the requirement set, naming
    /// every unmet requirement per device rather than reporting a bare "unsupported".
    [[nodiscard]] static std::expected<Renderer, std::string> create(
        const VulkanInstance& instance,
        const SurfaceTarget& target,
        const DeviceSelection& selection = {});

    ~Renderer();

    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// The device actually selected. Recorded in every evidence report, because on a hybrid
    /// laptop which device serves a surface is a driver policy rather than a fact about the
    /// hardware.
    [[nodiscard]] const DeviceCapabilities& selectedDevice() const;

    /// Replaces the reference scene.
    void setScene(std::vector<SceneObject> objects);

    /// Enables planet terrain with continuous LOD, or disables it when passed no value.
    void setTerrain(std::optional<TerrainSettings> terrain);

    /// Tells the renderer the framebuffer changed size. The swapchain is rebuilt on the next
    /// frame rather than immediately, so a burst of resize events costs one rebuild.
    void notifyResized(std::uint32_t width, std::uint32_t height);

    /// Renders and presents one frame.
    ///
    /// Returns without presenting, and without error, when the framebuffer has zero area — a
    /// minimised window is a normal state, not a failure.
    [[nodiscard]] std::expected<FrameStats, std::string> renderFrame(const CameraState& camera);

    /// Renders, presents, and reads the presented image back to host memory.
    ///
    /// This is the measurement path. The screen-space jitter gate needs the actual presented
    /// pixels — a per-frame centroid computed to sub-pixel accuracy — and an offscreen render
    /// would measure a different image from the one the gate is about.
    ///
    /// @warning **Never use this for frame-time evidence.** It waits for the GPU to finish
    /// before reading, which serialises CPU and GPU and removes the pipelining that makes
    /// frame time mean anything. Frame-time measurements use @ref renderFrame.
    [[nodiscard]] std::expected<CapturedFrame, std::string> renderFrameCaptured(
        const CameraState& camera);

    /// Blocks until the device is idle. Call before destroying anything the GPU may still be
    /// reading; the destructor does this too.
    void waitIdle();

private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sol::render
