#include "space_world.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/faction_sim.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::sim::Galaxy;
using sol::sim::RaidCandidate;
using sol::sim::SystemSpec;

namespace {

bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

// Raids one system until its danger clears `want`, or gives up. Returns the
// danger reached. Raiding is the only way to move `danger` from outside the
// faction sim, and it is what the live rating is made of.
float raiseDanger(game::SpaceWorld& world, std::uint32_t system, float want)
{
    sol::sim::FactionSim& factions = world.factionSim();
    std::vector<RaidCandidate> candidates;
    for (int attempt = 0; attempt < 12 && factions.danger(system) < want; ++attempt) {
        bool raided = false;
        for (std::uint32_t faction = 0; faction < world.factions().size() && !raided; ++faction) {
            factions.raidCandidates(world.galaxy(), faction, candidates);
            for (const RaidCandidate& candidate : candidates) {
                if (candidate.system == system) {
                    raided = factions.commitRaid(world.galaxy(), nullptr, faction, system);
                    break;
                }
            }
        }
        if (!raided) {
            break;
        }
    }
    return factions.danger(system);
}

} // namespace

// ⚑⚑⚑⚑ THE SPIRAL RULING, WHICH decisions/019 TOOK RATHER THAN DISCOVERED,
// AND WHICH THE PHASE 30 SPEC ASKED FOR A TEST OF BY NAME: "the spiral is
// prevented by a ruling, not by a test - write the test that would catch it
// anyway, because a ruling in a document has never yet stopped anybody from
// wiring the obvious thing."
//
// The obvious wiring is `live = baseline - danger`. It is wrong because the
// SIGN of this scale is not a magnitude - it names WHO POLICES THE PLACE. A
// core system under sustained raiding would cross zero under that arithmetic
// and begin reporting that a pirate clan holds it, which is false, and which
// stage C's dispatcher and stage D's map colour would both then act on.
//
// So danger walks whoever holds a place toward zero and STOPS there. This test
// fails on `baseline - danger` and passes on the sign-preserving erosion.
SOL_TEST(danger_erodes_security_toward_zero_and_never_past_it)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    // ⚑⚑ `applyDefs` BEFORE `generateUniverse`, AND LEAVING IT OUT COSTS AN
    // HOUR. `generateUniverse` calls `initializeFactions`, which returns early
    // on a null `m_defs` - so the galaxy generates perfectly, the faction TABLE
    // comes out empty, and `raidCandidates` then returns nothing for every
    // faction. The symptom is a test that cannot find a system to raid, which
    // reads as the raid rules being stricter than they are rather than as the
    // world being half built. The boot log says it plainly: `0 faction(s)`.
    // The galaxy-only tests next door do not need this and do not do it.
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    // One system from each side of zero, chosen by the sign the generator
    // wrote rather than by index, so this keeps working as content grows -
    // the coupling Phase 29 stage D paid for.
    //
    // ⚑ And chosen from the systems somebody can actually RAID, which is not
    // the same set. The first pass of this test took the first system of each
    // sign and drew system 0 - a core capital at +0.850 that no faction has a
    // legal raid against on a fresh galaxy - so `danger` stayed at zero and
    // the guard below caught a test that would otherwise have "passed" every
    // check by never moving anything.
    // ⚑ The WEAKEST raidable system of each sign, not the first. Raiding tops
    // out around danger 0.5 here, so picking the first positive system drew a
    // core capital at +0.850 - eroded, sign intact, and the crossing case that
    // this whole test exists for guarded behind an `if` that never fired. The
    // thinnest garrison is the one danger can actually swamp.
    std::vector<RaidCandidate> candidates;
    std::uint32_t policed = sol::sim::kNoFaction;
    std::uint32_t clanHeld = sol::sim::kNoFaction;
    for (std::uint32_t faction = 0; faction < world.factions().size(); ++faction) {
        world.factionSim().raidCandidates(galaxy, faction, candidates);
        for (const RaidCandidate& candidate : candidates) {
            const float baseline = world.systemSecurityBaseline(candidate.system);
            if (baseline > 0.0f &&
                (policed == sol::sim::kNoFaction || baseline < world.systemSecurityBaseline(policed))) {
                policed = candidate.system;
            }
            if (baseline < 0.0f &&
                (clanHeld == sol::sim::kNoFaction || baseline > world.systemSecurityBaseline(clanHeld))) {
                clanHeld = candidate.system;
            }
        }
    }
    SOL_REQUIRE(policed != sol::sim::kNoFaction);
    SOL_REQUIRE(clanHeld != sol::sim::kNoFaction);

    // A quiet galaxy: the live rating IS the baseline, both sides of zero.
    SOL_CHECK(world.systemSecurity(policed) == world.systemSecurityBaseline(policed));
    SOL_CHECK(world.systemSecurity(clanHeld) == world.systemSecurityBaseline(clanHeld));

    for (const std::uint32_t system : {policed, clanHeld}) {
        const float baseline = world.systemSecurityBaseline(system);
        const float danger = raiseDanger(world, system, 0.9f);
        std::printf("  system %u: baseline %+.3f, danger %.3f, live %+.3f\n",
                    system,
                    static_cast<double>(baseline),
                    static_cast<double>(danger),
                    static_cast<double>(world.systemSecurity(system)));
        // ⚑ The whole test is vacuous without this: danger 0 leaves the live
        // rating equal to the baseline and every check below passes for the
        // wrong reason. `raiseDanger` gives up quietly when no faction can
        // legally raid the system, which is exactly how that would happen.
        SOL_REQUIRE(danger > 0.0f);

        const float live = world.systemSecurity(system);
        // It moved toward zero...
        SOL_CHECK(std::abs(live) < std::abs(baseline));
        // ...it did not cross it...
        SOL_CHECK(live * baseline >= 0.0f);
        // ...and it is exactly the sign-preserving erosion, not a subtraction.
        const float expected =
            baseline > 0.0f ? std::max(0.0f, baseline - danger) : -std::max(0.0f, -baseline - danger);
        SOL_CHECK(live == expected);
    }

    // ⚑⚑⚑ AND THE CASE THAT NAMES THE BUG, WHICH IS THE REASON THE SYSTEMS
    // ABOVE ARE CHOSEN THE WAY THEY ARE. Enough danger to swamp the baseline
    // reads as EXACTLY ZERO - "nobody's law reaches you here" - where
    // `baseline - danger` would hand back a negative number, and a negative
    // number on this scale is a positive claim that a pirate clan polices the
    // place. Both sides of zero, because the same lie is available in both
    // directions.
    for (const std::uint32_t system : {policed, clanHeld}) {
        const float baseline = world.systemSecurityBaseline(system);
        SOL_REQUIRE(world.factionSim().danger(system) > std::abs(baseline));
        SOL_CHECK(world.systemSecurity(system) == 0.0f);
    }
}

