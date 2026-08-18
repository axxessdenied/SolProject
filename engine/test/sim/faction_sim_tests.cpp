#include <sol/sim/faction_sim.hpp>
#include <sol/sim/universe.hpp>

#include <sol/core/serialize.hpp>
#include <sol/test/test.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using sol::sim::Economy;
using sol::sim::EconomyArchetype;
using sol::sim::EconomyCommodity;
using sol::sim::EconomyParams;
using sol::sim::FactionAgentParams;
using sol::sim::FactionDecision;
using sol::sim::FactionSim;
using sol::sim::FactionSimParams;
using sol::sim::Galaxy;
using sol::sim::kNoFaction;
using sol::sim::RaidCandidate;
using sol::sim::SystemSpec;
using sol::sim::TraderPhase;

namespace {

// Three systems in a chain, one station each: 0 (major A) - 1 (major B) -
// 2 (pirate clan C). A and B are rivals (-35); C hates both (-60 => war).
Galaxy chainGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 11;
    for (std::uint32_t i = 0; i < 3; ++i) {
        SystemSpec system;
        system.name = std::string("S") + static_cast<char>('0' + i);
        system.factionIndex = i;
        system.planets.push_back({.name = "P", .position = {}, .radius = 1.0e6});
        system.stations.push_back({.name = "St", .archetype = 0, .position = {}});
        if (i > 0) {
            system.gates.push_back({.toSystem = i - 1, .position = {}});
        }
        if (i < 2) {
            system.gates.push_back({.toSystem = i + 1, .position = {}});
        }
        galaxy.systems.push_back(std::move(system));
    }
    galaxy.links = {{0, 1}, {1, 2}};
    galaxy.clans.push_back({.name = "S2 Raiders", .templateIndex = 0, .seed = 3, .homeSystem = 2});
    return galaxy;
}

FactionSimParams chainParams()
{
    FactionSimParams params;
    params.agents = {FactionAgentParams{.aggression = 1.0f, .forgiveness = 0.5f},
                     FactionAgentParams{.aggression = 0.0f, .forgiveness = 0.5f},
                     FactionAgentParams{.aggression = 1.0f, .forgiveness = 0.1f,
                                        .pirate = true}};
    // Symmetric 3x3: A-B -35, A-C -60, B-C -60.
    params.baselineRelations = {0.0f, -35.0f, -60.0f, //
                                -35.0f, 0.0f, -60.0f, //
                                -60.0f, -60.0f, 0.0f};
    params.initialStandings = {0.0f, 0.0f, -20.0f};
    params.decisionInterval = 60.0;
    return params;
}

EconomyParams oneCommodityParams()
{
    EconomyParams params;
    params.commodities = {EconomyCommodity{.basePrice = 10.0f}};
    EconomyArchetype archetype;
    archetype.production = {1.0f};
    archetype.stockCapacity = 1'000.0f;
    params.archetypes = {archetype};
    params.traderCount = 1;
    return params;
}

// Four systems in a chain for the territory tests (Phase 8u): 0 - 1 - 2 - 3,
// with faction 0 holding TWO systems (0 and 1) so that one of them is not its
// home and can therefore change hands. Faction 1 holds 2, pirate clan 2 holds
// 3. Everyone is at war with everyone, so reach is the only thing gating a
// raid. raidReach = 1 by default, which is what makes "reach follows the
// border" observable rather than incidental.
Galaxy territoryGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 17;
    constexpr std::uint32_t kOwners[4] = {0, 0, 1, 2};
    for (std::uint32_t i = 0; i < 4; ++i) {
        SystemSpec system;
        system.name = std::string("T") + static_cast<char>('0' + i);
        system.factionIndex = kOwners[i];
        system.planets.push_back({.name = "P", .position = {}, .radius = 1.0e6});
        system.stations.push_back({.name = "St", .archetype = 0, .position = {}});
        if (i > 0) {
            system.gates.push_back({.toSystem = i - 1, .position = {}});
        }
        if (i < 3) {
            system.gates.push_back({.toSystem = i + 1, .position = {}});
        }
        galaxy.systems.push_back(std::move(system));
    }
    galaxy.links = {{0, 1}, {1, 2}, {2, 3}};
    galaxy.clans.push_back({.name = "T3 Raiders", .templateIndex = 0, .seed = 3, .homeSystem = 3});
    return galaxy;
}

