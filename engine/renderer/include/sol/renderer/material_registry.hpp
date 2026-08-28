#pragma once

// The pipelines the `[[material]]` rows ask for (engine plan Phase 25 stages B
// and C).
//
// ⚑⚑ THE WALL THIS REMOVES, WHICH IS THE THIRD AND LAST INSTANCE OF ONE
// PATTERN. Phase 9 stage A's diagnosis read "the runtime cannot reference a
// mesh it was not compiled to know about"; Phase 24 stage S's read "the
// runtime cannot reference an asset that was not in the build"; this one read
// "nothing in this engine can draw with a shader it was not compiled to know
// about" - nine pipelines from fourteen shaders across seven renderers, every
// pair named by a hardcoded filename inside its own C++. A material names its
// shader pair and its state as data, and this builds the pipeline.
//
// ⚑ IT IS A *MESH* MATERIAL REGISTRY AND SAYS SO. The vertex format is
// `MeshVertex` and set 0 is `MeshRenderer`'s single combined-image-sampler plus
// the 128-byte push block. Phase 25 decision 4 keeps vertex formats out of this
// phase.
//
// ⚑⚑ WHAT STAGE C ADDED, AND THE ONE SENTENCE THAT EXPLAINS THE WHOLE SHAPE:
// SET 0 IS THE ENGINE'S AND SET 1 IS THE MATERIAL'S. Set 0 is the albedo every
// mesh material has, bound per draw, unchanged since Phase 3 - which is why
// `mesh.frag`, `membrane.frag` and the Forge's viewport needed no edit for this
// stage. Set 1 holds what a material DECLARES: its extra textures at bindings
// 0..n-1 and, after them, one uniform buffer of its scalar params, written once
// at load. A material declaring neither gets no set 1 and the same pipeline
// layout every material had before stage C.
//
// ⚑⚑ AND THE DECLARATION IS CHECKED AGAINST THE SPIR-V (`spirv_reflect.hpp`).
// The layout is still BUILT from the declaration, which is Phase 25 decision 2;
// reflection is only what turns a declaration that lies into a refusal naming
// the slot. Without it the sole signal is the Vulkan validation layer, which is
// on in dev builds and off in shipping - i.e. present exactly where the author
// already is and absent exactly where the player is.

#include "sol/assets/data_defs.hpp"
#include "sol/renderer/material_state.hpp"
#include "sol/renderer/spirv_reflect.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/descriptors.hpp"
#include "sol/rhi/resources.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sol::renderer {

class MaterialRegistry
{
public:
    // `shaderSearchPath` is an ordered list of directories, highest priority
    // first, each ending in '/'. One entry today; a mod layer's is what stage E
    // adds, and it is a list here so that stage is a caller change and not a
    // signature change.
    //
    // ⚑ `textureSetLayout` is set 0's, owned by `MeshRenderer` because that is
    // what allocates a set per uploaded texture. This class owns every PIPELINE
    // layout, including the one for a material that declares nothing - a single
    // owner, so no two places can disagree about what a pipeline was built
    // against.
    [[nodiscard]] bool initialize(rhi::Context& context,
                                  VkFormat colorFormat,
                                  VkFormat depthFormat,
                                  VkDescriptorSetLayout textureSetLayout,
                                  std::uint32_t pushConstantSize,
                                  std::span<const std::string> shaderSearchPath);
    void shutdown();

    // Builds one pipeline per DISTINCT state across `materials`, checks each
    // material's declaration against its shader, and allocates the set 1
    // resources a declaration asks for. Replaces whatever was built before, so
    // a def hot-reload that adds a material is this call again.
    //
    // ⚑ A material whose SPIR-V will not load, or whose declaration does not
    // match it, does not fail the build - it gets no pipeline, and `pipeline()`
    // returns VK_NULL_HANDLE so the caller can mark the models using it
    // undrawable. That is Phase 24 stage S's rule arriving one asset kind over:
    // one broken mod must not stop the game booting. False is returned only
    // when NOTHING built, which is structural (no shader directory at all)
    // rather than one bad row.
    [[nodiscard]] bool build(std::span<const assets::MaterialDef> materials);

