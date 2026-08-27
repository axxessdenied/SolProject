#pragma once

#include "lod_report.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/assets/font.hpp"
#include "sol/core/math/math.hpp"
#include "sol/renderer/debug_draw_renderer.hpp"
#include "sol/renderer/impostor_renderer.hpp"
#include "sol/renderer/mesh_renderer.hpp"
#include "sol/renderer/particle_renderer.hpp"
#include "sol/renderer/sky_renderer.hpp"
#include "sol/renderer/tonemap_renderer.hpp"
#include "sol/renderer/ui_renderer.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/gpu_profiler.hpp"
#include "sol/rhi/resources.hpp"
#include "sol/rhi/swapchain.hpp"
#include "sol/ui/draw_list.hpp"
#include "sol/ui/imgui_host.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace game {

// One vertical FOV for the scene, the sky ray setup, and HUD projection.
inline constexpr float kCameraVerticalFov = sol::core::radians(70.0f);

// A camera pose in sim space; rendering is camera-relative, so only the
// rotation part ever becomes a matrix (large-world rule).
struct CameraFrame
{
    sol::core::DVec3 position;
    sol::core::Quat orientation = sol::core::Quat::identity();

    [[nodiscard]] sol::core::Mat4 viewRotation() const { return transpose(toMat4(orientation)); }
};

// Index into the model catalog, which is `[[model]]` def order (Phase 9).
// This was a five-member enum until the authoring tool needed a mesh to be
// able to reach the game without a C++ change; the type stays strong so an
// index into the wrong table is still a compile error, but the members are
// resolved by name at load rather than written here.
enum class ModelId : std::uint32_t
{
};

// No model. Drawing one is a no-op rather than a crash: a def layer can be
// edited at runtime and a stale name must not take the frame down.
inline constexpr ModelId kNoModel = static_cast<ModelId>(0xFFFFFFFFu);

[[nodiscard]] inline constexpr std::uint32_t modelIndex(ModelId id)
{
    return static_cast<std::uint32_t>(id);
}

// ⚑⚑ "THIS IS THE SAME OBJECT AS LAST FRAME" (Phase 18), which is the one
// thing a RenderInstance never used to say. LOD selection is otherwise a pure
// function of the current frame, so a radius parked on a threshold re-decides
// every frame; remembering what an instance drew last needs a key that
// survives to the next frame.
//
// ⚑ IT PACKS THE FULL ECS HANDLE, `{generation, index}`, AND NOT THE BARE
// INDEX. Slots are recycled and `Registry::destroy` bumps the generation for
// exactly that reason - keyed on the index alone, a despawned freighter's
// level would be inherited by whatever spawns into its slot, which is a wrong
// draw with no error and is Phase 15's row-vs-nav-slot in new clothes.
//
// ⚑ Packed rather than typed so `sol/ecs` stays out of the renderer's public
// header, and so "no identity" is a default rather than a branch: the cockpit
// is pushed by the game layer with no entity behind it, and needs none - it
// has no chain, so its level is always 0 however it is asked.
using RenderInstanceKey = std::uint64_t;
inline constexpr RenderInstanceKey kNoInstanceKey = 0;

[[nodiscard]] inline constexpr RenderInstanceKey makeInstanceKey(std::uint32_t index,
                                                                 std::uint32_t generation)
{
    // +1 on the index so slot 0 of generation 0 is not the "no identity"
    // sentinel - the player's own ship has been entity 0 since Phase 4.
    return (static_cast<RenderInstanceKey>(generation) << 32) | (index + 1u);
}

// One drawable produced by the sim for the current frame; positions are
// sim-space doubles, made camera-relative at record time (large-world rule).
struct RenderInstance
{
    sol::core::DVec3 position;
    sol::core::Quat rotation = sol::core::Quat::identity();
    sol::core::Vec3 scale = {1.0f, 1.0f, 1.0f};
    ModelId model = kNoModel;
    RenderInstanceKey key = kNoInstanceKey;
};

// One additive billboard in sim space (thruster exhaust etc.).
struct ParticleInstance
{
    sol::core::DVec3 position;
    float size = 0.5f;
    sol::core::Vec4 color; // rgb = linear HDR, a = fade
};

// Sim-space celestial body handed to the impostor pass each frame; palette
// picks one of the renderer's planet color pairs (deterministic per body).
struct CelestialDraw
{
    sol::core::DVec3 position;
    double radius = 0.0;
    std::uint32_t palette = 0;
};

// Everything scene-wide the renderer needs for one frame.
struct SceneInfo
{
    CelestialDraw sun;
    std::vector<CelestialDraw> planets;
    float exposure = 1.0f;
    // The jump tunnel (Phase 8v). `skyWarp` is the streak strength (0 at rest),
    // `skyScale` multiplies the sky's resting intensity, and `travelDirection`
    // is the world-space axis the streaks converge on -- the ship's nose, not
    // the camera's, so the tunnel stays anchored to where the ship is actually
    // going. Deliberately NOT a FOV change: one FOV feeds the scene, the sky
    // and the cockpit's HUD projection, so widening it would slide every dash
    // anchor (Phase 8m's angles-vs-pixels rule).
    float skyWarp = 0.0f;
    float skyScale = 1.0f;
    sol::core::Vec3 travelDirection = {0.0f, 0.0f, -1.0f};
};

