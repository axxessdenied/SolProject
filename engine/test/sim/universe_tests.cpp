#include <sol/sim/universe.hpp>

#include <sol/test/test.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using sol::sim::Galaxy;
using sol::sim::GalaxyParams;
using sol::sim::GateLink;
using sol::sim::generateGalaxy;
using sol::sim::kNoFaction;
using sol::sim::Region;
using sol::sim::routeBetween;
using sol::sim::StationRule;
using sol::sim::SystemSpec;

namespace {

GalaxyParams testParams(std::uint64_t seed)
{
    GalaxyParams params;
    params.seed = seed;
    params.systemCount = 60;
    params.factionCount = 3;
    params.stationRules = {StationRule{{3.0f, 1.0f, 0.5f}}, StationRule{{1.0f, 2.0f, 1.0f}}};
    return params;
}

bool specsEqual(const SystemSpec& a, const SystemSpec& b)
{
    if (a.name != b.name || a.region != b.region || a.factionIndex != b.factionIndex
        || a.seed != b.seed || a.planets.size() != b.planets.size()
        || a.stations.size() != b.stations.size() || a.gates.size() != b.gates.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.planets.size(); ++i) {
        if (a.planets[i].name != b.planets[i].name
            || a.planets[i].position.x != b.planets[i].position.x
            || a.planets[i].radius != b.planets[i].radius) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.stations.size(); ++i) {
        if (a.stations[i].name != b.stations[i].name
            || a.stations[i].archetype != b.stations[i].archetype
            || a.stations[i].position.x != b.stations[i].position.x) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.gates.size(); ++i) {
        if (a.gates[i].toSystem != b.gates[i].toSystem
            || a.gates[i].position.x != b.gates[i].position.x) {
            return false;
        }
    }
    return true;
}

} // namespace

SOL_TEST(universe_same_seed_same_galaxy)
{
    const Galaxy first = generateGalaxy(testParams(1234));
    const Galaxy second = generateGalaxy(testParams(1234));
    SOL_REQUIRE(first.systems.size() == second.systems.size());
    SOL_REQUIRE(first.links.size() == second.links.size());
    for (std::size_t i = 0; i < first.links.size(); ++i) {
        SOL_CHECK(first.links[i].a == second.links[i].a);
        SOL_CHECK(first.links[i].b == second.links[i].b);
    }
    for (std::size_t i = 0; i < first.systems.size(); ++i) {
        SOL_CHECK(specsEqual(first.systems[i], second.systems[i]));
    }
}

SOL_TEST(universe_different_seed_differs)
{
    const Galaxy first = generateGalaxy(testParams(1234));
    const Galaxy second = generateGalaxy(testParams(4321));
    bool differs = first.systems.size() != second.systems.size();
    for (std::size_t i = 0; !differs && i < first.systems.size(); ++i) {
        differs = !specsEqual(first.systems[i], second.systems[i]);
    }
    SOL_CHECK(differs);
}

SOL_TEST(universe_gate_graph_connected)
{
    const Galaxy galaxy = generateGalaxy(testParams(77));
    for (std::uint32_t target = 0; target < galaxy.systems.size(); ++target) {
        SOL_CHECK(!routeBetween(galaxy, 0, target).empty());
    }
}

SOL_TEST(universe_gates_mirror_links)
{
    const Galaxy galaxy = generateGalaxy(testParams(9001));
    std::size_t gateCount = 0;
    for (const SystemSpec& system : galaxy.systems) {
        gateCount += system.gates.size();
    }
    SOL_CHECK(gateCount == galaxy.links.size() * 2); // every lane has a gate at both ends
    for (const GateLink& link : galaxy.links) {
        SOL_CHECK(link.a < link.b);
        bool forward = false;
        bool backward = false;
        for (const auto& gate : galaxy.systems[link.a].gates) {
            forward = forward || gate.toSystem == link.b;
        }
        for (const auto& gate : galaxy.systems[link.b].gates) {
            backward = backward || gate.toSystem == link.a;
        }
        SOL_CHECK(forward && backward);
    }
}

SOL_TEST(universe_regions_and_factions)
{
    const GalaxyParams params = testParams(31337);
    const Galaxy galaxy = generateGalaxy(params);
    const float coreRadius = params.galaxyRadius * params.coreRadiusFraction;
    const float frontierRadius = params.galaxyRadius * params.frontierRadiusFraction;
    std::vector<bool> factionSeen(params.factionCount, false);
    for (const SystemSpec& system : galaxy.systems) {
        const float r = sol::core::length(system.mapPosition);
        const Region expected = r < coreRadius        ? Region::Core
                                : r < frontierRadius ? Region::Frontier
                                                     : Region::Fringe;
        SOL_CHECK(system.region == expected);
        if (system.factionIndex != kNoFaction) {
            SOL_REQUIRE(system.factionIndex < params.factionCount);
            factionSeen[system.factionIndex] = true;
        } else {
            // Only fringe systems may go unclaimed once factions exist.
            SOL_CHECK(system.region == Region::Fringe);
        }
    }
    for (std::uint32_t f = 0; f < params.factionCount; ++f) {
        SOL_CHECK(factionSeen[f]);
    }
}

