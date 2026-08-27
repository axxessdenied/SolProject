#include "sol/rhi/descriptors.hpp"

#include "vk_check.hpp"

#include <vector>

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

VkDescriptorPool createTextureDescriptorPool(VkDevice device, std::uint32_t maxSets, bool allowFree)
{
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = maxSets;

    VkDescriptorPoolCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // The cast is for GCC, not for correctness: the two arms of the conditional
    // are an enumerator and an unsigned, which -Wextra rejects and /W4 does not
    // (Phase 21). Same value either way.
    createInfo.flags =
        allowFree
            ? static_cast<VkDescriptorPoolCreateFlags>(VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
            : 0u;
    createInfo.maxSets = maxSets;
    createInfo.poolSizeCount = 1;
    createInfo.pPoolSizes = &poolSize;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateDescriptorPool(device, &createInfo, nullptr, &pool));
    return pool;
}

VkDescriptorSet allocateDescriptorSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout)
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

VkDescriptorSetLayout createMaterialSetLayout(VkDevice device, std::uint32_t samplerCount, bool uniformBuffer)
{
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(static_cast<std::size_t>(samplerCount) + 1u);
    for (std::uint32_t i = 0; i < samplerCount; ++i) {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = i;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(binding);
    }
    if (uniformBuffer) {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = samplerCount;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(binding);
    }

    VkDescriptorSetLayoutCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    createInfo.pBindings = bindings.empty() ? nullptr : bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateDescriptorSetLayout(device, &createInfo, nullptr, &layout));
    return layout;
}

VkDescriptorPool createMaterialDescriptorPool(VkDevice device,
                                              std::uint32_t maxSets,
                                              std::uint32_t samplerCount,
                                              std::uint32_t uniformBufferCount)
{
    VkDescriptorPoolSize sizes[2] = {};
    std::uint32_t sizeCount = 0;
    if (samplerCount > 0) {
        sizes[sizeCount].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[sizeCount].descriptorCount = samplerCount;
        ++sizeCount;
    }
    if (uniformBufferCount > 0) {
        sizes[sizeCount].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[sizeCount].descriptorCount = uniformBufferCount;
        ++sizeCount;
    }

    VkDescriptorPoolCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    createInfo.maxSets = maxSets;
    createInfo.poolSizeCount = sizeCount;
    createInfo.pPoolSizes = sizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateDescriptorPool(device, &createInfo, nullptr, &pool));
    return pool;
}

void writeMaterialDescriptors(VkDevice device,
                              VkDescriptorSet set,
                              const MaterialTextureBinding* textures,
                              std::uint32_t textureCount,
                              VkBuffer buffer,
                              VkDeviceSize bufferSize)
{
    // ⚑ Both arrays are built in full before the single update call because
    // `VkWriteDescriptorSet` holds POINTERS into them: filling and submitting
    // them one at a time would work, and growing a vector mid-loop would
    // dangle every pointer written so far. Reserved, not grown.
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkWriteDescriptorSet> writes;
    imageInfos.reserve(textureCount);
    writes.reserve(static_cast<std::size_t>(textureCount) + 1u);

    for (std::uint32_t i = 0; i < textureCount; ++i) {
        VkDescriptorImageInfo imageInfo = {};
        imageInfo.sampler = textures[i].sampler;
        imageInfo.imageView = textures[i].view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.push_back(imageInfo);
    }
    for (std::uint32_t i = 0; i < textureCount; ++i) {
        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfos[i];
        writes.push_back(write);
    }

    VkDescriptorBufferInfo bufferInfo = {};
    if (buffer != VK_NULL_HANDLE) {
        bufferInfo.buffer = buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = bufferSize;

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = textureCount;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        writes.push_back(write);
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

} // namespace sol::rhi
