#pragma once

// The pipelines the `[[material]]` rows ask for (engine plan Phase 25 stage B).
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
// `MeshVertex` and the layout is `MeshRenderer`'s - one combined-image-sampler
// at set 0 plus the 128-byte push block. Phase 25 decision 4 keeps vertex
// formats out of this phase, and finding 2 measured that the engine has
// exactly one descriptor set layout shape, so there is nothing here to vary
// yet. Stage C is what gives a material its own slots and parameters.

#include "sol/assets/data_defs.hpp"
#include "sol/renderer/material_state.hpp"
#include "sol/rhi/context.hpp"

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
    [[nodiscard]] bool initialize(rhi::Context& context,
                                  VkFormat colorFormat,
                                  VkFormat depthFormat,
                                  VkPipelineLayout layout,
                                  std::span<const std::string> shaderSearchPath);
    void shutdown();

    // Builds one pipeline per DISTINCT state across `materials` and remembers
    // which material got which. Replaces whatever was built before, so a def
    // hot-reload that adds a material is this call again.
    //
    // ⚑ A material whose SPIR-V will not load does not fail the build - it
    // gets no pipeline, and `pipeline()` returns VK_NULL_HANDLE so the caller
    // can mark the models using it undrawable. That is Phase 24 stage S's rule
    // arriving one asset kind over: one broken mod must not stop the game
    // booting. False is returned only when NOTHING built, which is structural
    // (no shader directory at all) rather than one bad row.
    [[nodiscard]] bool build(std::span<const assets::MaterialDef> materials);

    // Rebuilds every pipeline from the SPIR-V on disk, for the shader watcher's
    // F5. Caller must ensure the device is idle. ⚑ All or nothing: a reload
    // that built some pipelines and failed others would leave the frame drawing
    // a mix of two shader versions, which is worse than keeping the old ones.
    [[nodiscard]] bool reloadPipelines();

    // The pipeline for a material index, or VK_NULL_HANDLE when it has none.
    [[nodiscard]] VkPipeline pipeline(std::uint32_t materialIndex) const;

    // How many materials there are against how many pipelines they needed -
    // the two numbers that say whether sharing is working. Logged at load.
    [[nodiscard]] std::size_t materialCount() const { return m_materialPipeline.size(); }

    [[nodiscard]] std::size_t pipelineCount() const { return m_pipelines.size(); }

    // Where a shader stem is looked for, for an error that names the places.
    [[nodiscard]] std::string describeSearchPath() const;

private:
    [[nodiscard]] std::string resolveShader(const std::string& stem, const char* stage) const;
    [[nodiscard]] bool createPipelines();
    void destroyPipelines();

    rhi::Context* m_context = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    std::vector<std::string> m_shaderSearchPath;

    // The distinct states, and the pipeline built for each. Parallel. The
    // grouping itself is `groupMaterialsByState`, which is pure and tested;
    // what lives here is only the Vulkan object it maps to.
    std::vector<MaterialPipelineState> m_states;
    std::vector<VkPipeline> m_pipelines;
    // Material index -> index into m_pipelines, or kNoPipeline.
    static constexpr std::uint32_t kNoPipeline = 0xFFFFFFFFu;
    std::vector<std::uint32_t> m_materialPipeline;
    // Kept so a reload can name the material behind a failed shader load.
    std::vector<std::string> m_materialIds;
};

} // namespace sol::renderer
