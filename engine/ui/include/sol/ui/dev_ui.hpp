#pragma once

#include "sol/core/math/vec.hpp"
#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sol::ui {

struct OverlayStats
{
    float fps = 0.0f;
    float frameMilliseconds = 0.0f;
    core::DVec3 cameraPosition;
    float cameraSpeed = 0.0f;
    std::uint32_t drawCalls = 0;
    std::uint64_t simTicks = 0;
    std::uint32_t simEntities = 0;
    float simAlpha = 0.0f;
};

// Dear ImGui dev/debug overlay (never player-facing UI - see engine plan 2.9).
class DevUi
{
public:
    [[nodiscard]] bool initialize(platform::Window& window, rhi::Context& context,
                                  VkFormat colorFormat, VkFormat depthFormat,
                                  std::uint32_t swapchainImageCount);
    void shutdown();

    // Once per frame, before recording; builds the overlay + console windows.
    void beginFrame(const OverlayStats& stats);

    // Records draw data; must be called inside the scene's dynamic rendering pass.
    void render(VkCommandBuffer commandBuffer);

    // Call instead of render() when the frame is abandoned (swapchain out of date).
    void discardFrame();

private:
    void buildWindows(const OverlayStats& stats);

    bool m_initialized = false;
    bool m_frameOpen = false;
    bool m_showConsole = true;
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
};

} // namespace sol::ui
