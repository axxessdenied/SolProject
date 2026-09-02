// Shadow modules (engine plan Phase 34 stage E): a black-market service on a
// station whose operator is not the station's owner.
//
// ⚑⚑⚑⚑ THE TEST THIS FILE EXISTS FOR IS THE FIFTH ONE, AND IT IS THE ONLY ONE
// THAT COULD HAVE CAUGHT THE MISTAKE THIS STAGE WAS SHAPED TO AVOID. The plan
// words the field as a comparison - "a module present on a station whose owner
// is not the station's owner" - and the right-hand side of that comparison
// MOVES: ownership has been dynamic since Phase 8u, and the shipped galaxy
// hands systems back and forth several times a minute. Had stage E stored the
// verdict instead of the operator, it would have been a fact about the FOUNDING
// claim, wrong within a minute of play, and **invisible to every test written
// at t = 0, because t = 0 is the one moment the founding claim and the live
// owner cannot disagree.** So test 5 moves a border and checks the answer moved
// with it. This project has met that trap twice already - `SystemSpec`'s
// unstored garrison sign, and the legality table Phase 33 stage D had to
// re-point at the live owner - and this is the first time it was designed out
// in advance rather than corrected afterwards.
//
// ⚑⚑ WHAT IS STORED AND WHAT IS DERIVED, because the whole stage is that line:
// `StationSpec::shadowOwner` is WHO runs the fence and never changes; whether
// that constitutes a SHADOW presence is computed per call against whoever holds
// the system now. The clan that takes a system inherits its own fence, and the
// fence stops being shadow at the moment it does.

#include "space_world.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::ModuleFamily;

namespace {

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

// `applyDefs` before `generateUniverse`, the order every galaxy-scoring suite in
// this directory uses: an unowned galaxy has no station bias, and the bias is
// most of what makes the real mix.
[[nodiscard]] bool buildShippedGalaxy(const DefDatabase& defs, game::SpaceWorld& world)
{
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    if (!world.generateUniverse(defs)) {
        std::printf("  generateUniverse refused the shipped defs\n");
        return false;
    }
    return true;
}

[[nodiscard]] bool hasShadowModule(const game::SpaceWorld& world,
                                   const DefDatabase& defs,
                                   std::uint32_t system,
                                   std::uint32_t station)
{
    for (const std::uint32_t module : world.stationModules(system, station)) {
        if (module < defs.modules().size() && defs.modules()[module].family == ModuleFamily::Shadow) {
            return true;
        }
    }
    return false;
}

} // namespace

// ⚑⚑⚑ THE BICONDITIONAL, AND IT IS CHECKED IN BOTH DIRECTIONS ON PURPOSE. A
// field that is set where it should be is half a guard; the half that catches a
// pass which staffs everything is the other one. "Every fence has an operator"
// and "nothing but a fence has one" are different failures with different
// causes, and only writing both down distinguishes a picker that skipped a
// station from one that staffed the whole galaxy.
SOL_TEST(a_station_names_a_shadow_operator_exactly_when_it_has_a_shadow_module)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));
    SOL_REQUIRE(!defs.modules().empty());

    std::size_t stations = 0;
    std::size_t withModule = 0;
    std::size_t withOperator = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const auto& list = world.galaxy().systems[s].stations;
        for (std::uint32_t t = 0; t < list.size(); ++t) {
            ++stations;
            const bool module = hasShadowModule(world, defs, s, t);
            const bool named = world.stationShadowOwner(s, t) != sol::sim::kNoFaction;
            withModule += module ? 1 : 0;
            withOperator += named ? 1 : 0;
            if (module != named) {
                std::printf("  %s: shadow module %d, operator named %d\n",
                            list[t].name.c_str(),
                            module ? 1 : 0,
                            named ? 1 : 0);
            }
            SOL_CHECK(module == named);
        }
    }

    // Anti-vacuity, the guard stage D had to add to the composition golden after
    // it passed for a whole stage over a galaxy with nothing in it. A run where
    // no station carries a shadow module proves the biconditional trivially.
    std::printf("  %zu station(s), %zu with a shadow module, %zu with an operator\n",
                stations,
                withModule,
                withOperator);
    SOL_REQUIRE(withModule > 0);
    SOL_CHECK(withModule < stations); // and it is a minority, not the default
}