FactionSimParams territoryParams()
{
    FactionSimParams params = chainParams();
    params.baselineRelations = {0.0f, -60.0f, -60.0f, //
                                -60.0f, 0.0f, -60.0f, //
                                -60.0f, -60.0f, 0.0f};
    params.raidReach = 1;
    return params;
}

// Raids a system until it changes hands, so a test asserts the outcome rather
// than counting floating-point increments. Returns false if it never flips.
bool raidUntilFlipped(FactionSim& sim, const Galaxy& galaxy, std::uint32_t attacker,
                      std::uint32_t system)
{
    for (int i = 0; i < 32 && sim.systemOwner(system) != attacker; ++i) {
        if (!sim.commitRaid(galaxy, nullptr, attacker, system)) {
            return false;
        }
    }
    return sim.systemOwner(system) == attacker;
}

} // namespace

SOL_TEST(faction_sim_init_war_flags_and_candidates)
{
    const Galaxy galaxy = chainGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, chainParams(), 42);

    SOL_CHECK(sim.factionCount() == 3);
    SOL_CHECK(sim.relation(0, 1) == -35.0f);
    SOL_CHECK(sim.relation(1, 0) == -35.0f);
    SOL_CHECK(!sim.atWar(0, 1));    // -35 is hostile but above the war line
    SOL_CHECK(sim.atWar(0, 2));     // -60 opens at war
    SOL_CHECK(sim.atWar(2, 1));

    // A reaches B's system (1 jump) and C's (2 jumps); both are raidable.
    std::vector<RaidCandidate> candidates;
    sim.raidCandidates(galaxy, 0, candidates);
    SOL_REQUIRE(candidates.size() == 2);
    SOL_CHECK(candidates[0].system == 1 && candidates[0].owner == 1);
    SOL_CHECK(candidates[1].system == 2 && candidates[1].owner == 2);

    // Own systems and out-of-reach targets are never candidates.
    sim.raidCandidates(galaxy, 2, candidates);
    for (const RaidCandidate& candidate : candidates) {
        SOL_CHECK(candidate.owner != 2);
    }
}

SOL_TEST(faction_sim_raid_hits_relations_economy_and_intensity)
{
    const Galaxy galaxy = chainGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, chainParams(), 42);
    Economy economy;
    economy.initialize(galaxy, oneCommodityParams(), 42);
    const float stockBefore = economy.stock(1, 0); // system 1's market

    SOL_CHECK(sim.commitRaid(galaxy, &economy, 0, 1));
    SOL_CHECK(sim.relation(0, 1) == -39.0f);
    SOL_CHECK(sim.relation(1, 0) == -39.0f);
    SOL_CHECK(economy.stock(1, 0) == stockBefore * 0.75f);
    SOL_CHECK(sim.raidIntensity(1) == 1.0f);
    SOL_CHECK(sim.lastRaider(1) == 0);
    SOL_CHECK(sim.raidIntensity(0) == 0.0f);

    // Repeated raids push the pair over the war threshold (hysteresis: war
    // stays on until relations recover past peaceThreshold).
    for (int i = 0; i < 3; ++i) {
        SOL_CHECK(sim.commitRaid(galaxy, nullptr, 0, 1));
    }
    SOL_CHECK(sim.relation(0, 1) == -51.0f);
    SOL_CHECK(sim.atWar(0, 1));
    sim.setRelation(0, 1, -30.0f);
    SOL_CHECK(sim.atWar(0, 1)); // -30 is above war but below peace: still on
    sim.setRelation(0, 1, -20.0f);
    SOL_CHECK(!sim.atWar(0, 1));

    // Invalid raids are refused: own territory, unreachable, or friendly.
    SOL_CHECK(!sim.commitRaid(galaxy, nullptr, 0, 0));
    SOL_CHECK(!sim.commitRaid(galaxy, nullptr, 0, 99));
    SOL_CHECK(!sim.commitRaid(galaxy, nullptr, 0, 1)); // -20 now: no cause
}