SOL_TEST(universe_pirate_clans_claim_lawless_neighborhoods)
{
    GalaxyParams params = testParams(31337);
    params.pirateTemplateCount = 2;
    const Galaxy galaxy = generateGalaxy(params);

    // With templates, no system stays lawless: every ex-kNoFaction system
    // belongs to exactly one clan, indexed past the majors.
    SOL_CHECK(!galaxy.clans.empty());
    std::vector<bool> clanSeen(galaxy.clans.size(), false);
    for (const SystemSpec& system : galaxy.systems) {
        SOL_REQUIRE(system.factionIndex != kNoFaction);
        if (system.factionIndex >= params.factionCount) {
            const std::uint32_t clan = system.factionIndex - params.factionCount;
            SOL_REQUIRE(clan < galaxy.clans.size());
            SOL_CHECK(system.region == Region::Fringe); // lawless rolls are fringe-only
            clanSeen[clan] = true;
        }
    }
    for (std::size_t c = 0; c < galaxy.clans.size(); ++c) {
        SOL_CHECK(clanSeen[c]);
        SOL_CHECK(galaxy.clans[c].templateIndex < params.pirateTemplateCount);
        SOL_CHECK(!galaxy.clans[c].name.empty());
        // Home is a member of its own clan.
        SOL_CHECK(galaxy.systems[galaxy.clans[c].homeSystem].factionIndex ==
                  params.factionCount + static_cast<std::uint32_t>(c));
    }

    // Deterministic per seed; majors' claims are untouched by the clan pass.
    const Galaxy again = generateGalaxy(params);
    SOL_REQUIRE(again.clans.size() == galaxy.clans.size());
    for (std::size_t c = 0; c < galaxy.clans.size(); ++c) {
        SOL_CHECK(again.clans[c].name == galaxy.clans[c].name);
        SOL_CHECK(again.clans[c].templateIndex == galaxy.clans[c].templateIndex);
        SOL_CHECK(again.clans[c].seed == galaxy.clans[c].seed);
        SOL_CHECK(again.clans[c].homeSystem == galaxy.clans[c].homeSystem);
    }
    const Galaxy withoutClans = generateGalaxy(testParams(31337));
    SOL_REQUIRE(withoutClans.systems.size() == galaxy.systems.size());
    for (std::size_t i = 0; i < galaxy.systems.size(); ++i) {
        if (withoutClans.systems[i].factionIndex != kNoFaction) {
            SOL_CHECK(galaxy.systems[i].factionIndex ==
                      withoutClans.systems[i].factionIndex);
        }
    }
}

SOL_TEST(universe_playfield_within_leg_budget)
{
    const GalaxyParams params = testParams(555);
    const Galaxy galaxy = generateGalaxy(params);
    for (const SystemSpec& system : galaxy.systems) {
        SOL_REQUIRE(!system.planets.empty());
        SOL_REQUIRE(system.primaryPlanet < system.planets.size());
        const sol::core::DVec3 hub = system.planets[system.primaryPlanet].position;
        const std::size_t tier = static_cast<std::size_t>(system.region);
        SOL_CHECK(system.stations.size() >= params.stationCount[tier][0]);
        SOL_CHECK(system.stations.size() <= params.stationCount[tier][1]);
        for (const auto& station : system.stations) {
            const double distance = sol::core::length(station.position - hub);
            SOL_CHECK(distance >= params.stationMinDistance * 0.99);
            SOL_CHECK(distance <= params.stationMaxDistance * 1.01);
            SOL_CHECK(station.archetype < params.stationRules.size());
        }
        for (const auto& gate : system.gates) {
            const double distance = sol::core::length(gate.position - hub);
            SOL_CHECK(std::abs(distance - params.gateDistance) < params.gateDistance * 0.01);
        }
    }
}

SOL_TEST(universe_route_is_shortest_and_valid)
{
    const Galaxy galaxy = generateGalaxy(testParams(2468));
    const std::uint32_t last = static_cast<std::uint32_t>(galaxy.systems.size() - 1);
    const std::vector<std::uint32_t> route = routeBetween(galaxy, 0, last);
    SOL_REQUIRE(route.size() >= 2);
    SOL_CHECK(route.front() == 0);
    SOL_CHECK(route.back() == last);
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
        bool linked = false;
        for (const auto& gate : galaxy.systems[route[i]].gates) {
            linked = linked || gate.toSystem == route[i + 1];
        }
        SOL_CHECK(linked);
    }
    SOL_CHECK(routeBetween(galaxy, 3, 3).size() == 1);
    SOL_CHECK(routeBetween(galaxy, 0, 100000).empty());
}
