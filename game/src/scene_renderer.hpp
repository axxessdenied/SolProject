#pragma once

#include "sol/core/math/math.hpp"
#include "sol/renderer/debug_draw_renderer.hpp"
#include "sol/renderer/impostor_renderer.hpp"
#include "sol/renderer/mesh_renderer.hpp"
#include "sol/renderer/particle_renderer.hpp"
#include "sol/renderer/sky_renderer.hpp"
#include "sol/renderer/tonemap_renderer.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/resources.hpp"
#include "sol/rhi/swapchain.hpp"
#include "sol/ui/dev_ui.hpp"

#include <cstdint>
#include <span>
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

    [[nodiscard]] sol::core::Mat4 viewRotation() const
    {
        return transpose(toMat4(orientation));
    }
};

// The slice's fixed model catalog; data-driven assets arrive in Phase 5.
enum class ModelId : std::uint32_t
{
    Cube,
    Station,
    Ship,
};

// One drawable produced by the sim for the current frame; positions are
// sim-space doubles, made camera-relative at record time (large-world rule).
struct RenderInstance
{
    sol::core::DVec3 position;
    sol::core::Quat rotation = sol::core::Quat::identity();
    sol::core::Vec3 scale = {1.0f, 1.0f, 1.0f};
    ModelId model = ModelId::Cube;
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

    [[nodiscard]] bool initialize(sol::rhi::Context& context, sol::rhi::Swapchain& swapchain,
                                  const char* shaderDirectory, const char* cookedDirectory);
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
               m_debugDraw.reloadPipeline() && m_particleRenderer.reloadPipeline();
    }

    [[nodiscard]] std::uint32_t drawCallCount() const { return m_drawCallCount; }

    // Optional dev overlay, rendered inside the scene pass.
    void setDevUi(sol::ui::DevUi* devUi) { m_devUi = devUi; }

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
    void recordCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                        const CameraFrame& camera, std::span<const RenderInstance> instances,
                        std::span<const ParticleInstance> particles, const SceneInfo& scene);

    sol::rhi::Context* m_context = nullptr;
    sol::rhi::Swapchain* m_swapchain = nullptr;

    sol::renderer::MeshRenderer m_meshRenderer;
    sol::renderer::SkyRenderer m_skyRenderer;
    sol::renderer::ImpostorRenderer m_impostorRenderer;
    sol::renderer::TonemapRenderer m_tonemapRenderer;
    sol::renderer::DebugDrawRenderer m_debugDraw;
    sol::renderer::ParticleRenderer m_particleRenderer;
    std::vector<sol::renderer::ParticleRenderer::Particle> m_particleScratch;
    sol::renderer::GpuMesh m_cubeMesh;
    sol::renderer::GpuMesh m_stationMesh;
    sol::renderer::GpuMesh m_shipMesh;
    sol::renderer::GpuTexture m_checkerTexture;
    sol::renderer::GpuTexture m_hullTexture;
    sol::rhi::Image m_depth;
    sol::rhi::Image m_hdrColor;

    sol::ui::DevUi* m_devUi = nullptr;
    FrameResources m_frames[kFramesInFlight];
    std::vector<VkSemaphore> m_renderFinished;
    std::uint32_t m_frameIndex = 0;
    std::uint32_t m_drawCallCount = 0;
};

} // namespace game
