// The verdict (engine plan Phase 36 stage D): judgement is a function call, and
// consequence is the stage.
//
// ⚑⚑⚑⚑ THE SPEC'S ONE FALSE CLAIM ABOUT THIS STAGE, AND IT SHAPED THE WHOLE
// THING: "fired on is `respondTo` with a new cause and nothing else." It is not.
// `respondTo` dispatches hulls to a PLACE, in `PilotState::Travel`; what makes
// any of them shoot the player is `pilotEngageEnemy`, which considers the player
// only when `playerHostile` - standing below -30. A dispatch with no standing
// behind it sends ships to fly to where you were and then go back to their beat.
// So being fired on is a STANDING consequence with a dispatch on top, which is
// why `settleVerdict` spends the standing before it calls for anybody, and why
// `the_third_crate_is_not_inspected` is the test that matters most here.
//
// ⚑⚑⚑ AND THE LADDER IS EMERGENT RATHER THAN AUTHORED, WHICH IS THE FINDING
// WORTH KEEPING. Nothing in this phase says "three seizures makes an enemy".
// What exists is: player standing never drifts (`FactionSim` touches
// `m_standings` only from an explicit call), a major starts you at 0,
// `hostileThreshold` is -30, and a seizure costs 12. Stage C already refuses to
// open a stop against a faction that is hostile to you, and `pilotEngageEnemy`
// already picks up a hostile player. Put those five facts in one room and the
// fourth crate is not inspected - it is met.
//
// ⚑⚑ THE USER'S THREE RULINGS (2026-09-01), WHICH THESE TESTS STATE:
//   1. Contraband: take it, post a price, do not shoot.
//   2. Running is the crime; lapsing is not.
//   3. Restricted is a bill, not a seizure - it is licensed cargo and this
//      game has no licence to sell.

#include "asset_paths.hpp"
#include "content.hpp"
#include "space_world.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/faction_sim.hpp>
#include <sol/test/test.hpp>

namespace {

using Notice = game::SpaceWorld::NoticeReason;
using Outcome = game::SpaceWorld::InspectionOutcome;
using Verdict = game::SpaceWorld::InspectionVerdict;
using Post = game::SpaceWorld::PatrolPost;
using Legality = sol::assets::Legality;

// The shipped galaxy through the real content path INCLUDING the mod layer -
// without it this is an 81-system galaxy where the running game has 85, and
// that is exactly the size of discrepancy nobody questions (Phase 36 stage B).
struct Galaxy
{
    game::SpaceWorld world;
    game::GameContent content;

    Galaxy()
    {
        world.spawn(game::kDefaultUniverseSeed);
        const std::string mods = std::string(SOL_DEF_DATA_DIR) + "/../mods";
        SOL_CHECK(content.initialize(SOL_DEF_DATA_DIR, game::discoverModLayers(mods), &world));
        SOL_CHECK(world.generateUniverse(content.defs()));
    }

    // A policed system whose holder calls `commodity` exactly `wanted`, with
    // a patrol to be stopped by. This is what makes "the law under your feet"
    // a testable claim rather than a slogan: the SAME crate, asked about in
    // two systems, gets two answers.
    [[nodiscard]] std::uint32_t systemCalling(std::uint32_t commodity, Legality wanted) const
    {
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            if (world.commodityLegality(s, commodity) != wanted) {
                continue;
            }
            if (world.systemSecurityBaseline(s) < 0.4f) {
                continue; // needs a garrison worth the name
            }
            const sol::sim::SystemSpec& spec = world.galaxy().systems[s];
            if (!spec.stations.empty() && !spec.gates.empty()) {
                return s;
            }
        }
        return 0xffff'ffffu;
    }

