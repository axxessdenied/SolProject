#include <sol/sim/universe.hpp>

// Phase 13: station placement consults the asteroid fields, so the coherence
// tests need the mining rule the generator now asks.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/sim/mining.hpp>
#include <sol/test/test.hpp>

using sol::sim::AuthoredSystem;
using sol::sim::fieldCountFor;
using sol::sim::Galaxy;
using sol::sim::GalaxyParams;
using sol::sim::GateLink;
using sol::sim::generateGalaxy;
using sol::sim::kNoFaction;
using sol::sim::MiningParams;
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
    if (a.name != b.name || a.region != b.region || a.factionIndex != b.factionIndex || a.seed != b.seed ||
        a.planets.size() != b.planets.size() || a.stations.size() != b.stations.size() ||
        a.gates.size() != b.gates.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.planets.size(); ++i) {
        if (a.planets[i].name != b.planets[i].name || a.planets[i].position.x != b.planets[i].position.x ||
            a.planets[i].radius != b.planets[i].radius) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.stations.size(); ++i) {
        if (a.stations[i].name != b.stations[i].name || a.stations[i].archetype != b.stations[i].archetype ||
            a.stations[i].position.x != b.stations[i].position.x) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.gates.size(); ++i) {
        if (a.gates[i].toSystem != b.gates[i].toSystem || a.gates[i].position.x != b.gates[i].position.x) {
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
        const Region expected = r < coreRadius       ? Region::Core
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
            SOL_CHECK(galaxy.systems[i].factionIndex == withoutClans.systems[i].factionIndex);
        }
    }
}

