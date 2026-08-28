#include "sol/renderer/material_state.hpp"

#include <cstddef>

namespace sol::renderer {

bool MaterialPipelineState::operator==(const MaterialPipelineState& other) const
{
    return vertexShader == other.vertexShader && fragmentShader == other.fragmentShader &&
           blend == other.blend && depthTest == other.depthTest && depthWrite == other.depthWrite &&
           cullBackFaces == other.cullBackFaces && slotCount == other.slotCount &&
           hasParams == other.hasParams;
}

MaterialPipelineState materialPipelineState(const assets::MaterialDef& material)
{
    return MaterialPipelineState{.vertexShader = material.vertexShader,
                                 .fragmentShader = material.fragmentShader,
                                 .blend = material.blend,
                                 .depthTest = material.depthTest,
                                 .depthWrite = material.depthWrite,
                                 .cullBackFaces = material.cullBackFaces,
                                 .slotCount = static_cast<std::uint32_t>(material.slots.size()),
                                 .hasParams = !material.params.empty()};
}

bool applyParamValues(assets::MaterialDef& material, std::span<const assets::MaterialParam> values)
{
    // ⚑ CHECKED IN FULL BEFORE ANYTHING IS WRITTEN. Applying as we go and
    // bailing on the first bad name would leave the material carrying half of
    // an edit that was reported as refused, which is the state a caller has no
    // way to reason about.
    for (const assets::MaterialParam& value : values) {
        bool declared = false;
        for (const assets::MaterialParam& param : material.params) {
            if (param.name == value.name) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            return false;
        }
    }
    for (const assets::MaterialParam& value : values) {
        for (assets::MaterialParam& param : material.params) {
            if (param.name == value.name) {
                param.value = value.value;
                break;
            }
        }
    }
    return true;
}

MaterialStateGrouping groupMaterialsByState(std::span<const assets::MaterialDef> materials)
{
    MaterialStateGrouping grouping;
    grouping.materialState.reserve(materials.size());

    for (const assets::MaterialDef& material : materials) {
        const MaterialPipelineState state = materialPipelineState(material);

        // ⚑⚑ A LINEAR SCAN ON PURPOSE. The distinct states in a material set
        // are a handful - the eight shipped materials produce two - so a hash
        // of six fields would cost more to write and to read than it saves,
        // and this runs once per def load rather than once per frame. If a set
        // ever reaches hundreds this is the line to change, and the shape of
        // the change is obvious from here.
        std::uint32_t slot = static_cast<std::uint32_t>(grouping.states.size());
        for (std::size_t i = 0; i < grouping.states.size(); ++i) {
            if (grouping.states[i] == state) {
                slot = static_cast<std::uint32_t>(i);
                break;
            }
        }
        if (slot == grouping.states.size()) {
            grouping.states.push_back(state);
        }
        grouping.materialState.push_back(slot);
    }
    return grouping;
}

std::vector<std::uint32_t> materialPipelineSlots(std::span<const std::uint32_t> materialState,
                                                 std::span<const std::uint32_t> unbuiltStates,
                                                 std::span<const std::uint32_t> rejected)
{
    std::vector<std::uint32_t> slots(materialState.begin(), materialState.end());
    for (const std::uint32_t material : rejected) {
        if (material < slots.size()) {
            slots[material] = kNoMaterialPipeline;
        }
    }
    for (std::uint32_t& slot : slots) {
        if (slot == kNoMaterialPipeline) {
            continue;
        }
        for (const std::uint32_t unbuilt : unbuiltStates) {
            if (slot == unbuilt) {
                slot = kNoMaterialPipeline;
                break;
            }
        }
    }
    return slots;
}

} // namespace sol::renderer
