#pragma once

#include "sol/rhi/context.hpp"
#include "sol/rhi/swapchain.hpp"

#include <cstdint>
#include <vector>

namespace game {

// Phase 1 proof-of-life: draws one colored triangle with dynamic rendering.
class TriangleRenderer
{
public:
    enum class DrawResult
    {
        Success,
        NeedSwapchainRecreate,
        Failure,
    };

    [[nodiscard]] bool initialize(sol::rhi::Context& context,
                                  sol::rhi::Swapchain& swapchain,
                                  const char* shaderDirectory);
    void shutdown();

    [[nodiscard]] DrawResult drawFrame();

    // Call after the swapchain has been recreated (image count may change).
    [[nodiscard]] bool onSwapchainRecreated();

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
    void recordCommands(VkCommandBuffer commandBuffer, std::uint32_t imageIndex);

    sol::rhi::Context* m_context = nullptr;
    sol::rhi::Swapchain* m_swapchain = nullptr;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    FrameResources m_frames[kFramesInFlight];
    std::vector<VkSemaphore> m_renderFinished; // one per swapchain image
    std::uint32_t m_frameIndex = 0;
};

} // namespace game
