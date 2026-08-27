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
struct MaterialPipelineState
{
    std::string vertexShader;
    std::string fragmentShader;
    assets::MaterialBlend blend = assets::MaterialBlend::Opaque;
    bool depthTest = true;
    bool depthWrite = true;
    bool cullBackFaces = true;

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

[[nodiscard]] MaterialStateGrouping groupMaterialsByState(std::span<const assets::MaterialDef> materials);

} // namespace sol::renderer
