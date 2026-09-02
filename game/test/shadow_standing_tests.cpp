// The opposed axis (engine plan Phase 37 stage E): shadow standing earned by
// exactly the acts that cost standing with the law.
//
// ⚑⚑⚑⚑ THE WHOLE STAGE IS ONE SENTENCE APPLIED IN TWO PLACES. *Where a good is
// contraband to the jurisdiction you are standing in, moving it moves two
// reputations in opposite directions.* At a stop that is `inspectionSeize`; at a
// counter it is `recordPlayerTrade`. There is no rate between them - the shadow
// gain is the law's loss negated - so the guards below state an IDENTITY rather
// than a tuned number.
//
// ⚑⚑⚑⚑ AND THE STAGE OPENED WITH A DATA FINDING THAT INVERTED IT. The probe at
// the top of this file was written to ask which fences sit in law space, and it
// answered something nobody had asked: `sol.stims` and `sol.hot_parts` were
// contraband in ZERO of 81 systems. Stage A authored them as
// `GoodsClass::Illicit`, which is a fact about which WAREHOUSE holds a crate,
// and no legality table anywhere named them - so the phase's own exit sentence
// ("carry it into Hegemony space and lose it to the stop Phase 36 built") could
// not happen, `judgeHold` returned Clean on a hold carrying the black market's
// entire catalogue, and the suite was green over it. *A class is not a law, and
// the stage that authored the class never had to ask the law anything.*
//
// ⚑⚑⚑ THE USER'S TWO RULINGS (2026-09-02), BOTH TAKEN BEFORE A LINE WAS WRITTEN
// AND BOTH OFF THE PROBE'S NUMBERS:
//   1. The three majors that already keep a table ban them - Navy, Ironstar,
//      Ascendancy - which is 25 of 81 systems. The Freight Guild's 24 systems
//      and every clan stay with no opinion, so 5 of the 8 fences sit in space
//      that does not care and 3 do.
//   2. One key, two signs: the same magnitude, opposite directions, so shadow
//      standing is unearnable without spending somebody else's goodwill.

#include "asset_paths.hpp"
#include "content.hpp"
#include "space_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/faction_sim.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::GoodsClass;
using sol::assets::Legality;
using sol::assets::ModuleFamily;

namespace {

using Notice = game::SpaceWorld::NoticeReason;
using Outcome = game::SpaceWorld::InspectionOutcome;
using Verdict = game::SpaceWorld::InspectionVerdict;
using Post = game::SpaceWorld::PatrolPost;

constexpr std::uint32_t kNone = 0xffffffffu;

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

[[nodiscard]] const sol::assets::FactionDef* defFor(const DefDatabase& defs, const std::string& id)
{
    for (const sol::assets::FactionDef& candidate : defs.factions()) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

// The shipped galaxy through the real content path INCLUDING the mod layer, for
// the reason `verdict_tests.cpp` gives: without it this is an 81-system galaxy
// where the running game has 85.
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

    [[nodiscard]] std::uint32_t shadow() const { return world.shadowFactionIndex(); }

    [[nodiscard]] float standing(std::uint32_t faction) const { return world.factionSim().standing(faction); }

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
        return kNone;
    }

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

    // `verdict_tests.cpp`'s, verbatim, including the mark - see its comment on
    // why `verdict == None` is the wrong wait condition.
    bool stopAndSettle(std::uint32_t system, Notice reason = Notice::RandomCheck, double metres = 600.0)
    {
        const Post patrol = standOff(system, metres);
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
            return false;
        }
        content.tick(kStep);
        world.tick(kStep);
        return world.lastInspection().atWorldSeconds > mark &&
               world.lastInspection().verdict != Verdict::None;
    }

    // Docks at a fence and reports which system. `wantBanned` picks between the
    // three fences whose holder outlaws the goods and the five whose holder has
    // no opinion - the two halves of ruling 1, and the two halves of the axis.
    [[nodiscard]] std::uint32_t dockAtFence(std::uint32_t commodity, bool wantBanned)
    {
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            const sol::sim::SystemSpec& system = world.galaxy().systems[s];
            for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
                if (!hasShadowModule(world, content.defs(), s, t)) {
                    continue;
                }
                const bool banned = world.commodityLegality(s, commodity) == Legality::Contraband;
                if (banned != wantBanned) {
                    continue;
                }
                if (!world.enterSystem(s) || !world.warpToStationOffset(t, {150.0, 0.0, 0.0}) ||
                    !world.tryDockNearestStation(2000.0)) {
                    continue;
                }
                return s;
            }
        }
        return kNone;
    }
};

} // namespace

