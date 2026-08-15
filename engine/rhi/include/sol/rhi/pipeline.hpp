#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sol::rhi {

// Loads a SPIR-V file from disk; VK_NULL_HANDLE on failure.
[[nodiscard]] VkShaderModule createShaderModuleFromFile(VkDevice device, const char* path);

struct VertexAttribute
{
    std::uint32_t location = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t offset = 0;
};

enum class BlendMode
{
    Opaque,
    Alpha,    // premultiplied-style: src.rgb + dst.rgb * (1 - src.a)
    Additive, // src.rgb + dst.rgb
};

struct GraphicsPipelineDesc
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    BlendMode blendMode = BlendMode::Opaque;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Zero attributeCount = no vertex input (fullscreen/procedural vertices).
    std::uint32_t vertexStride = 0;
    const VertexAttribute* attributes = nullptr;
    std::uint32_t attributeCount = 0;

    // VK_FORMAT_UNDEFINED = no depth attachment. Depth testing uses the
    // engine-wide reversed-Z convention (GREATER_OR_EQUAL, clear to 0).
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    bool depthTest = false;
    bool depthWrite = false;

    bool cullBackFaces = false;
    // CCW front faces (glTF convention; pairs with the negative-viewport Y flip).
    bool frontFaceCounterClockwise = false;

    VkPipelineLayout layout = VK_NULL_HANDLE;
};

// Dynamic-rendering pipeline with dynamic viewport/scissor.
[[nodiscard]] bool createGraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& desc,
                                          VkPipeline& outPipeline);

// Pipeline layout from optional descriptor set layouts and one optional
// vertex+fragment push-constant range starting at offset 0.
[[nodiscard]] VkPipelineLayout createPipelineLayout(VkDevice device,
                                                    const VkDescriptorSetLayout* setLayouts,
                                                    std::uint32_t setLayoutCount,
                                                    std::uint32_t pushConstantSize);

} // namespace sol::rhi
