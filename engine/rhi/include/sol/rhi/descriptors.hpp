#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sol::rhi {

// Single combined-image-sampler binding at binding 0, fragment stage.
[[nodiscard]] VkDescriptorSetLayout createTextureSetLayout(VkDevice device);

// Pool sized for maxSets combined-image-sampler sets.
[[nodiscard]] VkDescriptorPool createTextureDescriptorPool(VkDevice device, std::uint32_t maxSets);

[[nodiscard]] VkDescriptorSet allocateDescriptorSet(VkDevice device, VkDescriptorPool pool,
                                                    VkDescriptorSetLayout layout);

void writeTextureDescriptor(VkDevice device, VkDescriptorSet set, VkImageView view, VkSampler sampler);

} // namespace sol::rhi