// ⚑⚑⚑⚑ THE RULING, RE-POINTED EXACTLY AS THE OLD ONE SAID IT WOULD BE. This
// test used to read `a_shadow_operator_is_always_a_pirate_clan`, and its comment
// ended "when Phase 37 adds `kind = \"shadow\"`, this is the assertion that has to
// be re-pointed, and it says so." Phase 34 stage E chose a clan because the
// shadow faction had not shipped and a field pointed at nothing would have been
// a null column on all 125 stations calling itself vocabulary. It has shipped.
//
// ⚑⚑⚑ AND THE ASSERTION GOT STRONGER RATHER THAN JUST DIFFERENT. "A clan" was
// a statement about a KIND and left ten candidates; this is a statement about
// an IDENTITY and leaves one, so every fence in the galaxy answering to the
// same people is now a thing a test can say. That is ruling 1 of the phase -
// one hand-authored black market, so the opposed axis is one number - checked
// against the galaxy rather than against the def file.
SOL_TEST(a_shadow_operator_is_always_the_one_black_market)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));
    SOL_REQUIRE(!world.factions().empty());

    const std::uint32_t shadow = world.shadowFactionIndex();
    SOL_REQUIRE(shadow != sol::sim::kNoFaction);

    std::size_t checked = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const auto& list = world.galaxy().systems[s].stations;
        for (std::uint32_t t = 0; t < list.size(); ++t) {
            const std::uint32_t owner = world.stationShadowOwner(s, t);
            if (owner == sol::sim::kNoFaction) {
                continue;
            }
            ++checked;
            SOL_REQUIRE(owner < world.factions().size());
            if (owner != shadow) {
                std::printf("  %s is run by %s, not the black market\n",
                            list[t].name.c_str(),
                            world.factions()[owner].name.c_str());
            }
            SOL_CHECK(owner == shadow);
            SOL_CHECK(world.factions()[owner].shadow());
            // ⚑ And never the law: a faction that claims nothing is never a
            // holder, so a fence is never the local authority's own shop. That
            // used to be a comparison this pass had to make by hand.
            SOL_CHECK(owner != world.systemOwnerFaction(s));
        }
    }
    std::printf("  %zu operator(s), all of them %s\n", checked, world.factions()[shadow].name.c_str());
    SOL_REQUIRE(checked > 0);
}

// ⚑⚑⚑⚑ THE FOUNDER RULE IS RETIRED AND THIS IS WHAT SURVIVED IT (Phase 37
// stage C). Phase 34 stage E had a real problem: the operator was a pirate clan,
// a clan founds systems, and a fence run by the clan that founded the place is
// the local boss's own shop rather than a shadow presence. So the picker stepped
// off the founding holder, and `no_station_hosts_the_shadow_operation_of_the_
// clan_that_founded_it` guarded it.
//
// ⚑⚑⚑ THE RULE DID NOT BECOME WRONG - ITS SUBJECT WENT AWAY. There is one
// black market now and it claims nothing, so it can never found or hold
// anything, and "the operator is not the founder" is true by construction at
// every station in every galaxy. Kept as an assertion rather than deleted with
// the picker, because it is the property the design still depends on: the day
// somebody gives the shadow faction a capital, this is what says so.
SOL_TEST(the_black_market_never_runs_a_fence_in_ground_it_founded)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::size_t checked = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const std::uint32_t owner = world.stationShadowOwner(s, t);
            if (owner == sol::sim::kNoFaction) {
                continue;
            }
            ++checked;
            SOL_CHECK(owner != system.factionIndex);         // the founding claim
            SOL_CHECK(owner != world.systemOwnerFaction(s)); // and the live holder
        }
    }
    std::printf("  %zu fence(s), none of them in their operator's own space\n", checked);
    SOL_REQUIRE(checked > 0);
}

