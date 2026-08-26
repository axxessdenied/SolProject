#pragma once

#include "sol/rhi/context.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sol::rhi {

// v0 memory strategy: one dedicated allocation per resource. A block
// sub-allocator replaces this when resource counts justify it (engine plan 2.3).

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t mipLevels = 1;
};

[[nodiscard]] Buffer createBuffer(Context& context,
                                  VkDeviceSize size,
                                  VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags memoryProperties);

// Staging-uploads data into a new DEVICE_LOCAL buffer (usage gets TRANSFER_DST added).
[[nodiscard]] Buffer
createDeviceLocalBuffer(Context& context, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

void destroyBuffer(Context& context, Buffer& buffer);

// Depth attachment image (D32_SFLOAT), view included; layout transitions are
// recorded per frame by the caller.
[[nodiscard]] Image createDepthImage(Context& context, VkExtent2D extent);

// Renderable + sampleable color target (e.g. the HDR scene buffer); layout
// transitions are recorded per frame by the caller.
[[nodiscard]] Image createColorTarget(Context& context, VkExtent2D extent, VkFormat format);

struct TextureUploadDesc
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED; // block-compressed formats supported
    std::uint32_t mipCount = 0;
    const std::uint8_t* const* mipData = nullptr; // mipCount pointers
    const std::uint32_t* mipSizes = nullptr;      // mipCount byte sizes

    // View component mapping; identity by default. Lets a single-channel
    // coverage texture (a glyph atlas) read as white with the coverage in
    // alpha, so it shares a shader with ordinary color textures.
    VkComponentMapping swizzle = {VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY};
};

// Creates a sampled image, uploads all mips via staging, and leaves it in
// SHADER_READ_ONLY_OPTIMAL.
[[nodiscard]] Image createSampledTexture(Context& context, const TextureUploadDesc& desc);

// Creates a cubemap from six equally-sized face payloads (+X,-X,+Y,-Y,+Z,-Z),
// uploads via staging, and leaves it in SHADER_READ_ONLY_OPTIMAL. One mip.
[[nodiscard]] Image createSampledCubemap(Context& context,
                                         std::uint32_t faceSize,
                                         VkFormat format,
                                         const std::uint8_t* const faceData[6],
                                         std::uint32_t faceByteSize);

void destroyImage(Context& context, Image& image);

// Trilinear repeat sampler covering the given mip count.
[[nodiscard]] VkSampler createSampler(Context& context, std::uint32_t mipLevels);

// Linear clamp-to-edge sampler, single mip (fullscreen sources, cubemaps).
[[nodiscard]] VkSampler createClampSampler(Context& context);

// One-shot command helpers: begin, record, then end (submits and waits).
[[nodiscard]] VkCommandBuffer beginOneShotCommands(Context& context);
void endOneShotCommands(Context& context, VkCommandBuffer commandBuffer);

} // namespace sol::rhi