// ⚑⚑ THE INVARIANT SIX UNGUARDED CALL SITES DEPEND ON, AND IT IS WIDENED HERE
// RATHER THAN RESTATED IN A SECOND TEST BESIDE IT. `space_world.cpp` at 2369,
// 3206, 3228, 3479 and 7410 and `content.cpp` at 2990 all index
// `spec.planets[spec.primaryPlanet]` with no bounds check, because
// `populateSystem` has always made at least one planet and put the primary
// inside it. `playfieldHub` clamps and none of those six goes through it. Phase
// 29 lets an author write both numbers, so the galaxy this runs over now
// CONTAINS an authored system - one with its own planets and its own primary,
// and one with neither - and the assertion has to hold for all three kinds.
SOL_TEST(universe_playfield_within_leg_budget)
{
    GalaxyParams params = testParams(555);
    params.authoredSystems.push_back({.id = "test.written",
                                      .name = "Written",
                                      .hasName = true,
                                      .primaryPlanet = 1,
                                      .hasPrimaryPlanet = true,
                                      .planets = {{.name = "Written Rock"}, {.name = "Written Hall"}},
                                      .stations = {{.name = "Written Dock", .archetype = 1}}});
    params.authoredSystems.push_back({.id = "test.bare", .name = "Bare", .hasName = true});
    const Galaxy galaxy = generateGalaxy(params);
    for (const SystemSpec& system : galaxy.systems) {
        SOL_REQUIRE(!system.planets.empty());
        SOL_REQUIRE(system.primaryPlanet < system.planets.size());
        const sol::core::DVec3 hub = system.planets[system.primaryPlanet].position;
        const std::size_t tier = static_cast<std::size_t>(system.region);
        // An authored system declares its own station list, so the region tier
        // no longer bounds it - that is what authoring one means.
        const bool authored = !system.authoredId.empty();
        SOL_CHECK(authored || system.stations.size() >= params.stationCount[tier][0]);
        SOL_CHECK(authored || system.stations.size() <= params.stationCount[tier][1]);
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

// Phase 13, note 5(a): a Mining Outpost in a system with no asteroid field
// produces nothing forever. The rule the def has stated since 8g -- "a system
// with no rock supports no mine" -- is now enforced at placement.
//
// The test is written as a BEFORE/AFTER over one galaxy rather than as a bare
// "no mines without rock", because the second half alone would pass on a
// galaxy that simply never rolled the case. Generating the identical galaxy
// with the rule off is what proves the rule is doing work.
SOL_TEST(universe_no_extractor_without_rock)
{
    GalaxyParams params = testParams(1701);
    // Two archetypes: rule 1 comes out of the ground, and is weighted heavily
    // in every tier so an unguarded galaxy is certain to misplace some.
    params.stationRules = {StationRule{{1.0f, 1.0f, 1.0f}, false}, StationRule{{4.0f, 4.0f, 4.0f}, true}};

    MiningParams mining;
    mining.ores.push_back({.commodity = 0, .weight = {1.0f, 1.0f, 1.0f}});
    // Core systems roll [0,1] fields, so the rockless case exists and is common.
    SOL_CHECK(mining.fieldCount[0][0] == 0);

    const Galaxy unguarded = generateGalaxy(params);
    const Galaxy guarded = generateGalaxy(params, &mining);
    SOL_REQUIRE(unguarded.systems.size() == guarded.systems.size());

    std::uint32_t rocklessSystems = 0;
    std::uint32_t minesWithoutRockBefore = 0;
    std::uint32_t minesWithoutRockAfter = 0;
    std::size_t stationsBefore = 0;
    std::size_t stationsAfter = 0;
    for (std::size_t i = 0; i < guarded.systems.size(); ++i) {
        stationsBefore += unguarded.systems[i].stations.size();
        stationsAfter += guarded.systems[i].stations.size();
        if (fieldCountFor(guarded.systems[i], mining) != 0) {
            continue;
        }
        ++rocklessSystems;
        for (const auto& station : unguarded.systems[i].stations) {
            minesWithoutRockBefore += params.stationRules[station.archetype].requiresField ? 1 : 0;
        }
        for (const auto& station : guarded.systems[i].stations) {
            minesWithoutRockAfter += params.stationRules[station.archetype].requiresField ? 1 : 0;
        }
    }

    // The case is reachable, the old behaviour hits it, the new one does not.
    SOL_CHECK(rocklessSystems > 0);
    SOL_CHECK(minesWithoutRockBefore > 0);
    SOL_CHECK(minesWithoutRockAfter == 0);
    // ⚑ A veto, not a re-roll: a rockless system builds something else, so the
    // galaxy keeps every station it would have had.
    SOL_CHECK(stationsBefore == stationsAfter);
}

// The fallback, which is the half nobody meets until a mod removes an
// archetype: if EVERY archetype comes out of the ground, a rockless system
// still gets its stations rather than being left empty.
SOL_TEST(universe_all_extractors_still_builds)
{
    GalaxyParams params = testParams(99);
    params.stationRules = {StationRule{{1.0f, 1.0f, 1.0f}, true}, StationRule{{2.0f, 2.0f, 2.0f}, true}};
    MiningParams mining;
    mining.ores.push_back({.commodity = 0, .weight = {1.0f, 1.0f, 1.0f}});

    const Galaxy guarded = generateGalaxy(params, &mining);
    const Galaxy unguarded = generateGalaxy(params);
    SOL_REQUIRE(guarded.systems.size() == unguarded.systems.size());
    std::size_t rockless = 0;
    for (std::size_t i = 0; i < guarded.systems.size(); ++i) {
        SOL_CHECK(guarded.systems[i].stations.size() == unguarded.systems[i].stations.size());
        for (const auto& station : guarded.systems[i].stations) {
            SOL_CHECK(station.archetype < params.stationRules.size());
        }
        rockless += fieldCountFor(guarded.systems[i], mining) == 0 ? 1 : 0;
    }
    SOL_CHECK(rockless > 0); // the fallback was actually exercised
}

// Phase 13, note 4: a faction builds what it is. The bias multiplies the
// region weight, so what is asserted is a SHIFT in the mix, not a hard rule --
// except for a zero, which is a deliberate "never" and is absolute.
SOL_TEST(universe_faction_character_shapes_what_it_builds)
{
    GalaxyParams params = testParams(31337);
    params.factionCount = 2;
    params.fringeLawlessChance = 0.0f; // keep every system claimed, so the mix is readable
    params.stationRules = {StationRule{{1.0f, 1.0f, 1.0f}}, StationRule{{1.0f, 1.0f, 1.0f}}};
    // Faction 0 only ever builds archetype 0; faction 1 leans hard the other
    // way but is not forbidden anything.
    params.factionStationBias = {{4.0f, 0.0f}, {1.0f, 4.0f}};

    const Galaxy galaxy = generateGalaxy(params);
    std::uint32_t built[2][2] = {{0, 0}, {0, 0}};
    for (const SystemSpec& system : galaxy.systems) {
        if (system.factionIndex >= 2) {
            continue; // lawless or a clan: no character, not part of the claim
        }
        for (const auto& station : system.stations) {
            ++built[system.factionIndex][station.archetype];
        }
    }
    // Both factions actually hold territory with stations on it.
    SOL_REQUIRE(built[0][0] + built[0][1] > 0);
    SOL_REQUIRE(built[1][0] + built[1][1] > 0);
    // A zero bias is absolute: faction 0 never builds archetype 1, anywhere.
    SOL_CHECK(built[0][1] == 0);
    // A non-zero lean is a tilt, not a rule, so it is asserted as a ratio.
    const double zeroShare = static_cast<double>(built[0][0]) / (built[0][0] + built[0][1]);
    const double oneShare = static_cast<double>(built[1][0]) / (built[1][0] + built[1][1]);
    SOL_CHECK(zeroShare > oneShare);

    // ⚑ And the neutral case is the one that must not regress: with no bias
    // table at all, the galaxy is exactly what it was before this key existed.
    GalaxyParams neutral = params;
    neutral.factionStationBias.clear();
    const Galaxy before = generateGalaxy(neutral);
    bool anyDifference = false;
    SOL_REQUIRE(before.systems.size() == galaxy.systems.size());
    for (std::size_t i = 0; i < galaxy.systems.size(); ++i) {
        anyDifference = anyDifference || !specsEqual(before.systems[i], galaxy.systems[i]);
    }
    SOL_CHECK(anyDifference); // the bias did something
    const Galaxy neutralAgain = generateGalaxy(neutral);
    for (std::size_t i = 0; i < before.systems.size(); ++i) {
        SOL_CHECK(specsEqual(before.systems[i], neutralAgain.systems[i]));
    }
}

// A faction that zeroes everything it could build still gets its stations,
// rather than holding empty systems. Reachable from a mod, so it is pinned.
SOL_TEST(universe_faction_that_forbids_everything_still_builds)
{
    GalaxyParams params = testParams(808);
    params.factionCount = 1;
    params.stationRules = {StationRule{{1.0f, 1.0f, 1.0f}}, StationRule{{1.0f, 1.0f, 1.0f}}};
    params.factionStationBias = {{0.0f, 0.0f}};

    const Galaxy guarded = generateGalaxy(params);
    GalaxyParams neutral = params;
    neutral.factionStationBias.clear();
    const Galaxy plain = generateGalaxy(neutral);

    std::size_t claimed = 0;
    SOL_REQUIRE(guarded.systems.size() == plain.systems.size());
    for (std::size_t i = 0; i < guarded.systems.size(); ++i) {
        SOL_CHECK(guarded.systems[i].stations.size() == plain.systems[i].stations.size());
        for (const auto& station : guarded.systems[i].stations) {
            SOL_CHECK(station.archetype < params.stationRules.size());
        }
        claimed += guarded.systems[i].factionIndex == 0 ? 1 : 0;
    }
    SOL_CHECK(claimed > 0); // the all-zero faction actually held something
}

// Nothing about the rule may cost determinism, which is the property the whole
// galaxy rests on.
SOL_TEST(universe_field_rule_is_deterministic)
{
    MiningParams mining;
    mining.ores.push_back({.commodity = 0, .weight = {1.0f, 1.0f, 1.0f}});
    const Galaxy first = generateGalaxy(testParams(555), &mining);
    const Galaxy second = generateGalaxy(testParams(555), &mining);
    SOL_REQUIRE(first.systems.size() == second.systems.size());
    for (std::size_t i = 0; i < first.systems.size(); ++i) {
        SOL_CHECK(specsEqual(first.systems[i], second.systems[i]));
    }
}

// ---------------------------------------------------------------------------
// Authored systems (Phase 29 stage A).
//
// ⚑⚑⚑ THE PHASE'S MOST LIKELY SILENT FAILURE IS AN AUTHORED FIELD THAT A LATER
// STAGE QUIETLY OVERWRITES, AND IT READS TO AN AUTHOR AS "MY TOML DID NOT WORK"
// RATHER THAN AS A BROKEN PIPELINE. Four stages rewrite unconditionally -
// `assignRegions`, the name loop, `claimTerritory`'s Dijkstra, and `spawnClans`
// - so there are four chances to miss one, and one test per field is the whole
// of the defence.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] const SystemSpec* findAuthored(const Galaxy& galaxy, const char* id)
{
    for (const SystemSpec& system : galaxy.systems) {
        if (system.authoredId == id) {
            return &system;
        }
    }
    return nullptr;
}

} // namespace

// The replacement contract, stated as a count: `random` takes an ordinary
// system's slot rather than adding one. Growing the galaxy is what `anywhere`
// means and it is stage B - so anything reading `galaxy.systems.size()` as
// `params.systemCount` is still right after this stage, and wrong after that one.
SOL_TEST(universe_authored_system_replaces_a_node_instead_of_adding_one)
{
    const GalaxyParams plain = testParams(31337);
    const Galaxy before = generateGalaxy(plain);

    GalaxyParams params = plain;
    params.authoredSystems.push_back({.id = "test.harrow", .name = "Harrow", .hasName = true});
    const Galaxy after = generateGalaxy(params);

    SOL_CHECK(after.systems.size() == before.systems.size());
    SOL_CHECK(after.links.size() == before.links.size());
    const SystemSpec* harrow = findAuthored(after, "test.harrow");
    SOL_REQUIRE(harrow != nullptr);
    SOL_CHECK(harrow->name == "Harrow");

    // Exactly one node changed hands, and it kept the map position, the region
    // and the gates the generator had already given it. That is what makes this
    // a replacement rather than the post-generation patching decisions/018
    // refused: an authored system declares no external links, so it contradicts
    // no gate graph.
    std::uint32_t claimed = 0;
    for (std::uint32_t i = 0; i < after.systems.size(); ++i) {
        if (!after.systems[i].authoredId.empty()) {
            ++claimed;
            SOL_CHECK(after.systems[i].mapPosition.x == before.systems[i].mapPosition.x);
            SOL_CHECK(after.systems[i].mapPosition.z == before.systems[i].mapPosition.z);
            SOL_CHECK(after.systems[i].gates.size() == before.systems[i].gates.size());
        }
    }
    SOL_CHECK(claimed == 1);
}

// ⚑ Placement draws from a dedicated stream, so it lands on the same node every
// time - and because that stream is shared with no generation stage, adding
// authored systems cannot shift anything else's rolls.
SOL_TEST(universe_authored_placement_is_deterministic)
{
    GalaxyParams params = testParams(4242);
    params.authoredSystems.push_back({.id = "test.a", .name = "Aye", .hasName = true});
    params.authoredSystems.push_back({.id = "test.b", .name = "Bee", .hasName = true});

    const Galaxy first = generateGalaxy(params);
    const Galaxy second = generateGalaxy(params);
    SOL_REQUIRE(findAuthored(first, "test.a") != nullptr);
    SOL_REQUIRE(findAuthored(second, "test.a") != nullptr);
    SOL_CHECK(findAuthored(first, "test.a") - first.systems.data() ==
              findAuthored(second, "test.a") - second.systems.data());

    // Two authored systems resolve in def order and cannot land on the same
    // node, which is the only reason def order has to be an order at all.
    SOL_REQUIRE(findAuthored(first, "test.b") != nullptr);
    SOL_CHECK(findAuthored(first, "test.a") != findAuthored(first, "test.b"));

    // ⚑ AND IT IS A DRAW RATHER THAN A CONSTANT, WHICH IS WORTH ASSERTING
    // BECAUSE THE FIRST REAL RUN LANDED ON SYSTEM 0 AND THAT IS EXACTLY WHAT A
    // BROKEN ONE LOOKS LIKE. A placement that always returned the lowest free
    // index would satisfy every other test in this file - determinism, def
    // order, one-node-each - and would quietly put every authored system in the
    // game on top of the campaign start.
    std::vector<std::uint32_t> landed;
    for (const std::uint64_t seed : {1701u, 31337u, 4242u, 99u, 555u, 8080u}) {
        GalaxyParams one = testParams(seed);
        one.authoredSystems.push_back({.id = "test.one", .name = "One", .hasName = true});
        const Galaxy galaxy = generateGalaxy(one);
        const SystemSpec* system = findAuthored(galaxy, "test.one");
        SOL_REQUIRE(system != nullptr);
        landed.push_back(static_cast<std::uint32_t>(system - galaxy.systems.data()));
    }
    std::uint32_t distinct = 0;
    for (std::size_t i = 0; i < landed.size(); ++i) {
        bool seen = false;
        for (std::size_t j = 0; j < i; ++j) {
            seen = seen || landed[i] == landed[j];
        }
        distinct += seen ? 0u : 1u;
    }
    if (distinct < 4) {
        for (const std::uint32_t index : landed) {
            std::printf("  landed at %u\n", index);
        }
    }
    SOL_CHECK(distinct >= 4);
}

// SKIP POINT: the name loop. It rewrites `name` for every index, so an authored
// name survives only because the loop is taught to leave it alone - and the SEED
// must still be assigned, or the authored system would generate its contents
// from seed 0.
SOL_TEST(universe_authored_name_survives_the_name_loop)
{
    GalaxyParams params = testParams(99);
    params.authoredSystems.push_back({.id = "test.named", .name = "Harrows Rest", .hasName = true});
    const Galaxy galaxy = generateGalaxy(params);

    const SystemSpec* system = findAuthored(galaxy, "test.named");
    SOL_REQUIRE(system != nullptr);
    SOL_CHECK(system->name == "Harrows Rest");
    SOL_CHECK(system->seed != 0);

    // And no procedural system took the same name: the uniqueness scan reaches
    // every other system rather than only the ones ahead of it, so a name
    // written in before the loop is one the loop will not hand out.
    std::uint32_t matches = 0;
    for (const SystemSpec& other : galaxy.systems) {
        matches += other.name == "Harrows Rest" ? 1 : 0;
    }
    SOL_CHECK(matches == 1);
}

// SKIP POINT: `assignRegions`, which rewrites `region` from map radius for every
// system in the vector. For a REPLACEMENT the authored write lands after that
// pass, so this holds by ordering rather than by a guard - which is worth
// pinning, because stage B appends nodes BEFORE that pass and will need the
// guard this test would then be holding.
SOL_TEST(universe_authored_region_survives_assign_regions)
{
    GalaxyParams params = testParams(1234);
    params.authoredSystems.push_back(
        {.id = "test.core", .name = "Deepcore", .hasName = true, .region = Region::Core, .hasRegion = true});
    params.authoredSystems.push_back({.id = "test.edge",
                                      .name = "Farthing",
                                      .hasName = true,
                                      .region = Region::Fringe,
                                      .hasRegion = true});
    const Galaxy galaxy = generateGalaxy(params);

    SOL_REQUIRE(findAuthored(galaxy, "test.core") != nullptr);
    SOL_REQUIRE(findAuthored(galaxy, "test.edge") != nullptr);
    SOL_CHECK(findAuthored(galaxy, "test.core")->region == Region::Core);
    SOL_CHECK(findAuthored(galaxy, "test.edge")->region == Region::Fringe);
}

// SKIP POINT: `claimTerritory`'s Dijkstra, which repaints `factionIndex` for
// everything it reaches - and its lawless-fringe roll, which repaints it again.
SOL_TEST(universe_authored_owner_survives_claim_territory)
{
    GalaxyParams params = testParams(777);
    params.authoredSystems.push_back({.id = "test.navy",
                                      .name = "Navyhold",
                                      .hasName = true,
                                      .region = Region::Fringe, // deep in claim-and-reroll country
                                      .hasRegion = true,
                                      .factionIndex = 2,
                                      .hasFaction = true});
    const Galaxy galaxy = generateGalaxy(params);

    const SystemSpec* system = findAuthored(galaxy, "test.navy");
    SOL_REQUIRE(system != nullptr);
    SOL_CHECK(system->factionIndex == 2);

    // The search still walks THROUGH it: an authored owner is not a wall, and
    // the galaxy around it is claimed exactly as it would have been.
    SOL_REQUIRE(!system->gates.empty());
    std::uint32_t claimedNeighbors = 0;
    for (const auto& gate : system->gates) {
        claimedNeighbors += galaxy.systems[gate.toSystem].factionIndex != kNoFaction ? 1u : 0u;
    }
    SOL_CHECK(claimedNeighbors > 0);
}

// SKIP POINT: `spawnClans`, and this is the one a sentinel could never have
// expressed. `factionIndex == kNoFaction` means BOTH "nobody has decided yet"
// and "the author decided nobody owns this", and only the second must survive a
// clan sweeping through - so the stage asks the presence FLAG, never the value.
SOL_TEST(universe_authored_lawlessness_is_not_swallowed_by_a_clan)
{
    GalaxyParams params = testParams(2024);
    params.pirateTemplateCount = 2;
    params.fringeLawlessChance = 1.0f; // every fringe system is clan country
    params.authoredSystems.push_back({.id = "test.haven",
                                      .name = "Haven",
                                      .hasName = true,
                                      .region = Region::Fringe,
                                      .hasRegion = true,
                                      .factionIndex = kNoFaction,
                                      .hasFaction = true});
    const Galaxy galaxy = generateGalaxy(params);

    const SystemSpec* haven = findAuthored(galaxy, "test.haven");
    SOL_REQUIRE(haven != nullptr);
    SOL_CHECK(haven->factionIndex == kNoFaction);

    // The test is only worth its bytes if a clan would otherwise have taken it,
    // so assert the sweep really ran and really did claim its neighbours.
    SOL_REQUIRE(!galaxy.clans.empty());
    SOL_REQUIRE(!haven->gates.empty());
    std::uint32_t clanNeighbors = 0;
    for (const auto& gate : haven->gates) {
        const std::uint32_t owner = galaxy.systems[gate.toSystem].factionIndex;
        clanNeighbors += owner != kNoFaction && owner >= params.factionCount ? 1u : 0u;
    }
    SOL_CHECK(clanNeighbors > 0);
}

// An author names planets and stations; the generator still lays out the orbits,
// so writing a system by hand never means writing metres.
SOL_TEST(universe_authored_contents_replace_the_rolled_ones)
{
    GalaxyParams params = testParams(8080);
    params.authoredSystems.push_back(
        {.id = "test.forge",
         .name = "Forgehold",
         .hasName = true,
         .primaryPlanet = 0,
         .hasPrimaryPlanet = true,
         .secret = true,
         .planets = {{.name = "The Anvil", .radius = 3.0e6, .hasRadius = true}},
         .stations = {{.name = "Anvil Dock", .archetype = 1}, {.name = "The Bellows", .archetype = 0}}});
    const Galaxy galaxy = generateGalaxy(params);

    const SystemSpec* system = findAuthored(galaxy, "test.forge");
    SOL_REQUIRE(system != nullptr);
    SOL_REQUIRE(system->planets.size() == 1);
    SOL_CHECK(system->planets[0].name == "The Anvil");
    SOL_CHECK(system->planets[0].radius == 3.0e6);
    SOL_CHECK(system->primaryPlanet == 0);
    SOL_CHECK(system->secret);
    SOL_REQUIRE(system->stations.size() == 2);
    SOL_CHECK(system->stations[0].name == "Anvil Dock");
    SOL_CHECK(system->stations[0].archetype == 1);
    SOL_CHECK(system->stations[1].name == "The Bellows");

    // Every authored body still sits in the orbit ladder rather than at the
    // origin: the generator placed it, the author only named it.
    SOL_CHECK(sol::core::length(system->planets[0].position) > 0.0);
}

// ⚑⚑ THE PHASE EXIT, ASSERTED WHERE THE GENERATOR CAN SEE IT. `game.unit`'s
// golden holds this against a digest recorded before any of this existed, which
// is the real statement; this is the in-process half, and it says something the
// golden cannot - that the systems a replacement did NOT take are individually
// untouched, rather than that the galaxy as a whole still hashes the same.
SOL_TEST(universe_authoring_one_system_leaves_every_other_one_alone)
{
    const GalaxyParams plain = testParams(60613);
    const Galaxy before = generateGalaxy(plain);

    GalaxyParams params = plain;
    params.authoredSystems.push_back({.id = "test.one",
                                      .name = "Solitary",
                                      .hasName = true,
                                      .stations = {{.name = "Solitary Dock", .archetype = 0}}});
    const Galaxy after = generateGalaxy(params);

    SOL_REQUIRE(after.systems.size() == before.systems.size());
    std::uint32_t untouched = 0;
    for (std::uint32_t i = 0; i < after.systems.size(); ++i) {
        if (!after.systems[i].authoredId.empty()) {
            continue;
        }
        SOL_CHECK(after.systems[i].name == before.systems[i].name);
        SOL_CHECK(after.systems[i].seed == before.systems[i].seed);
        SOL_CHECK(after.systems[i].region == before.systems[i].region);
        SOL_CHECK(after.systems[i].factionIndex == before.systems[i].factionIndex);
        SOL_CHECK(after.systems[i].planets.size() == before.systems[i].planets.size());
        SOL_CHECK(after.systems[i].stations.size() == before.systems[i].stations.size());
        ++untouched;
    }
    SOL_CHECK(untouched == after.systems.size() - 1);
    SOL_REQUIRE(after.links.size() == before.links.size());
    for (std::size_t i = 0; i < after.links.size(); ++i) {
        SOL_CHECK(after.links[i].a == before.links[i].a && after.links[i].b == before.links[i].b);
    }
}

// ---------------------------------------------------------------------------
// Phase 29 stage B: the other three placement rules.
// ---------------------------------------------------------------------------

namespace {

// `kInvalidIndex` is file-local to universe.cpp and deliberately not exported,
// so the tests carry their own "not placed" sentinel.
constexpr std::uint32_t kNotPlaced = 0xFFFFFFFFu;

std::uint32_t indexOfAuthored(const Galaxy& galaxy, const char* id)
{
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        if (galaxy.systems[i].authoredId == id) {
            return i;
        }
    }
    return kNotPlaced;
}

AuthoredSystem anywhereSystem(const char* id)
{
    AuthoredSystem authored;
    authored.id = id;
    authored.placement = sol::sim::Placement::Anywhere;
    return authored;
}

} // namespace