// ⚑⚑⚑⚑ THE PROBE, AND IT COMES FIRST BECAUSE THE AXIS HAS A PRICE ONLY IF THE
// LAW CAN SEE THE ACT. Whether it can is a fact about who holds the eight fence
// systems and about whether anybody's table names the goods at all. Run before a
// line of stage E was written, it reported `0 where illicit is contraband` -
// which is the finding this whole stage turns on. It stays as a printout, and
// the assertions under it are the two halves of ruling 1.
SOL_TEST(probe_what_the_law_says_about_the_black_markets_own_goods)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::vector<std::uint32_t> illicit;
    for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
        if (world.commodityClass(c) == GoodsClass::Illicit) {
            illicit.push_back(c);
        }
    }
    std::printf("  %zu illicit commodit(ies)\n", illicit.size());

    std::uint32_t fences = 0;
    std::uint32_t bansIllicit = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (!hasShadowModule(world, defs, s, t)) {
                continue;
            }
            ++fences;
            const std::uint32_t owner = world.systemOwnerFaction(s);
            const sol::assets::FactionDef* table = world.jurisdictionOf(s);
            const bool declares =
                table != nullptr && (!table->contraband.empty() || !table->restricted.empty());
            std::uint32_t banned = 0;
            for (const std::uint32_t c : illicit) {
                banned += world.commodityLegality(s, c) == Legality::Contraband ? 1u : 0u;
            }
            bansIllicit += banned != 0 ? 1u : 0u;
            std::printf("    %-24s in %-12s owner=%-20s table=%-4s %u/%zu illicit banned\n",
                        system.stations[t].name.c_str(),
                        system.name.c_str(),
                        owner < world.factions().size() ? world.factions()[owner].name.c_str() : "(nobody)",
                        declares ? "yes" : "NONE",
                        banned,
                        illicit.size());
        }
    }

    std::vector<std::uint32_t> held(world.factions().size() + 1, 0);
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const std::uint32_t owner = world.systemOwnerFaction(s);
        if (owner < held.size()) {
            ++held[owner];
        }
    }
    for (std::uint32_t f = 0; f < world.factions().size(); ++f) {
        if (world.factions()[f].pirate()) {
            continue;
        }
        const sol::assets::FactionDef* def = defFor(defs, world.factions()[f].defId);
        if (def == nullptr) {
            continue;
        }
        std::printf("    %-22s holds %2u system(s), contraband=%zu restricted=%zu\n",
                    world.factions()[f].name.c_str(),
                    held[f],
                    def->contraband.size(),
                    def->restricted.size());
    }

    std::size_t bannedSystems = 0;
    for (const std::uint32_t c : illicit) {
        std::size_t banning = 0;
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            banning += world.commodityLegality(s, c) == Legality::Contraband ? 1u : 0u;
        }
        bannedSystems = std::max(bannedSystems, banning);
        std::printf("    %-16s contraband in %zu of %zu system(s)\n",
                    world.commodityIds()[c].c_str(),
                    banning,
                    world.galaxy().systems.size());
    }
    std::printf(
        "  %u fence(s), %u of them where the black market's own stock is a crime\n", fences, bansIllicit);

    SOL_REQUIRE(fences > 0);
    SOL_REQUIRE(!illicit.empty());
    // Ruling 1, both halves. Somebody must ban it or the axis has no act to
    // mirror; somebody must NOT, or the goods class has become the law and
    // `contraband_tests.cpp` says at length why that is the wrong game.
    SOL_CHECK(bansIllicit > 0);
    SOL_CHECK(bansIllicit < fences);
    SOL_CHECK(bannedSystems > 0);
    SOL_CHECK(bannedSystems < world.galaxy().systems.size());
}