// ---------------------------------------------------------------------------
// Stage B: the two per-region tables become curves on the baseline.

// ⚑⚑⚑ THE CLAIM THAT MATTERS MOST IS THAT THE SKY DID NOT MOVE. Retiring a
// table into a curve is a refactor unless it changes something, and what it is
// ALLOWED to change is bounded: `kPatrolsPerRegion` was {3, 2, 1} keyed on
// region, and every system in the shipped galaxy must still get exactly the
// count its region used to give it. What the curve buys is that the INPUT is a
// number now, so stage E's authored `security =` moves the sky with no new
// code - which is checked separately below, because a curve that reproduced
// the table by ignoring its argument would pass this test perfectly.
SOL_TEST(patrol_strength_still_matches_the_region_table_it_replaced)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    constexpr std::uint32_t kPatrolsPerRegion[3] = {3, 2, 1};
    constexpr std::uint32_t kCiviliansPerRegion[3] = {4, 3, 1};
    std::uint32_t checked = 0;
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        const float baseline = world.systemSecurityBaseline(i);
        if (baseline <= 0.0f) {
            continue; // clan space and unpoliced space had no entry in the table
        }
        const auto tier = static_cast<std::size_t>(galaxy.systems[i].region);
        SOL_CHECK(game::patrolsFor(baseline) == kPatrolsPerRegion[tier]);
        SOL_CHECK(game::civiliansFor(baseline) == kCiviliansPerRegion[tier]);
        ++checked;
    }
    SOL_CHECK(checked > 0);
    std::printf("  %u major-held system(s) keep the count their region gave them\n", checked);
}

