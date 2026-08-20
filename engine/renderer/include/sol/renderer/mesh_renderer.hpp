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

    // Per-frame sun light: direction is surface-to-sun in world space (the
    // sun is far enough away to treat as directional), intensity in linear
    // HDR units.
    void setSunlight(core::Vec3 directionToSun, float intensity, float ambient)
    {
        m_sunDirection = directionToSun;
        m_sunIntensity = intensity;
        m_ambient = ambient;
    }

    // Recording: bind() once per pass, then draw() per object. emissive adds
    // unlit albedo glow (engine housings, windows).
    void bind(VkCommandBuffer commandBuffer, VkExtent2D extent) const;

    // Phase 12: the same shaders, layout and descriptor sets under alpha
    // blending, with no depth write and no back-face cull - a membrane is seen
    // through, and seen from both sides because you fly through it.
    //
    // ⚑ Anything bound with this MUST be recorded after the sky. The sky pass
    // survives wherever depth is still at the reversed-Z clear, and a
    // translucent draw deliberately writes no depth, so a membrane drawn in
    // the opaque block would be painted over by the sky and read as broken
    // blending rather than as a misplaced pass.
    void bindTranslucent(VkCommandBuffer commandBuffer, VkExtent2D extent) const;

    // alpha is coverage in 0..1 and reaches the shader in the push block's one
    // remaining dead lane. 1.0 is the opaque identity: the fragment shader
    // premultiplies by it, so an opaque draw emits exactly what it always did.
    void draw(VkCommandBuffer commandBuffer, const GpuMesh& mesh, const GpuTexture& texture,
              const core::Mat4& mvp, const core::Mat4& model, float emissive = 0.0f,
              float alpha = 1.0f) const;

private:
    [[nodiscard]] bool createPipeline();
    void bindPipeline(VkCommandBuffer commandBuffer, VkExtent2D extent, VkPipeline pipeline) const;

    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    std::string m_shaderDirectory;

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_translucentPipeline = VK_NULL_HANDLE;

    core::Vec3 m_sunDirection = {0.0f, 1.0f, 0.0f};
    float m_sunIntensity = 1.0f;
    float m_ambient = 0.03f;
};

} // namespace sol::renderer