// ⚑⚑⚑⚑ THE STAGE, IN ONE TEST: ONE ACT, TWO REPUTATIONS, OPPOSITE DIRECTIONS.
// This is the phase's exit sentence made assertable - and note that it could not
// have been written before this stage's data change, because with nobody banning
// stims the stop below judges the hold Clean and nothing moves at all.
SOL_TEST(a_seizure_spends_the_laws_goodwill_and_buys_exactly_that_much_with_the_shadow)
{
    Galaxy g;
    const std::uint32_t stims = g.world.commodityIndex("sol.stims");
    SOL_REQUIRE(stims != kNone);
    SOL_REQUIRE(g.shadow() != sol::sim::kNoFaction);

    const std::uint32_t forbids = g.systemCalling(stims, Legality::Contraband);
    SOL_REQUIRE(forbids != kNone);
    SOL_REQUIRE(g.world.enterSystem(forbids));
    const std::uint32_t law = g.world.systemOwnerFaction(forbids);
    SOL_REQUIRE(law < g.world.factions().size());

    SOL_REQUIRE(g.world.addPlayerCargo(stims, 8.0f) == 8.0f);
    const float lawBefore = g.standing(law);
    const float shadowBefore = g.standing(g.shadow());

    SOL_REQUIRE(g.stopAndSettle(forbids));
    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Seizure);
    SOL_CHECK(g.world.playerCargo(stims) == 0.0f); // taken

    const float lawSpent = lawBefore - g.standing(law);
    const float shadowGained = g.standing(g.shadow()) - shadowBefore;
    std::printf("  %s %+.1f -> %.1f, The Ninth Shift %+.1f -> %.1f\n",
                g.world.factions()[law].name.c_str(),
                -static_cast<double>(lawSpent),
                static_cast<double>(g.standing(law)),
                static_cast<double>(shadowGained),
                static_cast<double>(g.standing(g.shadow())));

    SOL_CHECK(lawSpent > 0.0f);
    SOL_CHECK(shadowGained > 0.0f);
    // The identity, which is the ruling. Not "roughly proportional", and not a
    // rate somebody has to keep in step with `contrabandStanding`.
    SOL_CHECK(std::abs(shadowGained - lawSpent) < 0.001f);
    SOL_CHECK(std::abs(lawSpent + g.world.verdictParams().contrabandStanding) < 0.001f);
}

// ⚑⚑⚑⚑ THE MUTATION THAT NEARLY SHIPPED, WRITTEN AS ITS OWN TEST. Keying the
// shadow credit off the CALL SITE rather than off what was in the hold pays a
// pilot who fled an empty-handed random check - `applyDefaultVerdict` answers
// any runner with `inspectionSeize`, whatever the hold held. That is free
// standing with the one faction whose standing is supposed to cost something,
// and it is reachable without ever touching contraband, which is precisely the
// phase risk register's failure mode.
SOL_TEST(running_with_a_clean_hold_costs_the_law_and_earns_the_shadow_nothing)
{
    Galaxy g;
    SOL_REQUIRE(g.shadow() != sol::sim::kNoFaction);
    const std::uint32_t stims = g.world.commodityIndex("sol.stims");
    SOL_REQUIRE(stims != kNone);
    const std::uint32_t forbids = g.systemCalling(stims, Legality::Contraband);
    SOL_REQUIRE(forbids != kNone);
    SOL_REQUIRE(g.world.enterSystem(forbids));
    const std::uint32_t law = g.world.systemOwnerFaction(forbids);
    SOL_REQUIRE(law < g.world.factions().size());

    // Nothing aboard at all.
    const float lawBefore = g.standing(law);
    const float shadowBefore = g.standing(g.shadow());
    const Post patrol = g.standOff(forbids, 600.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::RandomCheck));
    g.world.endInspection(Outcome::Ran, "test: ran");
    constexpr double kStep = 1.0 / 60.0;
    g.content.tick(kStep);
    g.world.tick(kStep);

    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Fled);
    std::printf("  fled clean: law %+.1f, shadow %+.1f\n",
                static_cast<double>(g.standing(law) - lawBefore),
                static_cast<double>(g.standing(g.shadow()) - shadowBefore));
    // Running is still the crime - Phase 36 ruling 2 - so the law is still out
    // of pocket...
    SOL_CHECK(g.standing(law) < lawBefore);
    // ...and the black market has no idea who you are.
    SOL_CHECK(std::abs(g.standing(g.shadow()) - shadowBefore) < 0.001f);
}

