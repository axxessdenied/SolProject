#include "sol/renderer/impostor_renderer.hpp"

#include "sol/rhi/pipeline.hpp"

namespace sol::renderer {

namespace {

struct PushConstants
{
    core::Mat4 viewProjection;
    core::Vec4 center;       // .xyz camera-relative, .w radius
    core::Vec4 colorA;       // .w = mode (0 planet, 1 star)
    core::Vec4 colorB;       // .w = quad scale (star glow halo)
    core::Vec4 sunDirection;
};
static_assert(sizeof(PushConstants) == 128);

} // namespace

bool ImpostorRenderer::initialize(rhi::Context& context, VkFormat colorFormat, VkFormat depthFormat,
                                  const char* shaderDirectory)
{
    m_context = &context;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_shaderDirectory = shaderDirectory;
    m_pipelineLayout = rhi::createPipelineLayout(context.device(), nullptr, 0, sizeof(PushConstants));
    return reloadPipeline();
}

bool ImpostorRenderer::reloadPipeline()
{
    const VkDevice device = m_context->device();
    const std::string vertexPath = m_shaderDirectory + "impostor.vert.spv";
    const std::string fragmentPath = m_shaderDirectory + "impostor.frag.spv";

    VkShaderModule vertexShader = rhi::createShaderModuleFromFile(device, vertexPath.c_str());
    VkShaderModule fragmentShader = rhi::createShaderModuleFromFile(device, fragmentPath.c_str());
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        return false;
    }

    rhi::GraphicsPipelineDesc desc = {};
    desc.vertexShader = vertexShader;
    desc.fragmentShader = fragmentShader;
    desc.colorFormat = m_colorFormat;
    desc.depthFormat = m_depthFormat;
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.layout = m_pipelineLayout;

    VkPipeline newPlanet = VK_NULL_HANDLE;
    if (!rhi::createGraphicsPipeline(device, desc, newPlanet)) {
        vkDestroyShaderModule(device, vertexShader, nullptr);
        vkDestroyShaderModule(device, fragmentShader, nullptr);
        return false;
    }

    desc.depthWrite = false;
    desc.blendMode = rhi::BlendMode::Additive;
    VkPipeline newStar = VK_NULL_HANDLE;
    const bool starCreated = rhi::createGraphicsPipeline(device, desc, newStar);

    vkDestroyShaderModule(device, vertexShader, nullptr);
    vkDestroyShaderModule(device, fragmentShader, nullptr);
    if (!starCreated) {
        vkDestroyPipeline(device, newPlanet, nullptr);
        return false;
    }

    if (m_planetPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_planetPipeline, nullptr);
    }
    if (m_starPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_starPipeline, nullptr);
    }
    m_planetPipeline = newPlanet;
    m_starPipeline = newStar;
    return true;
}

void ImpostorRenderer::draw(VkCommandBuffer commandBuffer, VkPipeline pipeline,
                            const core::Mat4& viewProjection, const Body& body, float mode,
                            float quadScale) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    PushConstants push = {};
    push.viewProjection = viewProjection;
    push.center = {body.centerRelative.x, body.centerRelative.y, body.centerRelative.z, body.radius};
    push.colorA = {body.colorA.x, body.colorA.y, body.colorA.z, mode};
    push.colorB = {body.colorB.x, body.colorB.y, body.colorB.z, quadScale};
    push.sunDirection = {body.sunDirection.x, body.sunDirection.y, body.sunDirection.z, 0.0f};
    vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    // Viewport/scissor inherited from the pass (same dynamic state).
    vkCmdDraw(commandBuffer, 6, 1, 0, 0);
}

void ImpostorRenderer::drawPlanet(VkCommandBuffer commandBuffer, const core::Mat4& viewProjection,
                                  const Body& body) const
{
    draw(commandBuffer, m_planetPipeline, viewProjection, body, 0.0f, 1.05f);
}

void ImpostorRenderer::drawStar(VkCommandBuffer commandBuffer, const core::Mat4& viewProjection,
                                const Body& body) const
{
    draw(commandBuffer, m_starPipeline, viewProjection, body, 1.0f, 8.0f);
}

void ImpostorRenderer::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();
    vkDestroyPipeline(device, m_planetPipeline, nullptr);
    vkDestroyPipeline(device, m_starPipeline, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    m_planetPipeline = VK_NULL_HANDLE;
    m_starPipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_context = nullptr;
}

} // namespace sol::renderer
