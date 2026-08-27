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
// ⚑⚑ AND SINCE STAGE C IT NO LONGER OWNS A PIPELINE LAYOUT EITHER. Stage B's
// comment here said the layout stays "until stage C gives materials their own
// slots", and that is exactly what happened: a material declaring textures or
// params needs a set 1, so the layout stopped being one thing shared by every
// material and became one per declaration SHAPE. `MaterialRegistry` owns them,
// because a pipeline and the layout it was built against must have a single
// owner or they can disagree.
//
// ⚑ WHAT STAYS IS SET 0's DESCRIPTOR SET LAYOUT, and it stays because this is
// what allocates a set per uploaded texture. Set 0 is the albedo every mesh
// material has, bound per draw, unchanged since Phase 3 - which is the sentence
// that let stage C leave `mesh.frag`, `membrane.frag` and the Forge alone.
class MeshRenderer
{
public:
    // ⚑ No formats and no shader directory any more: both existed only to
    // build the two pipelines this class used to own, and a parameter kept
    // "for symmetry" after its reason has gone is how a signature starts
    // lying about what a class does.
    [[nodiscard]] bool initialize(rhi::Context& context);
    void shutdown();

    // Set 0's layout, which a `MaterialRegistry` puts in front of every
    // pipeline layout it builds.
    [[nodiscard]] VkDescriptorSetLayout textureSetLayout() const { return m_textureSetLayout; }

    // ⚑ The push block's size, exported so the registry can build a layout for
    // it without this file's `PushConstants` becoming public. The 128-byte
    // guarantee is the floor this engine deliberately targets and the
    // `static_assert` that enforces it now checks against this constant, so the
    // two cannot drift.
    static constexpr std::uint32_t kPushConstantSize = 128;

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

    // Recording: bind a material once, then draw() per object.
    //
    // ⚑ ANYTHING BOUND WITH A BLENDING PIPELINE MUST BE RECORDED AFTER THE
    // SKY, and that rule outlived the `bindTranslucent` that used to carry it.
    // The sky pass survives wherever depth is still at the reversed-Z clear
    // and a blended draw deliberately writes no depth, so a membrane drawn in
    // the opaque block would be painted over by the sky and read as broken
    // blending rather than as a misplaced pass.
    //
    // ⚑ `materialSet` is set 1, bound here rather than in `draw` because it is
    // the one thing in this call that is constant across every object wearing
    // the material. VK_NULL_HANDLE for a material that declares nothing, which
    // is every material this engine had before Phase 25 stage C.
    void bindMaterial(VkCommandBuffer commandBuffer,
                      VkExtent2D extent,
                      VkPipeline pipeline,
                      VkPipelineLayout layout,
                      VkDescriptorSet materialSet = VK_NULL_HANDLE) const;

    // emissive adds unlit albedo glow (engine housings, windows). alpha is
    // coverage in 0..1 and reaches the shader in the push block's one remaining
    // dead lane; 1.0 is the opaque identity, since the fragment shader
    // premultiplies by it and so an opaque draw emits exactly what it always
    // did.
    //
    // ⚑ `layout` is the material's, from the registry, because a push constant
    // range and a descriptor binding are both addressed through it and it is no
    // longer one value for the whole engine.
    void draw(VkCommandBuffer commandBuffer,
              const GpuMesh& mesh,
              const GpuTexture& texture,
              VkPipelineLayout layout,
              const core::Mat4& mvp,
              const core::Mat4& model,
              float emissive = 0.0f,
              float alpha = 1.0f) const;

private:
    rhi::Context* m_context = nullptr;

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    core::Vec3 m_sunDirection = {0.0f, 1.0f, 0.0f};
    float m_sunIntensity = 1.0f;
    float m_ambient = 0.03f;
};

} // namespace sol::renderer
