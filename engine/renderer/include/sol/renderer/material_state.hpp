#pragma once

// What a `[[material]]` row asks a pipeline to BE, and which rows can share one
// (engine plan Phase 25 stage B).
//
// ⚑⚑ SPLIT OUT OF `material_registry.hpp` FOR ONE REASON: THIS HEADER PULLS NO
// VULKAN. The sharing rule is the central claim of the stage - eight materials
// must produce two pipelines, and two rows that say the same thing must land on
// the same one - and until it lived here the only thing asserting it was a log
// line somebody read by eye. `outputs.hpp` and `asset_paths.hpp` made the same
// move for the cooker and for asset search: the DECISIONS go somewhere a test
// can reach, and the thing that touches a device keeps only the doing.

#include "sol/assets/data_defs.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sol::renderer {

// ⚑ THE FIELDS HERE ARE EXACTLY THE ONES A PIPELINE IS BUILT FROM, AND NOTHING
// ELSE. `texture`, `emissive` and `alpha` are deliberately absent: they are
// per-DRAW, so two materials differing only in texture share a pipeline and
// cost nothing extra. Adding a field to `GraphicsPipelineDesc` that a material
// can vary means adding it here too, or the cache will hand back a pipeline
// that is wrong in the new dimension.
//
// ⚑⚑ WHICH IS EXACTLY WHY STAGE C HAD TO ADD TWO. A material's declared slots
// and params decide its set 1 LAYOUT, the layout is part of the pipeline
// layout, and the pipeline layout is a field of `GraphicsPipelineDesc` - so two
// materials sharing a shader pair and a blend mode but declaring different
// interfaces must NOT share a pipeline. Only the SHAPE matters, never the names
// or the values: `slotCount` textures, then one uniform buffer after them if
// `hasParams`. Two materials feeding the same shader different textures and
// different numbers still share one pipeline, which is the whole point.
struct MaterialPipelineState
{
    std::string vertexShader;
    std::string fragmentShader;
    assets::MaterialBlend blend = assets::MaterialBlend::Opaque;
    bool depthTest = true;
    bool depthWrite = true;
    bool cullBackFaces = true;
    std::uint32_t slotCount = 0;
    bool hasParams = false;

    [[nodiscard]] bool operator==(const MaterialPipelineState& other) const;

    [[nodiscard]] bool operator!=(const MaterialPipelineState& other) const { return !(*this == other); }
};

// The state a single material asks for.
[[nodiscard]] MaterialPipelineState materialPipelineState(const assets::MaterialDef& material);

// The distinct states across a material set, in FIRST-USE order, plus the index
// into that list for each material. First-use order rather than sorted so the
// mapping is stable against an unrelated material being added later, which is
// the same promise `DefDatabase` makes about def order.
struct MaterialStateGrouping
{
    std::vector<MaterialPipelineState> states;
    // Parallel to the material set handed in.
    std::vector<std::uint32_t> materialState;
};

// ⚑⚑ PHASE 25 STAGE D: NEW VALUES FOR PARAMS THAT ARE ALREADY DECLARED, WHICH
// IS THE ONLY KIND OF PARAM EDIT THAT IS NOT ALSO A LAYOUT CHANGE. A slider in
// an editor moves a number; it does not add a member to the shader's uniform
// block, and it must not be able to. So this refuses a name the material does
// not already declare rather than appending it - an undeclared param is a
// DECLARATION change, and a declaration change has to go back through `build`
// and the SPIR-V check that lives there.
//
// ⚑ BY NAME, NEVER BY POSITION, for the reason `spirv_reflect.hpp` gives: the
// values arriving here come from a def document whose keys an author reorders
// freely, and position matching is the silent misalignment stage C exists to
// avoid. ⚑ ALL OR NOTHING: false leaves `material` exactly as it was, so a
// caller cannot half-apply a set of values and then report a failure.
[[nodiscard]] bool applyParamValues(assets::MaterialDef& material,
                                    std::span<const assets::MaterialParam> values);

[[nodiscard]] MaterialStateGrouping groupMaterialsByState(std::span<const assets::MaterialDef> materials);

} // namespace sol::renderer