// Stage B's first exit clause: `anywhere` GROWS the galaxy, and it grows it at
// the end. The index arithmetic is the contract - an `anywhere` system is
// `systemCount + k` - because that is what lets everything the seed produced
// keep the index it had.
SOL_TEST(universe_anywhere_appends_a_node_and_moves_no_procedural_one)
{
    const GalaxyParams plain = testParams(31337);
    const Galaxy before = generateGalaxy(plain);

    GalaxyParams params = plain;
    params.authoredSystems.push_back(anywhereSystem("test.first"));
    params.authoredSystems.push_back(anywhereSystem("test.second"));
    const Galaxy after = generateGalaxy(params);

    SOL_REQUIRE(after.systems.size() == before.systems.size() + 2);
    SOL_CHECK(indexOfAuthored(after, "test.first") == plain.systemCount);
    SOL_CHECK(indexOfAuthored(after, "test.second") == plain.systemCount + 1);

    // EVERY PROCEDURAL POSITION IS UNMOVED, WHICH IS THE PROPERTY THAT
    // PRE-SEEDING WOULD HAVE COST. `scatterSystems` rejection-tests each
    // candidate against every system already placed, so a position seeded
    // ahead of it perturbs the whole scatter; appended after it, the authored
    // node cannot be seen by a draw that has already happened.
    for (std::uint32_t i = 0; i < before.systems.size(); ++i) {
        SOL_CHECK(after.systems[i].mapPosition.x == before.systems[i].mapPosition.x);
        SOL_CHECK(after.systems[i].mapPosition.y == before.systems[i].mapPosition.y);
        SOL_CHECK(after.systems[i].mapPosition.z == before.systems[i].mapPosition.z);
    }
}

