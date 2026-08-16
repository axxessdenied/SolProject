#include "sol/renderer/ui_renderer.hpp"

#include "sol/core/log.hpp"
#include "sol/rhi/descriptors.hpp"
#include "sol/rhi/pipeline.hpp"

#include <algorithm>
#include <cstring>

namespace sol::renderer {

namespace {

struct PushConstants
{
    core::Vec2 inverseScreenSize;
};

} // namespace

bool UiRenderer::initialize(rhi::Context& context, VkFormat colorFormat, VkFormat depthFormat,
                            const char* shaderDirectory, std::uint32_t framesInFlight)
{
    m_context = &context;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_shaderDirectory = shaderDirectory;

    m_vertexBuffers.resize(framesInFlight);
    m_indexBuffers.resize(framesInFlight);
    m_vertexMapped.resize(framesInFlight);
    m_indexMapped.resize(framesInFlight);
    for (std::uint32_t i = 0; i < framesInFlight; ++i) {
        m_vertexBuffers[i] =
            rhi::createBuffer(context, kMaxVertices * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        m_indexBuffers[i] = rhi::createBuffer(context, kMaxIndices * sizeof(std::uint16_t),
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkMapMemory(context.device(), m_vertexBuffers[i].memory, 0, VK_WHOLE_SIZE, 0,
                        &m_vertexMapped[i]) != VK_SUCCESS ||
            vkMapMemory(context.device(), m_indexBuffers[i].memory, 0, VK_WHOLE_SIZE, 0,
                        &m_indexMapped[i]) != VK_SUCCESS) {
            return false;
        }
    }

    m_setLayout = rhi::createTextureSetLayout(context.device());
    m_descriptorPool = rhi::createTextureDescriptorPool(context.device(), kMaxTextures);
    m_sampler = rhi::createClampSampler(context);

    // Texture 0: a single white pixel, so solid rectangles go through the same
    // pipeline as text instead of needing a branch.
    const std::uint8_t white = 0xFF;
    const std::uint8_t* whiteData = &white;
    const std::uint32_t whiteSize = 1;
    rhi::TextureUploadDesc whiteDesc = {};
    whiteDesc.width = 1;
    whiteDesc.height = 1;
    whiteDesc.format = VK_FORMAT_R8_UNORM;
    whiteDesc.mipCount = 1;
    whiteDesc.mipData = &whiteData;
    whiteDesc.mipSizes = &whiteSize;
    whiteDesc.swizzle = {VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE,
                         VK_COMPONENT_SWIZZLE_R};
    m_whiteImage = rhi::createSampledTexture(context, whiteDesc);
    if (registerTexture(m_whiteImage.view, m_sampler) != 0) {
        return false;
    }

    m_pipelineLayout = rhi::createPipelineLayout(context.device(), &m_setLayout, 1, sizeof(PushConstants));
    return reloadPipeline();
}

std::uint32_t UiRenderer::registerTexture(VkImageView view, VkSampler sampler)
{
    if (m_textureSets.size() >= kMaxTextures) {
        SOL_LOG_ERROR("ui: texture slots exhausted (%u)", kMaxTextures);
        return 0;
    }
    const VkDescriptorSet set =
        rhi::allocateDescriptorSet(m_context->device(), m_descriptorPool, m_setLayout);
    rhi::writeTextureDescriptor(m_context->device(), set, view, sampler);
    m_textureSets.push_back(set);
    return static_cast<std::uint32_t>(m_textureSets.size() - 1);
}

bool UiRenderer::reloadPipeline()
{
    const VkDevice device = m_context->device();
    const std::string vertexPath = m_shaderDirectory + "ui.vert.spv";
    const std::string fragmentPath = m_shaderDirectory + "ui.frag.spv";

    VkShaderModule vertexShader = rhi::createShaderModuleFromFile(device, vertexPath.c_str());
    VkShaderModule fragmentShader = rhi::createShaderModuleFromFile(device, fragmentPath.c_str());
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        return false;
    }

    static constexpr rhi::VertexAttribute kAttributes[] = {
        {0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, position)},
        {1, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color)},
    };

    rhi::GraphicsPipelineDesc desc = {};
    desc.vertexShader = vertexShader;
    desc.fragmentShader = fragmentShader;
    desc.colorFormat = m_colorFormat;
    desc.blendMode = rhi::BlendMode::Alpha;
    desc.vertexStride = sizeof(Vertex);
    desc.attributes = kAttributes;
    desc.attributeCount = 3;
    // The pass carries a depth attachment; the UI just ignores it (draw order
    // is the UI's own ordering), but the formats still have to agree.
    desc.depthFormat = m_depthFormat;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.layout = m_pipelineLayout;

    VkPipeline newPipeline = VK_NULL_HANDLE;
    const bool created = rhi::createGraphicsPipeline(device, desc, newPipeline);
    vkDestroyShaderModule(device, vertexShader, nullptr);
    vkDestroyShaderModule(device, fragmentShader, nullptr);
    if (!created) {
        return false;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
    }
    m_pipeline = newPipeline;
    return true;
}