    // Writes a material's set 1: its declared textures, in declared order, plus
    // its params buffer. The caller uploads textures, so it hands them in here
    // once they exist.
    //
    // ⚑⚑ A MATERIAL THAT NEEDS THIS AND DOES NOT GET IT STAYS UNDRAWABLE
    // rather than reaching the device with an unwritten descriptor set. That is
    // deliberate: an unwritten set is undefined behaviour that a validation
    // layer catches in dev and nothing catches in shipping, which is the exact
    // failure this stage exists to remove - so forgetting the call has to look
    // like every other missing asset instead.
    [[nodiscard]] bool writeMaterialSet(std::uint32_t materialIndex,
                                        std::span<const rhi::MaterialTextureBinding> textures);

    // ⚑⚑ PHASE 25 STAGE D: NEW VALUES FOR THE PARAMS A MATERIAL ALREADY
    // DECLARES, WITHOUT TOUCHING A SINGLE PIPELINE. Until stage D the only ways
    // params reached the GPU were `build` and `reloadPipelines`, and both
    // rebuild every pipeline in the registry and require an idle device - which
    // is the right price for a def load and an absurd one for one frame of a
    // drag in an editor. A param is a float in a mapped uniform buffer; moving
    // it is a memcpy.
    //
    // ⚑ It refuses a name the material does not declare (see
    // `applyParamValues`), because adding one is a LAYOUT change: the block's
    // size comes from the shader, and a value with nowhere to go would either
    // be dropped silently or write past the buffer. False when this material
    // has no params at all, or when any name is undeclared - and nothing is
    // written in either case.
    [[nodiscard]] bool setParams(std::uint32_t materialIndex, std::span<const assets::MaterialParam> params);

    // Rebuilds every pipeline from the SPIR-V on disk, for the shader watcher's
    // F5. Caller must ensure the device is idle. ⚑ All or nothing: a reload
    // that built some pipelines and failed others would leave the frame drawing
    // a mix of two shader versions, which is worse than keeping the old ones.
    // A shader edited into disagreeing with a material's declaration fails the
    // reload with the slot named and keeps the previous frame's shaders.
    [[nodiscard]] bool reloadPipelines();

    // The pipeline for a material index, or VK_NULL_HANDLE when it has none -
    // no shader, a refused declaration, or a set 1 that was never written.
    [[nodiscard]] VkPipeline pipeline(std::uint32_t materialIndex) const;

    // The pipeline layout a material's pipeline was built against. Needed for
    // push constants and for binding, and it varies with the declaration.
    [[nodiscard]] VkPipelineLayout pipelineLayout(std::uint32_t materialIndex) const;

    // A material's set 1, or VK_NULL_HANDLE when it declares nothing.
    [[nodiscard]] VkDescriptorSet materialSet(std::uint32_t materialIndex) const;

    // How many materials there are against how many pipelines they needed -
    // the two numbers that say whether sharing is working. Logged at load.
    [[nodiscard]] std::size_t materialCount() const { return m_materialPipeline.size(); }

    [[nodiscard]] std::size_t pipelineCount() const { return m_pipelines.size(); }

    // Where a shader stem is looked for, for an error that names the places.
    [[nodiscard]] std::string describeSearchPath() const;

private:
    // ⚑ The SAME value as `material_state.hpp`'s, by construction rather than
    // by both files spelling `0xFFFFFFFFu`: `materialPipelineSlots` returns it
    // and every read below compares against it (Phase 25 stage E).
    static constexpr std::uint32_t kNoPipeline = kNoMaterialPipeline;
    static constexpr std::uint32_t kNoLayout = 0xFFFFFFFFu;

    // One pipeline layout per distinct DECLARATION SHAPE, which is all a layout
    // can vary on: how many textures and whether there is a params buffer.
    struct InterfaceLayout
    {
        std::uint32_t slotCount = 0;
        bool hasParams = false;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE; // null for the empty shape
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    };

