#include "sol/renderer/debug_draw_renderer.hpp"

#include "sol/rhi/pipeline.hpp"

#include <cmath>
#include <cstring>

namespace sol::renderer {

namespace {

struct PushConstants
{
    core::Mat4 viewProjection;
};

} // namespace

bool DebugDrawRenderer::initialize(rhi::Context& context,
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
                              kMaxVertices * sizeof(Vertex),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkMapMemory(
                context.device(), m_vertexBuffers[i].memory, 0, VK_WHOLE_SIZE, 0, &m_mappedPointers[i]) !=
            VK_SUCCESS) {
            return false;
        }
    }

    m_pipelineLayout = rhi::createPipelineLayout(context.device(), nullptr, 0, sizeof(PushConstants));
    m_vertices.reserve(1024);
    return reloadPipeline();
}

bool DebugDrawRenderer::reloadPipeline()
{
    const VkDevice device = m_context->device();
    const std::string vertexPath = m_shaderDirectory + "debug_line.vert.spv";
    const std::string fragmentPath = m_shaderDirectory + "debug_line.frag.spv";

    VkShaderModule vertexShader = rhi::createShaderModuleFromFile(device, vertexPath.c_str());
    VkShaderModule fragmentShader = rhi::createShaderModuleFromFile(device, fragmentPath.c_str());
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        return false;
    }

    static constexpr rhi::VertexAttribute kAttributes[] = {
        {0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
    };

    rhi::GraphicsPipelineDesc desc = {};
    desc.vertexShader = vertexShader;
    desc.fragmentShader = fragmentShader;
    desc.colorFormat = m_colorFormat;
    desc.vertexStride = sizeof(Vertex);
    desc.attributes = kAttributes;
    desc.attributeCount = 2;
    desc.depthFormat = m_depthFormat;
    desc.depthTest = true; // occluded by geometry, but always on top of the sky
    desc.depthWrite = false;
    desc.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
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

void DebugDrawRenderer::line(core::Vec3 a, core::Vec3 b, core::Vec4 color)
{
    if (m_vertices.size() + 2 > kMaxVertices) {
        return;
    }
    m_vertices.push_back({a, color});
    m_vertices.push_back({b, color});
}

void DebugDrawRenderer::arrow(core::Vec3 from, core::Vec3 to, core::Vec4 color)
{
    line(from, to, color);

    const core::Vec3 shaft = to - from;
    const float shaftLength = length(shaft);
    if (shaftLength <= 0.0f) {
        return;
    }
    const core::Vec3 direction = shaft / shaftLength;
    const core::Vec3 reference =
        std::abs(direction.y) < 0.99f ? core::Vec3{0.0f, 1.0f, 0.0f} : core::Vec3{1.0f, 0.0f, 0.0f};
    const core::Vec3 side = normalize(cross(reference, direction));
    const core::Vec3 up = cross(direction, side);
    const float headLength = shaftLength * 0.15f;
    const core::Vec3 back = to - direction * headLength;
    line(to, back + side * (headLength * 0.5f), color);
    line(to, back - side * (headLength * 0.5f), color);
    line(to, back + up * (headLength * 0.5f), color);
    line(to, back - up * (headLength * 0.5f), color);
}

void DebugDrawRenderer::axes(core::Vec3 origin, const core::Quat& orientation, float axisLength)
{
    line(origin, origin + rotate(orientation, {axisLength, 0.0f, 0.0f}), {1.0f, 0.2f, 0.2f, 1.0f});
    line(origin, origin + rotate(orientation, {0.0f, axisLength, 0.0f}), {0.2f, 1.0f, 0.2f, 1.0f});
    line(origin, origin + rotate(orientation, {0.0f, 0.0f, -axisLength}), {0.3f, 0.5f, 1.0f, 1.0f});
}

void DebugDrawRenderer::draw(VkCommandBuffer commandBuffer,
                             std::uint32_t frameIndex,
                             const core::Mat4& viewProjection)
{
    if (m_vertices.empty()) {
        return;
    }
    std::memcpy(m_mappedPointers[frameIndex], m_vertices.data(), m_vertices.size() * sizeof(Vertex));

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const PushConstants push = {viewProjection};
    vkCmdPushConstants(commandBuffer,
                       m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,
                       sizeof(push),
                       &push);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffers[frameIndex].buffer, &offset);
    vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(m_vertices.size()), 1, 0, 0);
}

void DebugDrawRenderer::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();
    for (std::size_t i = 0; i < m_vertexBuffers.size(); ++i) {
        if (m_vertexBuffers[i].memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device, m_vertexBuffers[i].memory);
        }
        rhi::destroyBuffer(*m_context, m_vertexBuffers[i]);
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
