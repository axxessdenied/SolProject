// The shadow faction (engine plan Phase 37 stage B): a black market that is a
// FACTION rather than a place — no territory, no capital, no jurisdiction, and
// a row on the same table as the Solar Navy.
//
// ⚑⚑⚑⚑ EVERY ASSERTION IN THIS FILE IS A NEGATIVE, AND THAT IS THE STAGE. The
// dangerous line of the whole phase is one `[[faction]]` block: `factionCount`
// feeds `claimTerritory`, which hands out capitals and territory by that number,
// and the counting site was a TERNARY — "a pirate template, else a claimant" —
// so a third kind would have been counted as a major, handed a capital, and
// every system in the galaxy would have been redistributed around a faction
// whose entire definition is that it owns nothing. Two more sites had the same
// shape written as `if (pirate) continue`.
//
// ⚑⚑⚑ AND THE GALAXY IS ONLY HALF OF WHAT MUST NOT MOVE. `galaxy_golden_tests`
// pins the generated galaxy, which is a fact about t = 0; the faction sim runs
// forward from there, and it drew one roll per faction per decision interval
// from the same stream that rolls trader losses. A sixteenth faction that can
// never raid would still have displaced every loss after it, forever — so the
// shipped economy would have quietly re-run under a faction that does nothing.
// `a_faction_that_claims_nothing_takes_no_decision_and_spends_no_entropy` is the
// guard on that, and it is the reason `FactionAgentParams::territorial` exists.

#include "space_world.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/core/hash.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::FactionKind;

namespace {

// The one id this file is about, spelled once. ⚑ Named rather than found by
// scanning for `kind == Shadow`, which is Phase 36 stage B's rule and Phase 37
// stage A's repeat of it: a test that collects its own subject by reading back
// the thing under test cannot see the subject removed. Demote this row to
// `kind = "major"` and every check below fails loudly instead of vacuously.
constexpr const char* kShadowId = "sol.ninth_shift";

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

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

} // namespace

// ⚑⚑⚑⚑ THE HEADLINE, AND IT IS THE ONE THE TERNARY WOULD HAVE BROKEN SILENTLY.
// A claimant is a faction `claimTerritory` hands a capital to, and their number
// IS `GalaxyParams::factionCount`. This counts the def rows that ought to be
// claimants and asserts the generator agreed — so the check fails the moment a
// kind that claims nothing is counted as one, which is exactly the edit that
// would have redistributed the galaxy.
//
// ⚑ It is derived from the defs on both sides rather than written as "5",
// because a constant here would have to be edited by the same hand that broke
// it, and the edit would look like keeping the test up to date.
SOL_TEST(a_shadow_faction_is_not_counted_among_the_territory_claimants)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::uint32_t majors = 0;
    std::uint32_t templates = 0;
    std::uint32_t shadow = 0;
    for (const sol::assets::FactionDef& def : defs.factions()) {
        majors += def.kind == FactionKind::Major ? 1u : 0u;
        templates += def.kind == FactionKind::Pirate ? 1u : 0u;
        shadow += def.kind == FactionKind::Shadow ? 1u : 0u;
    }
    SOL_REQUIRE(shadow > 0); // or this suite is testing a galaxy without one
    std::printf("  %u major(s), %u clan template(s), %u shadow faction(s)\n", majors, templates, shadow);

    // The generator was told how many claimants there are, and the shadow rows
    // are not among them.
    SOL_CHECK(world.galaxyParams().factionCount == majors);
    SOL_CHECK(world.galaxyParams().pirateTemplateCount == templates);

    // And the bias rows are POSITIONAL — `factionStationBias[majorIndex]` — so a
    // shadow row let through would not merely add a dead row, it would slide
    // every major's character onto somebody else's territory.
    if (!world.galaxyParams().factionStationBias.empty()) {
        SOL_CHECK(world.galaxyParams().factionStationBias.size() == majors);
    }
}