// Space-scene renderer: HDR pass (sun-lit meshes, planet/star impostors,
// starfield sky) resolved to the swapchain via exposure + tonemap, dev UI on
// top. All positions camera-relative (large-world rule).
class SceneRenderer
{
public:
    enum class DrawResult
    {
        Success,
        NeedSwapchainRecreate,
        Failure,
    };

    // `cookedSearchPath` is the ordered list from `asset_paths.hpp`, highest
    // priority first, and it must outlive nothing - it is read here and again
    // in `loadModels`, never stored (Phase 24 stage S).
    [[nodiscard]] bool initialize(sol::rhi::Context& context,
                                  sol::rhi::Swapchain& swapchain,
                                  const char* shaderDirectory,
                                  std::span<const std::string> cookedSearchPath);

    // Uploads the catalog the `[[model]]` defs name (Phase 9). Separate from
    // initialize because the pipelines come up before the def database is
    // loaded, and because a mod layer that adds a model should be able to
    // reload the catalog without rebuilding the swapchain. Meshes and
    // textures are cached by cooked stem, so ten models sharing hull.stex
    // upload it once.
    //
    // ⚑ Phase 25 stage A: the surface comes from `materials`, indexed by the
    // model's own `materialIndex`, and this function no longer reads a model's
    // `texture`/`emissive`/`translucent`/`alpha` at all. `DefDatabase` fills
    // that index for every row, synthesising a material for one that names
    // none, so the two spans are always consistent by construction.
    [[nodiscard]] bool loadModels(std::span<const sol::assets::ModelDef> models,
                                  std::span<const sol::assets::MaterialDef> materials,
                                  std::span<const std::string> cookedSearchPath);
    void unloadModels();
    void shutdown();

    [[nodiscard]] DrawResult drawFrame(const CameraFrame& camera,
                                       std::span<const RenderInstance> instances,
                                       std::span<const ParticleInstance> particles,
                                       const SceneInfo& scene);

    // Call after the swapchain has been recreated (device must be idle).
    [[nodiscard]] bool onSwapchainRecreated();

    // Recreates every pipeline from SPIR-V on disk (device must be idle).
    [[nodiscard]] bool reloadShaders()
    {
        return m_meshRenderer.reloadPipeline() && m_skyRenderer.reloadPipeline() &&
               m_impostorRenderer.reloadPipeline() && m_tonemapRenderer.reloadPipeline() &&
               m_debugDraw.reloadPipeline() && m_particleRenderer.reloadPipeline() &&
               m_uiRenderer.reloadPipeline();
    }

    [[nodiscard]] std::uint32_t drawCallCount() const { return m_drawCallCount; }

    // Publishes any GPU timings the device has finished with into the frame
    // profiler (Phase 8o). Call at the top of the frame with no CPU zone
    // open - see GpuProfiler::publishPending for why that is not advice.
    void publishGpuTimings() { m_gpuProfiler.publishPending(); }

    [[nodiscard]] bool gpuTimingAvailable() const { return m_gpuProfiler.available(); }

    // Optional ImGui host, recorded last in the present pass so the dev
    // overlay and console sit on top of everything (Phase 9 stage C: the pass
    // records the host, not one of its clients).
    void setImGuiHost(sol::ui::ImGuiHost* host) { m_imguiHost = host; }

    // Game UI geometry for this frame, drawn after tonemap and before the dev
    // overlay so ImGui always sits on top. The list is owned by the caller.
    void setUiDrawList(const sol::ui::DrawList* drawList) { m_uiDrawList = drawList; }

    // Player-facing UI scale. The UI is built against a virtual screen of
    // (pixels / scale) and stretched back over the real one, so one number
    // resizes every surface without any widget knowing about it.
    void setUiScale(float scale) { m_uiScale = scale > 0.0f ? scale : 1.0f; }

    // The cooked UI font and the texture slot its atlas occupies, for callers
    // building draw lists.
    [[nodiscard]] const sol::assets::Font& uiFont() const { return m_uiFont; }

    [[nodiscard]] std::uint32_t uiFontTexture() const { return m_uiFontTexture; }

    // Add camera-relative debug lines each frame before drawFrame; the list
    // is drawn inside the HDR pass and cleared afterwards.
    [[nodiscard]] sol::renderer::DebugDrawRenderer& debugDraw() { return m_debugDraw; }

private:
    static constexpr std::uint32_t kFramesInFlight = 2;

    struct FrameResources
    {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };

    [[nodiscard]] bool createPerImageSemaphores();
    void destroyPerImageSemaphores();
    void recordCommands(VkCommandBuffer commandBuffer,
                        std::uint32_t imageIndex,
                        const CameraFrame& camera,
                        std::span<const RenderInstance> instances,
                        std::span<const ParticleInstance> particles,
                        const SceneInfo& scene);

