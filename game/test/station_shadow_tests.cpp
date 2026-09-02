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

// ⚑⚑ THE RULING, AS A TEST: the operator is a pirate clan, never a major and
// never the law. Ruled 2026-08-31 - Phase 37's shadow faction has not shipped,
// so a field pointed at it would be a null column on all 125 stations; a clan is
// a criminal organisation that already exists and already fences past `min_rep`.
// When Phase 37 stage A adds `kind = "shadow"`, this is the assertion that has
// to be re-pointed, and it says so.
SOL_TEST(a_shadow_operator_is_always_a_pirate_clan)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));
    SOL_REQUIRE(!world.factions().empty());

    std::size_t checked = 0;
    std::vector<std::uint32_t> operators;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const auto& list = world.galaxy().systems[s].stations;
        for (std::uint32_t t = 0; t < list.size(); ++t) {
            const std::uint32_t owner = world.stationShadowOwner(s, t);
            if (owner == sol::sim::kNoFaction) {
                continue;
            }
            ++checked;
            SOL_REQUIRE(owner < world.factions().size());
            if (!world.factions()[owner].pirate()) {
                std::printf("  %s is run by %s, which is not a clan\n",
                            list[t].name.c_str(),
                            world.factions()[owner].name.c_str());
            }
            SOL_CHECK(world.factions()[owner].pirate());
            operators.push_back(owner);
        }
    }

    std::sort(operators.begin(), operators.end());
    const std::size_t distinct =
        static_cast<std::size_t>(std::unique(operators.begin(), operators.end()) - operators.begin());
    std::printf("  %zu shadow operator(s) drawn from %zu distinct clan(s)\n", checked, distinct);
    SOL_REQUIRE(checked > 0);
    // A uniform roll over ten clans that produced one name every time would be a
    // picker reading a constant, which is the failure a "they are all clans"
    // check cannot see on its own.
    SOL_CHECK(distinct > 1);
}

// ⚑⚑⚑ THE RULE THAT MAKES THE FIELD MEAN ANYTHING: a fence the local boss runs
// is his own shop, not a shadow presence. The composer skips the FOUNDING
// holder, which is all it can see - `initializeFactions` has not run when it
// draws - and this checks that half. Test 5 checks the half that has to survive
// the border moving.
//
// ⚑⚑ AND THIS TEST IS VACUOUS ON THE SHIPPED SEED, WHICH IS WRITTEN DOWN HERE
// RATHER THAN LEFT TO BE REDISCOVERED. Measured: deleting the skip from
// `assignShadowOwners` leaves this assertion - and every other one in the
// project - green, because at seed 1701 none of the ten rolls lands on its own
// founder. What it really guards is that the field never drifts INTO that state;
// the rule itself is guarded by `the_picking_rule_steps_off_a_founder_it_lands_on`
// below, which is why that test exists at all.
SOL_TEST(no_station_hosts_the_shadow_operation_of_the_clan_that_founded_it)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::size_t checked = 0;
    std::size_t underClanLaw = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const std::uint32_t owner = world.stationShadowOwner(s, t);
            if (owner == sol::sim::kNoFaction) {
                continue;
            }
            ++checked;
            if (system.factionIndex < world.factions().size() &&
                world.factions()[system.factionIndex].pirate()) {
                ++underClanLaw; // a dock already under clan law, hosting a RIVAL clan's fence
            }
            if (owner == system.factionIndex) {
                std::printf(
                    "  %s is founded by and fenced for faction %u\n", system.stations[t].name.c_str(), owner);
            }
            SOL_CHECK(owner != system.factionIndex);
            // And it is a shadow presence at t = 0, when the live owner and the
            // founding claim still agree - the reading the plan's own sentence
            // asks for, checked at the one moment it is easy.
            SOL_CHECK(world.stationHasShadowPresence(s, t));
        }
    }
    std::printf("  %zu operator(s) checked, %zu of them on docks under clan law\n", checked, underClanLaw);
    SOL_REQUIRE(checked > 0);
}

