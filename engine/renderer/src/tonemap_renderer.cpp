#include "sol/renderer/tonemap_renderer.hpp"

#include "sol/core/math/math.hpp"
#include "sol/rhi/descriptors.hpp"
#include "sol/rhi/pipeline.hpp"
#include "sol/rhi/resources.hpp"

namespace sol::renderer {

namespace {

struct PushConstants
{
    core::Vec4 params; // .x = exposure
};

} // namespace

bool TonemapRenderer::initialize(rhi::Context& context,
                                 VkFormat colorFormat,
                                 VkFormat depthFormat,
                                 const char* shaderDirectory)
{
    m_context = &context;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_shaderDirectory = shaderDirectory;

    m_setLayout = rhi::createTextureSetLayout(context.device());
    m_descriptorPool = rhi::createTextureDescriptorPool(context.device(), 1);
    m_descriptorSet = rhi::allocateDescriptorSet(context.device(), m_descriptorPool, m_setLayout);
    m_sampler = rhi::createClampSampler(context);
    m_pipelineLayout = rhi::createPipelineLayout(context.device(), &m_setLayout, 1, sizeof(PushConstants));
    return reloadPipeline();
}

bool TonemapRenderer::reloadPipeline()
{
    const VkDevice device = m_context->device();
    const std::string vertexPath = m_shaderDirectory + "fullscreen.vert.spv";
    const std::string fragmentPath = m_shaderDirectory + "tonemap.frag.spv";

    VkShaderModule vertexShader = rhi::createShaderModuleFromFile(device, vertexPath.c_str());
    VkShaderModule fragmentShader = rhi::createShaderModuleFromFile(device, fragmentPath.c_str());
    if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE) {
        return false;
    }

    rhi::GraphicsPipelineDesc desc = {};
    desc.vertexShader = vertexShader;
    desc.fragmentShader = fragmentShader;
    desc.colorFormat = m_colorFormat;
    desc.depthFormat = m_depthFormat; // attachment present in the pass; no test
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

void TonemapRenderer::setSource(VkImageView hdrView)
{
    rhi::writeTextureDescriptor(m_context->device(), m_descriptorSet, hdrView, m_sampler);
}

void TonemapRenderer::draw(VkCommandBuffer commandBuffer, VkExtent2D extent, float exposure) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const VkViewport viewport = {
        0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const PushConstants push = {{exposure, 0.0f, 0.0f, 0.0f}};
    vkCmdPushConstants(commandBuffer,
                       m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,
                       sizeof(push),
                       &push);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

void TonemapRenderer::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();
    vkDestroyPipeline(device, m_pipeline, nullptr);
    vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
    vkDestroySampler(device, m_sampler, nullptr);
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    *this = {};
}

} // namespace sol::renderer