void UiRenderer::draw(VkCommandBuffer commandBuffer, std::uint32_t frameIndex, core::Vec2 uiSize,
                      VkExtent2D framebufferExtent, std::span<const Vertex> vertices,
                      std::span<const std::uint16_t> indices, std::span<const Batch> batches)
{
    if (batches.empty() || vertices.empty() || indices.empty() || uiSize.x <= 0.0f ||
        uiSize.y <= 0.0f || framebufferExtent.width == 0 || framebufferExtent.height == 0) {
        return;
    }
    if (vertices.size() > kMaxVertices || indices.size() > kMaxIndices) {
        SOL_LOG_ERROR("ui: geometry overflow (%zu verts, %zu indices) - frame dropped",
                      vertices.size(), indices.size());
        return;
    }

    std::memcpy(m_vertexMapped[frameIndex], vertices.data(), vertices.size() * sizeof(Vertex));
    std::memcpy(m_indexMapped[frameIndex], indices.data(), indices.size() * sizeof(std::uint16_t));

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    PushConstants push = {};
    push.inverseScreenSize = {1.0f / uiSize.x, 1.0f / uiSize.y};

    // Clip rectangles arrive in UI pixels; scissors are framebuffer pixels.
    const float clipScaleX = static_cast<float>(framebufferExtent.width) / uiSize.x;
    const float clipScaleY = static_cast<float>(framebufferExtent.height) / uiSize.y;
    const float maxScissorX = static_cast<float>(framebufferExtent.width);
    const float maxScissorY = static_cast<float>(framebufferExtent.height);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffers[frameIndex].buffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffers[frameIndex].buffer, 0, VK_INDEX_TYPE_UINT16);

    std::uint32_t boundTexture = kMaxTextures; // force the first bind
    for (const Batch& batch : batches) {
        if (batch.indexCount == 0 ||
            static_cast<std::size_t>(batch.firstIndex) + batch.indexCount > indices.size()) {
            continue;
        }

        // An empty clip rect means the whole screen; anything else is clamped
        // so a negative or oversized rect cannot fault the scissor.
        VkRect2D scissor = {};
        if (batch.clipMax.x > batch.clipMin.x && batch.clipMax.y > batch.clipMin.y) {
            const float minX = std::clamp(batch.clipMin.x * clipScaleX, 0.0f, maxScissorX);
            const float minY = std::clamp(batch.clipMin.y * clipScaleY, 0.0f, maxScissorY);
            const float maxX = std::clamp(batch.clipMax.x * clipScaleX, minX, maxScissorX);
            const float maxY = std::clamp(batch.clipMax.y * clipScaleY, minY, maxScissorY);
            scissor.offset = {static_cast<std::int32_t>(minX), static_cast<std::int32_t>(minY)};
            scissor.extent = {static_cast<std::uint32_t>(maxX - minX),
                              static_cast<std::uint32_t>(maxY - minY)};
        } else {
            scissor.extent = framebufferExtent;
        }
        if (scissor.extent.width == 0 || scissor.extent.height == 0) {
            continue; // fully clipped away
        }
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        const std::uint32_t texture = batch.texture < m_textureSets.size() ? batch.texture : 0;
        if (texture != boundTexture) {
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                                    &m_textureSets[texture], 0, nullptr);
            boundTexture = texture;
        }
        vkCmdDrawIndexed(commandBuffer, batch.indexCount, 1, batch.firstIndex, 0, 0);
    }
}

void UiRenderer::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();
    for (std::size_t i = 0; i < m_vertexBuffers.size(); ++i) {
        if (m_vertexBuffers[i].memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device, m_vertexBuffers[i].memory);
        }
        if (m_indexBuffers[i].memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device, m_indexBuffers[i].memory);
        }
        rhi::destroyBuffer(*m_context, m_vertexBuffers[i]);
        rhi::destroyBuffer(*m_context, m_indexBuffers[i]);
    }
    m_vertexBuffers.clear();
    m_indexBuffers.clear();
    m_vertexMapped.clear();
    m_indexMapped.clear();
    m_textureSets.clear();

    rhi::destroyImage(*m_context, m_whiteImage);
    vkDestroySampler(device, m_sampler, nullptr);
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);

    m_sampler = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_context = nullptr;
}

} // namespace sol::renderer
