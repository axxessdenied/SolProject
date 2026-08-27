#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sol::rhi {

// Single combined-image-sampler binding at binding 0, fragment stage.
[[nodiscard]] VkDescriptorSetLayout createTextureSetLayout(VkDevice device);

// Pool sized for maxSets combined-image-sampler sets. allowFree permits
// individual vkFreeDescriptorSets (needed by consumers that recycle sets).
[[nodiscard]] VkDescriptorPool
createTextureDescriptorPool(VkDevice device, std::uint32_t maxSets, bool allowFree = false);

[[nodiscard]] VkDescriptorSet
allocateDescriptorSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout);

void writeTextureDescriptor(VkDevice device, VkDescriptorSet set, VkImageView view, VkSampler sampler);

// A MATERIAL's own set (Phase 25 stage C): `samplerCount` combined image
// samplers at bindings 0..n-1, then one uniform buffer at binding n if
// `uniformBuffer`. Fragment stage, like everything else here.
//
// ⚑ This is the second descriptor set layout SHAPE in the engine, and the
// first one built from data rather than named in code - Phase 25's diagnosis
// counted exactly one ("single combined-image-sampler at binding 0"), which is
// what `createTextureSetLayout` above still is and still exclusively serves set
// 0. A material declaring nothing gets no set of this kind at all.
[[nodiscard]] VkDescriptorSetLayout
createMaterialSetLayout(VkDevice device, std::uint32_t samplerCount, bool uniformBuffer);

// A pool sized for the material sets above. Zero of either kind is legal and
// that size is simply omitted, because Vulkan rejects a pool size of 0.
[[nodiscard]] VkDescriptorPool createMaterialDescriptorPool(VkDevice device,
                                                            std::uint32_t maxSets,
                                                            std::uint32_t samplerCount,
                                                            std::uint32_t uniformBufferCount);

struct MaterialTextureBinding
{
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

// Writes a whole material set in one update: the textures at bindings
// 0..count-1, and the uniform buffer at binding `count` when `buffer` is not
// VK_NULL_HANDLE. One call rather than one per binding, because a half-written
// set is a state no caller should be able to reach.
void writeMaterialDescriptors(VkDevice device,
                              VkDescriptorSet set,
                              const MaterialTextureBinding* textures,
                              std::uint32_t textureCount,
                              VkBuffer buffer,
                              VkDeviceSize bufferSize);

} // namespace sol::rhi
