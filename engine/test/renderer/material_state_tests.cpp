#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/renderer/material_state.hpp>
#include <sol/test/test.hpp>

using sol::assets::MaterialBlend;
using sol::assets::MaterialDef;
using sol::renderer::applyParamValues;
using sol::renderer::groupMaterialsByState;
using sol::renderer::kNoMaterialPipeline;
using sol::renderer::materialPipelineSlots;
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

// ⚑⚑ THE SHIPPED SET, AND THE NUMBER THIS PHASE IS MEASURED BY. The five
// derived materials all ask for the stock lambert pair under Phase 12's opaque
// state; the membrane brings its own fragment stage and blends; the cockpit
// brings its own fragment stage and its own declared interface. Seven rows,
// three pipelines - and a regression here is a pipeline count that grows with
// the catalog, which nothing else would notice.
//
// ⚑ THESE NUMBERS ARE SUPPOSED TO MOVE WHEN THE CATALOG DOES. Stage B asserted
// eight rows and two states; stage C made the two cockpits share one material
// and gave it a third state, so this now reads seven and three. A failure here
// is a question - "did the catalog change, or did sharing break?" - and the
// checks below are what tell the two apart.
SOL_TEST(materialStateShippedCatalogNeedsThreePipelines)
{
    sol::assets::DefDatabase db;
    std::string error;
    SOL_REQUIRE(db.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_REQUIRE(db.validateMaterials(&error));

    const MaterialStateGrouping grouping = groupMaterialsByState(db.materials());
    SOL_CHECK(db.materials().size() == 7);
    SOL_CHECK(grouping.states.size() == 3);

    // The membrane is on its own because of its SHADER as much as its blend.
    const std::uint32_t membrane = db.materialIndex("sol.gate_membrane");
    SOL_REQUIRE(membrane != sol::assets::kNoMaterial);
    const std::uint32_t hull = db.materialIndex("sol.auto.ship");
    SOL_REQUIRE(hull != sol::assets::kNoMaterial);
    SOL_CHECK(grouping.materialState[membrane] != grouping.materialState[hull]);
    SOL_CHECK(grouping.states[grouping.materialState[hull]] ==
              materialPipelineState(db.materials()[db.materialIndex("sol.auto.station")]));

    // ⚑ THE COCKPIT IS THE FIRST MATERIAL TWO MODELS WEAR, which is why the
    // model count and the material count finally disagree. Asserted here
    // because it is the thing that makes seven rather than eight, and a future
    // edit that gave the freighter its own material would otherwise put the
    // count back to eight and look like this test passing again.
    const std::uint32_t cockpit = db.materialIndex("sol.cockpit");
    SOL_REQUIRE(cockpit != sol::assets::kNoMaterial);
    std::size_t wearers = 0;
    for (const sol::assets::ModelDef& model : db.models()) {
        wearers += model.materialIndex == cockpit ? 1u : 0u;
    }
    SOL_CHECK(wearers == 2);

    // ⚑ AND ITS STATE IS ITS OWN BECAUSE OF ITS DECLARED INTERFACE, not only
    // its shader. A material declaring a slot or a param needs a set 1, and a
    // pipeline is built against a layout - so sharing a pipeline across
    // different interfaces would bind the wrong one.
    SOL_CHECK(grouping.materialState[cockpit] != grouping.materialState[hull]);
    SOL_CHECK(grouping.materialState[cockpit] != grouping.materialState[membrane]);
    SOL_CHECK(grouping.states[grouping.materialState[cockpit]].slotCount == 1);
    SOL_CHECK(grouping.states[grouping.materialState[cockpit]].hasParams);
    SOL_CHECK(grouping.states[grouping.materialState[membrane]].slotCount == 0);
    SOL_CHECK(!grouping.states[grouping.materialState[membrane]].hasParams);
}

// ⚑ THE INTERFACE IS PART OF THE PIPELINE KEY, AND THIS IS THE TEST THAT SAYS
// SO. Two materials identical in every field the state carried before stage C
// but differing in what they DECLARE must not share a pipeline: the layout is
// built from the declaration, and a pipeline built against one layout bound
// with another is undefined behaviour that no log line would mention.
SOL_TEST(materialStateInterfaceSplitsAnOtherwiseIdenticalPair)
{
    sol::assets::MaterialDef plain = material("plain");
    sol::assets::MaterialDef slotted = material("slotted");
    slotted.slots.push_back({.name = "glow", .texture = "cockpit_glow"});
    sol::assets::MaterialDef tuned = material("tuned");
    tuned.params.push_back({.name = "gain", .value = 1.0f});

    const std::vector<sol::assets::MaterialDef> materials = {plain, slotted, tuned};
    const MaterialStateGrouping grouping = groupMaterialsByState(materials);
    SOL_CHECK(grouping.states.size() == 3);
    SOL_CHECK(grouping.materialState[0] != grouping.materialState[1]);
    SOL_CHECK(grouping.materialState[0] != grouping.materialState[2]);
    SOL_CHECK(grouping.materialState[1] != grouping.materialState[2]);

    // ⚑ But the VALUES inside the declaration are not part of the key: two
    // materials feeding the same shader different textures and different
    // numbers still share one pipeline, which is the entire point of caching on
    // state rather than on identity.
    sol::assets::MaterialDef otherSlot = material("other_slot");
    otherSlot.slots.push_back({.name = "glow", .texture = "checker"});
    sol::assets::MaterialDef otherTuning = material("other_tuning");
    otherTuning.params.push_back({.name = "gain", .value = 9.0f});
    const std::vector<sol::assets::MaterialDef> pairs = {slotted, otherSlot, tuned, otherTuning};
    const MaterialStateGrouping shared = groupMaterialsByState(pairs);
    SOL_CHECK(shared.states.size() == 2);
    SOL_CHECK(shared.materialState[0] == shared.materialState[1]);
    SOL_CHECK(shared.materialState[2] == shared.materialState[3]);
}

// ⚑⚑ PHASE 25 STAGE D. A slider must be able to move a number that is already
// declared and must NOT be able to invent one - because the param block's size
// comes from the shader, so a value with nowhere to go is either dropped in
// silence or written past a mapped buffer. This is the pure half of
// `MaterialRegistry::setParams`; the other half is one memcpy.
SOL_TEST(materialStateAppliesDeclaredParamValuesByName)
{
    sol::assets::MaterialDef tuned = material("tuned");
    tuned.params.push_back({.name = "glow_strength", .value = 2.2f});
    tuned.params.push_back({.name = "rim", .value = 0.5f});

    // ⚑ Handed in REVERSED, which is the assertion: a def document's keys are
    // in whatever order an author wrote them, so matching by position would
    // have put the rim value on the glow.
    const std::vector<sol::assets::MaterialParam> values = {{.name = "rim", .value = 0.25f},
                                                            {.name = "glow_strength", .value = 4.0f}};
    SOL_CHECK(applyParamValues(tuned, values));
    SOL_CHECK(tuned.params[0].name == "glow_strength");
    SOL_CHECK(tuned.params[0].value == 4.0f);
    SOL_CHECK(tuned.params[1].value == 0.25f);
}

SOL_TEST(materialStateRefusesAnUndeclaredParamAndChangesNothing)
{
    sol::assets::MaterialDef tuned = material("tuned");
    tuned.params.push_back({.name = "glow_strength", .value = 2.2f});

    // One name is declared and one is not. ⚑ The declared one is FIRST, so a
    // check-as-you-go implementation would have written it before discovering
    // the second - which is the half-applied state the all-or-nothing rule
    // exists to make unreachable.
    const std::vector<sol::assets::MaterialParam> values = {{.name = "glow_strength", .value = 4.0f},
                                                            {.name = "rim", .value = 0.25f}};
    SOL_CHECK(!applyParamValues(tuned, values));
    SOL_CHECK(tuned.params.size() == 1);
    SOL_CHECK(tuned.params[0].value == 2.2f);
}

// ---------------------------------------------------------------------------
// Which materials end up with no pipeline (engine plan Phase 25 stage E).
//
// ⚑⚑ THIS IS THE PURE HALF OF A DEFECT STAGE E FOUND BY DRIVING IT. There are
// two ways to be undrawable and they are counted at different granularities: a
// declaration that does not match its SPIR-V is refused per MATERIAL, while a
// `.spv` that is missing or unreadable fails the whole STATE. `build` wrote
// down only the first, so a material of the second kind kept an index into a
// null pipeline and slipped past a guard written to stop it - and reported
// "its params block reflected as empty" about a material whose shader was
// simply not there, sending an author to check the one thing that was right.

SOL_TEST(materialPipelineSlotsPassesEveryMaterialThroughWhenEverythingBuilt)
{
    const std::vector<std::uint32_t> materialState = {0, 1, 0};
    const std::vector<std::uint32_t> slots = materialPipelineSlots(materialState, {}, {});

    SOL_REQUIRE(slots.size() == 3);
    SOL_CHECK(slots[0] == 0);
    SOL_CHECK(slots[1] == 1);
    SOL_CHECK(slots[2] == 0);
}

// ⚑ THE CASE THAT WAS WRONG, AND THE SHARING IS WHAT MAKES IT MATTER: one
// missing `.spv` takes out every material naming that shader pair, not one.
SOL_TEST(materialPipelineSlotsClearsEveryMaterialSharingAnUnbuiltState)
{
    const std::vector<std::uint32_t> materialState = {0, 1, 0, 2};
    const std::vector<std::uint32_t> unbuilt = {0};
    const std::vector<std::uint32_t> slots = materialPipelineSlots(materialState, unbuilt, {});

    SOL_REQUIRE(slots.size() == 4);
    SOL_CHECK(slots[0] == kNoMaterialPipeline);
    SOL_CHECK(slots[2] == kNoMaterialPipeline); // the second user of state 0
    SOL_CHECK(slots[1] == 1);                   // untouched
    SOL_CHECK(slots[3] == 2);
}

// ⚑ The other kind, and it is per-MATERIAL: two materials share state 0 and
// only the one whose declaration lied loses its pipeline. Clearing both would
// be the same bug from the other side.
SOL_TEST(materialPipelineSlotsClearsOnlyTheRejectedMaterialNotItsStateSiblings)
{
    const std::vector<std::uint32_t> materialState = {0, 0, 1};
    const std::vector<std::uint32_t> rejected = {1};
    const std::vector<std::uint32_t> slots = materialPipelineSlots(materialState, {}, rejected);

    SOL_REQUIRE(slots.size() == 3);
    SOL_CHECK(slots[0] == 0);
    SOL_CHECK(slots[1] == kNoMaterialPipeline);
    SOL_CHECK(slots[2] == 1);
}

// ⚑ Both at once, because the live failure is usually both: a mod ships a
// material whose shader is missing AND another whose declaration is stale.
SOL_TEST(materialPipelineSlotsAppliesBothKindsOfRefusalTogether)
{
    const std::vector<std::uint32_t> materialState = {0, 1, 2};
    const std::vector<std::uint32_t> unbuilt = {2};
    const std::vector<std::uint32_t> rejected = {0};
    const std::vector<std::uint32_t> slots = materialPipelineSlots(materialState, unbuilt, rejected);

    SOL_REQUIRE(slots.size() == 3);
    SOL_CHECK(slots[0] == kNoMaterialPipeline); // rejected declaration
    SOL_CHECK(slots[1] == 1);                   // the survivor
    SOL_CHECK(slots[2] == kNoMaterialPipeline); // unbuilt state
}

// ⚑ The two index spaces are different sizes and a material index arriving in
// the state list - or the reverse - must not read out of bounds. `build` hands
// these in from two different loops, which is exactly how they get swapped.
SOL_TEST(materialPipelineSlotsIgnoresIndicesOutsideItsOwnRange)
{
    const std::vector<std::uint32_t> materialState = {0, 1};
    const std::vector<std::uint32_t> unbuilt = {7};
    const std::vector<std::uint32_t> rejected = {9};
    const std::vector<std::uint32_t> slots = materialPipelineSlots(materialState, unbuilt, rejected);

    SOL_REQUIRE(slots.size() == 2);
    SOL_CHECK(slots[0] == 0);
    SOL_CHECK(slots[1] == 1);
}
