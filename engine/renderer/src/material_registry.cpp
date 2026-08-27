#include "sol/renderer/material_registry.hpp"

#include "sol/assets/asset_loader.hpp"
#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/rhi/pipeline.hpp"

#include <cstddef>
#include <utility>

namespace sol::renderer {

namespace {

// ⚑ The one place `assets::MaterialBlend` becomes `rhi::BlendMode`. The def
// layer deliberately mirrors the enum rather than including Vulkan headers, so
// this switch is the seam - and it is a switch with no default so that adding
// a blend mode to either side is a compile error rather than a silent Opaque.
[[nodiscard]] rhi::BlendMode toRhiBlend(assets::MaterialBlend blend)
{
    switch (blend) {
    case assets::MaterialBlend::Opaque:
        return rhi::BlendMode::Opaque;
    case assets::MaterialBlend::Alpha:
        return rhi::BlendMode::Alpha;
    case assets::MaterialBlend::Additive:
        return rhi::BlendMode::Additive;
    }
    return rhi::BlendMode::Opaque;
}

// The mesh vertex layout, unchanged since Phase 3 and deliberately not
// varied here (Phase 25 decision 4 prices tangents as a format version bump
// plus a full re-cook and keeps them out of this phase).
constexpr rhi::VertexAttribute kMeshAttributes[] = {
    {0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(assets::MeshVertex, position)},
    {1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(assets::MeshVertex, normal)},
    {2, VK_FORMAT_R32G32_SFLOAT, offsetof(assets::MeshVertex, uv)},
};

} // namespace

bool MaterialRegistry::initialize(rhi::Context& context,
                                  VkFormat colorFormat,
                                  VkFormat depthFormat,
                                  VkPipelineLayout layout,
                                  std::span<const std::string> shaderSearchPath)
{
    m_context = &context;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_layout = layout;
    m_shaderSearchPath.assign(shaderSearchPath.begin(), shaderSearchPath.end());
    return m_layout != VK_NULL_HANDLE && !m_shaderSearchPath.empty();
}

void MaterialRegistry::shutdown()
{
    destroyPipelines();
    m_states.clear();
    m_materialPipeline.clear();
    m_materialIds.clear();
    m_shaderSearchPath.clear();
    m_layout = VK_NULL_HANDLE;
    m_context = nullptr;
}

void MaterialRegistry::destroyPipelines()
{
    if (m_context == nullptr) {
        return;
    }
    for (VkPipeline pipeline : m_pipelines) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_context->device(), pipeline, nullptr);
        }
    }
    m_pipelines.clear();
}

std::string MaterialRegistry::resolveShader(const std::string& stem, const char* stage) const
{
    // ⚑ An existence PROBE rather than a load attempt, and this is not
    // fussiness: `rhi::createShaderModuleFromFile` logs an error for a file it
    // cannot read, which is right when somebody named that file and wrong for
    // every directory a search walks past on the way to the right one.
    const std::string name = stem + stage + ".spv";
    for (const std::string& directory : m_shaderSearchPath) {
        const std::string candidate = directory + name;
        if (platform::fileModificationTime(candidate.c_str()) != 0) {
            return candidate;
        }
    }
    return {};
}

std::string MaterialRegistry::describeSearchPath() const
{
    std::string description;
    for (const std::string& directory : m_shaderSearchPath) {
        if (!description.empty()) {
            description += ", ";
        }
        description += directory;
    }
    return description.empty() ? std::string("(no shader directories)") : description;
}

bool MaterialRegistry::build(std::span<const assets::MaterialDef> materials)
{
    destroyPipelines();
    m_states.clear();
    m_materialPipeline.clear();
    m_materialIds.clear();

    // ⚑ The sharing itself is pure and lives in `material_state.hpp`, where a
    // suite can assert it without a device. This function only turns the
    // answer into Vulkan objects.
    MaterialStateGrouping grouping = groupMaterialsByState(materials);
    m_states = std::move(grouping.states);
    m_materialPipeline = std::move(grouping.materialState);
    m_materialIds.reserve(materials.size());
    for (const assets::MaterialDef& material : materials) {
        m_materialIds.push_back(material.id);
    }

    if (!createPipelines()) {
        return false;
    }

    // ⚑ Two numbers, because one of them alone says nothing. "8 materials"
    // does not tell you sharing works and "2 pipelines" does not tell you
    // anything at all; the pair is the readout for the whole stage.
    std::size_t usable = 0;
    for (const std::uint32_t slot : m_materialPipeline) {
        usable += (slot != kNoPipeline && m_pipelines[slot] != VK_NULL_HANDLE) ? 1u : 0u;
    }
    SOL_LOG_INFO("materials: %zu (%zu pipeline(s), %zu undrawable)",
                 m_materialPipeline.size(),
                 m_pipelines.size(),
                 m_materialPipeline.size() - usable);
    return true;
}