// The other arm of the same branch: run with their crates aboard and they hear
// about it, because they are the people who sold them to you.
SOL_TEST(running_with_their_cargo_aboard_is_worth_the_same_as_losing_it)
{
    Galaxy g;
    SOL_REQUIRE(g.shadow() != sol::sim::kNoFaction);
    const std::uint32_t stims = g.world.commodityIndex("sol.stims");
    SOL_REQUIRE(stims != kNone);
    const std::uint32_t forbids = g.systemCalling(stims, Legality::Contraband);
    SOL_REQUIRE(forbids != kNone);
    SOL_REQUIRE(g.world.enterSystem(forbids));
    const std::uint32_t law = g.world.systemOwnerFaction(forbids);
    SOL_REQUIRE(law < g.world.factions().size());

    SOL_REQUIRE(g.world.addPlayerCargo(stims, 6.0f) == 6.0f);
    const float lawBefore = g.standing(law);
    const float shadowBefore = g.standing(g.shadow());
    const Post patrol = g.standOff(forbids, 600.0);
    SOL_REQUIRE(g.world.beginInspection(patrol.pilotIndex, Notice::RandomCheck));
    g.world.endInspection(Outcome::Ran, "test: ran");
    constexpr double kStep = 1.0 / 60.0;
    g.content.tick(kStep);
    g.world.tick(kStep);

    SOL_CHECK(g.world.lastInspection().verdict == Verdict::Fled);
    // A patrol cannot lift a crate off a ship that is not there (Phase 36).
    SOL_CHECK(g.world.playerCargo(stims) == 6.0f);
    const float lawSpent = lawBefore - g.standing(law);
    const float shadowGained = g.standing(g.shadow()) - shadowBefore;
    std::printf("  fled loaded: law %+.1f, shadow %+.1f\n",
                -static_cast<double>(lawSpent),
                static_cast<double>(shadowGained));
    SOL_CHECK(std::abs(shadowGained - lawSpent) < 0.001f);
    SOL_CHECK(std::abs(lawSpent + g.world.verdictParams().fledStanding) < 0.001f);
}

// ⚑⚑⚑⚑ THE COUNTER'S HALF, AND IT IS A BUG FIX AS MUCH AS A FEATURE. The back
// room's rows go through `playerBuy`/`playerSell` like every other row, and
// those credited `systemOwnerFaction` UNCONDITIONALLY from Phase 8 until this
// stage - so buying contraband in a Hegemony-held station's back room earned
// HEGEMONY GOODWILL. The law thanking you for smuggling in its own space.
SOL_TEST(buying_their_stock_where_it_is_banned_moves_both_numbers_and_the_right_way)
{
    Galaxy g;
    SOL_REQUIRE(g.shadow() != sol::sim::kNoFaction);
    const std::uint32_t stims = g.world.commodityIndex("sol.stims");
    SOL_REQUIRE(stims != kNone);

    const std::uint32_t system = g.dockAtFence(stims, true);
    SOL_REQUIRE(system != kNone);
    const std::uint32_t law = g.world.systemOwnerFaction(system);
    SOL_REQUIRE(law < g.world.factions().size());
    g.world.addCredits(100000.0);

    const float lawBefore = g.standing(law);
    const float shadowBefore = g.standing(g.shadow());
    const sol::sim::TradeResult bought = g.world.playerBuy(stims, 20.0f);
    SOL_REQUIRE(bought.units > 0.0f);

    const float lawSpent = lawBefore - g.standing(law);
    const float shadowGained = g.standing(g.shadow()) - shadowBefore;
    std::printf("  %.1f units for %.0f cr at a fence in %s: law %+.2f, shadow %+.2f\n",
                static_cast<double>(bought.units),
                bought.credits,
                g.world.galaxy().systems[system].name.c_str(),
                -static_cast<double>(lawSpent),
                static_cast<double>(shadowGained));

    SOL_CHECK(shadowGained > 0.0f);
    SOL_CHECK(lawSpent > 0.0f); // NOT goodwill, which is what it used to be
    SOL_CHECK(std::abs(shadowGained - lawSpent) < 0.001f);
}

