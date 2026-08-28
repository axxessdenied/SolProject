#pragma once

// The Forge's frame (engine plan Phase 9 stage C). It is deliberately thinner
// than the game's: HDR target, the game's own mesh pipeline, debug lines for
// the grid and the scale references, tonemap to the swapchain, ImGui on top.
// No sky, no impostors, no particles, no game UI.
//
// ⚑ It is a separate frame rather than a reuse of `game/src/scene_renderer` on
// purpose: AGENTS §4 forbids a tool including game code. What IS shared is
// everything that matters for judging an asset - `sol::renderer::MeshRenderer`
// is the pipeline the game draws with, down to the shader and the emissive
// push constant.

#include "sol/core/math/math.hpp"
#include "sol/renderer/debug_draw_renderer.hpp"
#include "sol/renderer/material_registry.hpp"
#include "sol/renderer/mesh_renderer.hpp"
#include "sol/renderer/tonemap_renderer.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/resources.hpp"
#include "sol/rhi/swapchain.hpp"
#include "sol/ui/imgui_host.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace forge {

// ⚑ The game's own camera and lighting numbers, COPIED rather than shared.
// AGENTS §4 forbids a tool including game code, and all three live in
// `game/src` - the field of view in `scene_renderer.hpp`, the sun and ambient
// in `scene_renderer.cpp` under a comment calling them provisional until
// materials are data-driven. The viewer's entire claim is that it shows an
// asset under the light the game will actually use, so if those move these
// must move with them. Nothing checks that they agree; it is this stage's
// known gap, named here rather than left to be discovered.
inline constexpr float kCameraVerticalFov = sol::core::radians(70.0f);
inline constexpr float kSunIntensity = 3.2f;
inline constexpr float kAmbient = 0.012f;

// One drawable this frame. Positions are plain floats: an authored asset is
// metres across, not astronomical units, so the large-world rule that forces
// the game to render camera-relative has nothing to bite on here.
struct DrawItem
{
    const sol::renderer::GpuMesh* mesh = nullptr;
    const sol::renderer::GpuTexture* texture = nullptr;
    sol::core::Mat4 model = sol::core::Mat4::identity();
    float emissive = 0.0f;
    // ⚑ Phase 25 stage D. An index into whatever this frame's `MaterialRegistry`
    // was last built from. 0 is right by construction rather than by luck: with
    // no def data the registry holds exactly the one stock `forge.viewport` row
    // `initialize` builds, which is what this tool drew before materials existed.
    std::uint32_t material = 0;
    // Coverage, so a translucent material previews as one. Per-draw like
    // `emissive`, in the push block's last lane.
    float alpha = 1.0f;
};

struct FrameDesc
{
    sol::core::Mat4 view = sol::core::Mat4::identity();
    sol::core::Vec3 sunDirection = {0.0f, 1.0f, 0.0f}; // surface-to-sun
    float exposure = 1.0f;
    std::vector<DrawItem> items;
};

class ForgeView
{
public:
    enum class DrawResult
    {
        Success,
        NeedSwapchainRecreate,
        Failure,
    };

    // ⚑⚑ A SEARCH PATH RATHER THAN A DIRECTORY SINCE PHASE 24 STAGE V, and it
    // is the same change `SceneRenderer::initialize` took at Phase 25 stage E,
    // for the same reason and with the same rule about its LAST entry: a
    // project's `shaders/` comes first so a mod can replace one stage of a
    // pair, and the install's comes last because the renderers that are not
    // materials - tonemap and the debug lines in this tool - must resolve there
    // and nowhere else. Their descriptor sets and push blocks are C++ contracts
    // that no `[[material]]` row declares, so nothing would check a substitute.
    [[nodiscard]] bool initialize(sol::rhi::Context& context,
                                  sol::rhi::Swapchain& swapchain,
                                  std::span<const std::string> shaderSearchPath);
    void shutdown();
    [[nodiscard]] bool onSwapchainRecreated();

    [[nodiscard]] sol::renderer::MeshRenderer& meshes() { return m_meshRenderer; }

    // ⚑ Phase 25 stage D. The registry the viewport binds through, so the tool
    // can rebuild it from `materials.toml` and rewrite one material's params
    // when a slider moves. `initialize` leaves one stock row in it, which is
    // what draws when there is no def data to read.
    [[nodiscard]] sol::renderer::MaterialRegistry& materials() { return m_materials; }

    // Accumulate this frame's grid and reference lines here; the list is drawn
    // inside the HDR pass and cleared afterwards.
    [[nodiscard]] sol::renderer::DebugDrawRenderer& debugDraw() { return m_debugDraw; }

    void setImGuiHost(sol::ui::ImGuiHost* host) { m_imguiHost = host; }

    [[nodiscard]] DrawResult drawFrame(const FrameDesc& frame);

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
    void recordCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, const FrameDesc& frame);

    sol::rhi::Context* m_context = nullptr;
    sol::rhi::Swapchain* m_swapchain = nullptr;

    sol::renderer::MeshRenderer m_meshRenderer;
    // ⚑ Phase 25 stage B: the tool builds its viewport pipeline through the
    // same registry the game does, from one stock `MaterialDef`. It is not a
    // material EDITOR - that is stage D - but going through the registry is
    // what keeps "the Forge draws what the game draws" true by construction
    // rather than by two call sites agreeing about a `GraphicsPipelineDesc`.
    sol::renderer::MaterialRegistry m_materials;
    sol::renderer::DebugDrawRenderer m_debugDraw;
    sol::renderer::TonemapRenderer m_tonemapRenderer;

    sol::rhi::Image m_depth;
    sol::rhi::Image m_hdrColor;

    sol::ui::ImGuiHost* m_imguiHost = nullptr;
    FrameResources m_frames[kFramesInFlight];
    std::vector<VkSemaphore> m_renderFinished;
    std::uint32_t m_frameIndex = 0;
};

} // namespace forge
