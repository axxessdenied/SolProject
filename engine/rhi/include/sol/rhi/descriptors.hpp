#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sol::rhi {

// Single combined-image-sampler binding at binding 0, fragment stage.
[[nodiscard]] VkDescriptorSetLayout createTextureSetLayout(VkDevice device);

// Pool sized for maxSets combined-image-sampler sets. allowFree permits
// individual vkFreeDescriptorSets (needed by consumers that recycle sets).
[[nodiscard]] VkDescriptorPool createTextureDescriptorPool(VkDevice device, std::uint32_t maxSets,
                                                           bool allowFree = false);

[[nodiscard]] VkDescriptorSet allocateDescriptorSet(VkDevice device, VkDescriptorPool pool,
                                                    VkDescriptorSetLayout layout);

void writeTextureDescriptor(VkDevice device, VkDescriptorSet set, VkImageView view, VkSampler sampler);

} // namespace sol::rhi