    // A system held by somebody who declares nothing illegal at all. The
    // Freight Guild holds 25 of 85 and is the largest holder in the galaxy.
    [[nodiscard]] std::uint32_t tablelessSystem() const
    {
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            const sol::assets::FactionDef* law = world.jurisdictionOf(s);
            if (law != nullptr && law->contraband.empty() && law->restricted.empty() &&
                world.systemSecurityBaseline(s) > 0.4f && !world.galaxy().systems[s].stations.empty()) {
                return s;
            }
        }
        return 0xffff'ffffu;
    }

    // ⚑⚑ THE SAME THING WITHOUT RE-ENTERING THE SYSTEM, AND THE DIFFERENCE IS
    // NOT COSMETIC: `enterSystem` RESPAWNS AMBIENT TRAFFIC. Anything that
    // measures what happened TO the local wing - who is out on a call, who is
    // already engaged - has to stay in the system it started in, or every
    // round begins with a brand-new garrison and the measurement is of nothing.
    // `running_from_a_stop_diverts_the_local_law_and_launches_nobody` looped
    // twelve times against a wing that was fresh every time before this split.
    [[nodiscard]] Post warpToPatrol(double metres)
    {
        std::vector<Post> posts;
        world.patrolPosts(posts);
        SOL_CHECK(!posts.empty());
        SOL_CHECK(world.warpTo(posts[0].position, metres));
        world.patrolPosts(posts);
        return posts[0];
    }

    // Stand `metres` off the nearest patrol's HULL in `system` and return it.
    [[nodiscard]] Post standOff(std::uint32_t system, double metres)
    {
        SOL_CHECK(world.enterSystem(system));
        std::vector<Post> posts;
        world.patrolPosts(posts);
        SOL_CHECK(!posts.empty());
        SOL_CHECK(world.warpTo(posts[0].position, metres));
        world.patrolPosts(posts);
        return posts[0];
    }

    void run(double seconds)
    {
        constexpr double kStep = 1.0 / 60.0;
        for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i) {
            content.tick(kStep);
            world.tick(kStep);
        }
    }

    // Opens a stop and runs it to a verdict. Returns false if it never reached
    // one. `metres` well inside `inspectionScanRange` so the scan starts at
    // once - the distance-to-warning relationship is stage C's test, not this
    // file's, and every test here wants the RULING rather than the approach.
    bool stopAndSettle(std::uint32_t system, Notice reason = Notice::RandomCheck, double metres = 600.0)
    {
        const Post patrol = standOff(system, metres);
        // ⚑⚑ THE MARK IS THE PREVIOUS RULING'S TIMESTAMP, NOT `verdict ==
        // None`. `m_lastInspection` is a LAST-record, so it still carries the
        // previous stop's verdict when this one opens - waiting for it to be
        // `None` returns instantly on the second stop of any test, reports the
        // stale ruling as this one's, and leaves a hold still running that
        // makes the THIRD stop refuse to open. Both failures this file opened
        // with were that one line.
        const double mark = world.lastInspection().atWorldSeconds;
        if (!world.beginInspection(patrol.pilotIndex, reason)) {
            return false;
        }
        constexpr double kStep = 1.0 / 60.0;
        for (int i = 0; i < 90 * 60 && world.heldForInspection(); ++i) {
            content.tick(kStep);
            world.tick(kStep);
        }
        if (world.heldForInspection()) {
            return false; // never ended: the caller is asserting about nothing
        }
        // ⚑ One more pair of ticks: the ruling lands in `GameContent::tick`
        // the frame AFTER the hold ends, exactly as a docking answer does.
        content.tick(kStep);
        world.tick(kStep);
        return world.lastInspection().atWorldSeconds > mark &&
               world.lastInspection().verdict != Verdict::None;
    }
};

// Every line any verdict can say, so one test can measure them all against the
// panel. They are the scriptless defaults; `init.lua` writes its own and the
// same ceiling applies, which is why the Lua ones are copied here too.
const char* const kVerdictLines[] = {
    // C++ (space_world.cpp, applyDefaultVerdict)
    "You ran. There's a price on you now.",
    "Contraband. We're seizing it, and posting you.",
    "Licensed cargo. We'll take what you have.",
    "Licensed cargo. Duty is 8888 credits.", // the widest a %.0f duty gets
    "Hold's clean. Safe flying.",
    "Nothing here we care about. Fly on.",
    // Lua (init.lua, inspection_verdict)
    "Sorry. That has to come off, and it's on record.",
};

} // namespace

