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

// Textured mesh draws: the layout, the descriptor pool, the GPU resources and
// the push block. ⚑⚑ IT NO LONGER OWNS A PIPELINE (Phase 25 stage B). It used
// to build two by hand from `mesh.vert.spv`/`mesh.frag.spv` - the opaque one
// and Phase 12's translucent variant - and those were two of the nine pipelines
// named by a hardcoded filename inside their own C++ that this phase exists to
// remove. `MaterialRegistry` builds them from `[[material]]` rows now, against
// the layout below, and the caller binds one before drawing.
//
// ⚑ The LAYOUT stays here on purpose. It is what the push block and the single
// combined-image-sampler set are, it is shared by every material in this
// phase, and finding 2 measured that the engine has exactly one descriptor set
// layout shape - so there is nothing for a material to vary about it until
// stage C gives materials their own slots.
class MeshRenderer
{
public:
    // ⚑ No formats and no shader directory any more: both existed only to
    // build the two pipelines this class used to own, and a parameter kept
    // "for symmetry" after its reason has gone is how a signature starts
    // lying about what a class does.
    [[nodiscard]] bool initialize(rhi::Context& context);
    void shutdown();

    // What a `MaterialRegistry` builds its pipelines against.
    [[nodiscard]] VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }

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

    // Recording: bind a material's pipeline once, then draw() per object.
    // emissive adds unlit albedo glow (engine housings, windows).
    //
    // ⚑ ANYTHING BOUND WITH A BLENDING PIPELINE MUST BE RECORDED AFTER THE
    // SKY, and that rule outlived the `bindTranslucent` that used to carry it.
    // The sky pass survives wherever depth is still at the reversed-Z clear
    // and a blended draw deliberately writes no depth, so a membrane drawn in
    // the opaque block would be painted over by the sky and read as broken
    // blending rather than as a misplaced pass.
    void bindPipeline(VkCommandBuffer commandBuffer, VkExtent2D extent, VkPipeline pipeline) const;

    // alpha is coverage in 0..1 and reaches the shader in the push block's one
    // remaining dead lane. 1.0 is the opaque identity: the fragment shader
    // premultiplies by it, so an opaque draw emits exactly what it always did.
    void draw(VkCommandBuffer commandBuffer,
              const GpuMesh& mesh,
              const GpuTexture& texture,
              const core::Mat4& mvp,
              const core::Mat4& model,
              float emissive = 0.0f,
              float alpha = 1.0f) const;

private:
    rhi::Context* m_context = nullptr;

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    core::Vec3 m_sunDirection = {0.0f, 1.0f, 0.0f};
    float m_sunIntensity = 1.0f;
    float m_ambient = 0.03f;
};

} // namespace sol::renderer
