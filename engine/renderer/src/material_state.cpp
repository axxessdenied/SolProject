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

} // namespace sol::renderer