// ⚑⚑⚑⚑ THE PHASE'S EXIT CRITERION, IN ONE TEST: THE SAME CRATE, TWO
// JURISDICTIONS, TWO ANSWERS, AND NOTHING IN THE HOLD MOVED. `sol.salvage` is
// contraband to the Ironstar Hegemony and merely restricted to the Solar Navy
// and the Helios Ascendancy - Phase 33 ruling 10 chose it on purpose, because
// salvage is what a wreck pays out, so the pilot who lives by killing things
// earns a hold full of the one good the most policed space will not have.
SOL_TEST(the_same_crate_is_seized_in_one_jurisdiction_and_taxed_in_the_next)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);

    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    const std::uint32_t licenses = g.systemCalling(salvage, Legality::Restricted);
    SOL_REQUIRE(forbids != 0xffff'ffffu);
    SOL_REQUIRE(licenses != 0xffff'ffffu);

    // Somewhere that licenses it: a bill, and the crate stays aboard.
    SOL_REQUIRE(g.world.enterSystem(licenses));
    SOL_REQUIRE(g.world.addPlayerCargo(salvage, 10.0f) == 10.0f);
    const double purse = g.world.playerCredits();
    SOL_REQUIRE(g.stopAndSettle(licenses));
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Duty);
    SOL_CHECK(g.world.playerCargo(salvage) == 10.0f); // ruling 3: the hold is yours
    SOL_CHECK(g.world.playerCredits() < purse);       // and it cost something
    SOL_CHECK(g.world.lastInspection().unitsSeized == 0.0f);

    // Somewhere that forbids it: taken, and a price goes up.
    SOL_REQUIRE(g.world.enterSystem(forbids));
    SOL_REQUIRE(g.stopAndSettle(forbids));
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Seizure);
    SOL_CHECK(g.world.playerCargo(salvage) == 0.0f);
    SOL_CHECK(g.world.lastInspection().unitsSeized == 10.0f);
    SOL_CHECK(g.world.lastInspection().bountyPosted > 0.0f);
}

// ⚑⚑⚑ THE ANSWER THE MECHANISM PRODUCES FOR FREE, AND THE PHASE'S BEST SINGLE
// DEMONSTRATION THAT JURISDICTION IS REAL: somebody with territory, patrols and
// a working scanner stops you, reads your hold, and has no opinion about any of
// it. ⚑ It is a MAJOR rather than a clan, and the spec expected a clan: clan
// systems field raiders and every road to a stop needs `PilotRole::Patrol`, so
// the clan version is unreachable at any lever. The Freight Guild - 25 of 85
// systems, the largest holder in the galaxy - is what makes it flyable.
SOL_TEST(a_jurisdiction_with_no_law_stops_you_and_has_nothing_to_charge_you_with)
{
    Galaxy g;
    const std::uint32_t nowhere = g.tablelessSystem();
    SOL_REQUIRE(nowhere != 0xffff'ffffu);
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);

    SOL_REQUIRE(g.world.enterSystem(nowhere));
    SOL_REQUIRE(g.world.addPlayerCargo(salvage, 20.0f) == 20.0f);
    // The exact hold the Hegemony would seize outright.
    SOL_CHECK(g.world.judgeHold().worst == Legality::Legal);
    SOL_CHECK(!g.world.judgeHold().holderHasTable);

    const double purse = g.world.playerCredits();
    SOL_REQUIRE(g.stopAndSettle(nowhere));
    // ⚑ NOT `Clean`. The two are the same outcome and a different sentence,
    // and telling them apart is the entire point: `Clean` is a jurisdiction
    // that looked, `NoLaw` is one with nothing to look it up in.
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::NoLaw);
    SOL_CHECK(g.world.playerCargo(salvage) == 20.0f);
    SOL_CHECK(g.world.playerCredits() == purse);
    SOL_CHECK(!g.world.factionSim().wanted());
}

// A hold with nothing objectionable in it, in a system whose holder DOES keep a
// table. The control for the test above, and the state most stops end in.
SOL_TEST(a_clean_hold_in_policed_space_is_waved_on_and_costs_nothing)
{
    Galaxy g;
    const std::uint32_t ore = g.world.commodityIndex("sol.ore");
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(ore != 0xffff'ffffu && salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);

    SOL_REQUIRE(g.world.enterSystem(forbids));
    SOL_REQUIRE(g.world.addPlayerCargo(ore, 30.0f) == 30.0f);
    const double purse = g.world.playerCredits();
    const float standing = g.world.factionSim().standing(g.world.systemOwnerFaction(forbids));

    SOL_REQUIRE(g.stopAndSettle(forbids));
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Clean);
    SOL_CHECK(g.world.playerCargo(ore) == 30.0f);
    SOL_CHECK(g.world.playerCredits() == purse);
    SOL_CHECK(g.world.factionSim().standing(g.world.systemOwnerFaction(forbids)) == standing);
    SOL_CHECK(!g.world.factionSim().wanted());
    // ⚑ AND NOBODY WAS CALLED. The dispatch hangs off the FLED verdict alone,
    // and a version that fired on every ruling would look identical from every
    // other assertion in this file - the player would simply find the local
    // wing converging on them every time they were waved through.
    SOL_CHECK(g.world.lastResponse().diverted == 0);
    SOL_CHECK(g.world.lastResponse().spawned == 0);
}