    // ⚑ ONE rule for both draw paths, which is why this is a function and not
    // two `if`s: the translucent pass already carries a comment about refusing
    // to become a second rule about which mesh to use, and a pin or a
    // hysteresis band honoured in one path and not the other would be a third.
    // Reads `m_lodLast`, writes `m_lodThis`.
    [[nodiscard]] std::uint32_t
    chooseLevel(RenderInstanceKey key, float screenRadiusPixels, std::uint32_t levelCount);

    sol::rhi::Context* m_context = nullptr;
    sol::rhi::Swapchain* m_swapchain = nullptr;

    sol::renderer::MeshRenderer m_meshRenderer;
    sol::renderer::SkyRenderer m_skyRenderer;
    sol::renderer::ImpostorRenderer m_impostorRenderer;
    sol::renderer::TonemapRenderer m_tonemapRenderer;
    sol::renderer::DebugDrawRenderer m_debugDraw;
    sol::renderer::ParticleRenderer m_particleRenderer;
    std::vector<sol::renderer::ParticleRenderer::Particle> m_particleScratch;

    // The model catalog (Phase 9), parallel to `[[model]]` def order. Meshes
    // and textures are owned by stem so a shared asset is uploaded once, and
    // a model row holds the indices into those two pools.
    struct CatalogEntry
    {
        // Level 0 first, then whatever `.lodN.smesh` siblings the cook left
        // beside it. `levelCount` is 1 for every model that has no chain,
        // which is what makes a model without levels draw exactly as it did
        // before stage F rather than through a special case.
        std::uint32_t levels[kMaxDrawLevels] = {};
        std::uint32_t levelCount = 1;
        // The def's own radius, kept here because selection needs it every
        // frame and the def database is not in the draw's reach. ⚑ It is the
        // DRAWING's radius only: collision, weapons and avoidance keep reading
        // the def, because a level is a picture of a shape and not the shape.
        float radius = 1.0f;
        // ⚑ THE FOUR BELOW COME FROM THE MODEL'S MATERIAL (Phase 25 stage A),
        // not from the model. They stay flattened into the entry because the
        // draw loop reads them per instance and an indirection there would buy
        // nothing: a material is resolved once, at load, exactly as a mesh and
        // a texture stem are. What changed is who owns the values.
        std::uint32_t texture = 0;
        float emissive = 0.0f;
        // Phase 12: translucency is a property of the SURFACE, declared in a
        // def row rather than on the instance - so the second translucent
        // thing in this game is a def row and no C++ at all.
        bool translucent = false;
        float alpha = 1.0f;
        // ⚑⚑ Phase 24 stage S. False when this model's mesh or texture could
        // not be found in any layer's cooked directory. The row KEEPS ITS SLOT
        // - `ModelId` is an index into `defs.models()` and `m_models` is built
        // parallel to it, so skipping one would silently re-point every model
        // after it - and the draw loop passes over it instead, which is the
        // rule it already applies to a stale index one line up.
        bool drawable = true;
    };

    std::vector<CatalogEntry> m_models;
    // ⚑⚑ WHAT EACH INSTANCE DREW LAST FRAME (Phase 18), which is the only
    // state in this whole draw path. The RULE lives in `mesh_lod.hpp` and
    // takes the previous level as an argument, so it stays pure and stays
    // assertable without a device; what has to live here is the memory,
    // because only the renderer knows which instance is which.
    //
    // ⚑ TWO MAPS RATHER THAN ONE PLUS A SWEEP: each frame reads `m_lodLast`
    // and writes `m_lodThis`, then they swap. An instance that was not drawn
    // this frame simply does not carry over, so eviction is free and a
    // despawned entity cannot accumulate - which matters because a system
    // jump retires every entity in the bubble at once.
    std::unordered_map<RenderInstanceKey, std::uint32_t> m_lodLast;
    std::unordered_map<RenderInstanceKey, std::uint32_t> m_lodThis;
    // Translucent instances deferred out of the opaque loop and drawn after
    // the sky (see the draw path for why the order is not negotiable).
    std::vector<const RenderInstance*> m_translucentScratch;
    std::vector<sol::renderer::GpuMesh> m_meshes;
    std::vector<sol::renderer::GpuTexture> m_textures;
    std::vector<std::string> m_meshStems;
    std::vector<std::string> m_textureStems;
    sol::rhi::Image m_depth;
    sol::rhi::Image m_hdrColor;

    sol::renderer::UiRenderer m_uiRenderer;
    sol::assets::Font m_uiFont;
    sol::rhi::Image m_uiFontAtlas;
    VkSampler m_uiFontSampler = VK_NULL_HANDLE;
    std::uint32_t m_uiFontTexture = 0;
    const sol::ui::DrawList* m_uiDrawList = nullptr;
    float m_uiScale = 1.0f;

    sol::ui::ImGuiHost* m_imguiHost = nullptr;
    sol::rhi::GpuProfiler m_gpuProfiler;
    FrameResources m_frames[kFramesInFlight];
    std::vector<VkSemaphore> m_renderFinished;
    std::uint32_t m_frameIndex = 0;
    std::uint32_t m_drawCallCount = 0;
};

} // namespace game