    // What one material owns beyond its pipeline.
    struct MaterialResources
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        rhi::Buffer params;
        // ⚑ HOST-VISIBLE AND KEPT MAPPED, which decision 3's "written once at
        // load" does not forbid: it says WHEN, not WHERE. Staging it into
        // device-local memory would be the obvious reading and would make the
        // shader watcher's F5 wrong - a reloaded shader can move a param's
        // offset, and a buffer that cannot be rewritten would keep feeding the
        // old layout to the new code. This is eight bytes per material.
        void* mapped = nullptr;
        // False until `writeMaterialSet` has run, for a material that needs it.
        bool ready = true;
    };

    [[nodiscard]] std::string resolveShader(const std::string& stem, const char* stage) const;
    [[nodiscard]] bool createPipelines();
    void destroyPipelines();
    void destroyMaterialResources();
    [[nodiscard]] std::uint32_t interfaceLayout(std::uint32_t slotCount, bool hasParams);
    [[nodiscard]] bool allocateMaterialResources(std::span<const assets::MaterialDef> materials);
    // The declaration/SPIR-V check. False with `outError` set, naming the slot.
    [[nodiscard]] static bool checkDeclaration(const assets::MaterialDef& material,
                                               const ShaderInterface& fragment,
                                               const ShaderInterface& vertex,
                                               std::string* outError);
    // The params blob a material's UBO holds, packed at the shader's own
    // offsets. Empty when the material declares no params.
    [[nodiscard]] static std::vector<std::uint8_t> packParams(const assets::MaterialDef& material,
                                                              const ShaderInterface& fragment);
    // Every material drawn by state `stateIndex`, comma separated, for an error
    // that names rows an author can edit rather than an internal grouping.
    [[nodiscard]] std::string describeStateUsers(std::size_t stateIndex) const;

    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    std::uint32_t m_pushConstantSize = 0;
    std::vector<std::string> m_shaderSearchPath;

    // The distinct states, and the pipeline built for each. Parallel. The
    // grouping itself is `groupMaterialsByState`, which is pure and tested;
    // what lives here is only the Vulkan object it maps to.
    std::vector<MaterialPipelineState> m_states;
    std::vector<VkPipeline> m_pipelines;
    // What each state's fragment stage actually declares, kept so a material's
    // declaration can be checked against it and so a reload can re-check.
    std::vector<ShaderInterface> m_stateFragment;
    std::vector<ShaderInterface> m_stateVertex;
    // ⚑⚑ TWO ARRAYS, AND THE DIFFERENCE IS LOAD-BEARING. `m_materialState` is
    // the grouping's answer - which state a material asked for - and is never
    // mutated. `m_materialPipeline` is the same thing MINUS whatever was
    // refused, and refusals are what a caller reads to decide drawability.
    // Keeping the pristine mapping is what lets a shader reload RE-CHECK a
    // material that was refused last time: collapsing them would make a
    // refusal permanent until the next def load, so fixing the shader in the
    // dev loop would look like it had not worked.
    std::vector<std::uint32_t> m_materialState;
    std::vector<std::uint32_t> m_materialPipeline;
    // Materials the last `createPipelines` refused, so `build` can apply them
    // and `reloadPipelines` can treat any of them as a failed reload.
    std::vector<std::uint32_t> m_rejected;
    // Material index -> index into m_layouts, or kNoLayout.
    std::vector<std::uint32_t> m_materialLayout;
    std::vector<MaterialResources> m_materialResources;
    // ⚑ THE ROWS THEMSELVES, NOT JUST THEIR IDS. `reloadPipelines` has to
    // re-run the declaration check - a shader edited in the dev loop can stop
    // matching the material that names it, and that is precisely the moment a
    // named refusal is worth most - and it cannot do that without the
    // declarations. Eight rows; the copy is nothing.
    std::vector<assets::MaterialDef> m_materials;

    std::vector<InterfaceLayout> m_layouts;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
};

} // namespace sol::renderer