// ⚑⚑ A SEIZURE TAKES WHAT THE LOCAL TABLE OBJECTS TO AND NOT THE HOLD. Ore is
// legal everywhere in the shipped galaxy, and a patrol that emptied the whole
// ship would be indistinguishable from one enforcing the law until the day
// somebody looked at their manifest.
SOL_TEST(a_seizure_takes_the_contraband_and_leaves_the_rest_of_the_hold)
{
    Galaxy g;
    const std::uint32_t ore = g.world.commodityIndex("sol.ore");
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(ore != 0xffff'ffffu && salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);

    SOL_REQUIRE(g.world.enterSystem(forbids));
    SOL_REQUIRE(g.world.addPlayerCargo(ore, 20.0f) == 20.0f);
    SOL_REQUIRE(g.world.addPlayerCargo(salvage, 8.0f) == 8.0f);
    // ⚑ The WORST thing aboard is what the stop is about, and the legal ore
    // beside it does not soften the answer.
    SOL_CHECK(g.world.judgeHold().worst == Legality::Contraband);
    SOL_CHECK(g.world.judgeHold().units == 8.0f);

    SOL_REQUIRE(g.stopAndSettle(forbids));
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Seizure);
    SOL_CHECK(g.world.playerCargo(salvage) == 0.0f);
    SOL_CHECK(g.world.playerCargo(ore) == 20.0f);
    SOL_CHECK(g.world.lastInspection().unitsSeized == 8.0f);
}

// ⚑⚑⚑ RULING 3, MEANT LITERALLY. `Legality::Restricted`'s own comment is
// "licensed: carriable, and a patrol will want papers" - and there is no
// licence anywhere in this game to want. So it is a bill: credits only, hold
// intact, nothing posted and nothing remembered. A pilot who pays the duty is
// not a criminal, and the standing check below is what says so.
SOL_TEST(restricted_cargo_is_a_bill_and_nothing_else)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t licenses = g.systemCalling(salvage, Legality::Restricted);
    SOL_REQUIRE(licenses != 0xffff'ffffu);

    SOL_REQUIRE(g.world.enterSystem(licenses));
    SOL_REQUIRE(g.world.addPlayerCargo(salvage, 40.0f) == 40.0f);
    const std::uint32_t owner = g.world.systemOwnerFaction(licenses);
    const float standing = g.world.factionSim().standing(owner);
    const double purse = g.world.playerCredits();

    SOL_REQUIRE(g.stopAndSettle(licenses));
    const game::SpaceWorld::InspectionReport& report = g.world.lastInspection();
    SOL_CHECK(report.verdict == Verdict::Duty);
    SOL_CHECK(report.creditsTaken > 0.0);
    SOL_CHECK(g.world.playerCredits() == purse - report.creditsTaken);
    SOL_CHECK(g.world.playerCargo(salvage) == 40.0f);            // carriable
    SOL_CHECK(report.bountyPosted == 0.0f);                      // not posted
    SOL_CHECK(g.world.factionSim().standing(owner) == standing); // not remembered
    SOL_CHECK(!g.world.factionSim().wanted());
}

// ⚑⚑ A DUTY IS A TRANSACTION, AND A TRANSACTION AGAINST AN EMPTY ACCOUNT IS AN
// EMPTY ACCOUNT. No debt, no fallback seizure, no negative credits - the last
// of those being the one that would have been a real bug, because
// `m_playerCredits` is a bare double and every price in the game reads it.
SOL_TEST(a_duty_is_capped_at_what_the_pilot_actually_has)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t licenses = g.systemCalling(salvage, Legality::Restricted);
    SOL_REQUIRE(licenses != 0xffff'ffffu);

    SOL_REQUIRE(g.world.enterSystem(licenses));
    SOL_REQUIRE(g.world.addPlayerCargo(salvage, 50.0f) == 50.0f);
    g.world.addCredits(-g.world.playerCredits() + 7.0); // seven credits to their name
    SOL_REQUIRE(g.world.playerCredits() < 8.0);

    SOL_REQUIRE(g.stopAndSettle(licenses));
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Duty);
    SOL_CHECK(g.world.playerCredits() >= 0.0);
    SOL_CHECK(g.world.lastInspection().creditsTaken <= 7.0);
    SOL_CHECK(g.world.playerCargo(salvage) == 50.0f); // still not a seizure
}