// ⚑⚑⚑⚑ AND THIS IS THE ONE THAT ACTUALLY GUARDS THE SKIP, BECAUSE THE TEST
// ABOVE CANNOT. Measured while writing this file: at seed 1701 no roll lands on
// its own founder - ten shadow stations against ten clans - so deleting the skip
// from the composer outright leaves the entire suite green. A rule the shipped
// galaxy never exercises is a rule with no test behind it, whatever a
// galaxy-level assertion appears to say about it. So the rule is a function, and
// this hands it the case the galaxy declines to produce.
SOL_TEST(the_picking_rule_steps_off_a_founder_it_lands_on)
{
    constexpr std::uint32_t kBase = 5;  // five majors, clans start here
    constexpr std::uint32_t kClans = 4; // indices 5, 6, 7, 8

    // The ordinary case: nobody to avoid, the roll is the answer.
    SOL_CHECK(game::SpaceWorld::shadowOperatorFor(0, kBase, kClans, sol::sim::kNoFaction) == 5);
    SOL_CHECK(game::SpaceWorld::shadowOperatorFor(3, kBase, kClans, sol::sim::kNoFaction) == 8);

    // The case the shipped seed never produces: the roll names the founder.
    SOL_CHECK(game::SpaceWorld::shadowOperatorFor(0, kBase, kClans, 5) == 6);
    SOL_CHECK(game::SpaceWorld::shadowOperatorFor(2, kBase, kClans, 7) == 8);
    // ...including the wrap, which a "+1" with no modulo would run off the end.
    SOL_CHECK(game::SpaceWorld::shadowOperatorFor(3, kBase, kClans, 8) == 5);

    // A major founded the place: no clan is ever the founder, so no step.
    SOL_CHECK(game::SpaceWorld::shadowOperatorFor(1, kBase, kClans, 2) == 6);

    // The degenerate galaxies, where the honest answer is nobody.
    SOL_CHECK(game::SpaceWorld::shadowOperatorFor(0, kBase, 0, sol::sim::kNoFaction) == sol::sim::kNoFaction);
    SOL_CHECK(game::SpaceWorld::shadowOperatorFor(0, kBase, 1, 5) == sol::sim::kNoFaction);
}

// ⚑⚑⚑⚑ THE ONE THIS FILE IS FOR. Hand the system to the very clan that runs its
// fence and the fence stops being a shadow presence, because it is now the
// local boss's own shop - while the OPERATOR does not move, because who runs it
// was never a fact about the border. A stored verdict passes tests 1-3 and fails
// this one, which is exactly the discrimination the stage needed and exactly
// what no t = 0 test can provide.
SOL_TEST(a_clan_that_takes_a_system_inherits_its_own_fence_and_it_stops_being_shadow)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    // The first dock in the galaxy with an operator on it.
    std::uint32_t system = 0;
    std::uint32_t station = 0;
    std::uint32_t owner = sol::sim::kNoFaction;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size() && owner == sol::sim::kNoFaction; ++s) {
        const auto& list = world.galaxy().systems[s].stations;
        for (std::uint32_t t = 0; t < list.size(); ++t) {
            if (world.stationShadowOwner(s, t) != sol::sim::kNoFaction) {
                system = s;
                station = t;
                owner = world.stationShadowOwner(s, t);
                break;
            }
        }
    }
    SOL_REQUIRE(owner != sol::sim::kNoFaction);
    SOL_REQUIRE(owner < world.factions().size());

    const std::uint32_t before = world.systemOwnerFaction(system);
    std::printf("  %s: held by %u, fenced for %s\n",
                world.galaxy().systems[system].name.c_str(),
                before,
                world.factions()[owner].name.c_str());
    SOL_REQUIRE(before != owner); // otherwise the flip below proves nothing
    SOL_CHECK(world.stationHasShadowPresence(system, station));

    // The dev/test lever Phase 8u left for exactly this: hand the place over.
    SOL_REQUIRE(world.factionSim().flipSystem(system, owner));
    SOL_CHECK(world.systemOwnerFaction(system) == owner);

    // The operator is untouched - it is a fact about the station, not the border.
    SOL_CHECK(world.stationShadowOwner(system, station) == owner);
    // And the verdict has moved with the border, which is the whole stage.
    std::printf("  after the flip: operator %u, shadow presence %d\n",
                world.stationShadowOwner(system, station),
                world.stationHasShadowPresence(system, station) ? 1 : 0);
    SOL_CHECK(!world.stationHasShadowPresence(system, station));

    // Hand it back to somebody else and it is a shadow presence again. A
    // one-way check would pass on a function that simply returned false after
    // any flip at all.
    const std::uint32_t other = owner == 0 ? 1u : 0u;
    SOL_REQUIRE(other < world.factions().size());
    SOL_REQUIRE(world.factionSim().flipSystem(system, other));
    SOL_CHECK(world.stationShadowOwner(system, station) == owner);
    SOL_CHECK(world.stationHasShadowPresence(system, station));
}

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
