#include "sol/renderer/mesh_renderer.hpp"

#include "sol/core/log.hpp"
#include "sol/rhi/descriptors.hpp"
#include "sol/rhi/pipeline.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace sol::renderer {

namespace {

// The model's upper 3x3 (normal transform) rides as three vec4 columns whose
// .w lanes carry the lighting scalars, keeping the whole block within the
// guaranteed 128-byte push constant minimum.
struct PushConstants
{
    core::Mat4 mvp;
    core::Vec4 modelColumn0; // .w = ambient
    core::Vec4 modelColumn1; // .w = emissive
    core::Vec4 modelColumn2; // .w = sun intensity
    core::Vec4 sunDirection; // .xyz = surface-to-sun, world space
};

// ⚑ Checked against the exported constant rather than against a literal, so
// the size the registry builds a pipeline layout with and the size this file
// pushes cannot drift apart silently.
static_assert(sizeof(PushConstants) == MeshRenderer::kPushConstantSize,
              "must fit the guaranteed push constant minimum");

constexpr std::uint32_t kMaxTextures = 256;

} // namespace

bool MeshRenderer::initialize(rhi::Context& context)
{
    m_context = &context;
    m_textureSetLayout = rhi::createTextureSetLayout(context.device());
    m_descriptorPool = rhi::createTextureDescriptorPool(context.device(), kMaxTextures);
    return m_textureSetLayout != VK_NULL_HANDLE && m_descriptorPool != VK_NULL_HANDLE;
}

void MeshRenderer::shutdown()
{
    if (m_context == nullptr) {
        return;
    }
    const VkDevice device = m_context->device();
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_textureSetLayout, nullptr);
    m_descriptorPool = VK_NULL_HANDLE;
    m_textureSetLayout = VK_NULL_HANDLE;
    m_context = nullptr;
}

GpuMesh MeshRenderer::createMesh(const assets::MeshData& data)
{
    GpuMesh mesh;
    mesh.vertexBuffer = rhi::createDeviceLocalBuffer(*m_context,
                                                     data.vertices.data(),
                                                     data.vertices.size() * sizeof(assets::MeshVertex),
                                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    mesh.indexBuffer = rhi::createDeviceLocalBuffer(*m_context,
                                                    data.indices.data(),
                                                    data.indices.size() * sizeof(std::uint32_t),
                                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    mesh.indexCount = static_cast<std::uint32_t>(data.indices.size());
    return mesh;
}

void MeshRenderer::destroyMesh(GpuMesh& mesh)
{
    rhi::destroyBuffer(*m_context, mesh.vertexBuffer);
    rhi::destroyBuffer(*m_context, mesh.indexBuffer);
    mesh.indexCount = 0;
}

GpuTexture MeshRenderer::createTexture(const assets::TextureData& data)
{
    std::vector<const std::uint8_t*> mipPointers;
    std::vector<std::uint32_t> mipSizes;
    for (const std::vector<std::uint8_t>& mip : data.mips) {
        mipPointers.push_back(mip.data());
        mipSizes.push_back(static_cast<std::uint32_t>(mip.size()));
    }

    rhi::TextureUploadDesc uploadDesc = {};
    uploadDesc.width = data.width;
    uploadDesc.height = data.height;
    uploadDesc.format = VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    uploadDesc.mipCount = static_cast<std::uint32_t>(data.mips.size());
    uploadDesc.mipData = mipPointers.data();
    uploadDesc.mipSizes = mipSizes.data();

    GpuTexture texture;
    texture.image = rhi::createSampledTexture(*m_context, uploadDesc);
    texture.sampler = rhi::createSampler(*m_context, uploadDesc.mipCount);
    texture.descriptorSet =
        rhi::allocateDescriptorSet(m_context->device(), m_descriptorPool, m_textureSetLayout);
    rhi::writeTextureDescriptor(
        m_context->device(), texture.descriptorSet, texture.image.view, texture.sampler);
    return texture;
}

void MeshRenderer::destroyTexture(GpuTexture& texture)
{
    if (texture.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_context->device(), texture.sampler, nullptr);
        texture.sampler = VK_NULL_HANDLE;
    }
    rhi::destroyImage(*m_context, texture.image);
    texture.descriptorSet = VK_NULL_HANDLE; // pool-owned
}

void MeshRenderer::bindMaterial(VkCommandBuffer commandBuffer,
                                VkExtent2D extent,
                                VkPipeline pipeline,
                                VkPipelineLayout layout,
                                VkDescriptorSet materialSet) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Negative viewport height: flips Vulkan's y-down clip space so the
    // engine's Y-up matrices land right side up (see math conventions).
    const VkViewport viewport = {0.0f,
                                 static_cast<float>(extent.height),
                                 static_cast<float>(extent.width),
                                 -static_cast<float>(extent.height),
                                 0.0f,
                                 1.0f};
    const VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // ⚑ ONCE PER MATERIAL, NOT ONCE PER DRAW. Set 1 is the material's - its
    // declared textures and its params buffer - so every object wearing the
    // material binds the same one, and binding it here is what makes that true
    // by construction rather than by a caller remembering.
    if (materialSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &materialSet, 0, nullptr);
    }
}

void MeshRenderer::draw(VkCommandBuffer commandBuffer,
                        const GpuMesh& mesh,
                        const GpuTexture& texture,
                        VkPipelineLayout layout,
                        const core::Mat4& mvp,
                        const core::Mat4& model,
                        float emissive,
                        float alpha) const
{
    PushConstants push = {};
    push.mvp = mvp;
    push.modelColumn0 = model.column(0);
    push.modelColumn0.w = m_ambient;
    push.modelColumn1 = model.column(1);
    push.modelColumn1.w = emissive;
    push.modelColumn2 = model.column(2);
    push.modelColumn2.w = m_sunIntensity;
    // .w was a dead lane until Phase 12; it is the alpha the shader
    // premultiplies by, and 1.0 reproduces the pre-Phase-12 output exactly.
    push.sunDirection = {m_sunDirection.x, m_sunDirection.y, m_sunDirection.z, alpha};
    vkCmdPushConstants(commandBuffer,
                       layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,
                       sizeof(push),
                       &push);
    vkCmdBindDescriptorSets(
        commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &texture.descriptorSet, 0, nullptr);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &mesh.vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, mesh.indexCount, 1, 0, 0, 0);
}

} // namespace sol::renderer