// ⚑⚑⚑⚑ RULING 2, BOTH HALVES, AND THE SECOND HALF IS THE ONE A CARELESS
// IMPLEMENTATION LOSES. Leaving the envelope is an offence whatever was aboard,
// because fleeing tells them everything. A hold that timed out because the
// patrol never closed is NOT: from their side they simply lost you, and stage C
// measured that the only pilots who can lapse are the ones spotted from 20 km
// out who never did anything but keep flying.
SOL_TEST(running_is_the_crime_and_lapsing_is_not)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);
    const std::uint32_t owner = g.world.systemOwnerFaction(forbids);

    // --- LAPSED: stopped from far enough out that the patrol never reads you.
    SOL_REQUIRE(g.world.enterSystem(forbids));
    const float before = g.world.factionSim().standing(owner);
    const Post far = g.standOff(forbids, 40'000.0);
    SOL_REQUIRE(g.world.beginInspection(far.pilotIndex, Notice::RandomCheck));
    g.run(70.0); // past the 60 s grant
    SOL_CHECK(g.world.lastInspection().outcome == Outcome::Lapsed);
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::None); // nobody read anything
    SOL_CHECK(g.world.factionSim().standing(owner) == before);
    SOL_CHECK(!g.world.factionSim().wanted());

    // --- RAN: stopped close, then out past the 80 km envelope.
    const Post near = g.standOff(forbids, 600.0);
    g.world.clearNoticeCooldown();
    SOL_REQUIRE(g.world.beginInspection(near.pilotIndex, Notice::RandomCheck));
    SOL_REQUIRE(g.world.warpTo(near.position, 200'000.0));
    g.run(2.0);
    SOL_CHECK(g.world.lastInspection().outcome == Outcome::Ran);
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Fled);
    SOL_CHECK(g.world.factionSim().standing(owner) < before);
    SOL_CHECK(g.world.factionSim().bounty(owner) > 0.0f);
    SOL_CHECK(g.world.factionSim().wanted());
}

// ⚑⚑⚑⚑ THE LADDER, AND NOTHING AUTHORS IT. Player standing never drifts back,
// a major starts you at 0, a seizure costs 12, and `hostileThreshold` is -30.
// So the third seizure puts a pilot under it - and at that point stage C's
// "a faction already shooting at you does not check your papers" guard turns
// the mechanic OFF, while `pilotEngageEnemy` turns a different one on. The
// fourth crate is not inspected. It is met.
//
// ⚑ This is also the test that pins the spec's false claim. If being fired on
// were "`respondTo` with a new cause and nothing else", nothing here would move
// `playerHostile` at all and the fourth stop would open exactly like the first.
SOL_TEST(the_third_crate_is_not_inspected_it_is_met)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);
    const std::uint32_t owner = g.world.systemOwnerFaction(forbids);

    SOL_REQUIRE(g.world.enterSystem(forbids));
    SOL_REQUIRE(g.world.factionSim().standing(owner) == 0.0f); // a major starts neutral
    SOL_REQUIRE(!g.world.factionSim().playerHostile(owner));

    for (int caught = 0; caught < 3; ++caught) {
        SOL_REQUIRE(g.world.addPlayerCargo(salvage, 4.0f) == 4.0f);
        g.world.clearNoticeCooldown();
        SOL_REQUIRE(g.stopAndSettle(forbids));
        SOL_CHECK(g.world.lastInspection().verdict == Verdict::Seizure);
    }
    SOL_CHECK(g.world.lastInspection().seizures == 3);
    SOL_CHECK(g.world.factionSim().standing(owner) <= -36.0f);
    SOL_CHECK(g.world.factionSim().playerHostile(owner));

    // The fourth stop cannot be opened at all - and that IS the consequence.
    SOL_REQUIRE(g.world.addPlayerCargo(salvage, 4.0f) == 4.0f);
    g.world.clearNoticeCooldown();
    const Post patrol = g.standOff(forbids, 600.0);
    SOL_CHECK(!g.world.beginInspection(patrol.pilotIndex, Notice::RandomCheck));
    SOL_CHECK(!g.world.heldForInspection());
    // Notice will not fire either, so this is not a hole in one road only.
    g.world.clearNoticeCooldown();
    for (int i = 0; i < 60 * 60; ++i) {
        SOL_CHECK(!g.world.heldForInspection());
        g.world.tick(1.0 / 60.0);
    }
    SOL_CHECK(g.world.playerCargo(salvage) == 4.0f); // nobody took it; nobody asked
}

