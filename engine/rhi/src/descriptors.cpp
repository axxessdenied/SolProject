#include "sol/rhi/descriptors.hpp"

#include "vk_check.hpp"

namespace sol::rhi {

VkDescriptorSetLayout createTextureSetLayout(VkDevice device)
{
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.bindingCount = 1;
    createInfo.pBindings = &binding;

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateDescriptorSetLayout(device, &createInfo, nullptr, &layout));
    return layout;
}

VkDescriptorPool createTextureDescriptorPool(VkDevice device, std::uint32_t maxSets)
{
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = maxSets;

    VkDescriptorPoolCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    createInfo.maxSets = maxSets;
    createInfo.poolSizeCount = 1;
    createInfo.pPoolSizes = &poolSize;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateDescriptorPool(device, &createInfo, nullptr, &pool));
    return pool;
}

VkDescriptorSet allocateDescriptorSet(VkDevice device, VkDescriptorPool pool,
                                      VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo allocateInfo = {};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = pool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkAllocateDescriptorSets(device, &allocateInfo, &set));
    return set;
}

void writeTextureDescriptor(VkDevice device, VkDescriptorSet set, VkImageView view, VkSampler sampler)
{
    VkDescriptorImageInfo imageInfo = {};
    imageInfo.sampler = sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

} // namespace sol::rhi