// ⚑⚑⚑ AND THE FIVE FENCES WHERE NOBODY OBJECTS EARN NOTHING AT ALL, WHICH IS
// THE RISK REGISTER'S TEST WRITTEN OUT. "If shadow standing has no price, the
// allegiance is a bonus" - so a back room in Freight Guild or clan space, where
// the same crate is nobody's business, must not be a place to farm it. A
// transaction no one with an opinion witnessed changes no one's opinion.
SOL_TEST(the_same_purchase_where_nobody_objects_earns_nobody_anything)
{
    Galaxy g;
    SOL_REQUIRE(g.shadow() != sol::sim::kNoFaction);
    const std::uint32_t stims = g.world.commodityIndex("sol.stims");
    SOL_REQUIRE(stims != kNone);

    const std::uint32_t system = g.dockAtFence(stims, false);
    SOL_REQUIRE(system != kNone);
    const std::uint32_t law = g.world.systemOwnerFaction(system);
    g.world.addCredits(100000.0);

    const float lawBefore = law < g.world.factions().size() ? g.standing(law) : 0.0f;
    const float shadowBefore = g.standing(g.shadow());
    const sol::sim::TradeResult bought = g.world.playerBuy(stims, 20.0f);
    SOL_REQUIRE(bought.units > 0.0f);

    std::printf("  %.1f units for %.0f cr at a fence in %s (%s): law %+.2f, shadow %+.2f\n",
                static_cast<double>(bought.units),
                bought.credits,
                g.world.galaxy().systems[system].name.c_str(),
                law < g.world.factions().size() ? g.world.factions()[law].name.c_str() : "nobody",
                law < g.world.factions().size() ? static_cast<double>(g.standing(law) - lawBefore) : 0.0,
                static_cast<double>(g.standing(g.shadow()) - shadowBefore));

    SOL_CHECK(std::abs(g.standing(g.shadow()) - shadowBefore) < 0.001f);
    if (law < g.world.factions().size()) {
        // And no goodwill either: they did not see it, so it buys nothing.
        SOL_CHECK(std::abs(g.standing(law) - lawBefore) < 0.001f);
    }
}

// Ordinary honest trade is untouched, which is the assertion that keeps the
// clause above from having quietly deleted commerce goodwill for everybody.
SOL_TEST(an_ordinary_cargo_still_earns_the_owners_goodwill)
{
    Galaxy g;
    const std::uint32_t food = g.world.commodityIndex("sol.food");
    SOL_REQUIRE(food != kNone);
    const std::uint32_t stims = g.world.commodityIndex("sol.stims");
    SOL_REQUIRE(stims != kNone);

    // Any fence will do - the point is the crate, not the counter.
    const std::uint32_t system = g.dockAtFence(stims, true);
    SOL_REQUIRE(system != kNone);
    const std::uint32_t law = g.world.systemOwnerFaction(system);
    SOL_REQUIRE(law < g.world.factions().size());
    SOL_REQUIRE(g.world.commodityLegality(system, food) == Legality::Legal);
    g.world.addCredits(100000.0);

    const float lawBefore = g.standing(law);
    const float shadowBefore = g.standing(g.shadow());
    const sol::sim::TradeResult bought = g.world.playerBuy(food, 20.0f);
    SOL_REQUIRE(bought.units > 0.0f);
    std::printf("  %.1f units of food for %.0f cr: law %+.2f, shadow %+.2f\n",
                static_cast<double>(bought.units),
                bought.credits,
                static_cast<double>(g.standing(law) - lawBefore),
                static_cast<double>(g.standing(g.shadow()) - shadowBefore));
    SOL_CHECK(g.standing(law) > lawBefore);
    SOL_CHECK(std::abs(g.standing(g.shadow()) - shadowBefore) < 0.001f);
}

