#pragma once

#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sol::ui {

// The Dear ImGui host: context, platform backend, Vulkan backend and the
// per-frame bracket. It draws nothing of its own - a frame contains whatever
// its clients build between beginFrame() and render().
//
// Split out of DevUi in Phase 9 stage C. DevUi *was* the host: one call
// brought ImGui up and built the game's overlay and console together, which
// left a second application (tools/forge) no way to have ImGui without also
// having the game's windows. DevUi is the first client of this class and the
// Forge is the second.
//
// ⚑ ImGui's context and its platform hook are process-global, so exactly one
// host may be live at a time; a second initialize() fails loudly rather than
// quietly stamping on the first.
class ImGuiHost
{
public:
    [[nodiscard]] bool initialize(platform::Window& window, rhi::Context& context,
                                  VkFormat colorFormat, VkFormat depthFormat,
                                  std::uint32_t swapchainImageCount);
    void shutdown();

    // Opens the frame. Clients build their windows after this returns.
    void beginFrame();

    // Records draw data; must be called inside a dynamic rendering pass whose
    // attachment formats match the ones initialize() was given.
    void render(VkCommandBuffer commandBuffer);

    // Call instead of render() when the frame is abandoned (swapchain out of
    // date): ImGui insists every NewFrame is closed, one way or the other.
    void discardFrame();

    [[nodiscard]] bool initialized() const { return m_initialized; }
    [[nodiscard]] bool frameOpen() const { return m_frameOpen; }

    // True while ImGui wants the input - a game action like the weapon trigger
    // stands down rather than firing through a console click.
    [[nodiscard]] bool wantsMouseCapture() const;
    [[nodiscard]] bool wantsKeyboardCapture() const;

private:
    bool m_initialized = false;
    bool m_frameOpen = false;
    VkDevice m_device = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
};

} // namespace sol::ui
