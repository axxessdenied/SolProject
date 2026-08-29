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

// ---------------------------------------------------------------------------
// Stage C: somebody comes, or nobody does.

namespace {

// A world standing in a chosen system, with the ambient wings its security
// rating calls for. `loadSystem` is what puts hulls in the sky, and `sol.jump`
// is how the game itself gets there, so the test uses the same door.
bool standIn(game::SpaceWorld& world, const DefDatabase& defs, std::uint32_t system)
{
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    if (!world.generateUniverse(defs)) {
        return false;
    }
    return world.enterSystem(system);
}

// The first system whose baseline sits in [low, high], or kNoFaction.
std::uint32_t systemInBand(const game::SpaceWorld& world, float low, float high)
{
    for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
        const float baseline = world.systemSecurityBaseline(i);
        if (baseline >= low && baseline <= high) {
            return i;
        }
    }
    return sol::sim::kNoFaction;
}

} // namespace

// ⚑⚑⚑⚑ THE PRIMITIVE decisions/019 §3 GOT WRONG, ASSERTED DIRECTLY, BECAUSE
// THE WHOLE STAGE RESTS ON THE DIFFERENCE. §3 said a long-haul response is
// "`pilotPatrolTo` plus `PilotState::Travel`". `pilotPatrolTo` sets PATROL -
// combat-scale steering that "closes to 50 m and stops" - so the two named
// functions never composed, and a responder built on them would grind across
// 600,000 km on dogfight steering. This is the difference, in two lines.
SOL_TEST(a_dispatched_responder_travels_rather_than_patrols)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(standIn(world, defs, 0));

    const sol::ecs::Entity ship =
        world.spawnPilotFromDef(*defs.findShip("sol.interceptor"), defs, game::PilotRole::Patrol, 0);
    const sol::core::DVec3 far{6.0e8, 0.0, 0.0};

    SOL_REQUIRE(world.pilotPatrolTo(ship, far));
    SOL_CHECK(world.pilotStateOf(ship) == game::PilotState::Patrol); // the wrong one

    SOL_REQUIRE(world.pilotTravelTo(ship, far));
    SOL_CHECK(world.pilotStateOf(ship) == game::PilotState::Travel); // the cruise drive
}

