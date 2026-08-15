#pragma once

#include "sol/assets/asset_loader.hpp"
#include "sol/core/math/math.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/resources.hpp"

#include <cstdint>
#include <string>

namespace sol::renderer {

struct GpuMesh
{
    rhi::Buffer vertexBuffer;
    rhi::Buffer indexBuffer;
    std::uint32_t indexCount = 0;
};

struct GpuTexture
{
    rhi::Image image;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

// Textured, lambert-lit, depth-tested mesh pipeline (push-constant transforms).
class MeshRenderer
{
public:
    [[nodiscard]] bool initialize(rhi::Context& context, VkFormat colorFormat, VkFormat depthFormat,
                                  const char* shaderDirectory);
    void shutdown();

    // Recompiles nothing; recreates the pipeline from the (possibly re-cooked)
    // SPIR-V on disk. Caller must ensure the device is idle.
    [[nodiscard]] bool reloadPipeline();

    [[nodiscard]] GpuMesh createMesh(const assets::MeshData& data);
    void destroyMesh(GpuMesh& mesh);

    [[nodiscard]] GpuTexture createTexture(const assets::TextureData& data);
    void destroyTexture(GpuTexture& texture);

    // Recording: bind() once per pass, then draw() per object.
    void bind(VkCommandBuffer commandBuffer, VkExtent2D extent) const;
    void draw(VkCommandBuffer commandBuffer, const GpuMesh& mesh, const GpuTexture& texture,
              const core::Mat4& mvp, const core::Mat4& model) const;

private:
    [[nodiscard]] bool createPipeline();

    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::string m_shaderDirectory;

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
};

} // namespace sol::renderer