SOL_TEST(faction_sim_decisions_are_deterministic_and_default_policy_raids)
{
    const Galaxy galaxy = chainGalaxy();
    FactionSim first;
    first.initialize(galaxy, chainParams(), 7);
    FactionSim second;
    second.initialize(galaxy, chainParams(), 7);

    std::vector<FactionDecision> firstDecisions;
    std::vector<FactionDecision> secondDecisions;
    for (int i = 0; i < 61; ++i) {
        first.tick(1.0);
        second.tick(1.0);
    }
    first.takeDueDecisions(firstDecisions);
    second.takeDueDecisions(secondDecisions);
    SOL_REQUIRE(firstDecisions.size() == 3); // one per faction, index order
    SOL_REQUIRE(secondDecisions.size() == 3);
    for (std::size_t i = 0; i < firstDecisions.size(); ++i) {
        SOL_CHECK(firstDecisions[i].faction == static_cast<std::uint32_t>(i));
        SOL_CHECK(firstDecisions[i].roll == secondDecisions[i].roll);
    }
    // Taking drains the queue.
    firstDecisions.clear();
    first.takeDueDecisions(firstDecisions);
    SOL_CHECK(firstDecisions.empty());

    // Default policy: aggression 1.0 always raids the worst relation in
    // reach (faction 0's is the clan at -60); aggression 0.0 never raids.
    first.applyDefaultDecision(galaxy, nullptr, {.faction = 0, .roll = 0.5f});
    SOL_CHECK(first.raidIntensity(2) == 1.0f);
    SOL_CHECK(first.lastRaider(2) == 0);
    first.applyDefaultDecision(galaxy, nullptr, {.faction = 1, .roll = 0.5f});
    SOL_CHECK(first.raidIntensity(0) == 0.0f && first.raidIntensity(1) == 0.0f);
}

SOL_TEST(faction_sim_kill_web_and_commerce_move_standings)
{
    const Galaxy galaxy = chainGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, chainParams(), 1);

    // Killing an A ship: A drops by the penalty; B gains via the -35
    // rivalry (10 * 0.5 * 0.35); C gains via the -60 war (10 * 0.5 * 0.60).
    sim.recordShipKill(0);
    SOL_CHECK(sim.standing(0) == -10.0f);
    SOL_CHECK(sim.standing(1) == 1.75f);
    SOL_CHECK(sim.standing(2) == -17.0f);
    SOL_CHECK(!sim.playerHostile(0));
    sim.recordShipKill(0);
    sim.recordShipKill(0);
    sim.recordShipKill(0);
    SOL_CHECK(sim.standing(0) == -40.0f);
    SOL_CHECK(sim.playerHostile(0));

    // Commerce trickle: 5000 credits of trade with B is +5 standing (B is
    // at 4 x 1.75 = 7 from the kill web by now).
    sim.recordTrade(1, 5'000.0);
    SOL_CHECK(sim.standing(1) == 12.0f);
    sim.setStanding(1, 40.0f);
    SOL_CHECK(sim.playerFriendly(1));

    // Standings clamp.
    for (int i = 0; i < 20; ++i) {
        sim.recordShipKill(0);
    }
    SOL_CHECK(sim.standing(0) == -100.0f);
}

SOL_TEST(faction_sim_drift_and_intensity_decay)
{
    const Galaxy galaxy = chainGalaxy();
    FactionSimParams params = chainParams();
    params.driftPerSecond = 0.1f;
    FactionSim sim;
    sim.initialize(galaxy, params, 3);
    SOL_CHECK(sim.commitRaid(galaxy, nullptr, 0, 1)); // -35 -> -39
    const float intensityBefore = sim.raidIntensity(1);
    for (int i = 0; i < 59; ++i) { // stay under the decision interval
        sim.tick(1.0);
    }
    // Drift pulled the pair back toward the -35 baseline but not past it.
    SOL_CHECK(sim.relation(0, 1) > -39.0f);
    SOL_CHECK(sim.relation(0, 1) <= -35.0f);
    SOL_CHECK(sim.relation(0, 1) == sim.relation(1, 0));
    SOL_CHECK(sim.raidIntensity(1) < intensityBefore);
    SOL_CHECK(sim.raidIntensity(1) > 0.0f);
}

