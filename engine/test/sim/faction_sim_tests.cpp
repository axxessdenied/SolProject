#include <sol/sim/faction_sim.hpp>
#include <sol/sim/universe.hpp>

#include <sol/core/serialize.hpp>
#include <sol/test/test.hpp>

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