// ⚑⚑⚑⚑ `the_picking_rule_steps_off_a_founder_it_lands_on` LIVED HERE AND WENT
// WITH THE RULE IT TESTED (Phase 37 stage C). It existed for a measured reason
// worth keeping on the record: the shipped seed never exercised the skip - ten
// shadow stations, ten clans, and at 1701 no roll landed on its own founder - so
// the galaxy-level guard above was VACUOUSLY TRUE and deleting the skip entirely
// left the whole suite green. The answer was a named function a test could hand
// the case the galaxy declined to produce.
//
// ⚑⚑ `shadowOperatorFor` IS DELETED NOW, and with it the roll, the step-off and
// the single-clan case: against one hand-authored black market the whole
// function collapses to a constant. A test for a function that no longer exists
// is not a test, and this note is here so the vacuity finding is not lost with
// it - it is the reason `where_the_shadow_row_sits_in_def_order_changes_nothing_
// about_the_galaxy` in `shadow_faction_tests.cpp` was written the way it was.

// ⚑⚑⚑⚑ AND THE TEST THIS FILE WAS WRITTEN FOR IS GONE TOO, WHICH IS THE
// LOUDEST THING IN THIS COMMIT AND IS NOT A REGRESSION.
// `a_clan_that_takes_a_system_inherits_its_own_fence_and_it_stops_being_shadow`
// handed a system to the very clan running its fence and checked that the fence
// stopped being a shadow presence while the OPERATOR did not move. It was the
// only test in the project that could tell a stored verdict from a derived one,
// and the file's own header calls it "the one this file exists for".
//
// ⚑⚑⚑ IT CANNOT BE WRITTEN AGAINST THE SHIPPED DESIGN ANY MORE, because its
// premise - that the operator is somebody who can hold territory - is exactly
// what ruling 1 of Phase 37 removed. `stationHasShadowPresence` is now
// `shadowOwner != kNoFaction` and there is nothing left to derive. ⚑ What
// replaced its guarantee is `the_black_market_never_runs_a_fence_in_ground_it_
// founded` above, which asserts the same property from the other end: not "the
// comparison still works" but "the thing it compared can never happen".

// ⚑ Derived from the seed and never serialized, the rule the whole phase runs
// on: two worlds built from one seed name the same operators, and the field is
// nowhere in the save file. `loadFrom` re-composes, and `assignShadowOwners`
// rides at the tail of `composeStations` precisely so that it cannot be
// forgotten there.
SOL_TEST(one_seed_names_the_same_shadow_operators_twice)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld a;
    game::SpaceWorld b;
    SOL_REQUIRE(buildShippedGalaxy(defs, a));
    SOL_REQUIRE(buildShippedGalaxy(defs, b));
    SOL_REQUIRE(a.galaxy().systems.size() == b.galaxy().systems.size());

    std::size_t compared = 0;
    for (std::uint32_t s = 0; s < a.galaxy().systems.size(); ++s) {
        SOL_REQUIRE(a.galaxy().systems[s].stations.size() == b.galaxy().systems[s].stations.size());
        for (std::uint32_t t = 0; t < a.galaxy().systems[s].stations.size(); ++t) {
            SOL_CHECK(a.stationShadowOwner(s, t) == b.stationShadowOwner(s, t));
            compared += a.stationShadowOwner(s, t) != sol::sim::kNoFaction ? 1 : 0;
        }
    }
    std::printf("  %zu operator(s) reproduced from the seed\n", compared);
    SOL_REQUIRE(compared > 0);
}
