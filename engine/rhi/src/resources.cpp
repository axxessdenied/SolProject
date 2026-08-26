#include "sol/rhi/resources.hpp"

#include "vk_check.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/log.hpp"

#include <algorithm>
#include <cstring>

namespace sol::rhi {

namespace {

std::uint32_t findMemoryType(Context& context, std::uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    vkGetPhysicalDeviceMemoryProperties(context.physicalDevice(), &memoryProperties);
    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    SOL_LOG_FATAL("No compatible Vulkan memory type (bits 0x%x, props 0x%x)", typeBits, properties);
    return 0;
}

} // namespace

Buffer createBuffer(Context& context,
                    VkDeviceSize size,
                    VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags memoryProperties)
{
    Buffer result;
    result.size = size;

    VkBufferCreateInfo bufferCreateInfo = {};
    bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCreateInfo.size = size;
    bufferCreateInfo.usage = usage;
    bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    SOL_VK_CHECK(vkCreateBuffer(context.device(), &bufferCreateInfo, nullptr, &result.buffer));

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(context.device(), result.buffer, &requirements);

    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(context, requirements.memoryTypeBits, memoryProperties);
    SOL_VK_CHECK(vkAllocateMemory(context.device(), &allocateInfo, nullptr, &result.memory));
    SOL_VK_CHECK(vkBindBufferMemory(context.device(), result.buffer, result.memory, 0));
    return result;
}

Buffer
createDeviceLocalBuffer(Context& context, const void* data, VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buffer staging = createBuffer(context,
                                  size,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    SOL_VK_CHECK(vkMapMemory(context.device(), staging.memory, 0, size, 0, &mapped));
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(context.device(), staging.memory);

    Buffer result = createBuffer(
        context, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkCommandBuffer commandBuffer = beginOneShotCommands(context);
    VkBufferCopy region = {0, 0, size};
    vkCmdCopyBuffer(commandBuffer, staging.buffer, result.buffer, 1, &region);
    endOneShotCommands(context, commandBuffer);

    destroyBuffer(context, staging);
    return result;
}

void destroyBuffer(Context& context, Buffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(context.device(), buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(context.device(), buffer.memory, nullptr);
    }
    buffer = {};
}

Image createDepthImage(Context& context, VkExtent2D extent)
{
    Image result;
    result.extent = extent;
    result.format = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo imageCreateInfo = {};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = result.format;
    imageCreateInfo.extent = {extent.width, extent.height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    SOL_VK_CHECK(vkCreateImage(context.device(), &imageCreateInfo, nullptr, &result.image));

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(context.device(), result.image, &requirements);
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        findMemoryType(context, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    SOL_VK_CHECK(vkAllocateMemory(context.device(), &allocateInfo, nullptr, &result.memory));
    SOL_VK_CHECK(vkBindImageMemory(context.device(), result.image, result.memory, 0));

    VkImageViewCreateInfo viewCreateInfo = {};
    viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCreateInfo.image = result.image;
    viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCreateInfo.format = result.format;
    viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewCreateInfo.subresourceRange.levelCount = 1;
    viewCreateInfo.subresourceRange.layerCount = 1;
    SOL_VK_CHECK(vkCreateImageView(context.device(), &viewCreateInfo, nullptr, &result.view));
    return result;
}

Image createColorTarget(Context& context, VkExtent2D extent, VkFormat format)
{
    Image result;
    result.extent = extent;
    result.format = format;

    VkImageCreateInfo imageCreateInfo = {};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = format;
    imageCreateInfo.extent = {extent.width, extent.height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    SOL_VK_CHECK(vkCreateImage(context.device(), &imageCreateInfo, nullptr, &result.image));

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(context.device(), result.image, &requirements);
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        findMemoryType(context, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    SOL_VK_CHECK(vkAllocateMemory(context.device(), &allocateInfo, nullptr, &result.memory));
    SOL_VK_CHECK(vkBindImageMemory(context.device(), result.image, result.memory, 0));

    VkImageViewCreateInfo viewCreateInfo = {};
    viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCreateInfo.image = result.image;
    viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCreateInfo.format = format;
    viewCreateInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    SOL_VK_CHECK(vkCreateImageView(context.device(), &viewCreateInfo, nullptr, &result.view));
    return result;
}

Image createSampledCubemap(Context& context,
                           std::uint32_t faceSize,
                           VkFormat format,
                           const std::uint8_t* const faceData[6],
                           std::uint32_t faceByteSize)
{
    Image result;
    result.extent = {faceSize, faceSize};
    result.format = format;

    VkImageCreateInfo imageCreateInfo = {};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = format;
    imageCreateInfo.extent = {faceSize, faceSize, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 6;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    SOL_VK_CHECK(vkCreateImage(context.device(), &imageCreateInfo, nullptr, &result.image));

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(context.device(), result.image, &requirements);
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        findMemoryType(context, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    SOL_VK_CHECK(vkAllocateMemory(context.device(), &allocateInfo, nullptr, &result.memory));
    SOL_VK_CHECK(vkBindImageMemory(context.device(), result.image, result.memory, 0));

    Buffer staging = createBuffer(context,
                                  VkDeviceSize{faceByteSize} * 6,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    {
        void* mapped = nullptr;
        SOL_VK_CHECK(vkMapMemory(context.device(), staging.memory, 0, staging.size, 0, &mapped));
        std::uint8_t* cursor = static_cast<std::uint8_t*>(mapped);
        for (int face = 0; face < 6; ++face) {
            std::memcpy(cursor, faceData[face], faceByteSize);
            cursor += faceByteSize;
        }
        vkUnmapMemory(context.device(), staging.memory);
    }

    VkCommandBuffer commandBuffer = beginOneShotCommands(context);

    VkImageMemoryBarrier2 toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.image = result.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};

    VkDependencyInfo dependencyInfo = {};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &toTransfer;
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6};
    region.imageExtent = {faceSize, faceSize, 1};
    vkCmdCopyBufferToImage(
        commandBuffer, staging.buffer, result.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 toSampled = toTransfer;
    toSampled.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toSampled.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dependencyInfo.pImageMemoryBarriers = &toSampled;
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    endOneShotCommands(context, commandBuffer);
    destroyBuffer(context, staging);

    VkImageViewCreateInfo viewCreateInfo = {};
    viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCreateInfo.image = result.image;
    viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewCreateInfo.format = format;
    viewCreateInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    SOL_VK_CHECK(vkCreateImageView(context.device(), &viewCreateInfo, nullptr, &result.view));
    return result;
}

Image createSampledTexture(Context& context, const TextureUploadDesc& desc)
{
    SOL_ASSERT(desc.mipCount > 0 && desc.mipData != nullptr && desc.mipSizes != nullptr);

    Image result;
    result.extent = {desc.width, desc.height};
    result.format = desc.format;
    result.mipLevels = desc.mipCount;

    VkImageCreateInfo imageCreateInfo = {};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = desc.format;
    imageCreateInfo.extent = {desc.width, desc.height, 1};
    imageCreateInfo.mipLevels = desc.mipCount;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    SOL_VK_CHECK(vkCreateImage(context.device(), &imageCreateInfo, nullptr, &result.image));

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(context.device(), result.image, &requirements);
    VkMemoryAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        findMemoryType(context, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    SOL_VK_CHECK(vkAllocateMemory(context.device(), &allocateInfo, nullptr, &result.memory));
    SOL_VK_CHECK(vkBindImageMemory(context.device(), result.image, result.memory, 0));

    // Staging buffer holding every mip back to back.
    VkDeviceSize totalSize = 0;
    for (std::uint32_t mip = 0; mip < desc.mipCount; ++mip) {
        totalSize += desc.mipSizes[mip];
    }
    Buffer staging = createBuffer(context,
                                  totalSize,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    {
        void* mapped = nullptr;
        SOL_VK_CHECK(vkMapMemory(context.device(), staging.memory, 0, totalSize, 0, &mapped));
        std::uint8_t* cursor = static_cast<std::uint8_t*>(mapped);
        for (std::uint32_t mip = 0; mip < desc.mipCount; ++mip) {
            std::memcpy(cursor, desc.mipData[mip], desc.mipSizes[mip]);
            cursor += desc.mipSizes[mip];
        }
        vkUnmapMemory(context.device(), staging.memory);
    }

    VkCommandBuffer commandBuffer = beginOneShotCommands(context);

    VkImageMemoryBarrier2 toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.image = result.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipCount, 0, 1};

    VkDependencyInfo dependencyInfo = {};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &toTransfer;
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    VkDeviceSize bufferOffset = 0;
    for (std::uint32_t mip = 0; mip < desc.mipCount; ++mip) {
        VkBufferImageCopy region = {};
        region.bufferOffset = bufferOffset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        region.imageExtent = {std::max(desc.width >> mip, 1u), std::max(desc.height >> mip, 1u), 1};
        vkCmdCopyBufferToImage(
            commandBuffer, staging.buffer, result.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        bufferOffset += desc.mipSizes[mip];
    }

    VkImageMemoryBarrier2 toSampled = toTransfer;
    toSampled.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    toSampled.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toSampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toSampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dependencyInfo.pImageMemoryBarriers = &toSampled;
    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);

    endOneShotCommands(context, commandBuffer);
    destroyBuffer(context, staging);

    VkImageViewCreateInfo viewCreateInfo = {};
    viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCreateInfo.image = result.image;
    viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCreateInfo.format = desc.format;
    viewCreateInfo.components = desc.swizzle;
    viewCreateInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipCount, 0, 1};
    SOL_VK_CHECK(vkCreateImageView(context.device(), &viewCreateInfo, nullptr, &result.view));
    return result;
}

void destroyImage(Context& context, Image& image)
{
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(context.device(), image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(context.device(), image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(context.device(), image.memory, nullptr);
    }
    image = {};
}

VkSampler createSampler(Context& context, std::uint32_t mipLevels)
{
    VkSamplerCreateInfo samplerCreateInfo = {};
    samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
    samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
    samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerCreateInfo.maxLod = static_cast<float>(mipLevels);

    VkSampler sampler = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateSampler(context.device(), &samplerCreateInfo, nullptr, &sampler));
    return sampler;
}

VkSampler createClampSampler(Context& context)
{
    VkSamplerCreateInfo samplerCreateInfo = {};
    samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
    samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
    samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VkSampler sampler = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateSampler(context.device(), &samplerCreateInfo, nullptr, &sampler));
    return sampler;
}

VkCommandBuffer beginOneShotCommands(Context& context)
{
    VkCommandBufferAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = context.transientCommandPool();
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkAllocateCommandBuffers(context.device(), &allocateInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SOL_VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
    return commandBuffer;
}

void endOneShotCommands(Context& context, VkCommandBuffer commandBuffer)
{
    SOL_VK_CHECK(vkEndCommandBuffer(commandBuffer));

    VkCommandBufferSubmitInfo commandBufferInfo = {};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = commandBuffer;

    VkSubmitInfo2 submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;

    SOL_VK_CHECK(vkQueueSubmit2(context.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE));
    SOL_VK_CHECK(vkQueueWaitIdle(context.graphicsQueue()));

    vkFreeCommandBuffers(context.device(), context.transientCommandPool(), 1, &commandBuffer);
}

} // namespace sol::rhi
