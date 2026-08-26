#include "sol/renderer/particle_renderer.hpp"

#include "sol/rhi/pipeline.hpp"

#include <algorithm>
#include <cstring>

namespace sol::renderer {

namespace {

struct PushConstants
{
    core::Mat4 viewProjection;
    core::Vec4 cameraRight;
    core::Vec4 cameraUp;
};

} // namespace

bool ParticleRenderer::initialize(rhi::Context& context,
                                  VkFormat colorFormat,
                                  VkFormat depthFormat,
                                  const char* shaderDirectory,
                                  std::uint32_t framesInFlight)
{
    m_context = &context;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_shaderDirectory = shaderDirectory;

    m_vertexBuffers.resize(framesInFlight);
    m_mappedPointers.resize(framesInFlight);
    for (std::uint32_t i = 0; i < framesInFlight; ++i) {
        m_vertexBuffers[i] =
            rhi::createBuffer(context,
                              kMaxParticles * 6 * sizeof(Vertex),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkMapMemory(
                context.device(), m_vertexBuffers[i].memory, 0, VK_WHOLE_SIZE, 0, &m_mappedPointers[i]) !=
            VK_SUCCESS) {
            return false;
        }
    }

    m_pipelineLayout = rhi::createPipelineLayout(context.device(), nullptr, 0, sizeof(PushConstants));
    return reloadPipeline();
}

bool ParticleRenderer::reloadPipeline()
{
    const VkDevice device = m_context->device();
    const std::string vertexPath = m_shaderDirectory + "particle.vert.spv";
    const std::string fragmentPath = m_shaderDirectory + "particle.frag.spv";

    VkShaderModule vertexShader = rhi::createShaderModuleFromFile(device, vertexPath.c_str());
    VkShaderModule fragmentShader = rhi::createShaderModuleFromFile(device, fragmentPath.c_str());
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        return false;
    }

    static constexpr rhi::VertexAttribute kAttributes[] = {
        {0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, corner)},
        {2, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        {3, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, size)},
    };

    rhi::GraphicsPipelineDesc desc = {};
    desc.vertexShader = vertexShader;
    desc.fragmentShader = fragmentShader;
    desc.colorFormat = m_colorFormat;
    desc.blendMode = rhi::BlendMode::Additive;
    desc.vertexStride = sizeof(Vertex);
    desc.attributes = kAttributes;
    desc.attributeCount = 4;
    desc.depthFormat = m_depthFormat;
    desc.depthTest = true; // hidden behind hulls, no write (additive)
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

void ParticleRenderer::draw(VkCommandBuffer commandBuffer,
                            std::uint32_t frameIndex,
                            const core::Mat4& viewProjection,
                            const core::Quat& cameraOrientation,
                            std::span<const Particle> particles)
{
    if (particles.empty()) {
        return;
    }
    const std::uint32_t count = std::min(static_cast<std::uint32_t>(particles.size()), kMaxParticles);

    static constexpr core::Vec2 kCorners[6] = {
        {-1.0f, -1.0f},
        {1.0f, -1.0f},
        {1.0f, 1.0f},
        {-1.0f, -1.0f},
        {1.0f, 1.0f},
        {-1.0f, 1.0f},
    };
    Vertex* vertex = static_cast<Vertex*>(m_mappedPointers[frameIndex]);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Particle& particle = particles[i];
        for (const core::Vec2 corner : kCorners) {
            *vertex++ = {particle.position, corner, particle.color, particle.size};
        }
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    PushConstants push = {};
    push.viewProjection = viewProjection;
    const core::Vec3 right = rotate(cameraOrientation, {1.0f, 0.0f, 0.0f});
    const core::Vec3 up = rotate(cameraOrientation, {0.0f, 1.0f, 0.0f});
    push.cameraRight = {right.x, right.y, right.z, 0.0f};
    push.cameraUp = {up.x, up.y, up.z, 0.0f};
    vkCmdPushConstants(commandBuffer,
                       m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,
                       sizeof(push),
                       &push);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffers[frameIndex].buffer, &offset);
    vkCmdDraw(commandBuffer, count * 6, 1, 0, 0);
}

void ParticleRenderer::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();
    for (rhi::Buffer& buffer : m_vertexBuffers) {
        if (buffer.memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device, buffer.memory);
        }
        rhi::destroyBuffer(*m_context, buffer);
    }
    m_vertexBuffers.clear();
    m_mappedPointers.clear();
    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_context = nullptr;
}

} // namespace sol::renderer