SOL_TEST(faction_sim_save_load_round_trips_and_stays_deterministic)
{
    const Galaxy galaxy = chainGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, chainParams(), 5);
    Economy economy;
    economy.initialize(galaxy, oneCommodityParams(), 5);

    std::vector<FactionDecision> decisions;
    for (int i = 0; i < 200; ++i) {
        sim.tick(1.0);
        decisions.clear();
        sim.takeDueDecisions(decisions);
        for (const FactionDecision& decision : decisions) {
            sim.applyDefaultDecision(galaxy, &economy, decision);
        }
    }
    sim.recordShipKill(1);
    sim.recordTrade(0, 2'000.0);

    sol::core::BinaryWriter writer;
    sim.save(writer);
    FactionSim restored;
    restored.initialize(galaxy, chainParams(), 999); // wrong seed on purpose
    const std::span<const std::byte> bytes(writer.data());
    sol::core::BinaryReader reader(bytes);
    SOL_REQUIRE(restored.load(reader));

    SOL_CHECK(restored.standing(1) == sim.standing(1));
    SOL_CHECK(restored.relation(0, 2) == sim.relation(0, 2));
    SOL_CHECK(restored.atWar(0, 2) == sim.atWar(0, 2));
    SOL_CHECK(restored.raidIntensity(1) == sim.raidIntensity(1));
    SOL_CHECK(restored.lastRaider(1) == sim.lastRaider(1));

    // Identical trajectories after the restore.
    for (int i = 0; i < 200; ++i) {
        sim.tick(1.0);
        restored.tick(1.0);
        decisions.clear();
        sim.takeDueDecisions(decisions);
        for (const FactionDecision& decision : decisions) {
            sim.applyDefaultDecision(galaxy, nullptr, decision);
        }
        decisions.clear();
        restored.takeDueDecisions(decisions);
        for (const FactionDecision& decision : decisions) {
            restored.applyDefaultDecision(galaxy, nullptr, decision);
        }
    }
    for (std::uint32_t a = 0; a < 3; ++a) {
        for (std::uint32_t b = 0; b < 3; ++b) {
            SOL_CHECK(sim.relation(a, b) == restored.relation(a, b));
            SOL_CHECK(sim.atWar(a, b) == restored.atWar(a, b));
        }
        SOL_CHECK(sim.raidIntensity(a) == restored.raidIntensity(a));
    }

    // Truncated buffers and mismatched layouts fail cleanly.
    sol::core::BinaryReader truncated(bytes.subspan(0, 6));
    FactionSim broken;
    broken.initialize(galaxy, chainParams(), 5);
    SOL_CHECK(!broken.load(truncated));
}

SOL_TEST(faction_sim_territory_starts_at_the_founding_claim)
{
    const Galaxy galaxy = territoryGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, territoryParams(), 3);

    for (std::uint32_t s = 0; s < 4; ++s) {
        SOL_CHECK(sim.systemOwner(s) == galaxy.systems[s].factionIndex);
        SOL_CHECK(sim.foundingClaim(s) == galaxy.systems[s].factionIndex);
        SOL_CHECK(!sim.contestOf(s).live());
        SOL_CHECK(!sim.contested(s));
    }
    // The home system is the lowest-index system holding the claim, which is
    // the rule ClanSpec::homeSystem already states for generated clans.
    SOL_CHECK(sim.homeSystem(0) == 0);
    SOL_CHECK(sim.homeSystem(1) == 2);
    SOL_CHECK(sim.homeSystem(2) == 3);
    SOL_CHECK(sim.systemOwner(99) == kNoFaction);
}

SOL_TEST(faction_sim_raids_open_a_contest_that_flips_the_system)
{
    const Galaxy galaxy = territoryGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, territoryParams(), 3);

    // One raid is pressure, not a conquest: below the threshold nothing in
    // the game is told a contest exists.
    SOL_REQUIRE(sim.commitRaid(galaxy, nullptr, 1, 1));
    SOL_CHECK(sim.contestOf(1).attacker == 1);
    SOL_CHECK(sim.contestOf(1).pressure == 0.2f);
    SOL_CHECK(!sim.contested(1));
    SOL_REQUIRE(sim.commitRaid(galaxy, nullptr, 1, 1));
    SOL_CHECK(sim.contested(1));
    SOL_CHECK(sim.systemOwner(1) == 0); // still theirs until it resolves

    SOL_REQUIRE(raidUntilFlipped(sim, galaxy, 1, 1));
    SOL_CHECK(sim.systemOwner(1) == 1);
    SOL_CHECK(!sim.contestOf(1).live()); // the contest clears with the claim
    // The generated plan is not rewritten: the galaxy stays a pure function
    // of its seed, and a caller can still tell a moved border from an
    // original one.
    SOL_CHECK(sim.foundingClaim(1) == 0);
    SOL_CHECK(galaxy.systems[1].factionIndex == 0);

    std::vector<sol::sim::ContestResolution> resolutions;
    sim.takeResolutions(resolutions);
    SOL_REQUIRE(resolutions.size() == 1);
    SOL_CHECK(resolutions[0].system == 1);
    SOL_CHECK(resolutions[0].winner == 1);
    SOL_CHECK(resolutions[0].loser == 0);
    SOL_CHECK(resolutions[0].flipped);
    resolutions.clear();
    sim.takeResolutions(resolutions);
    SOL_CHECK(resolutions.empty()); // drained, not repeated
}