// ⚑⚑⚑ NO CAPITAL, NO TERRITORY, NO JURISDICTION — CHECKED AGAINST THE GALAXY
// RATHER THAN AGAINST THE RULE THAT BUILT IT. The test above asks whether the
// generator was told the right number; this asks what it actually did with it,
// which is the different question a mis-wired count would still pass.
SOL_TEST(the_shadow_faction_holds_no_ground_anywhere_in_the_shipped_galaxy)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::uint32_t shadowIndex = world.shadowFactionIndex();
    SOL_REQUIRE(shadowIndex != sol::sim::kNoFaction);
    SOL_REQUIRE(shadowIndex < world.factions().size());
    SOL_CHECK(world.factions()[shadowIndex].defId == kShadowId);
    SOL_CHECK(world.factions()[shadowIndex].shadow());
    SOL_CHECK(!world.factions()[shadowIndex].pirate());

    std::uint32_t claimed = 0;
    std::uint32_t held = 0;
    std::uint32_t judged = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        claimed += world.galaxy().systems[s].factionIndex == shadowIndex ? 1u : 0u;
        held += world.systemOwnerFaction(s) == shadowIndex ? 1u : 0u;
        // The law of a place. A faction with no space has no table, which is why
        // Phase 37 stage A's illicit goods carry no legality label anywhere.
        const sol::assets::FactionDef* law = world.jurisdictionOf(s);
        judged += law != nullptr && law->id == kShadowId ? 1u : 0u;
    }
    std::printf("  %u system(s): shadow claims %u, holds %u, judges %u\n",
                static_cast<std::uint32_t>(world.galaxy().systems.size()),
                claimed,
                held,
                judged);
    SOL_CHECK(claimed == 0);
    SOL_CHECK(held == 0);
    SOL_CHECK(judged == 0);

    // ⚑ And it has no home to be driven back to, which is `FactionSim`'s own
    // derivation saying the same thing: the home system is the lowest-index
    // system holding a faction's founding claim, and there is none.
    SOL_CHECK(world.factionSim().homeSystem(shadowIndex) == sol::sim::kNoFaction);
}

// ⚑⚑⚑⚑ APPENDED LAST, AND THIS IS THE CHECK THAT KEEPS THE REST OF THE GAME
// POINTING AT THE RIGHT FACTIONS. A clan's index is `factionCount + clanIndex`,
// computed BY HAND in three places — `assignShadowOwners` says so under the
// comment "the arithmetic is the table lookup, without the table". Insert a
// shadow row among the majors and every one of those hand computations names
// the faction next door, silently, in a galaxy that still generates fine.
SOL_TEST(the_shadow_rows_sit_past_every_index_the_generator_computes_by_hand)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::uint32_t clanBase = world.galaxyParams().factionCount;
    const auto clans = static_cast<std::uint32_t>(world.galaxy().clans.size());
    SOL_REQUIRE(clans > 0);

    // Majors occupy [0, clanBase) in def order.
    std::uint32_t majorIndex = 0;
    for (const sol::assets::FactionDef& def : defs.factions()) {
        if (def.kind != FactionKind::Major) {
            continue;
        }
        SOL_REQUIRE(majorIndex < world.factions().size());
        SOL_CHECK(world.factions()[majorIndex].defId == def.id);
        SOL_CHECK(world.factions()[majorIndex].kind == FactionKind::Major);
        ++majorIndex;
    }
    SOL_CHECK(majorIndex == clanBase);

    // Clans occupy [clanBase, clanBase + clans) in galaxy order — the exact span
    // the hand arithmetic addresses.
    for (std::uint32_t c = 0; c < clans; ++c) {
        const std::uint32_t index = clanBase + c;
        SOL_REQUIRE(index < world.factions().size());
        SOL_CHECK(world.factions()[index].pirate());
        SOL_CHECK(world.factions()[index].name == world.galaxy().clans[c].name);
    }

    // And the shadow rows start exactly where the clans end, which is also the
    // identity `m_factionTable.size() - clans` USED to have and no longer does.
    SOL_CHECK(world.shadowFactionBase() == clanBase + clans);
    for (std::uint32_t f = world.shadowFactionBase(); f < world.factions().size(); ++f) {
        SOL_CHECK(world.factions()[f].shadow());
    }
    std::printf("  %u row(s): %u major(s), %u clan(s), then shadow at %u\n",
                static_cast<std::uint32_t>(world.factions().size()),
                clanBase,
                clans,
                world.shadowFactionBase());
}