// THE NAME STREAM SURVIVES AN INSERTION, AND THIS IS THE TEST THAT SAYS SO.
// Stage A's expensive finding was that touching the name loop for an authored
// index renames the galaxy; an appended node grows that loop, which is a
// second way to reach the same disaster. It does not, because the extra draws
// all land AFTER the procedural ones and the uniqueness rescan only ever looks
// backwards at j < i.
SOL_TEST(universe_anywhere_does_not_rename_the_galaxy)
{
    const GalaxyParams plain = testParams(31337);
    const Galaxy before = generateGalaxy(plain);

    GalaxyParams params = plain;
    params.authoredSystems.push_back(anywhereSystem("test.newcomer"));
    const Galaxy after = generateGalaxy(params);

    SOL_REQUIRE(after.systems.size() == before.systems.size() + 1);
    for (std::uint32_t i = 0; i < before.systems.size(); ++i) {
        SOL_CHECK(after.systems[i].name == before.systems[i].name);
    }
    // And the newcomer got a name of its own rather than an empty one.
    SOL_CHECK(!after.systems.back().name.empty());
}

// An appended node is not a stranded island: Prim runs over the grown vector,
// so it takes an MST edge like everything else and is reachable from system 0.
SOL_TEST(universe_anywhere_is_gated_in_like_any_other_node)
{
    GalaxyParams params = testParams(909);
    params.authoredSystems.push_back(anywhereSystem("test.reachable"));
    const Galaxy galaxy = generateGalaxy(params);

    const std::uint32_t index = indexOfAuthored(galaxy, "test.reachable");
    SOL_REQUIRE(index != kNotPlaced);
    SOL_CHECK(!galaxy.systems[index].gates.empty());
    SOL_CHECK(!routeBetween(galaxy, 0, index).empty());
}