// The zero band, which is the answer that costs nothing to give and is the
// hardest to notice is missing: at 0.1 nobody comes, and that is a RULE rather
// than a system with no patrols in it.
SOL_TEST(nobody_comes_in_a_system_nobody_polices)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld probe;
    probe.spawn(game::kDefaultUniverseSeed);
    probe.applyDefs(defs);
    SOL_REQUIRE(probe.generateUniverse(defs));

    // `sol.lantern` is the shipped galaxy's one unpoliced system - authored
    // lawless in Phase 29, before this phase existed to give it a number.
    std::uint32_t unpoliced = sol::sim::kNoFaction;
    for (std::uint32_t i = 0; i < probe.galaxy().systems.size(); ++i) {
        if (probe.galaxy().systems[i].authoredId == "sol.lantern") {
            unpoliced = i;
        }
    }
    SOL_REQUIRE(unpoliced != sol::sim::kNoFaction);
    SOL_CHECK(probe.systemSecurityBaseline(unpoliced) == 0.0f);

    game::SpaceWorld world;
    SOL_REQUIRE(standIn(world, defs, unpoliced));
    const sol::core::DVec3 at = sol::sim::playfieldHub(world.galaxy().systems[unpoliced]);
    SOL_CHECK(world.respondTo(at, 0xffff'ffffu, game::ResponseCause::WeaponsFire) == 0);
    SOL_CHECK(world.lastResponse().diverted == 0);
    SOL_CHECK(world.lastResponse().spawned == 0);
}

// ⚑⚑⚑ THE SILENCE BAND, REACHED THE ONLY WAY IT CAN BE. An unowned system
// returns before the band is ever consulted, so the obvious test for "nobody
// comes" - stand in `sol.lantern` and call - proves the OWNER check and leaves
// `kResponseSilenceBand` entirely untested. The counterfactual said so: setting
// the band to zero changed nothing anywhere. What actually exercises it is a
// system that IS policed, whose live rating has been eroded to near zero by
// danger - a place with a garrison that has stopped being able to answer.
SOL_TEST(a_system_whose_rating_has_collapsed_stops_answering_at_all)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    // The thinnest raidable garrison there is, so danger can swamp it.
    std::vector<RaidCandidate> candidates;
    std::uint32_t target = sol::sim::kNoFaction;
    for (std::uint32_t faction = 0; faction < world.factions().size(); ++faction) {
        world.factionSim().raidCandidates(world.galaxy(), faction, candidates);
        for (const RaidCandidate& candidate : candidates) {
            const float baseline = world.systemSecurityBaseline(candidate.system);
            if (baseline > 0.0f &&
                (target == sol::sim::kNoFaction || baseline < world.systemSecurityBaseline(target))) {
                target = candidate.system;
            }
        }
    }
    SOL_REQUIRE(target != sol::sim::kNoFaction);
    SOL_REQUIRE(world.enterSystem(target));
    const sol::core::DVec3 at = sol::sim::playfieldHub(world.galaxy().systems[target]);

    // It answers while it is quiet...
    SOL_REQUIRE(world.respondTo(at, 0xffff'ffffu, game::ResponseCause::WeaponsFire) > 0);

    SOL_REQUIRE(raiseDanger(world, target, 0.9f) > 0.0f);
    SOL_REQUIRE(world.systemSecurity(target) == 0.0f); // eroded to the zero band
    std::printf("  %s: baseline %+.3f, live %+.3f -> nobody comes\n",
                world.galaxy().systems[target].name.c_str(),
                static_cast<double>(world.systemSecurityBaseline(target)),
                static_cast<double>(world.systemSecurity(target)));

    // ...and stops when nobody's law reaches it any more. The garrison is still
    // there - stage B holds that - it just is not coming for you.
    SOL_CHECK(world.respondTo(at, 0xffff'ffffu, game::ResponseCause::WeaponsFire) == 0);
    SOL_CHECK(world.lastResponse().diverted == 0);
    SOL_CHECK(world.lastResponse().spawned == 0);
    SOL_CHECK(game::patrolsFor(world.systemSecurityBaseline(target)) > 0);
}

// High security over the pad: somebody is already there, so nothing is created.
// This is decisions/019 decision 3's first clause - divert first - and the
// reason a response can be fast without anything appearing from nowhere.
SOL_TEST(a_call_over_the_station_diverts_hulls_that_already_exist)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld probe;
    probe.spawn(game::kDefaultUniverseSeed);
    probe.applyDefs(defs);
    SOL_REQUIRE(probe.generateUniverse(defs));
    const std::uint32_t core = systemInBand(probe, 0.70f, 1.0f);
    SOL_REQUIRE(core != sol::sim::kNoFaction);

    game::SpaceWorld world;
    SOL_REQUIRE(standIn(world, defs, core));
    const sol::sim::SystemSpec& spec = world.galaxy().systems[core];
    SOL_REQUIRE(!spec.stations.empty());

    const std::uint32_t sent =
        world.respondTo(spec.stations[0].position, 0xffff'ffffu, game::ResponseCause::WeaponsFire);
    const game::SpaceWorld::ResponseReport& report = world.lastResponse();
    std::printf("  core %u: live %+.3f, reach %.0f km -> %u diverted, %u launched\n",
                core,
                static_cast<double>(report.live),
                report.reach / 1000.0,
                report.diverted,
                report.spawned);
    SOL_CHECK(sent > 0);
    SOL_CHECK(report.diverted > 0);
    SOL_CHECK(report.spawned == 0); // nothing materialised; the garrison answered

    // ⚑⚑⚑ AND THE DIVERTED HULLS ARE ON THE CRUISE DRIVE, WHICH IS THE CALL
    // SITE OBEYING decisions/019's CORRECTION RATHER THAN THE PRIMITIVE MERELY
    // EXISTING. The counterfactual - swapping `pilotTravelTo` back to the
    // `pilotPatrolTo` §3 actually names - left every other test in this file
    // green, because the state difference was only ever asserted on a pilot
    // the test drove by hand.
    std::vector<game::ResponderInfo> responders;
    world.responderInfo(responders);
    SOL_REQUIRE(responders.size() == report.diverted + report.spawned);
    for (const game::ResponderInfo& responder : responders) {
        SOL_CHECK(responder.state == game::PilotState::Travel);
    }
}

// ⚑⚑⚑ AND THE TRAP decisions/019 NAMED BEFORE ANYBODY COULD TAKE IT: a
// response wing must NOT materialise in the offender's face. `spawnPilotFromDef`
// places a ship 150-250 m directly in front of the player - correct for the dev
// console it was written for - and a fallback built on it would make "response
// time" a lie told instantly. A call at a far gate with nobody in range has to
// launch from a station or a gate, which is to say from somewhere ELSE.
SOL_TEST(a_launched_response_starts_somewhere_other_than_the_incident)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld probe;
    probe.spawn(game::kDefaultUniverseSeed);
    probe.applyDefs(defs);
    SOL_REQUIRE(probe.generateUniverse(defs));
    // ⚑⚑⚑ A THIN SYSTEM, AND THAT IS THE WHOLE SETUP. The first version of
    // this test used a CORE system and reported "2 diverted, 0 launched" - it
    // passed every assertion while never once reaching the code its name is
    // about. Reach scales with the rating, so at +0.850 it is 1,020,000 km and
    // covers a 600,000 km system entirely: there is always somebody to divert,
    // and the spawn fallback is unreachable. Only a system whose reach is
    // SHORTER than the distance to its own garrison can produce a launch.
    const std::uint32_t fringe = systemInBand(probe, 0.15f, 0.32f);
    SOL_REQUIRE(fringe != sol::sim::kNoFaction);

    game::SpaceWorld world;
    SOL_REQUIRE(standIn(world, defs, fringe));
    const sol::core::DVec3 hub = sol::sim::playfieldHub(world.galaxy().systems[fringe]);
    const sol::core::DVec3 at = hub + sol::core::DVec3{5.0e8, 0.0, 0.0};

    const std::uint32_t sent = world.respondTo(at, 0xffff'ffffu, game::ResponseCause::WeaponsFire);
    const game::SpaceWorld::ResponseReport& report = world.lastResponse();
    std::printf("  fringe %u: live %+.3f, reach %.0f km -> %u diverted, %u launched\n",
                fringe,
                static_cast<double>(report.live),
                report.reach / 1000.0,
                report.diverted,
                report.spawned);
    SOL_REQUIRE(sent > 0);
    // ⚑ Named, so this can never again pass by diverting instead.
    SOL_CHECK(report.spawned > 0);
    SOL_CHECK(report.diverted == 0);

    std::vector<game::ResponderInfo> responders;
    world.responderInfo(responders);
    SOL_REQUIRE(!responders.empty());
    for (const game::ResponderInfo& responder : responders) {
        // ⚑⚑ THE TRAP decisions/019 NAMED: `spawnPilotFromDef` puts a hull
        // 150-250 m in front of the PLAYER, so a fallback built on it would
        // make "response time" a lie told instantly. Every responder starts a
        // long way from the incident, and then has to fly.
        SOL_CHECK(length(responder.position - at) > 100'000.0);
        SOL_CHECK(responder.distanceToIncident > 100'000.0);
    }
}

// Reach reads the LIVE rating, so a system being fought over answers its far
// corners worse - decisions/019's single permitted coupling from danger to
// enforcement. The garrison itself is untouched, which stage B already holds.
SOL_TEST(a_raided_system_answers_its_far_corners_worse)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    std::vector<RaidCandidate> candidates;
    std::uint32_t target = sol::sim::kNoFaction;
    for (std::uint32_t faction = 0; faction < world.factions().size() && target == sol::sim::kNoFaction;
         ++faction) {
        world.factionSim().raidCandidates(world.galaxy(), faction, candidates);
        for (const RaidCandidate& candidate : candidates) {
            if (world.systemSecurityBaseline(candidate.system) > 0.5f) {
                target = candidate.system;
                break;
            }
        }
    }
    SOL_REQUIRE(target != sol::sim::kNoFaction);
    SOL_REQUIRE(world.enterSystem(target));

    const sol::core::DVec3 at = sol::sim::playfieldHub(world.galaxy().systems[target]);
    const float baselineBefore = world.systemSecurityBaseline(target);
    const std::uint32_t patrolsBefore = game::patrolsFor(baselineBefore);
    const std::uint32_t sentBefore = world.respondTo(at, 0xffff'ffffu, game::ResponseCause::WeaponsFire);
    const double reachBefore = world.lastResponse().reach;
    SOL_REQUIRE(sentBefore > 0);

    SOL_REQUIRE(raiseDanger(world, target, 0.9f) > 0.0f);
    (void)world.respondTo(at, 0xffff'ffffu, game::ResponseCause::WeaponsFire);
    const double reachAfter = world.lastResponse().reach;

    std::printf("  reach %.0f km -> %.0f km under raiding, responders %u -> %u\n",
                reachBefore / 1000.0,
                reachAfter / 1000.0,
                sentBefore,
                world.lastResponse().diverted + world.lastResponse().spawned);
    SOL_CHECK(reachAfter < reachBefore);
    // ⚑⚑⚑ AND HOW MANY CAME DID NOT MOVE, WHICH IS THE OTHER HALF AND THE ONE
    // THE SPIRAL WOULD EAT. Reach reads the LIVE rating because a busy system
    // really is slower; the SIZE of the answer reads the baseline, or a raid
    // would thin the response to itself and make the next raid cheaper. The
    // counterfactual - `respondersFor(live)` - is the naive wiring.
    SOL_CHECK(world.lastResponse().diverted + world.lastResponse().spawned == sentBefore);
    // ...and the garrison that answers is still the size it was, which is the
    // half that keeps this from being the spiral wearing a different hat.
    SOL_CHECK(world.systemSecurityBaseline(target) == baselineBefore);
    SOL_CHECK(game::patrolsFor(world.systemSecurityBaseline(target)) == patrolsBefore);
}

// ⚑⚑⚑⚑ THE TRIGGER, WHICH IS THE HALF A DRIVE COULD NOT PIN DOWN. `respondTo`
// is well covered above, but everything there calls it directly - nothing said
// that a HIT turns into a call, and the obvious way to show that (fly the game,
// shoot a Navy hull, read the log) failed for a reason worth writing down: the
// gun fired and the capacitor drained, but whether a bolt CONNECTS at 200 m is
// not something a scripted drive can promise, and five bursts produced no
// incident. The policy is stated here instead, where it is deterministic.
SOL_TEST(a_hit_on_the_local_law_is_what_calls_the_local_law)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(standIn(world, defs, 0));

    const std::uint32_t owner = world.systemOwnerFaction(world.currentSystemIndex());
    SOL_REQUIRE(owner < world.factions().size());
    SOL_REQUIRE(world.systemSecurityBaseline(world.currentSystemIndex()) > 0.0f);
    const std::uint32_t outsider = owner == 0 ? 1u : 0u;
    const sol::core::DVec3 at = sol::sim::playfieldHub(world.galaxy().systems[world.currentSystemIndex()]);

    // The attacker is a hull of another faction rather than the player, which
    // is the second trigger the phase names anyway - raiders engaging traders -
    // and needs nothing private to say it.
    const sol::ecs::Entity localHull =
        world.spawnPilotFromDef(*defs.findShip("sol.shuttle"), defs, game::PilotRole::Trader, owner);
    const sol::ecs::Entity raider =
        world.spawnPilotFromDef(*defs.findShip("sol.interceptor"), defs, game::PilotRole::Fighter, outsider);

    // 1. One of the owner's hulls takes fire from somebody else: they come.
    world.considerResponse(localHull.index, raider.index, at);
    const std::uint32_t called = world.lastResponse().diverted + world.lastResponse().spawned;
    std::printf("  hit on a %s hull by %s -> %u responder(s)\n",
                world.factions()[owner].name.c_str(),
                world.factions()[outsider].name.c_str(),
                called);
    SOL_CHECK(called > 0);

    // 2. Immediately again: the throttle holds, so a burst of projectile hits
    // is ONE call rather than one call per bolt.
    world.considerResponse(localHull.index, raider.index, at);
    SOL_CHECK(world.lastResponse().diverted + world.lastResponse().spawned == called);

    // 3. A hull the local law is NOT responsible for. A fresh world, so the
    // throttle is down and a pass here cannot be the cooldown talking.
    game::SpaceWorld elsewhere;
    SOL_REQUIRE(standIn(elsewhere, defs, 0));
    const sol::ecs::Entity foreignHull =
        elsewhere.spawnPilotFromDef(*defs.findShip("sol.shuttle"), defs, game::PilotRole::Trader, outsider);
    const sol::ecs::Entity attacker =
        elsewhere.spawnPilotFromDef(*defs.findShip("sol.interceptor"), defs, game::PilotRole::Fighter, owner);
    elsewhere.considerResponse(foreignHull.index, attacker.index, at);
    SOL_CHECK(elsewhere.lastResponse().diverted + elsewhere.lastResponse().spawned == 0);

    // 4. And the owner shooting its own is not an incident anybody attends.
    game::SpaceWorld friendly;
    SOL_REQUIRE(standIn(friendly, defs, 0));
    const sol::ecs::Entity victim =
        friendly.spawnPilotFromDef(*defs.findShip("sol.shuttle"), defs, game::PilotRole::Trader, owner);
    const sol::ecs::Entity shooter =
        friendly.spawnPilotFromDef(*defs.findShip("sol.interceptor"), defs, game::PilotRole::Patrol, owner);
    friendly.considerResponse(victim.index, shooter.index, at);
    SOL_CHECK(friendly.lastResponse().diverted + friendly.lastResponse().spawned == 0);
}