SOL_TEST(faction_sim_contest_lapses_when_nobody_sustains_it)
{
    const Galaxy galaxy = territoryGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, territoryParams(), 3);

    sim.setContest(1, 1, 0.3f);
    SOL_REQUIRE(sim.contested(1));
    sim.tick(3'600.0); // four half-lives: 0.3 -> ~0.019, under the floor

    SOL_CHECK(!sim.contestOf(1).live());
    SOL_CHECK(sim.systemOwner(1) == 0); // the defender kept it by attrition
    std::vector<sol::sim::ContestResolution> resolutions;
    sim.takeResolutions(resolutions);
    SOL_REQUIRE(resolutions.size() == 1);
    SOL_CHECK(!resolutions[0].flipped);
    SOL_CHECK(resolutions[0].winner == 0);
    SOL_CHECK(resolutions[0].loser == 1);
}

SOL_TEST(faction_sim_home_system_is_never_contestable)
{
    const Galaxy galaxy = territoryGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, territoryParams(), 3);

    // System 2 is faction 1's only claim, so it is their home. A faction that
    // could lose it could be erased, and an erased faction takes its boards,
    // its catalogs and the player's standing with it.
    SOL_REQUIRE(sim.commitRaid(galaxy, nullptr, 2, 2)); // the raid still lands
    SOL_CHECK(sim.raidIntensity(2) == 1.0f);            // markets still drained
    SOL_CHECK(!sim.contestOf(2).live());                // but no claim opens
    sim.setContest(2, 2, 0.9f);
    SOL_CHECK(!sim.contestOf(2).live()); // the dev lever refuses it too
    SOL_CHECK(sim.systemOwner(2) == 1);

    // Beaten back to one system, a faction stops losing ground: take its
    // second system and the first becomes its home ground by construction.
    SOL_REQUIRE(raidUntilFlipped(sim, galaxy, 1, 1));
    SOL_CHECK(sim.systemOwner(1) == 1);
    SOL_CHECK(sim.homeSystem(0) == 0);
    sim.setContest(0, 1, 0.9f);
    SOL_CHECK(!sim.contestOf(0).live());
}

SOL_TEST(faction_sim_player_kills_push_a_contest_back)
{
    const Galaxy galaxy = territoryGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, territoryParams(), 3);

    const auto nearly = [](float value, float expected) {
        return std::fabs(value - expected) < 1.0e-5f;
    };
    sim.setContest(1, 1, 0.3f);
    sim.recordContestKill(1, 1);
    SOL_CHECK(nearly(sim.contestOf(1).pressure, 0.25f));
    // Killing anyone but the faction pressing the claim changes nothing —
    // including the defender's own ships.
    sim.recordContestKill(1, 0);
    sim.recordContestKill(1, 2);
    SOL_CHECK(nearly(sim.contestOf(1).pressure, 0.25f));
    // A kill in a system with no contest is not an error, it is just quiet.
    sim.recordContestKill(3, 1);
    SOL_CHECK(!sim.contestOf(3).live());

    for (int i = 0; i < 5; ++i) {
        sim.recordContestKill(1, 1);
    }
    SOL_CHECK(!sim.contestOf(1).live());
    SOL_CHECK(sim.systemOwner(1) == 0);
    std::vector<sol::sim::ContestResolution> resolutions;
    sim.takeResolutions(resolutions);
    SOL_REQUIRE(resolutions.size() == 1);
    SOL_CHECK(!resolutions[0].flipped);
    SOL_CHECK(resolutions[0].winner == 0);
}