// AN INSERTION NEEDS NO `assignRegions` SKIP POINT, WHICH REFUTES STAGE A'S
// OWN FORWARD-LOOKING COMMENT. The node is appended BEFORE regions are
// assigned - so a guard looked mandatory - but the author's fields are applied
// in a second pass that runs AFTER them, so the write is simply the last word.
// Splitting "make the node" from "apply the fields" removed the guard instead
// of adding one.
SOL_TEST(universe_anywhere_keeps_its_authored_region)
{
    GalaxyParams params = testParams(1234);
    AuthoredSystem authored = anywhereSystem("test.core");
    authored.region = Region::Core;
    authored.hasRegion = true;
    params.authoredSystems.push_back(authored);

    const Galaxy galaxy = generateGalaxy(params);
    const SystemSpec* spec = findAuthored(galaxy, "test.core");
    SOL_REQUIRE(spec != nullptr);
    SOL_CHECK(spec->region == Region::Core);
}

// Stage B's second exit clause, and it is measured with the primitive
// decisions/018 named rather than with a second opinion written beside it.
SOL_TEST(universe_jumps_from_lands_inside_the_ring_it_asked_for)
{
    GalaxyParams params = testParams(77777);
    params.authoredSystems.push_back({.id = "test.anchor"});
    AuthoredSystem ring;
    ring.id = "test.ring";
    ring.placement = sol::sim::Placement::JumpsFrom;
    ring.anchorId = "test.anchor";
    ring.jumpsMin = 2;
    ring.jumpsMax = 4;
    params.authoredSystems.push_back(ring);

    std::vector<sol::sim::AuthoredPlacementFailure> failures;
    const Galaxy galaxy = generateGalaxy(params, nullptr, &failures);
    SOL_CHECK(failures.empty());

    const std::uint32_t anchor = indexOfAuthored(galaxy, "test.anchor");
    const std::uint32_t placed = indexOfAuthored(galaxy, "test.ring");
    SOL_REQUIRE(anchor != kNotPlaced);
    SOL_REQUIRE(placed != kNotPlaced);

    const std::vector<std::uint32_t> route = routeBetween(galaxy, anchor, placed);
    SOL_REQUIRE(!route.empty());
    const std::uint32_t jumps = static_cast<std::uint32_t>(route.size()) - 1;
    std::printf("  ring: anchor %u -> placed %u is %u jump(s)\n", anchor, placed, jumps);
    SOL_CHECK(jumps >= 2);
    SOL_CHECK(jumps <= 4);
}