// ⚑⚑⚑⚑ THE ONE THING `pirate = false` GETS WRONG FOR FREE, AND THE ONE IT GETS
// RIGHT. Unspecified pairs that differ in pirate-ness open at -60, so without
// the two authored rows in `factions.toml` the black market would have opened at
// open war with every clan in the galaxy — with the people who run its fences.
// The other half needs no fixing and is asserted so nobody "fixes" it: every
// major opens at 0, because a secret organisation is not at war with the law,
// it is hidden from it.
SOL_TEST(the_black_market_opens_friendly_to_the_clans_and_invisible_to_the_law)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::uint32_t shadowIndex = world.shadowFactionIndex();
    SOL_REQUIRE(shadowIndex != sol::sim::kNoFaction);

    std::uint32_t clanPairs = 0;
    std::uint32_t majorPairs = 0;
    std::uint32_t wars = 0;
    float worstClan = 100.0f;
    for (std::uint32_t f = 0; f < world.factions().size(); ++f) {
        if (f == shadowIndex) {
            continue;
        }
        const float relation = world.factionSim().relation(shadowIndex, f);
        wars += world.factionSim().atWar(shadowIndex, f) ? 1u : 0u;
        if (world.factions()[f].pirate()) {
            ++clanPairs;
            worstClan = relation < worstClan ? relation : worstClan;
            // The authored value, inherited from the template. The number that
            // matters is that it is nowhere near the -60 default.
            SOL_CHECK(relation > 0.0f);
        } else if (world.factions()[f].kind == FactionKind::Major) {
            ++majorPairs;
            SOL_CHECK(relation == 0.0f);
        }
    }
    std::printf("  %u clan pair(s) at worst %+.0f, %u major pair(s) at 0, %u war(s)\n",
                clanPairs,
                static_cast<double>(worstClan),
                majorPairs,
                wars);
    SOL_REQUIRE(clanPairs > 0); // ten clans inherit two authored rows
    SOL_REQUIRE(majorPairs > 0);
    SOL_CHECK(wars == 0);

    // ⚑ And the player opens NEUTRAL, not wary. `kClanInitialStanding` is -20
    // and it is keyed on pirate-ness, so a shadow faction takes the major's 0 —
    // which is right, and is the number Phase 37 stage E moves. A player who has
    // never smuggled has neither earned nor spent anything here.
    SOL_CHECK(world.factionSim().standing(shadowIndex) == 0.0f);
    SOL_CHECK(!world.factionSim().playerHostile(shadowIndex));
    SOL_CHECK(!world.factionSim().playerFriendly(shadowIndex));
}

// ⚑⚑⚑⚑ THE GUARD THAT KEEPS THE LIVE SIMULATION AS UNTOUCHED AS THE GALAXY, AND
// IT WAS BOUGHT BY A FAILURE. Adding this faction turned a bar test's own
// premise check from 8 reachable shortages to 0 — not because the galaxy moved
// (it is bit-identical; the golden digest never budged) but because the
// sixteenth agent drew one float per decision interval from the stream that also
// rolls trader losses, and displaced every loss after it for the rest of the run.
//
// ⚑⚑⚑ A ROLL WITH ONE POSSIBLE OUTCOME IS NOT A DECISION. `raidCandidates`
// seeds its search from systems the faction HOLDS, so a faction that holds
// nothing has an empty frontier and can reach nowhere, whatever it rolls. Both
// halves are asserted below, because "it takes no roll" and "it could not have
// acted on one" are different claims and only the second is a statement about
// the design.
SOL_TEST(a_faction_that_claims_nothing_takes_no_decision_and_spends_no_entropy)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::uint32_t shadowIndex = world.shadowFactionIndex();
    SOL_REQUIRE(shadowIndex != sol::sim::kNoFaction);

    // It could not act even if it were asked: no held ground, so no reach.
    std::vector<sol::sim::RaidCandidate> reach;
    world.factionSim().raidCandidates(world.galaxy(), shadowIndex, reach);
    SOL_CHECK(reach.empty());

    // Several decision intervals of sim time, drained the way GameContent drains
    // them. Every claimant is offered a decision every interval; the shadow
    // faction is offered none, ever.
    std::vector<sol::sim::FactionDecision> decisions;
    std::vector<std::uint32_t> seen(world.factions().size(), 0);
    std::uint32_t taken = 0;
    for (double elapsed = 0.0; elapsed < 300.0; elapsed += 1.0 / 30.0) {
        world.tick(1.0 / 30.0);
        decisions.clear();
        world.factionSim().takeDueDecisions(decisions);
        for (const sol::sim::FactionDecision& decision : decisions) {
            SOL_REQUIRE(decision.faction < seen.size());
            ++seen[decision.faction];
            ++taken;
            world.applyDefaultFactionDecision(decision);
        }
    }
    SOL_REQUIRE(taken > 0);
    std::uint32_t silent = 0;
    for (std::uint32_t f = 0; f < seen.size(); ++f) {
        if (seen[f] != 0) {
            continue;
        }
        ++silent;
        if (!world.factions()[f].shadow()) {
            std::printf("  %s took no decision in five sim minutes and is not shadow\n",
                        world.factions()[f].name.c_str());
        }
        SOL_CHECK(world.factions()[f].shadow());
    }
    const auto shadowRows = static_cast<std::uint32_t>(world.factions().size() - world.shadowFactionBase());
    std::printf("  %u decision(s) over five sim minutes; %u faction(s) took none, %u shadow row(s)\n",
                taken,
                silent,
                shadowRows);
    // ⚑ Exactly the shadow rows are silent — not "at least". A claimant that
    // stopped thinking would be the same defect pointed the other way, and this
    // is the line that would catch a `territorial` flag set from the wrong side.
    SOL_CHECK(silent == shadowRows);
    SOL_CHECK(seen[shadowIndex] == 0);

    // It ends where it began: no ground taken, none lost.
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        SOL_CHECK(world.systemOwnerFaction(s) != shadowIndex);
    }
}

