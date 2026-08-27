#include "sol/renderer/material_registry.hpp"

#include "sol/assets/asset_loader.hpp"
#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/rhi/pipeline.hpp"

#include <cstddef>
#include <cstring>
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

// ⚑ SET 0 IS THE ENGINE'S, SET 1 IS THE MATERIAL'S. Spelled out rather than
// written as a bare 1 at four call sites, because the whole of stage C's
// descriptor story is this one number and a reader should be able to find every
// use of it.
constexpr std::uint32_t kMaterialSet = 1;

// Reads a whole .spv into words. ⚑ Bytes rather than words on disk, so the
// size check is what stands between a truncated file and a reinterpret_cast
// over the end of a buffer.
[[nodiscard]] bool readSpirv(const std::string& path, std::vector<std::uint32_t>& out)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path.c_str(), bytes) || bytes.empty() || bytes.size() % 4 != 0) {
        return false;
    }
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

// The bindings a module declares in the material's set, in binding order.
[[nodiscard]] std::vector<const ShaderBinding*> materialSetBindings(const ShaderInterface& interface)
{
    std::vector<const ShaderBinding*> bindings;
    for (const ShaderBinding& binding : interface.bindings) {
        if (binding.set == kMaterialSet) {
            bindings.push_back(&binding);
        }
    }
    return bindings; // reflectShader sorts by (set, binding), so this is in order
}

} // namespace

bool MaterialRegistry::initialize(rhi::Context& context,
                                  VkFormat colorFormat,
                                  VkFormat depthFormat,
                                  VkDescriptorSetLayout textureSetLayout,
                                  std::uint32_t pushConstantSize,
                                  std::span<const std::string> shaderSearchPath)
{
    m_context = &context;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    m_textureSetLayout = textureSetLayout;
    m_pushConstantSize = pushConstantSize;
    m_shaderSearchPath.assign(shaderSearchPath.begin(), shaderSearchPath.end());
    return m_textureSetLayout != VK_NULL_HANDLE && !m_shaderSearchPath.empty();
}

void MaterialRegistry::shutdown()
{
    destroyPipelines();
    destroyMaterialResources();
    if (m_context != nullptr) {
        const VkDevice device = m_context->device();
        for (InterfaceLayout& layout : m_layouts) {
            if (layout.pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, layout.pipelineLayout, nullptr);
            }
            if (layout.setLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, layout.setLayout, nullptr);
            }
        }
    }
    m_layouts.clear();
    m_states.clear();
    m_stateFragment.clear();
    m_stateVertex.clear();
    m_materialState.clear();
    m_materialPipeline.clear();
    m_materialLayout.clear();
    m_materials.clear();
    m_shaderSearchPath.clear();
    m_textureSetLayout = VK_NULL_HANDLE;
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

