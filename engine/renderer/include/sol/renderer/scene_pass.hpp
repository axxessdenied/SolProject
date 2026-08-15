#pragma once

#include "sol/rhi/resources.hpp"
#include "sol/rhi/swapchain.hpp"

#include <vulkan/vulkan.h>

namespace sol::renderer {

// Records the swapchain/depth transitions and begins dynamic rendering with a
// cleared color + reversed-Z depth (clear depth 0.0) attachment pair.
void beginScenePass(VkCommandBuffer commandBuffer, const rhi::Swapchain& swapchain,
                    std::uint32_t imageIndex, const rhi::Image& depth, VkClearColorValue clearColor);

// Ends rendering and transitions the swapchain image for present.
void endScenePass(VkCommandBuffer commandBuffer, const rhi::Swapchain& swapchain,
                  std::uint32_t imageIndex);

} // namespace sol::renderer