// ⚑⚑ WHAT THE PLAYER IS TOLD IT IS, IN ONE WORD, FROM THE ONE PLACE THE THREE
// WORDS LIVE. Both screens that name a faction's kind — the dock's Factions tab
// and the console's `sol.factions` — carried their own `pirate ? "pirate clan" :
// "major"` until this stage. Two copies of a two-word vocabulary was survivable;
// two copies of a three-word one, where the third word is the game's only secret
// organisation, is how a black market gets labelled "major" on one screen.
SOL_TEST(the_three_kinds_have_three_words_and_the_black_market_is_a_syndicate)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    bool sawMajor = false;
    bool sawClan = false;
    bool sawShadow = false;
    for (const game::GameFaction& faction : world.factions()) {
        const std::string label = game::factionKindLabel(faction);
        switch (faction.kind) {
        case FactionKind::Major:
            sawMajor = true;
            SOL_CHECK(label == "major");
            break;
        case FactionKind::Pirate:
            sawClan = true;
            SOL_CHECK(label == "pirate clan");
            break;
        case FactionKind::Shadow:
            sawShadow = true;
            SOL_CHECK(label == "syndicate");
            break;
        }
    }
    SOL_CHECK(sawMajor);
    SOL_CHECK(sawClan);
    SOL_CHECK(sawShadow);

    // ⚑ And it is NOT the def keyword. `shadow` is what an author writes in
    // `factions.toml` and what the module family is called; a player reading the
    // Factions tab is being told what sort of organisation this is.
    const std::uint32_t shadowIndex = world.shadowFactionIndex();
    SOL_REQUIRE(shadowIndex != sol::sim::kNoFaction);
    const std::string label = game::factionKindLabel(world.factions()[shadowIndex]);
    SOL_CHECK(label != "shadow");
    std::printf("  the Factions tab reads '%s  %s, 0 system(s)'\n",
                world.factions()[shadowIndex].name.c_str(),
                label.c_str());
}

// ⚑⚑⚑ A DEF SET WITH NO SHADOW ROW STILL WORKS, AND SAYS SO WITH `kNoFaction`
// RATHER THAN WITH INDEX ZERO. Several suites in this directory build trimmed
// def sets and a mod may ship none, so "there is no black market" has to be a
// representable answer and not the first row of the table. ⚑ It is also the
// half of `shadowFactionIndex()` the shipped galaxy can never exercise, which is
// exactly the half a stage C reader would trip over first.
SOL_TEST(a_galaxy_with_no_shadow_faction_has_no_shadow_index)
{
    constexpr const char* kDefs = R"(
[[faction]]
id = "test.major"
name = "Test Major"
color = [0.5, 0.5, 0.5]
kind = "major"
builds_no = ["patrol", "raider", "trader"]
)";
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kDefs, std::strlen(kDefs), "shadow_faction_tests.toml", &error));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));
    SOL_CHECK(world.shadowFactionIndex() == sol::sim::kNoFaction);
    SOL_CHECK(world.shadowFactionBase() == sol::sim::kNoFaction);
    SOL_REQUIRE(!world.factions().empty());
    for (const game::GameFaction& faction : world.factions()) {
        SOL_CHECK(!faction.shadow());
    }
}

