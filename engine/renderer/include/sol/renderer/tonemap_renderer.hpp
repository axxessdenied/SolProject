#pragma once

#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <string>

namespace sol::renderer {

// Fullscreen HDR -> swapchain resolve: exposure + ACES-fitted tonemap. The
// sRGB swapchain format handles gamma encoding.
class TonemapRenderer
{
public:
    [[nodiscard]] bool initialize(rhi::Context& context, VkFormat colorFormat, VkFormat depthFormat,
                                  const char* shaderDirectory);
    void shutdown();
    [[nodiscard]] bool reloadPipeline();

    // (Re)binds the HDR source; call at init and whenever the HDR target is
    // recreated (device must be idle).
    void setSource(VkImageView hdrView);

    void draw(VkCommandBuffer commandBuffer, VkExtent2D extent, float exposure) const;

private:
    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::string m_shaderDirectory;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace sol::renderer
