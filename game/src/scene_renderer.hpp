#pragma once

#include "fly_camera.hpp"

#include "sol/core/math/math.hpp"
#include "sol/renderer/mesh_renderer.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/resources.hpp"
#include "sol/rhi/swapchain.hpp"
#include "sol/ui/dev_ui.hpp"

#include <cstdint>
#include <vector>

namespace game {

// Phase 2 test scene: textured cubes with depth, drawn camera-relatively.
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

    [[nodiscard]] DrawResult drawFrame(const FlyCamera& camera, double timeSeconds);

    // Call after the swapchain has been recreated (device must be idle).
    [[nodiscard]] bool onSwapchainRecreated();

    // Recreates the mesh pipeline from SPIR-V on disk (device must be idle).
    [[nodiscard]] bool reloadShaders() { return m_meshRenderer.reloadPipeline(); }

    [[nodiscard]] std::uint32_t drawCallCount() const { return m_drawCallCount; }

    // Optional dev overlay, rendered inside the scene pass.
    void setDevUi(sol::ui::DevUi* devUi) { m_devUi = devUi; }

private:
    static constexpr std::uint32_t kFramesInFlight = 2;

    struct FrameResources
    {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };

    struct Instance
    {
        sol::core::DVec3 position;
        sol::core::Quat rotation = sol::core::Quat::identity();
        sol::core::Vec3 scale = {1.0f, 1.0f, 1.0f};
        bool spins = false;
    };

    [[nodiscard]] bool createPerImageSemaphores();
    void destroyPerImageSemaphores();
    void recordCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex,
                        const FlyCamera& camera, double timeSeconds);

    sol::rhi::Context* m_context = nullptr;
    sol::rhi::Swapchain* m_swapchain = nullptr;

    sol::renderer::MeshRenderer m_meshRenderer;
    sol::renderer::GpuMesh m_cubeMesh;
    sol::renderer::GpuTexture m_checkerTexture;
    sol::rhi::Image m_depth;
    std::vector<Instance> m_instances;

    sol::ui::DevUi* m_devUi = nullptr;
    FrameResources m_frames[kFramesInFlight];
    std::vector<VkSemaphore> m_renderFinished;
    std::uint32_t m_frameIndex = 0;
    std::uint32_t m_drawCallCount = 0;
};

} // namespace game
