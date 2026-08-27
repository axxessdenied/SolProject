#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/renderer/material_state.hpp>
#include <sol/test/test.hpp>

using sol::assets::MaterialBlend;
using sol::assets::MaterialDef;
using sol::renderer::groupMaterialsByState;
using sol::renderer::materialPipelineState;
using sol::renderer::MaterialStateGrouping;

namespace {

MaterialDef material(std::string id)
{
    MaterialDef def;
    def.id = std::move(id);
    def.texture = "hull";
    return def;
}

} // namespace

// ⚑⚑ THE CENTRAL CLAIM OF PHASE 25 STAGE B, AND UNTIL THIS SUITE EXISTED THE
// ONLY THING ASSERTING IT WAS A LOG LINE READ BY EYE: two materials that ask
// for the same pipeline get the same pipeline. It matters because a material
// is meant to be cheap - the whole design says "add a material" rather than
// "add a pipeline" - and because the alternative failure is silent: one
// VkPipeline per material still draws correctly, it just costs.
SOL_TEST(materialStateSharesOnePipelineAcrossMaterialsThatAgree)
{
    std::vector<MaterialDef> materials;
    // Four rows differing only in what is PER-DRAW rather than per-pipeline.
    materials.push_back(material("a"));
    materials.push_back(material("b"));
    materials[1].texture = "checker";
    materials.push_back(material("c"));
    materials[2].emissive = 0.5f;
    materials.push_back(material("d"));
    materials[3].alpha = 0.25f;

    const MaterialStateGrouping grouping = groupMaterialsByState(materials);
    SOL_REQUIRE(grouping.materialState.size() == 4);
    SOL_CHECK(grouping.states.size() == 1);
    for (const std::uint32_t slot : grouping.materialState) {
        SOL_CHECK(slot == 0);
    }
}

// The other half: every field a pipeline is actually built from splits it.
SOL_TEST(materialStateSplitsOnEveryPipelineField)
{
    const MaterialDef base = material("base");

    const auto splits = [&](const MaterialDef& other) {
        const MaterialDef set[] = {base, other};
        const MaterialStateGrouping grouping = groupMaterialsByState(set);
        return grouping.states.size() == 2 && grouping.materialState[0] == 0 &&
               grouping.materialState[1] == 1;
    };

    MaterialDef vertexShader = base;
    vertexShader.vertexShader = "billboard";
    SOL_CHECK(splits(vertexShader));

    MaterialDef fragmentShader = base;
    fragmentShader.fragmentShader = "membrane";
    SOL_CHECK(splits(fragmentShader));

    MaterialDef blend = base;
    blend.blend = MaterialBlend::Additive;
    SOL_CHECK(splits(blend));

    MaterialDef depthTest = base;
    depthTest.depthTest = false;
    SOL_CHECK(splits(depthTest));

    MaterialDef depthWrite = base;
    depthWrite.depthWrite = false;
    SOL_CHECK(splits(depthWrite));

    MaterialDef cull = base;
    cull.cullBackFaces = false;
    SOL_CHECK(splits(cull));

    // ⚑ And the ones that must NOT split it, asserted rather than assumed:
    // these three are per-draw, and treating one as pipeline state would cost
    // a pipeline per texture in the game.
    MaterialDef texture = base;
    texture.texture = "checker";
    SOL_CHECK(!splits(texture));

    MaterialDef emissive = base;
    emissive.emissive = 1.0f;
    SOL_CHECK(!splits(emissive));

    MaterialDef alpha = base;
    alpha.alpha = 0.5f;
    SOL_CHECK(!splits(alpha));
}

// ⚑ FIRST-USE ORDER, not sorted: a material added later must not renumber the
// slots the ones before it resolved to. Same promise `DefDatabase` makes about
// def order, and for the same reason - an index is held, not re-looked-up.
SOL_TEST(materialStateKeepsFirstUseOrder)
{
    std::vector<MaterialDef> materials;
    materials.push_back(material("opaque_one"));
    materials.push_back(material("film"));
    materials[1].blend = MaterialBlend::Alpha;
    materials[1].depthWrite = false;
    materials.push_back(material("opaque_two"));

    MaterialStateGrouping grouping = groupMaterialsByState(materials);
    SOL_REQUIRE(grouping.states.size() == 2);
    SOL_CHECK(grouping.materialState[0] == 0);
    SOL_CHECK(grouping.materialState[1] == 1);
    SOL_CHECK(grouping.materialState[2] == 0); // rejoins the first, not a third

    // Appending leaves the earlier answers alone.
    materials.push_back(material("opaque_three"));
    const MaterialStateGrouping again = groupMaterialsByState(materials);
    SOL_REQUIRE(again.states.size() == 2);
    for (std::size_t i = 0; i < grouping.materialState.size(); ++i) {
        SOL_CHECK(again.materialState[i] == grouping.materialState[i]);
    }
    SOL_CHECK(again.materialState[3] == 0);
}

// ⚑⚑ THE SHIPPED SET, AND THE NUMBER THIS STAGE IS MEASURED BY. The seven
// derived materials all ask for the stock lambert pair under Phase 12's opaque
// state; the membrane brings its own fragment stage and blends. Eight rows,
// two pipelines - and a regression here is a pipeline count that grows with
// the catalog, which nothing else would notice.
SOL_TEST(materialStateShippedCatalogNeedsTwoPipelines)
{
    sol::assets::DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_REQUIRE(db.validateMaterials(&error));

    const MaterialStateGrouping grouping = groupMaterialsByState(db.materials());
    SOL_CHECK(db.materials().size() == 8);
    SOL_CHECK(grouping.states.size() == 2);

    // The membrane is the one that is on its own, and it is on its own because
    // of its SHADER as much as its blend.
    const std::uint32_t membrane = db.materialIndex("sol.gate_membrane");
    SOL_REQUIRE(membrane != sol::assets::kNoMaterial);
    const std::uint32_t hull = db.materialIndex("sol.auto.ship");
    SOL_REQUIRE(hull != sol::assets::kNoMaterial);
    SOL_CHECK(grouping.materialState[membrane] != grouping.materialState[hull]);
    SOL_CHECK(grouping.states[grouping.materialState[hull]] ==
              materialPipelineState(db.materials()[db.materialIndex("sol.auto.station")]));
}