// ⚑⚑⚑⚑ THE GUARD ON THE TWO GENERATION ARMS THE SHIPPED CONTENT CANNOT
// REACH, AND IT IS HERE BECAUSE WRITING THEM WAS NOT ENOUGH. `factions.toml`
// puts the shadow row LAST, so `!= Major` and `== Pirate` give the same answer
// at both the station-bias loop and `majorIndexOf` for every galaxy this game
// ships - both arms are correct and both are unexercised, which is the
// vacuously-true shape Phase 34 stage E named and Phase 37 stage A met again.
//
// ⚑⚑⚑ SO THE ASSERTION IS ABOUT DEF ORDER RATHER THAN ABOUT A NUMBER: the
// same galaxy, generated twice from def sets that differ ONLY in where the
// shadow row sits, must come out identical. Both arms count POSITIONS among the
// claimants - `factionStationBias[majorIndex]` is who builds what, and
// `majorIndexOf` is whose capital an authored system becomes - so a shadow row
// let through at either site slides every major after it onto somebody else's
// territory, and moving the row is what makes that visible.
SOL_TEST(where_the_shadow_row_sits_in_def_order_changes_nothing_about_the_galaxy)
{
    // Two majors with real characters, an authored capital for the SECOND of
    // them (so a one-place slide is expressible), and the shadow row moved from
    // the end of the file to the front.
    constexpr const char* kTail = R"(
[[faction]]
id = "test.alpha"
name = "Alpha Combine"
color = [0.2, 0.4, 1.0]
kind = "major"
station_bias = ["test.agri:2.5", "test.mine:0.3"]
builds_no = ["patrol", "raider", "trader"]

[[faction]]
id = "test.beta"
name = "Beta Union"
color = [0.9, 0.7, 0.2]
kind = "major"
station_bias = ["test.agri:0.3", "test.mine:2.5"]
builds_no = ["patrol", "raider", "trader"]

[[system]]
id = "test.beta_seat"
name = "Beta Seat"
placement = "at_system"
at_system = "test.beta"

[[station]]
id = "test.agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5

[[station]]
id = "test.mine"
name = "Mining Outpost"
weight_core = 0.5
weight_frontier = 1.5
weight_fringe = 2.0
)";
    constexpr const char* kShadowRow = R"(
[[faction]]
id = "test.shadow"
name = "Test Syndicate"
color = [0.5, 0.5, 0.5]
kind = "shadow"
builds_no = ["patrol", "raider", "trader"]
)";

    const std::string last = std::string(kTail) + kShadowRow;
    const std::string first = std::string(kShadowRow) + kTail;

    const auto digestOf = [](const std::string& toml, std::uint32_t& outClaimants, std::uint64_t& out) {
        DefDatabase defs;
        std::string error;
        SOL_REQUIRE(defs.mergeToml(toml.c_str(), toml.size(), "shadow_faction_tests.toml", &error));
        game::SpaceWorld world;
        world.spawn(game::kDefaultUniverseSeed);
        world.applyDefs(defs);
        SOL_REQUIRE(world.generateUniverse(defs));
        outClaimants = world.galaxyParams().factionCount;

        std::uint64_t digest = sol::core::kFnvOffsetBasis;
        for (const sol::sim::SystemSpec& system : world.galaxy().systems) {
            for (const char c : system.name) {
                digest = sol::core::hashCombine(digest, static_cast<std::uint64_t>(c));
            }
            digest = sol::core::hashCombine(digest, system.factionIndex);
            for (const sol::sim::StationSpec& station : system.stations) {
                digest = sol::core::hashCombine(digest, station.archetype);
            }
        }
        out = digest;
    };

    std::uint32_t claimantsLast = 0;
    std::uint32_t claimantsFirst = 0;
    std::uint64_t withRowLast = 0;
    std::uint64_t withRowFirst = 0;
    digestOf(last, claimantsLast, withRowLast);
    digestOf(first, claimantsFirst, withRowFirst);
    std::printf("  shadow row last: %u claimant(s), digest %016llx\n",
                claimantsLast,
                static_cast<unsigned long long>(withRowLast));
    std::printf("  shadow row first: %u claimant(s), digest %016llx\n",
                claimantsFirst,
                static_cast<unsigned long long>(withRowFirst));
    SOL_CHECK(claimantsLast == 2);
    SOL_CHECK(claimantsFirst == 2);
    SOL_CHECK(withRowLast == withRowFirst);
}
