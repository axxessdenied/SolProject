#pragma once

#include "sol/rhi/resources.hpp"
#include "sol/rhi/swapchain.hpp"

#include <vulkan/vulkan.h>

namespace sol::renderer {

// HDR scene pass: transitions the HDR color target and depth image and begins
// dynamic rendering with cleared color + reversed-Z depth (clear depth 0.0).
void beginHdrScenePass(VkCommandBuffer commandBuffer,
                       const rhi::Image& hdrColor,
                       const rhi::Image& depth,
                       VkClearColorValue clearColor);

// Ends the HDR pass and transitions the HDR target for fragment sampling.
void endHdrScenePass(VkCommandBuffer commandBuffer, const rhi::Image& hdrColor);

// Present pass: renders to the swapchain image (tonemap + dev UI). The depth
// image rides along untouched so pipelines built with a depth format match.
void beginPresentPass(VkCommandBuffer commandBuffer,
                      const rhi::Swapchain& swapchain,
                      std::uint32_t imageIndex,
                      const rhi::Image& depth);

// Ends rendering and transitions the swapchain image for present.
void endPresentPass(VkCommandBuffer commandBuffer, const rhi::Swapchain& swapchain, std::uint32_t imageIndex);

} // namespace sol::renderer