// ...and the half that proves the curve is a curve. A function that returned
// the old table by looking at nothing would satisfy the test above; these are
// values the shipped bands never produce.
SOL_TEST(a_security_rating_outside_the_shipped_bands_moves_the_garrison)
{
    // A fortress and a token presence, both reachable only by authoring
    // (stage E) or retuning - which is exactly the point of the curve.
    SOL_CHECK(game::patrolsFor(1.00f) == 4);
    SOL_CHECK(game::patrolsFor(0.95f) == 4);
    SOL_CHECK(game::patrolsFor(0.05f) == 1); // floored: an owner keeps something
    SOL_CHECK(game::patrolsFor(0.0f) == 0);  // nobody holds it, nobody garrisons it

    // Monotone, so "more secure" can never mean "fewer hulls".
    for (int step = 1; step <= 100; ++step) {
        const float lower = static_cast<float>(step - 1) / 100.0f;
        const float higher = static_cast<float>(step) / 100.0f;
        SOL_CHECK(game::patrolsFor(higher) >= game::patrolsFor(lower));
        SOL_CHECK(game::civiliansFor(higher) >= game::civiliansFor(lower));
        SOL_CHECK(game::raidersFor(-higher) >= game::raidersFor(-lower));
    }
}

// The negative band is where stage B is meant to be FELT: every clan system
// holds a flat two today, and a clan's home should be thick with hostiles
// while its periphery is thin.
SOL_TEST(clan_space_thickens_toward_the_clans_home)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    std::uint32_t thickest = 0;
    std::uint32_t thinnest = 0xffff'ffffu;
    std::uint32_t clanSystems = 0;
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        const float baseline = world.systemSecurityBaseline(i);
        if (baseline >= 0.0f) {
            continue;
        }
        const std::uint32_t raiders = game::raidersFor(baseline);
        SOL_CHECK(raiders >= 1u); // a clan that holds a place is present in it
        thickest = std::max(thickest, raiders);
        thinnest = std::min(thinnest, raiders);
        ++clanSystems;
    }
    SOL_REQUIRE(clanSystems > 0);
    std::printf(
        "  %u clan system(s), %u..%u raider(s) each (was a flat 2)\n", clanSystems, thinnest, thickest);
    // The whole point: it is no longer flat, and the deep end is heavier than
    // the two every clan system used to hold.
    SOL_CHECK(thickest > thinnest);
    SOL_CHECK(thickest > 2u);
}

// ⚑⚑⚑⚑ THE ANTI-SPIRAL WIRING, CHECKED WHERE IT IS ACTUALLY WIRED. The test
// above it proves the RULING about the arithmetic; this one proves the CALL
// SITE obeys it. Raiding a system hard enough to halve its live rating must
// leave its garrison untouched, because a navy digs in rather than evaporating
// - and because the alternative is a raid making the next raid cheaper.
SOL_TEST(raiding_a_system_does_not_thin_the_garrison_that_defends_it)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));
    const Galaxy& galaxy = world.galaxy();

    std::vector<RaidCandidate> candidates;
    std::uint32_t target = sol::sim::kNoFaction;
    for (std::uint32_t faction = 0; faction < world.factions().size(); ++faction) {
        world.factionSim().raidCandidates(galaxy, faction, candidates);
        for (const RaidCandidate& candidate : candidates) {
            if (world.systemSecurityBaseline(candidate.system) > 0.0f) {
                target = candidate.system;
                break;
            }
        }
        if (target != sol::sim::kNoFaction) {
            break;
        }
    }
    SOL_REQUIRE(target != sol::sim::kNoFaction);

    const float baselineBefore = world.systemSecurityBaseline(target);
    const std::uint32_t patrolsBefore = game::patrolsFor(baselineBefore);
    const float danger = raiseDanger(world, target, 0.9f);
    SOL_REQUIRE(danger > 0.0f); // or the check below is about nothing at all

    const float liveAfter = world.systemSecurity(target);
    std::printf("  system %u: baseline %+.3f (unmoved), live %+.3f, patrols %u\n",
                target,
                static_cast<double>(world.systemSecurityBaseline(target)),
                static_cast<double>(liveAfter),
                patrolsBefore);
    // The live rating fell...
    SOL_CHECK(liveAfter < baselineBefore);
    // ...the baseline did not, and neither did the garrison it sizes.
    SOL_CHECK(world.systemSecurityBaseline(target) == baselineBefore);
    SOL_CHECK(game::patrolsFor(world.systemSecurityBaseline(target)) == patrolsBefore);
    // And the spiral, stated directly: sizing off the live rating would have
    // thinned it, which is the wiring this phase refuses.
    SOL_CHECK(game::patrolsFor(liveAfter) < patrolsBefore);
}