// ⚑⚑⚑ THE LOOP CLOSES, AND IT CLOSES THROUGH A FIELD STAGE A SHIPPED WITH NO
// WRITER. `considerNotice` already picks `Wanted` off `bounty(owner) > 0` - it
// has since stage B - and until this stage the only thing that could set that
// was a dev console command. A seizure is the first real writer, so the price
// a stop puts on you is what makes the next one more likely.
SOL_TEST(a_price_posted_by_one_stop_is_the_reason_for_the_next)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);
    const std::uint32_t owner = g.world.systemOwnerFaction(forbids);

    SOL_REQUIRE(g.world.enterSystem(forbids));
    SOL_REQUIRE(g.world.transponderOn()); // lit, so `Dark` cannot be the reason
    SOL_REQUIRE(g.world.factionSim().bounty(owner) == 0.0f);

    // Lit and clean: a random check is all this pilot can attract.
    const Post patrol = g.standOff(forbids, 2'000.0);
    (void)patrol;
    g.world.clearNoticeCooldown();
    SOL_CHECK(g.world.considerNotice(1.0) != Notice::Wanted);

    SOL_REQUIRE(g.world.addPlayerCargo(salvage, 6.0f) == 6.0f);
    g.world.clearNoticeCooldown();
    SOL_REQUIRE(g.stopAndSettle(forbids));
    SOL_REQUIRE(g.world.lastInspection().verdict == Verdict::Seizure);
    SOL_REQUIRE(g.world.factionSim().bounty(owner) > 0.0f);

    // Still lit, hold now empty - and the reason has changed anyway.
    g.world.clearNoticeCooldown();
    (void)g.standOff(forbids, 2'000.0);
    Notice seen = Notice::None;
    for (int i = 0; i < 600 * 60 && seen == Notice::None; ++i) {
        seen = g.world.considerNotice(1.0 / 60.0);
    }
    SOL_CHECK(seen == Notice::Wanted);
}

// ⚑⚑ THE CAUSE IS READ, AND WHAT IT DECIDES IS WHETHER THE LAW MAY CONJURE
// HULLS. Weapons fire is somebody dying, so a short-handed garrison tops itself
// up from the nearest station. A pilot who declined a paperwork check is not
// worth launching a wing over 600,000 km of empty lane - that one diverts only.
// Without the split, running from a stop makes ships appear out of nothing,
// which is `017`'s tax arriving by a side door.
SOL_TEST(running_from_a_stop_diverts_the_local_law_and_launches_nobody)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);

    const std::uint32_t owner = g.world.systemOwnerFaction(forbids);

    SOL_REQUIRE(g.world.enterSystem(forbids));
    const Post patrol = g.standOff(forbids, 600.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Dark));
    SOL_REQUIRE(g.world.warpTo(patrol.position, 200'000.0));
    g.run(2.0);

    SOL_REQUIRE(g.world.lastInspection().verdict == Verdict::Fled);
    // ⚑ Anti-vacuity: somebody DID come. A test that only asserted "spawned
    // == 0" would pass with the whole dispatch deleted.
    SOL_CHECK(g.world.lastResponse().diverted > 0);
    SOL_CHECK(g.world.lastResponse().spawned == 0);

    // ⚑⚑⚑⚑ AND NOW THE HALF THAT ACTUALLY PINS THE RULE, WHICH THE FIRST
    // VERSION OF THIS TEST MISSED ENTIRELY. `respondTo` only reaches its
    // spawning half when the local wing is SHORT - so with a full garrison
    // free to divert, letting a fled dispatch top up is invisible. Mutation
    // testing caught that: `topUp = true` passed everything above.
    //
    // The shortfall is made honestly rather than contrived. Every hull just
    // diverted carries `respondTimer = kResponseGiveUpSeconds` (180 s) and is
    // then skipped as "already on a call", so running from stop after stop
    // inside that window empties the wing - which is also exactly what a
    // player does who keeps running. When there is nobody left to divert, a
    // cause allowed to top up is a cause that manufactures interceptors out of
    // empty space, and THAT is the case this half exists to refuse.
    std::vector<Post> before;
    g.world.patrolPosts(before);
    const std::size_t hullsBefore = before.size();

    // ⚑ Standing is put back each round on purpose: what is under test is the
    // dispatch, and after three flights the ladder would make the faction
    // hostile and stop it opening stops at all - a different rule, tested
    // elsewhere, that would quietly end this loop early.
    int guard = 0;
    while (g.world.lastResponse().diverted > 0 && guard < 12) {
        ++guard;
        g.world.factionSim().setStanding(owner, 0.0f);
        const Post again = g.warpToPatrol(600.0); // NOT standOff: no respawn
        g.world.clearNoticeCooldown();
        SOL_REQUIRE(g.world.beginInspection(again.pilotIndex, Notice::Dark));
        SOL_REQUIRE(g.world.warpTo(again.position, 200'000.0));
        g.run(2.0);
        SOL_REQUIRE(g.world.lastInspection().verdict == Verdict::Fled);
    }
    SOL_CHECK(guard < 12);                           // the wing DID run out
    SOL_CHECK(g.world.lastResponse().diverted == 0); // nobody left to send
    SOL_CHECK(g.world.lastResponse().spawned == 0);  // and nobody was conjured
    std::vector<Post> after;
    g.world.patrolPosts(after);
    SOL_CHECK(after.size() == hullsBefore);
}

// ⚑⚑⚑⚑ THE OUTCOME THE SPEC CALLED "FIRED ON", DELIVERED THE ONLY WAY IT CAN
// BE: BY THE NUMBER, NOT BY THE DISPATCH. A pilot already deep in the red who
// runs from a stop crosses `hostileThreshold` on the way out - and it is
// crossing it, not the call for help, that turns the local wing into people
// shooting at them. `settleVerdict` spends the standing BEFORE it calls
// `respondTo` for exactly this reason: hulls that arrive while the number is
// still above the line find a pilot they have no quarrel with, resume their
// beat, and produce a bug that reads as the AI being broken.
//
// ⚑ This is the test that would fail if the two lines were swapped, and it is
// the only one - which is why the ordering is written down in the code as well
// as pinned here.
SOL_TEST(a_runner_who_was_already_in_the_red_gets_shot_at_rather_than_stopped)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);
    const std::uint32_t owner = g.world.systemOwnerFaction(forbids);

    SOL_REQUIRE(g.world.enterSystem(forbids));
    // Two seizures deep, which is -24 in the shipped tuning. Set directly
    // rather than flown, because what is under test is the crossing and not
    // the arithmetic that got here.
    g.world.factionSim().setStanding(owner, -25.0f);
    SOL_REQUIRE(!g.world.factionSim().playerHostile(owner));

    const Post patrol = g.standOff(forbids, 600.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::Wanted));
    SOL_REQUIRE(g.world.warpTo(patrol.position, 200'000.0));
    g.run(2.0);

    SOL_REQUIRE(g.world.lastInspection().verdict == Verdict::Fled);
    SOL_CHECK(g.world.factionSim().standing(owner) < -30.0f);
    SOL_CHECK(g.world.factionSim().playerHostile(owner)); // the line was crossed
    SOL_CHECK(g.world.lastResponse().diverted > 0);       // and somebody was called

    // Come back into their envelope: they are not checking papers now.
    SOL_REQUIRE(g.world.warpTo(patrol.position, 2'000.0));
    g.run(4.0);
    std::vector<Post> posts;
    g.world.patrolPosts(posts);
    bool hunted = false;
    for (const Post& post : posts) {
        hunted = hunted || post.state == game::PilotState::Attack;
    }
    SOL_CHECK(hunted);
    // And the stop is over as a mechanic for this pilot, in this space.
    SOL_CHECK(!posts.empty());
    SOL_CHECK(!g.world.beginInspection(posts[0].pilotIndex, Notice::Wanted));
}

// ⚑⚑⚑⚑ THE LIVE DRIVE FOUND THIS AND NOTHING IN THE TREE WOULD HAVE. A stop
// opened against a patrol that is outside the 80 km envelope ends as `Ran` on
// the very next frame, at 0% progress, because `tickInspection`'s first
// distance test throws it out. `considerNotice` has always refused to open one
// from out there - but `sol.inspect_me()` picks the NEAREST patrol with no
// range test of its own, and in a system whose nearest hull is a gate picket
// that is 600,000 km away.
//
// ⚑⚑⚑ IT WAS HARMLESS FOR THE WHOLE OF STAGE C AND BECAME A BUG THE MOMENT
// RUNNING HAD A PRICE. The drive was charged 400 credits of bounty and 8
// standing for fleeing a patrol it had never been within a hundred thousand
// kilometres of. The fix is at the CHOKE POINT rather than in the lever, so
// every road in obeys it - and this test is what stops the next lever, hook or
// mod re-opening the same hole.
SOL_TEST(a_stop_cannot_be_opened_by_a_patrol_that_cannot_see_you)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);
    const std::uint32_t owner = g.world.systemOwnerFaction(forbids);

    // The control first: from inside the envelope this patrol CAN stop you,
    // so a refusal below is about the distance and not about the patrol.
    const Post patrol = g.standOff(forbids, 600.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::RandomCheck));
    g.world.endInspection(Outcome::Lost, nullptr); // Lost reaches no verdict
    SOL_REQUIRE(g.world.lastInspection().verdict == Verdict::None);

    const float standing = g.world.factionSim().standing(owner);
    const float bounty = g.world.factionSim().bounty(owner);
    const double purse = g.world.playerCredits();

    // ⚑ 200 km out: two and a half times the notice envelope.
    SOL_REQUIRE(g.world.warpTo(patrol.position, 200'000.0));
    SOL_CHECK(!g.world.beginInspection(patrol.pilotIndex, Notice::RandomCheck));
    SOL_CHECK(!g.world.heldForInspection());

    // And nothing was charged for a stop that never happened, which is the
    // half that actually cost something before the fix.
    g.run(3.0);
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::None);
    SOL_CHECK(g.world.factionSim().standing(owner) == standing);
    SOL_CHECK(g.world.factionSim().bounty(owner) == bounty);
    SOL_CHECK(g.world.playerCredits() == purse);
}

// ⚑⚑⚑ MEASURED BEFORE THEY WERE DRAWN, NOT AFTER. Stage A shipped a
// 58-character refusal against a comms panel `game_ui.cpp` documents as "wide
// enough for a ~50-character line - measured, not guaranteed", and it reached
// the player as "Squawk your transponder or s". Nothing in this project
// measures a string against a panel, so counting is the only defence - and the
// Lua hook's lines are held to the same ceiling as the C++ ones because they
// are drawn in the same box.
SOL_TEST(every_line_a_verdict_speaks_fits_the_comms_panel)
{
    for (const char* line : kVerdictLines) {
        SOL_CHECK(std::strlen(line) <= 50);
        SOL_CHECK(std::strlen(line) > 0);
    }
    // And the four verdicts a player can actually be handed all have a name,
    // because a report that prints "none" for a real ruling is a probe that
    // lies.
    SOL_CHECK(std::strcmp(game::SpaceWorld::inspectionVerdictName(Verdict::Clean), "none") != 0);
    SOL_CHECK(std::strcmp(game::SpaceWorld::inspectionVerdictName(Verdict::NoLaw), "none") != 0);
    SOL_CHECK(std::strcmp(game::SpaceWorld::inspectionVerdictName(Verdict::Duty), "none") != 0);
    SOL_CHECK(std::strcmp(game::SpaceWorld::inspectionVerdictName(Verdict::Seizure), "none") != 0);
    SOL_CHECK(std::strcmp(game::SpaceWorld::inspectionVerdictName(Verdict::Fled), "none") != 0);
    SOL_CHECK(std::strcmp(game::SpaceWorld::inspectionVerdictName(Verdict::None), "none") == 0);
}

// The hold is judged against whoever holds the system RIGHT NOW, which is Phase
// 33 stage D's ruling and not an approximation - `commodityLegality` reads
// `systemOwnerFaction`, not the founding claim. So a border that moves while a
// pilot is in the lane moves the law with it, and this states that as a fact
// about `judgeHold` rather than trusting the layer underneath.
SOL_TEST(the_hold_is_judged_by_whoever_holds_the_system_right_now)
{
    Galaxy g;
    const std::uint32_t salvage = g.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage != 0xffff'ffffu);
    const std::uint32_t forbids = g.systemCalling(salvage, Legality::Contraband);
    SOL_REQUIRE(forbids != 0xffff'ffffu);
    const std::uint32_t hardliner = g.world.systemOwnerFaction(forbids);

    SOL_REQUIRE(g.world.enterSystem(forbids));
    SOL_REQUIRE(g.world.addPlayerCargo(salvage, 5.0f) == 5.0f);
    SOL_CHECK(g.world.judgeHold().worst == Legality::Contraband);

    // Hand the system to somebody with no opinion, without touching the hold.
    const std::uint32_t easygoing = g.world.systemOwnerFaction(g.tablelessSystem());
    SOL_REQUIRE(easygoing != hardliner);
    SOL_REQUIRE(g.world.factionSim().flipSystem(forbids, easygoing));

    const game::SpaceWorld::HoldJudgement after = g.world.judgeHold();
    SOL_CHECK(after.worst == Legality::Legal);
    SOL_CHECK(!after.holderHasTable);
    SOL_CHECK(g.world.playerCargo(salvage) == 5.0f); // nothing in the ship moved
}