// THIS IS THE TEST THAT WOULD HAVE FAILED BEFORE THE GATE LISTS WERE HOISTED,
// AND IT IS WORTH SAYING WHY IT IS NOT REDUNDANT WITH THE ONE ABOVE.
// `routeBetween` walks `SystemSpec::gates`. Until stage B those were filled
// fifty lines below the placement pass, so at the moment a ring was measured
// every gate list in the galaxy was still empty - and an empty graph makes
// EVERY candidate unreachable, which reads as "no system satisfies this ring"
// rather than as a crash. A ring that CAN be satisfied and is reported
// unsatisfiable is the exact shape of that bug.
SOL_TEST(universe_a_satisfiable_ring_is_not_reported_unsatisfiable)
{
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        GalaxyParams params = testParams(seed);
        params.authoredSystems.push_back({.id = "test.anchor"});
        AuthoredSystem ring;
        ring.id = "test.ring";
        ring.placement = sol::sim::Placement::JumpsFrom;
        ring.anchorId = "test.anchor";
        ring.jumpsMin = 1;
        ring.jumpsMax = 3;
        params.authoredSystems.push_back(ring);

        std::vector<sol::sim::AuthoredPlacementFailure> failures;
        const Galaxy galaxy = generateGalaxy(params, nullptr, &failures);
        SOL_CHECK(failures.empty());
        SOL_CHECK(indexOfAuthored(galaxy, "test.ring") != kNotPlaced);
    }
}