bool MaterialRegistry::createPipelines()
{
    const VkDevice device = m_context->device();
    std::vector<VkPipeline> built(m_states.size(), VK_NULL_HANDLE);

    for (std::size_t i = 0; i < m_states.size(); ++i) {
        const MaterialPipelineState& state = m_states[i];
        const std::string vertexPath = resolveShader(state.vertexShader, ".vert");
        const std::string fragmentPath = resolveShader(state.fragmentShader, ".frag");
        if (vertexPath.empty() || fragmentPath.empty()) {
            // ⚑ The MATERIALS are named, not the state index, because a state
            // is an internal grouping and a material id is what an author can
            // edit. Several materials can share one failure, and all of them
            // want to know.
            std::string users;
            for (std::size_t m = 0; m < m_materialPipeline.size(); ++m) {
                if (m_materialPipeline[m] == i) {
                    users += users.empty() ? "" : ", ";
                    users += m_materialIds[m];
                }
            }
            // ⚑ Only the stage that is actually MISSING is named. Printing
            // both sends the reader to check a file that is sitting right
            // there - and with two shader keys the common case is exactly one
            // of them being wrong.
            std::string missing;
            if (vertexPath.empty()) {
                missing = "'" + state.vertexShader + ".vert.spv'";
            }
            if (fragmentPath.empty()) {
                missing += missing.empty() ? "" : " and ";
                missing += "'" + state.fragmentShader + ".frag.spv'";
            }
            SOL_LOG_ERROR("material(s) %s: cannot find shader %s - they will draw nothing. "
                          "Looked in: %s",
                          users.c_str(),
                          missing.c_str(),
                          describeSearchPath().c_str());
            continue;
        }

        VkShaderModule vertexShader = rhi::createShaderModuleFromFile(device, vertexPath.c_str());
        VkShaderModule fragmentShader = rhi::createShaderModuleFromFile(device, fragmentPath.c_str());
        if (vertexShader != VK_NULL_HANDLE && fragmentShader != VK_NULL_HANDLE) {
            rhi::GraphicsPipelineDesc desc = {};
            desc.vertexShader = vertexShader;
            desc.fragmentShader = fragmentShader;
            desc.colorFormat = m_colorFormat;
            desc.blendMode = toRhiBlend(state.blend);
            desc.vertexStride = sizeof(assets::MeshVertex);
            desc.attributes = kMeshAttributes;
            desc.attributeCount = 3;
            desc.depthFormat = m_depthFormat;
            desc.depthTest = state.depthTest;
            desc.depthWrite = state.depthWrite;
            desc.cullBackFaces = state.cullBackFaces;
            desc.frontFaceCounterClockwise = true;
            desc.layout = m_layout;
            if (!rhi::createGraphicsPipeline(device, desc, built[i])) {
                built[i] = VK_NULL_HANDLE;
            }
        }
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, vertexShader, nullptr);
        }
        if (fragmentShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, fragmentShader, nullptr);
        }
    }

    // ⚑⚑ NOTHING BUILT IS STRUCTURAL, ONE THING FAILING IS NOT. Phase 24 stage
    // S drew exactly this line for cooked assets: a mod naming a missing mesh
    // leaves that row undrawable, but an empty BASE cooked directory still
    // refuses to start, because an install that unpacked without its files must
    // not boot into an invisible galaxy and blame the player's mods.
    bool any = false;
    for (const VkPipeline pipeline : built) {
        any = any || pipeline != VK_NULL_HANDLE;
    }
    if (!m_states.empty() && !any) {
        SOL_LOG_ERROR("materials: not one pipeline could be built - no shaders were found at all. "
                      "Looked in: %s",
                      describeSearchPath().c_str());
        return false; // nothing was built, so there is nothing to destroy
    }

    m_pipelines = std::move(built);
    return true;
}

bool MaterialRegistry::reloadPipelines()
{
    std::vector<VkPipeline> previous = std::move(m_pipelines);
    m_pipelines.clear();
    if (!createPipelines()) {
        m_pipelines = std::move(previous);
        return false;
    }
    // The new set is in place; the old one goes now rather than before, so a
    // failed reload above leaves the frame drawing what it was already drawing.
    for (VkPipeline pipeline : previous) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_context->device(), pipeline, nullptr);
        }
    }
    return true;
}

VkPipeline MaterialRegistry::pipeline(std::uint32_t materialIndex) const
{
    if (materialIndex >= m_materialPipeline.size()) {
        return VK_NULL_HANDLE;
    }
    const std::uint32_t slot = m_materialPipeline[materialIndex];
    return slot < m_pipelines.size() ? m_pipelines[slot] : VK_NULL_HANDLE;
}

} // namespace sol::renderer
