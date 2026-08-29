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