// ⚑⚑⚑⚑ THE PHASE'S PLAYTEST, ARITHMETIC RATHER THAN OPINION: IS +12 WITH THE
// NINTH SHIFT WORTH -12 WITH THE HEGEMONY? Phase 36 left four constants nobody
// coordinated - a seizure costs 12, `hostileThreshold` is -30, a major opens you
// at 0, and player standing NEVER DRIFTS - and the Null Signature Suite wants
// +25. Put those in one room and the answer is a route rather than a number:
// three seizures is what the Suite costs, three in ONE jurisdiction is -36 and
// they shoot you, and three spread across three is -12 each and all of them
// still talk to you. *The axis teaches you to spread your crimes across borders,
// and nothing anywhere says so.*
SOL_TEST(the_suite_costs_three_seizures_and_they_have_to_be_in_three_jurisdictions)
{
    Galaxy g;
    SOL_REQUIRE(g.shadow() != sol::sim::kNoFaction);
    const float seizure = -g.world.verdictParams().contrabandStanding;
    const float hostile = g.world.factionSim().params().hostileThreshold;
    SOL_REQUIRE(seizure > 0.0f);

    // What the one locked row in the game asks for.
    const sol::assets::ComponentDef* suite = g.content.defs().findComponent("sol.signature_dampener_mk2");
    SOL_REQUIRE(suite != nullptr);
    const float wants = suite->gate.minRep;
    const int seizures =
        static_cast<int>(std::ceil(static_cast<double>(wants) / static_cast<double>(seizure)));
    std::printf("  the Suite wants %+.0f; a seizure is worth %+.0f, so %d of them\n",
                static_cast<double>(wants),
                static_cast<double>(seizure),
                seizures);
    std::printf("  %d in one jurisdiction is %.0f against a hostile threshold of %.0f\n",
                seizures,
                -static_cast<double>(seizures) * static_cast<double>(seizure),
                static_cast<double>(hostile));

    SOL_CHECK(wants > 0.0f);
    // It costs more than one act, or the allegiance is free.
    SOL_CHECK(seizures > 1);
    // And it cannot be paid in one jurisdiction without that jurisdiction
    // turning on you - which is what makes it a route decision.
    SOL_CHECK(-static_cast<float>(seizures) * seizure < hostile);
    // While one apiece, spread across that many, stays the right side of the
    // line - so the route exists and the phase's question has an answer.
    SOL_CHECK(-seizure > hostile);
}

// ⚑⚑⚑⚑ AND THE DOOR ACTUALLY OPENS, WHICH IS THE ONLY REASON ANY OF THIS IS
// CONTENT. Phase 37 stage C shipped the Null Signature Suite visible and
// unbuyable at every fence in the galaxy and said so in writing: "if stage E
// slips, this line is the thing to revisit - a permanently unbuyable row is dead
// content, and it is only not dead because the next stage is what opens it."
// This is that stage, so this is that assertion: earn the standing the way the
// game makes you earn it, and the row buys.
SOL_TEST(the_one_locked_row_in_the_game_opens_when_the_standing_is_earned)
{
    Galaxy g;
    SOL_REQUIRE(g.shadow() != sol::sim::kNoFaction);
    const std::uint32_t stims = g.world.commodityIndex("sol.stims");
    SOL_REQUIRE(stims != kNone);
    SOL_REQUIRE(g.dockAtFence(stims, true) != kNone);
    g.world.addCredits(100000.0);

    // Refused first, at standing 0 - which is where every new pilot starts and
    // is exactly what stage C shipped.
    std::string error;
    SOL_CHECK(!g.world.buyFitting("sol.signature_dampener_mk2", nullptr, &error));
    std::printf("  at %+.1f: %s\n", static_cast<double>(g.standing(g.shadow())), error.c_str());

    // Now pay for it in the only currency the game takes. Seizures, not a
    // setter: the point is that the axis is what opens it.
    const std::uint32_t forbids = g.systemCalling(stims, Legality::Contraband);
    SOL_REQUIRE(forbids != kNone);
    int seizures = 0;
    for (int attempt = 0; attempt < 8 && g.standing(g.shadow()) < 25.0f; ++attempt) {
        SOL_REQUIRE(g.world.enterSystem(forbids));
        SOL_REQUIRE(g.world.addPlayerCargo(stims, 4.0f) == 4.0f);
        if (!g.stopAndSettle(forbids)) {
            break;
        }
        if (g.world.lastInspection().verdict == Verdict::Seizure) {
            ++seizures;
        }
    }
    std::printf("  %d seizure(s) later: The Ninth Shift %+.1f\n",
                seizures,
                static_cast<double>(g.standing(g.shadow())));
    SOL_REQUIRE(g.standing(g.shadow()) >= 25.0f);

    // Back to a fence, and this time it sells.
    SOL_REQUIRE(g.dockAtFence(stims, true) != kNone);
    g.world.addCredits(100000.0);
    error.clear();
    const bool bought = g.world.buyFitting("sol.signature_dampener_mk2", nullptr, &error);
    if (!bought) {
        std::printf("  still refused: %s\n", error.c_str());
    }
    SOL_CHECK(bought);
}