void MaterialRegistry::destroyMaterialResources()
{
    if (m_context == nullptr) {
        return;
    }
    for (MaterialResources& resources : m_materialResources) {
        if (resources.mapped != nullptr) {
            vkUnmapMemory(m_context->device(), resources.params.memory);
            resources.mapped = nullptr;
        }
        rhi::destroyBuffer(*m_context, resources.params);
        resources.set = VK_NULL_HANDLE; // pool-owned
    }
    m_materialResources.clear();
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context->device(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
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

std::string MaterialRegistry::describeStateUsers(std::size_t stateIndex) const
{
    std::string users;
    for (std::size_t m = 0; m < m_materialState.size(); ++m) {
        if (m_materialState[m] == stateIndex) {
            users += users.empty() ? "" : ", ";
            users += m_materials[m].id;
        }
    }
    return users;
}

std::uint32_t MaterialRegistry::interfaceLayout(std::uint32_t slotCount, bool hasParams)
{
    for (std::size_t i = 0; i < m_layouts.size(); ++i) {
        if (m_layouts[i].slotCount == slotCount && m_layouts[i].hasParams == hasParams) {
            return static_cast<std::uint32_t>(i);
        }
    }

    const VkDevice device = m_context->device();
    InterfaceLayout layout;
    layout.slotCount = slotCount;
    layout.hasParams = hasParams;

    // ⚑ THE EMPTY SHAPE GETS NO SET 1 AT ALL, not an empty one. An empty
    // descriptor set layout is legal Vulkan and would have been one less
    // branch, but it would also put a set in every pipeline layout in the game
    // to serve one material - and "a material that declares nothing costs
    // exactly what it cost before stage C" is a claim worth being able to make
    // by construction.
    VkDescriptorSetLayout setLayouts[2] = {m_textureSetLayout, VK_NULL_HANDLE};
    std::uint32_t setLayoutCount = 1;
    if (slotCount > 0 || hasParams) {
        layout.setLayout = rhi::createMaterialSetLayout(device, slotCount, hasParams);
        setLayouts[1] = layout.setLayout;
        setLayoutCount = 2;
    }
    layout.pipelineLayout = rhi::createPipelineLayout(device, setLayouts, setLayoutCount, m_pushConstantSize);

    m_layouts.push_back(layout);
    return static_cast<std::uint32_t>(m_layouts.size() - 1);
}

bool MaterialRegistry::checkDeclaration(const assets::MaterialDef& material,
                                        const ShaderInterface& fragment,
                                        const ShaderInterface& vertex,
                                        std::string* outError)
{
    const auto fail = [&](std::string message) {
        if (outError != nullptr) {
            *outError = std::move(message);
        }
        return false;
    };

    // ⚑ SET 1 IS FRAGMENT-STAGE IN THIS ENGINE, and a vertex shader reaching
    // for it is refused by name rather than left to the validation layer. The
    // layout this class builds carries VK_SHADER_STAGE_FRAGMENT_BIT only, so a
    // vertex stage binding there is a genuine mismatch and not a limitation
    // worth hiding.
    for (const ShaderBinding& binding : vertex.bindings) {
        if (binding.set == kMaterialSet) {
            return fail("its vertex shader declares '" + binding.name + "' at set 1 binding " +
                        std::to_string(binding.binding) +
                        ", but a material's set is fragment-stage in this engine");
        }
    }

    const std::vector<const ShaderBinding*> declared = materialSetBindings(fragment);
    const std::size_t slotCount = material.slots.size();
    const bool hasParams = !material.params.empty();
    const std::size_t expected = slotCount + (hasParams ? 1u : 0u);

    // The shape first, because a count mismatch explains every name mismatch
    // that would follow it and saying both is noise.
    if (declared.size() != expected) {
        std::string shaderSide;
        for (const ShaderBinding* binding : declared) {
            shaderSide += shaderSide.empty() ? "" : ", ";
            shaderSide += "binding " + std::to_string(binding->binding) + " '" + binding->name + "'";
        }
        if (shaderSide.empty()) {
            shaderSide = "nothing";
        }
        return fail("it declares " + std::to_string(slotCount) + " texture slot(s)" +
                    (hasParams ? " and params" : " and no params") + ", but '" + material.fragmentShader +
                    ".frag' declares " + shaderSide + " at set 1");
    }

    // Then the slots, in order, because order is what a binding number is.
    for (std::size_t i = 0; i < slotCount; ++i) {
        const ShaderBinding& binding = *declared[i];
        if (binding.binding != i) {
            return fail("texture slot '" + material.slots[i].name + "' is the material's binding " +
                        std::to_string(i) + ", but '" + material.fragmentShader + ".frag' has '" +
                        binding.name + "' at set 1 binding " + std::to_string(binding.binding) +
                        " - set 1 bindings must run from 0");
        }
        if (binding.kind != ShaderResourceKind::SampledImage) {
            return fail("texture slot '" + material.slots[i].name + "' is declared at set 1 binding " +
                        std::to_string(i) + ", but '" + material.fragmentShader + ".frag' has '" +
                        binding.name + "' there, which is not a sampler2D");
        }
    }
    if (!hasParams) {
        return true;
    }

    const ShaderBinding& paramsBinding = *declared[slotCount];
    if (paramsBinding.kind != ShaderResourceKind::UniformBuffer || paramsBinding.binding != slotCount) {
        return fail("it declares params, which are the material's set 1 binding " +
                    std::to_string(slotCount) + ", but '" + material.fragmentShader + ".frag' has '" +
                    paramsBinding.name + "' at binding " + std::to_string(paramsBinding.binding) +
                    ", which is not a uniform block there");
    }

    const ShaderBlock* block = findBlock(fragment, kMaterialSet, static_cast<std::uint32_t>(slotCount));
    if (block == nullptr) {
        return fail("its params block could not be read out of '" + material.fragmentShader + ".frag'");
    }
    if (!block->hasMemberNames) {
        // ⚑ Refused rather than matched by position. Position matching is
        // exactly the silent misalignment the whole check exists to prevent,
        // and it would fail invisibly the first time somebody reordered two
        // floats in a shader.
        return fail("'" + material.fragmentShader +
                    ".frag' was compiled without debug names, so its params cannot be matched by "
                    "name - recompile it without -g0 / --strip-debug");
    }

    for (const assets::MaterialParam& param : material.params) {
        const ShaderBlockMember* member = nullptr;
        for (const ShaderBlockMember& candidate : block->members) {
            if (candidate.name == param.name) {
                member = &candidate;
                break;
            }
        }
        if (member == nullptr) {
            return fail("parameter '" + param.name + "' is not in the params block of '" +
                        material.fragmentShader + ".frag'");
        }
        if (member->componentCount != 1) {
            return fail("parameter '" + param.name + "' is a " +
                        (member->componentCount == 0 ? std::string("type this engine cannot fill")
                                                     : "vec" + std::to_string(member->componentCount)) +
                        " in '" + material.fragmentShader +
                        ".frag' - only single floats can be set from a [material.params] row");
        }
    }
    // ⚑ AND THE OTHER DIRECTION, WHICH IS THE ONE THAT WOULD ROT SILENTLY. A
    // shader member nobody declared is not written, so it holds whatever the
    // buffer was allocated with - a value that changes with the weather and
    // that nobody can find in a file.
    for (const ShaderBlockMember& member : block->members) {
        bool declaredHere = false;
        for (const assets::MaterialParam& param : material.params) {
            declaredHere = declaredHere || param.name == member.name;
        }
        if (!declaredHere) {
            return fail("'" + material.fragmentShader + ".frag' wants parameter '" + member.name +
                        "', which this material does not declare");
        }
    }
    return true;
}

std::vector<std::uint8_t> MaterialRegistry::packParams(const assets::MaterialDef& material,
                                                       const ShaderInterface& fragment)
{
    const ShaderBlock* block =
        findBlock(fragment, kMaterialSet, static_cast<std::uint32_t>(material.slots.size()));
    if (block == nullptr || block->size == 0) {
        return {};
    }
    // ⚑ PACKED AT THE SHADER'S OWN OFFSETS, never at offsets computed here. The
    // std140 rules that decide where a float lands are the compiler's, and a
    // second implementation of them in this file would be right until the first
    // time a shader put a vec3 in front of the float.
    std::vector<std::uint8_t> blob(block->size, 0);
    for (const assets::MaterialParam& param : material.params) {
        for (const ShaderBlockMember& member : block->members) {
            if (member.name != param.name || member.componentCount != 1) {
                continue;
            }
            if (member.offset + sizeof(float) <= blob.size()) {
                std::memcpy(blob.data() + member.offset, &param.value, sizeof(float));
            }
            break;
        }
    }
    return blob;
}

bool MaterialRegistry::build(std::span<const assets::MaterialDef> materials)
{
    destroyPipelines();
    destroyMaterialResources();
    m_states.clear();
    m_stateFragment.clear();
    m_stateVertex.clear();
    m_materialPipeline.clear();
    m_materialLayout.clear();
    m_materials.clear();

    // ⚑ The sharing itself is pure and lives in `material_state.hpp`, where a
    // suite can assert it without a device. This function only turns the
    // answer into Vulkan objects.
    MaterialStateGrouping grouping = groupMaterialsByState(materials);
    m_states = std::move(grouping.states);
    m_materialState = std::move(grouping.materialState);
    m_materialPipeline = m_materialState;
    m_materials.assign(materials.begin(), materials.end());

    // The layout a material's pipeline is built against is a fact about its
    // declaration, so it is resolved before any pipeline exists.
    m_materialLayout.reserve(materials.size());
    for (const assets::MaterialDef& material : materials) {
        m_materialLayout.push_back(
            interfaceLayout(static_cast<std::uint32_t>(material.slots.size()), !material.params.empty()));
    }

    if (!createPipelines()) {
        return false;
    }
    for (const std::uint32_t rejected : m_rejected) {
        m_materialPipeline[rejected] = kNoPipeline;
    }

    if (!allocateMaterialResources(materials)) {
        return false;
    }

    // ⚑ Two numbers, because one of them alone says nothing. "8 materials"
    // does not tell you sharing works and "2 pipelines" does not tell you
    // anything at all; the pair is the readout for the whole stage.
    std::size_t usable = 0;
    for (const std::uint32_t slot : m_materialPipeline) {
        usable += (slot != kNoPipeline && m_pipelines[slot] != VK_NULL_HANDLE) ? 1u : 0u;
    }
    SOL_LOG_INFO("materials: %zu (%zu pipeline(s), %zu layout(s), %zu undrawable)",
                 m_materialPipeline.size(),
                 m_pipelines.size(),
                 m_layouts.size(),
                 m_materialPipeline.size() - usable);
    return true;
}

bool MaterialRegistry::allocateMaterialResources(std::span<const assets::MaterialDef> materials)
{
    m_materialResources.assign(materials.size(), {});

    std::uint32_t setCount = 0;
    std::uint32_t samplerCount = 0;
    std::uint32_t uniformCount = 0;
    for (std::size_t m = 0; m < materials.size(); ++m) {
        if (m_materialPipeline[m] == kNoPipeline) {
            continue; // no pipeline, so nothing will ever bind its set
        }
        const assets::MaterialDef& material = materials[m];
        if (material.slots.empty() && material.params.empty()) {
            continue;
        }
        ++setCount;
        samplerCount += static_cast<std::uint32_t>(material.slots.size());
        uniformCount += material.params.empty() ? 0u : 1u;
    }
    if (setCount == 0) {
        return true; // every material is the shape this engine had before stage C
    }

    const VkDevice device = m_context->device();
    m_descriptorPool = rhi::createMaterialDescriptorPool(device, setCount, samplerCount, uniformCount);
    if (m_descriptorPool == VK_NULL_HANDLE) {
        SOL_LOG_ERROR("materials: could not create the descriptor pool for %u material set(s)", setCount);
        return false;
    }

    for (std::size_t m = 0; m < materials.size(); ++m) {
        const assets::MaterialDef& material = materials[m];
        if (m_materialPipeline[m] == kNoPipeline || (material.slots.empty() && material.params.empty())) {
            continue;
        }
        MaterialResources& resources = m_materialResources[m];
        const InterfaceLayout& layout = m_layouts[m_materialLayout[m]];
        resources.set = rhi::allocateDescriptorSet(device, m_descriptorPool, layout.setLayout);

        if (!material.params.empty()) {
            const std::vector<std::uint8_t> blob = packParams(material, m_stateFragment[m_materialState[m]]);
            if (blob.empty()) {
                SOL_LOG_ERROR("material '%s': its params block reflected as empty", material.id.c_str());
                m_materialPipeline[m] = kNoPipeline;
                continue;
            }
            resources.params =
                rhi::createBuffer(*m_context,
                                  blob.size(),
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkMapMemory(device, resources.params.memory, 0, VK_WHOLE_SIZE, 0, &resources.mapped);
            std::memcpy(resources.mapped, blob.data(), blob.size());
        }
        // A material with params but no textures can be written now; one with
        // textures waits for the caller to upload them.
        if (material.slots.empty()) {
            rhi::writeMaterialDescriptors(
                device, resources.set, nullptr, 0, resources.params.buffer, resources.params.size);
        } else {
            resources.ready = false;
        }
    }
    return true;
}

bool MaterialRegistry::writeMaterialSet(std::uint32_t materialIndex,
                                        std::span<const rhi::MaterialTextureBinding> textures)
{
    if (materialIndex >= m_materialResources.size()) {
        return false;
    }
    MaterialResources& resources = m_materialResources[materialIndex];
    if (resources.set == VK_NULL_HANDLE) {
        return false; // this material declares nothing; there is no set to write
    }
    const InterfaceLayout& layout = m_layouts[m_materialLayout[materialIndex]];
    if (textures.size() != layout.slotCount) {
        SOL_LOG_ERROR("material '%s': %zu texture(s) offered for %u declared slot(s)",
                      m_materials[materialIndex].id.c_str(),
                      textures.size(),
                      layout.slotCount);
        return false;
    }
    rhi::writeMaterialDescriptors(m_context->device(),
                                  resources.set,
                                  textures.data(),
                                  static_cast<std::uint32_t>(textures.size()),
                                  resources.params.buffer,
                                  resources.params.size);
    resources.ready = true;
    return true;
}

bool MaterialRegistry::setParams(std::uint32_t materialIndex, std::span<const assets::MaterialParam> params)
{
    if (materialIndex >= m_materialResources.size()) {
        return false;
    }
    MaterialResources& resources = m_materialResources[materialIndex];
    if (resources.mapped == nullptr) {
        return false; // no params buffer: this material declares none
    }
    const std::uint32_t state = m_materialState[materialIndex];
    if (state >= m_stateFragment.size()) {
        return false;
    }
    // ⚑ THE STORED ROW IS UPDATED FIRST AND IS WHAT GETS PACKED, rather than
    // packing the caller's span directly. `reloadPipelines` re-packs from
    // `m_materials` after an F5, so a registry that wrote new bytes without
    // recording them would put the load-time values back the next time a shader
    // changed - a slider that quietly undid itself several seconds later.
    assets::MaterialDef& material = m_materials[materialIndex];
    if (!applyParamValues(material, params)) {
        return false;
    }
    const std::vector<std::uint8_t> blob = packParams(material, m_stateFragment[state]);
    // ⚑ The size CANNOT have changed - the declaration is untouched by the line
    // above, and the reflection is the one this buffer was allocated against -
    // so this is an assertion rather than a case. It is written as a refusal
    // because a memcpy past a mapped range is not a bug that reports itself.
    if (blob.size() != resources.params.size) {
        return false;
    }
    std::memcpy(resources.mapped, blob.data(), blob.size());
    return true;
}

bool MaterialRegistry::createPipelines()
{
    const VkDevice device = m_context->device();
    std::vector<VkPipeline> built(m_states.size(), VK_NULL_HANDLE);
    m_stateFragment.assign(m_states.size(), {});
    m_stateVertex.assign(m_states.size(), {});
    m_rejected.clear();

    for (std::size_t i = 0; i < m_states.size(); ++i) {
        const MaterialPipelineState& state = m_states[i];
        const std::string vertexPath = resolveShader(state.vertexShader, ".vert");
        const std::string fragmentPath = resolveShader(state.fragmentShader, ".frag");
        if (vertexPath.empty() || fragmentPath.empty()) {
            // ⚑ The MATERIALS are named, not the state index, because a state
            // is an internal grouping and a material id is what an author can
            // edit. Several materials can share one failure, and all of them
            // want to know.
            //
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
                          describeStateUsers(i).c_str(),
                          missing.c_str(),
                          describeSearchPath().c_str());
            continue;
        }

        // ⚑ ONE READ EACH, FEEDING BOTH THE REFLECTION AND THE MODULE. Reading
        // the file twice would let the thing checked and the thing bound differ
        // by whatever an author saved in between.
        std::vector<std::uint32_t> vertexWords;
        std::vector<std::uint32_t> fragmentWords;
        if (!readSpirv(vertexPath, vertexWords) || !readSpirv(fragmentPath, fragmentWords)) {
            SOL_LOG_ERROR("material(s) %s: could not read '%s' or '%s' - they will draw nothing",
                          describeStateUsers(i).c_str(),
                          vertexPath.c_str(),
                          fragmentPath.c_str());
            continue;
        }
        std::string reflectError;
        if (!reflectShader(vertexWords, m_stateVertex[i], &reflectError) ||
            !reflectShader(fragmentWords, m_stateFragment[i], &reflectError)) {
            SOL_LOG_ERROR("material(s) %s: %s - they will draw nothing",
                          describeStateUsers(i).c_str(),
                          reflectError.c_str());
            continue;
        }

        // ⚑⚑⚑ THE DECLARATION IS CHECKED BEFORE THE PIPELINE IS CREATED, AND
        // THE ORDER IS THE WHOLE POINT OF THE CHECK. A pipeline is created
        // against a LAYOUT built from the declaration; if the declaration
        // disagrees with the shader, creating it is itself the thing that
        // produces the validation-layer wall of text this stage exists to
        // replace. Checking afterwards would name the slot correctly and still
        // have emitted the wall - which is exactly what the first draft of this
        // function did, and the only reason it was caught is that a negative
        // control ran the game rather than reading the log.
        //
        // ⚑ PER MATERIAL, NOT PER STATE, EVEN THOUGH THE SHADER IS THE
        // STATE'S. Two materials can share a shader pair and a declaration
        // SHAPE - which is all the state key holds - and still name their
        // params differently, and only one of them can be right. So the shader
        // is reflected once here and checked once per material. If even one
        // passes, the SHAPE is right and the pipeline is safe to create.
        bool anyValid = false;
        for (std::size_t m = 0; m < m_materialState.size(); ++m) {
            if (m_materialState[m] != i) {
                continue;
            }
            std::string error;
            if (checkDeclaration(m_materials[m], m_stateFragment[i], m_stateVertex[i], &error)) {
                anyValid = true;
            } else {
                SOL_LOG_ERROR(
                    "material '%s': %s - it will draw nothing", m_materials[m].id.c_str(), error.c_str());
                m_rejected.push_back(static_cast<std::uint32_t>(m));
            }
        }
        if (!anyValid) {
            continue; // every user was refused above, each by name
        }

        VkShaderModule vertexShader = rhi::createShaderModule(device, vertexWords);
        VkShaderModule fragmentShader = rhi::createShaderModule(device, fragmentWords);
        if (vertexShader != VK_NULL_HANDLE && fragmentShader != VK_NULL_HANDLE) {
            // ⚑ Every material in a state shares its declaration SHAPE - that
            // is what the state key holds - so the first user's layout is every
            // user's layout.
            VkPipelineLayout layout = VK_NULL_HANDLE;
            for (std::size_t m = 0; m < m_materialState.size(); ++m) {
                if (m_materialState[m] == i) {
                    layout = m_layouts[m_materialLayout[m]].pipelineLayout;
                    break;
                }
            }

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
            desc.layout = layout;
            if (layout == VK_NULL_HANDLE || !rhi::createGraphicsPipeline(device, desc, built[i])) {
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
    std::vector<ShaderInterface> previousFragment = std::move(m_stateFragment);
    std::vector<ShaderInterface> previousVertex = std::move(m_stateVertex);
    m_pipelines.clear();
    const auto keepPrevious = [&] {
        destroyPipelines();
        m_pipelines = std::move(previous);
        m_stateFragment = std::move(previousFragment);
        m_stateVertex = std::move(previousVertex);
        return false;
    };
    if (!createPipelines()) {
        return keepPrevious();
    }

    // ⚑⚑ A RELOAD THAT REFUSES ANY MATERIAL IS A FAILED RELOAD, and this is the
    // half a reload path usually forgets. `reloadPipelines` exists for the dev
    // loop's F5, and the edit somebody is most likely to have just made is to a
    // shader's own samplers or params - exactly the edit that can leave a
    // shipped material's declaration describing a shader that no longer exists.
    // `createPipelines` already named each one; all this does is refuse to be
    // half-applied, like the rest of this function, because a frame drawing two
    // shader versions is worse than a frame drawing the old one.
    if (!m_rejected.empty()) {
        SOL_LOG_ERROR("materials: %zu material(s) no longer match their shaders - keeping the ones "
                      "that were already loaded",
                      m_rejected.size());
        return keepPrevious();
    }

    for (std::size_t m = 0; m < m_materials.size(); ++m) {
        const std::uint32_t state = m_materialState[m];
        if (state >= m_pipelines.size() || m_pipelines[state] == VK_NULL_HANDLE) {
            continue;
        }
        // A param's OFFSET can move without its name changing, so the buffer is
        // repacked from the new reflection rather than trusted to still match.
        MaterialResources& resources = m_materialResources[m];
        if (resources.mapped == nullptr) {
            continue;
        }
        const std::vector<std::uint8_t> blob = packParams(m_materials[m], m_stateFragment[state]);
        if (blob.size() != resources.params.size) {
            SOL_LOG_ERROR("material '%s': its params block changed size (%zu -> %zu bytes), which "
                          "needs a def reload rather than a shader one - keeping the shaders that "
                          "were already loaded",
                          m_materials[m].id.c_str(),
                          static_cast<std::size_t>(resources.params.size),
                          blob.size());
            return keepPrevious();
        }
        std::memcpy(resources.mapped, blob.data(), blob.size());
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
    // ⚑ A material whose set 1 was never written is not drawable, and this is
    // the line that makes forgetting `writeMaterialSet` look like every other
    // missing asset instead of like undefined behaviour on a player's machine.
    // An empty resource list means `build` bailed out before allocating any,
    // which is a failed load and equally undrawable.
    if (materialIndex >= m_materialResources.size() || !m_materialResources[materialIndex].ready) {
        return VK_NULL_HANDLE;
    }
    const std::uint32_t slot = m_materialPipeline[materialIndex];
    return slot < m_pipelines.size() ? m_pipelines[slot] : VK_NULL_HANDLE;
}

VkPipelineLayout MaterialRegistry::pipelineLayout(std::uint32_t materialIndex) const
{
    if (materialIndex >= m_materialLayout.size()) {
        return VK_NULL_HANDLE;
    }
    const std::uint32_t slot = m_materialLayout[materialIndex];
    return slot < m_layouts.size() ? m_layouts[slot].pipelineLayout : VK_NULL_HANDLE;
}

VkDescriptorSet MaterialRegistry::materialSet(std::uint32_t materialIndex) const
{
    return materialIndex < m_materialResources.size() ? m_materialResources[materialIndex].set
                                                      : VK_NULL_HANDLE;
}

} // namespace sol::renderer