// Stage B's third exit clause: a ring nothing can satisfy refuses BY NAME
// rather than landing somewhere plausible. 60 systems on an MST plus extra
// lanes are nowhere near 40 jumps across.
SOL_TEST(universe_an_unsatisfiable_ring_refuses_by_name)
{
    GalaxyParams params = testParams(2468);
    params.authoredSystems.push_back({.id = "test.anchor"});
    AuthoredSystem ring;
    ring.id = "test.nowhere";
    ring.placement = sol::sim::Placement::JumpsFrom;
    ring.anchorId = "test.anchor";
    ring.jumpsMin = 40;
    ring.jumpsMax = 50;
    params.authoredSystems.push_back(ring);

    std::vector<sol::sim::AuthoredPlacementFailure> failures;
    const Galaxy galaxy = generateGalaxy(params, nullptr, &failures);

    SOL_REQUIRE(failures.size() == 1);
    SOL_CHECK(failures[0].id == "test.nowhere");
    SOL_CHECK(failures[0].rule == "jumps_from");
    std::printf("  refusal: %s\n", failures[0].reason.c_str());
    SOL_CHECK(failures[0].reason.find("test.anchor") != std::string::npos);

    // AND IT IS ABSENT RATHER THAN SOMEWHERE PLAUSIBLE, which is the half of
    // the clause a refusal message alone would not prove. The anchor, which
    // placed fine, is still there - one bad rule does not discard the file.
    SOL_CHECK(indexOfAuthored(galaxy, "test.nowhere") == kNotPlaced);
    SOL_CHECK(indexOfAuthored(galaxy, "test.anchor") != kNotPlaced);
}

// A ring anchored on a system that itself failed to place fails too, rather
// than anchoring on nothing and quietly becoming `random`.
SOL_TEST(universe_a_ring_anchored_on_a_failure_fails_too)
{
    GalaxyParams params = testParams(1357);
    params.authoredSystems.push_back({.id = "test.anchor"});
    AuthoredSystem doomed;
    doomed.id = "test.doomed";
    doomed.placement = sol::sim::Placement::JumpsFrom;
    doomed.anchorId = "test.anchor";
    doomed.jumpsMin = 40;
    doomed.jumpsMax = 50;
    params.authoredSystems.push_back(doomed);
    AuthoredSystem dependent;
    dependent.id = "test.dependent";
    dependent.placement = sol::sim::Placement::JumpsFrom;
    dependent.anchorId = "test.doomed";
    dependent.jumpsMin = 1;
    dependent.jumpsMax = 2;
    params.authoredSystems.push_back(dependent);

    std::vector<sol::sim::AuthoredPlacementFailure> failures;
    const Galaxy galaxy = generateGalaxy(params, nullptr, &failures);

    SOL_REQUIRE(failures.size() == 2);
    SOL_CHECK(failures[1].id == "test.dependent");
    SOL_CHECK(indexOfAuthored(galaxy, "test.dependent") == kNotPlaced);
}

// `at_system` lands on the capital of the faction it names, and it is a real
// capital: the node already belonged to that faction before anything was
// authored onto it.
SOL_TEST(universe_at_system_takes_the_named_factions_capital)
{
    const GalaxyParams plain = testParams(5150);
    const Galaxy before = generateGalaxy(plain);

    GalaxyParams params = plain;
    AuthoredSystem authored;
    authored.id = "test.home";
    authored.placement = sol::sim::Placement::AtSystem;
    authored.atFactionCapital = 1;
    params.authoredSystems.push_back(authored);

    std::vector<sol::sim::AuthoredPlacementFailure> failures;
    const Galaxy after = generateGalaxy(params, nullptr, &failures);
    SOL_REQUIRE(failures.empty());

    const std::uint32_t index = indexOfAuthored(after, "test.home");
    SOL_REQUIRE(index != kNotPlaced);
    SOL_CHECK(before.systems[index].factionIndex == 1);
    SOL_CHECK(after.systems[index].factionIndex == 1);
    SOL_CHECK(after.systems[index].region == Region::Core);
}

