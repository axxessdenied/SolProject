#pragma once

#include <vulkan/vulkan.h>

namespace sol::rhi {

// Loads a SPIR-V file from disk; VK_NULL_HANDLE on failure.
[[nodiscard]] VkShaderModule createShaderModuleFromFile(VkDevice device, const char* path);

struct GraphicsPipelineDesc
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
};

// Dynamic-rendering pipeline: no vertex input, dynamic viewport/scissor, no depth/blend.
[[nodiscard]] bool createGraphicsPipeline(VkDevice device,
                                          const GraphicsPipelineDesc& desc,
                                          VkPipelineLayout& outLayout,
                                          VkPipeline& outPipeline);

} // namespace sol::rhi