SOL_TEST(faction_sim_one_attacker_presses_a_system_at_a_time)
{
    const Galaxy galaxy = territoryGalaxy();
    FactionSimParams params = territoryParams();
    params.raidReach = 2; // both rivals can now reach system 1
    FactionSim sim;
    sim.initialize(galaxy, params, 3);

    SOL_REQUIRE(sim.commitRaid(galaxy, nullptr, 1, 1));
    SOL_CHECK(sim.contestOf(1).attacker == 1);
    // The clan raids the same system: the raid lands, but it does not take
    // over a claim someone else is already pressing.
    SOL_REQUIRE(sim.commitRaid(galaxy, nullptr, 2, 1));
    SOL_CHECK(sim.raidIntensity(1) == 2.0f);
    SOL_CHECK(sim.contestOf(1).attacker == 1);
    SOL_CHECK(sim.contestOf(1).pressure == 0.2f); // and does not feed it
}

SOL_TEST(faction_sim_raid_reach_follows_a_moved_border)
{
    const Galaxy galaxy = territoryGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, territoryParams(), 3);

    std::vector<RaidCandidate> candidates;
    sim.raidCandidates(galaxy, 1, candidates);
    const auto holds = [&](std::uint32_t system) {
        return std::any_of(candidates.begin(), candidates.end(),
                           [&](const RaidCandidate& c) { return c.system == system; });
    };
    SOL_CHECK(holds(1));  // one jump from system 2
    SOL_CHECK(holds(3));  // one jump the other way
    SOL_CHECK(!holds(0)); // two jumps: out of reach at raidReach 1

    SOL_REQUIRE(raidUntilFlipped(sim, galaxy, 1, 1));
    sim.raidCandidates(galaxy, 1, candidates);
    // Taking system 1 moved the front: system 0 is now one jump from held
    // ground, and system 1 is no longer a target because it is theirs.
    SOL_CHECK(holds(0));
    SOL_CHECK(!holds(1));
    // And the loser can press to take it back — the border is not a ratchet.
    sim.raidCandidates(galaxy, 0, candidates);
    SOL_CHECK(holds(1));
}

SOL_TEST(faction_sim_territory_survives_save_load)
{
    const Galaxy galaxy = territoryGalaxy();
    FactionSim sim;
    sim.initialize(galaxy, territoryParams(), 3);
    SOL_REQUIRE(raidUntilFlipped(sim, galaxy, 1, 1));
    // The dispossessed owner pressing to take it back: one system carrying
    // both a moved border and a live counter-claim.
    sim.setContest(1, 0, 0.4f);
    std::vector<sol::sim::ContestResolution> resolutions;
    sim.takeResolutions(resolutions);

    sol::core::BinaryWriter writer;
    sim.save(writer);
    FactionSim restored;
    restored.initialize(galaxy, territoryParams(), 999);
    const std::span<const std::byte> bytes(writer.data());
    sol::core::BinaryReader reader(bytes);
    SOL_REQUIRE(restored.load(reader));

    SOL_CHECK(restored.systemOwner(1) == 1);
    SOL_CHECK(restored.foundingClaim(1) == 0); // re-derived from the galaxy
    SOL_CHECK(restored.homeSystem(0) == 0);
    SOL_CHECK(restored.contestOf(1).attacker == 0);
    SOL_CHECK(restored.contestOf(1).pressure == 0.4f);
    // A load is not news: the resolutions queue does not replay old flips
    // as though they had just happened.
    resolutions.clear();
    restored.takeResolutions(resolutions);
    SOL_CHECK(resolutions.empty());
}

SOL_TEST(economy_raid_empties_inbound_trader_cargo)
{
    const Galaxy galaxy = chainGalaxy();
    Economy economy;
    economy.initialize(galaxy, oneCommodityParams(), 4);
    // Drain system 1 so hauls head there, then catch a loaded inbound trader.
    (void)economy.buy(1, 0, 1.0e9f);
    (void)economy.buy(2, 0, 1.0e9f);
    bool caught = false;
    for (int i = 0; i < 400 && !caught; ++i) {
        economy.tick(galaxy, 1.0);
        const sol::sim::EconomyTrader& trader = economy.traders()[0];
        if (trader.phase == TraderPhase::InTransit && trader.cargo > 0.0f) {
            const std::uint32_t destination =
                economy.markets()[trader.market].systemIndex;
            economy.raidSystem(destination, 0.25f);
            SOL_CHECK(economy.traders()[0].cargo == 0.0f);
            caught = true;
        }
    }
    SOL_CHECK(caught);
}