// THE HOIST THAT MADE `at_system` POSSIBLE DID NOT MOVE THE CAPITALS, AND THIS
// IS WHAT HOLDS THAT RATHER THAN THE COMMENT ON `chooseCapitals`. Selection was
// lifted out of `claimTerritory` to run before placement; it still takes the
// same first draw from the same faction stream, so a galaxy with no authored
// systems in it is unchanged in every field.
SOL_TEST(universe_lifting_capital_selection_left_the_galaxy_alone)
{
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        const Galaxy galaxy = generateGalaxy(testParams(seed));
        const Galaxy again = generateGalaxy(testParams(seed));
        SOL_REQUIRE(galaxy.systems.size() == again.systems.size());
        for (std::size_t i = 0; i < galaxy.systems.size(); ++i) {
            SOL_CHECK(specsEqual(galaxy.systems[i], again.systems[i]));
        }
    }
}

// An authored owner still outranks a capital, even when the authored system IS
// the capital. Guard 1 of 5 was written for a `random` system that happened to
// land on one; `at_system` makes that the deliberate case rather than the
// coincidence, so the guard now has a test aimed at it.
SOL_TEST(universe_at_system_can_declare_a_capital_lawless)
{
    GalaxyParams params = testParams(5150);
    AuthoredSystem authored;
    authored.id = "test.fallen";
    authored.placement = sol::sim::Placement::AtSystem;
    authored.atFactionCapital = 1;
    authored.hasFaction = true;
    authored.factionIndex = sol::sim::kNoFaction;
    params.authoredSystems.push_back(authored);

    const Galaxy galaxy = generateGalaxy(params);
    const SystemSpec* spec = findAuthored(galaxy, "test.fallen");
    SOL_REQUIRE(spec != nullptr);
    SOL_CHECK(spec->factionIndex == sol::sim::kNoFaction);
}

// Two systems cannot take one capital, and the second is told why rather than
// being quietly rehoused.
SOL_TEST(universe_two_systems_cannot_share_one_capital)
{
    GalaxyParams params = testParams(5150);
    AuthoredSystem first;
    first.id = "test.first";
    first.placement = sol::sim::Placement::AtSystem;
    first.atFactionCapital = 0;
    params.authoredSystems.push_back(first);
    AuthoredSystem second = first;
    second.id = "test.second";
    params.authoredSystems.push_back(second);

    std::vector<sol::sim::AuthoredPlacementFailure> failures;
    const Galaxy galaxy = generateGalaxy(params, nullptr, &failures);

    SOL_REQUIRE(failures.size() == 1);
    SOL_CHECK(failures[0].id == "test.second");
    SOL_CHECK(failures[0].rule == "at_system");
    SOL_CHECK(indexOfAuthored(galaxy, "test.first") != kNotPlaced);
    SOL_CHECK(indexOfAuthored(galaxy, "test.second") == kNotPlaced);
}

// `at_system` naming a faction that holds no capital - removed by a mod, or a
// core too small to give it one - refuses instead of falling back to random.
SOL_TEST(universe_at_system_without_a_capital_refuses)
{
    GalaxyParams params = testParams(5150);
    AuthoredSystem authored;
    authored.id = "test.homeless";
    authored.placement = sol::sim::Placement::AtSystem;
    authored.atFactionCapital = 9; // only three factions exist
    params.authoredSystems.push_back(authored);

    std::vector<sol::sim::AuthoredPlacementFailure> failures;
    const Galaxy galaxy = generateGalaxy(params, nullptr, &failures);
    SOL_REQUIRE(failures.size() == 1);
    SOL_CHECK(failures[0].rule == "at_system");
    SOL_CHECK(indexOfAuthored(galaxy, "test.homeless") == kNotPlaced);
}

// All four rules in one file, resolving in def order and reproducing exactly.
SOL_TEST(universe_every_placement_rule_together_is_deterministic)
{
    GalaxyParams params = testParams(31337);
    params.authoredSystems.push_back({.id = "test.random"});
    params.authoredSystems.push_back(anywhereSystem("test.anywhere"));
    AuthoredSystem capital;
    capital.id = "test.capital";
    capital.placement = sol::sim::Placement::AtSystem;
    capital.atFactionCapital = 2;
    params.authoredSystems.push_back(capital);
    AuthoredSystem ring;
    ring.id = "test.ring";
    ring.placement = sol::sim::Placement::JumpsFrom;
    ring.anchorId = "test.anywhere"; // an insertion is a legal anchor
    ring.jumpsMin = 1;
    ring.jumpsMax = 3;
    params.authoredSystems.push_back(ring);

    std::vector<sol::sim::AuthoredPlacementFailure> failures;
    const Galaxy first = generateGalaxy(params, nullptr, &failures);
    SOL_REQUIRE(failures.empty());
    const Galaxy second = generateGalaxy(params);

    SOL_REQUIRE(first.systems.size() == second.systems.size());
    SOL_CHECK(first.systems.size() == params.systemCount + 1); // one insertion
    for (std::size_t i = 0; i < first.systems.size(); ++i) {
        SOL_CHECK(specsEqual(first.systems[i], second.systems[i]));
        SOL_CHECK(first.systems[i].authoredId == second.systems[i].authoredId);
    }
    for (const char* id : {"test.random", "test.anywhere", "test.capital", "test.ring"}) {
        SOL_CHECK(indexOfAuthored(first, id) != kNotPlaced);
    }
}
