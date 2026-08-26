#include "sol/sim/universe.hpp"

// Station placement asks whether a system has rock (Phase 13). The dependency
// is one-way and lives here in the .cpp: mining.hpp includes universe.hpp, and
// universe.hpp only forward-declares MiningParams, so there is no cycle.
#include "sol/core/assert.hpp"
#include "sol/core/random.hpp"
#include "sol/sim/mining.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace sol::sim {

namespace {

// One Rng stream per generation stage, so adding draws to one stage never
// perturbs another (the random.hpp contract).
constexpr std::uint32_t kInvalidIndex = 0xffff'ffffu;

enum Stream : std::uint64_t
{
    kStreamPositions = 1,
    kStreamNames,
    kStreamFactions,
    kStreamContents,
    kStreamClans,
};

constexpr const char* kNamePrefixes[] = {
    "Al",  "Be",  "Ca",  "Dra", "Er",  "Fen", "Gal", "Hel", "Ith", "Ka",
    "Lyr", "Mar", "Nor", "Oph", "Pra", "Que", "Rig", "Sar", "Tau", "Vel",
};
constexpr const char* kNameMiddles[] = {
    "an",
    "ar",
    "en",
    "es",
    "ia",
    "io",
    "or",
    "ub",
    "un",
    "yr",
};
constexpr const char* kNameSuffixes[] = {
    "a",
    "ea",
    "ia",
    "is",
    "os",
    "ra",
    "th",
    "um",
    "us",
    "yx",
};
constexpr const char* kStationOrdinals[] = {
    "Alpha",
    "Beta",
    "Gamma",
    "Delta",
    "Epsilon",
    "Zeta",
    "Eta",
    "Theta",
};
constexpr const char* kPlanetNumerals[] = {"I", "II", "III", "IV", "V", "VI"};
constexpr const char* kClanSuffixes[] = {
    "Raiders",
    "Corsairs",
    "Cartel",
    "Syndicate",
    "Reavers",
    "Wolves",
    "Marauders",
    "Talons",
};

template <std::size_t N>
[[nodiscard]] const char* pick(core::Rng& rng, const char* const (&table)[N])
{
    return table[rng.range(static_cast<std::uint32_t>(N))];
}

[[nodiscard]] std::string makeSystemName(core::Rng& rng)
{
    std::string name = pick(rng, kNamePrefixes);
    if (rng.nextFloat01() < 0.5f) {
        name += pick(rng, kNameMiddles);
    }
    name += pick(rng, kNameSuffixes);
    return name;
}

// Scatter systems in a thin disc with a minimum pairwise separation
// (best-effort: separation relaxes if rejection keeps failing, so the count
// is always honored).
void scatterSystems(const GalaxyParams& params, core::Rng& rng, std::vector<SystemSpec>& systems)
{
    const float radius = params.galaxyRadius;
    float separation = radius / std::sqrt(static_cast<float>(params.systemCount)) * 1.1f;
    constexpr float kTau = 6.28318530717958647692f;
    while (systems.size() < params.systemCount) {
        bool placed = false;
        for (std::uint32_t tries = 0; tries < 64 && !placed; ++tries) {
            const float r = radius * std::sqrt(rng.nextFloat01());
            const float theta = kTau * rng.nextFloat01();
            const core::Vec3 candidate{
                r * std::cos(theta), 0.08f * radius * (rng.nextFloat01() * 2.0f - 1.0f), r * std::sin(theta)};
            bool clear = true;
            for (const SystemSpec& other : systems) {
                if (core::length(candidate - other.mapPosition) < separation) {
                    clear = false;
                    break;
                }
            }
            if (clear) {
                SystemSpec spec;
                spec.mapPosition = candidate;
                systems.push_back(std::move(spec));
                placed = true;
            }
        }
        if (!placed) {
            separation *= 0.9f;
        }
    }
}

[[nodiscard]] float mapDistance(const SystemSpec& a, const SystemSpec& b)
{
    return core::length(a.mapPosition - b.mapPosition);
}

// Minimum spanning tree (Prim, O(n^2) — fine at <=150 systems) guarantees
// connectivity; then up to extraGatesPerSystem nearest-neighbor links per
// system add loops so the graph isn't a tree.
void buildGateGraph(const GalaxyParams& params,
                    std::vector<SystemSpec>& systems,
                    std::vector<GateLink>& links)
{
    const std::uint32_t count = static_cast<std::uint32_t>(systems.size());
    std::vector<bool> inTree(count, false);
    std::vector<float> bestCost(count, std::numeric_limits<float>::max());
    std::vector<std::uint32_t> bestFrom(count, 0);
    std::vector<std::vector<std::uint32_t>> adjacency(count);

    const auto addLink = [&](std::uint32_t a, std::uint32_t b) {
        if (a > b) {
            std::swap(a, b);
        }
        for (const std::uint32_t neighbor : adjacency[a]) {
            if (neighbor == b) {
                return;
            }
        }
        adjacency[a].push_back(b);
        adjacency[b].push_back(a);
        links.push_back({a, b});
    };

    inTree[0] = true;
    for (std::uint32_t i = 1; i < count; ++i) {
        bestCost[i] = mapDistance(systems[0], systems[i]);
    }
    for (std::uint32_t added = 1; added < count; ++added) {
        std::uint32_t next = 0;
        float nextCost = std::numeric_limits<float>::max();
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!inTree[i] && bestCost[i] < nextCost) {
                nextCost = bestCost[i];
                next = i;
            }
        }
        inTree[next] = true;
        addLink(bestFrom[next], next);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!inTree[i]) {
                const float cost = mapDistance(systems[next], systems[i]);
                if (cost < bestCost[i]) {
                    bestCost[i] = cost;
                    bestFrom[i] = next;
                }
            }
        }
    }

    // Extra lanes: each system links to its nearest not-yet-linked neighbors,
    // capped by length so shortcuts stay local.
    const float maxExtraLength = params.galaxyRadius / std::sqrt(static_cast<float>(count)) * 3.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        for (std::uint32_t extra = 0; extra < params.extraGatesPerSystem; ++extra) {
            std::uint32_t nearest = kInvalidIndex;
            float nearestCost = maxExtraLength;
            for (std::uint32_t j = 0; j < count; ++j) {
                if (j == i) {
                    continue;
                }
                const bool linked =
                    std::find(adjacency[i].begin(), adjacency[i].end(), j) != adjacency[i].end();
                if (linked) {
                    continue;
                }
                const float cost = mapDistance(systems[i], systems[j]);
                if (cost < nearestCost) {
                    nearestCost = cost;
                    nearest = j;
                }
            }
            if (nearest != kInvalidIndex) {
                addLink(i, nearest);
            }
        }
    }
}

void assignRegions(const GalaxyParams& params, std::vector<SystemSpec>& systems)
{
    const float coreRadius = params.galaxyRadius * params.coreRadiusFraction;
    const float frontierRadius = params.galaxyRadius * params.frontierRadiusFraction;
    for (SystemSpec& system : systems) {
        const float r = core::length(system.mapPosition);
        system.region = r < coreRadius       ? Region::Core
                        : r < frontierRadius ? Region::Frontier
                                             : Region::Fringe;
    }
}

// Faction capitals spread greedily through the core (farthest-point), then
// multi-source Dijkstra over the gate graph claims territory; fringe systems
// roll to stay lawless.
void claimTerritory(const GalaxyParams& params,
                    core::Rng& rng,
                    std::vector<SystemSpec>& systems,
                    const std::vector<GateLink>& links)
{
    if (params.factionCount == 0) {
        return;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(systems.size());
    std::vector<std::uint32_t> candidates;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (systems[i].region == Region::Core) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) { // degenerate params: claim from anywhere
        for (std::uint32_t i = 0; i < count; ++i) {
            candidates.push_back(i);
        }
    }

    std::vector<std::uint32_t> capitals;
    capitals.push_back(candidates[rng.range(static_cast<std::uint32_t>(candidates.size()))]);
    while (capitals.size() < params.factionCount && capitals.size() < candidates.size()) {
        std::uint32_t farthest = candidates[0];
        float farthestCost = -1.0f;
        for (const std::uint32_t candidate : candidates) {
            float nearestCapital = std::numeric_limits<float>::max();
            for (const std::uint32_t capital : capitals) {
                nearestCapital = std::min(nearestCapital, mapDistance(systems[candidate], systems[capital]));
            }
            if (nearestCapital > farthestCost) {
                farthestCost = nearestCapital;
                farthest = candidate;
            }
        }
        if (farthestCost <= 0.0f) {
            break; // candidate already a capital: core smaller than factionCount
        }
        capitals.push_back(farthest);
    }

    std::vector<std::vector<std::uint32_t>> adjacency(count);
    for (const GateLink& link : links) {
        adjacency[link.a].push_back(link.b);
        adjacency[link.b].push_back(link.a);
    }

    // (distance, system) min-heap; ties break on system index for determinism.
    using Entry = std::pair<float, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    std::vector<float> bestCost(count, std::numeric_limits<float>::max());
    for (std::uint32_t f = 0; f < capitals.size(); ++f) {
        const std::uint32_t capital = capitals[f];
        systems[capital].factionIndex = f;
        bestCost[capital] = 0.0f;
        queue.push({0.0f, capital});
    }
    while (!queue.empty()) {
        const auto [cost, index] = queue.top();
        queue.pop();
        if (cost > bestCost[index]) {
            continue;
        }
        for (const std::uint32_t neighbor : adjacency[index]) {
            const float next = cost + mapDistance(systems[index], systems[neighbor]);
            if (next < bestCost[neighbor]) {
                bestCost[neighbor] = next;
                systems[neighbor].factionIndex = systems[index].factionIndex;
                queue.push({next, neighbor});
            }
        }
    }

    // Lawless fringe: one roll per system, in index order.
    for (SystemSpec& system : systems) {
        if (system.region == Region::Fringe && rng.nextFloat01() < params.fringeLawlessChance) {
            system.factionIndex = kNoFaction;
        }
    }
}

// Pirate clans (Phase 8b): every connected component of still-lawless
// systems becomes one generated clan; its members' factionIndex continues
// past the majors, so downstream tables (relations, reputation, catalogs)
// are just sized to factionCount + clans.size(). Component walk is in index
// order and all draws come from one dedicated stream => deterministic.
void spawnClans(const GalaxyParams& params,
                core::Rng& rng,
                std::vector<SystemSpec>& systems,
                const std::vector<GateLink>& links,
                std::vector<ClanSpec>& clans)
{
    if (params.factionCount == 0 || params.pirateTemplateCount == 0) {
        return;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(systems.size());
    std::vector<std::vector<std::uint32_t>> adjacency(count);
    for (const GateLink& link : links) {
        adjacency[link.a].push_back(link.b);
        adjacency[link.b].push_back(link.a);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (systems[i].factionIndex != kNoFaction) {
            continue;
        }
        const std::uint32_t clanFaction = params.factionCount + static_cast<std::uint32_t>(clans.size());
        std::vector<std::uint32_t> frontier{i};
        systems[i].factionIndex = clanFaction;
        while (!frontier.empty()) {
            const std::uint32_t index = frontier.back();
            frontier.pop_back();
            for (const std::uint32_t neighbor : adjacency[index]) {
                if (systems[neighbor].factionIndex == kNoFaction) {
                    systems[neighbor].factionIndex = clanFaction;
                    frontier.push_back(neighbor);
                }
            }
        }
        clans.push_back({
            .name = systems[i].name + " " + pick(rng, kClanSuffixes),
            .templateIndex = rng.range(params.pirateTemplateCount),
            .seed = rng.nextU64(),
            .homeSystem = i,
        });
    }
}

// The weight one archetype carries in one system: its region tuning, vetoed to
// zero when it needs rock the system does not have (Phase 13).
//
// ⚑ A veto, not a re-roll. Zeroing the weight and rolling over what is left
// means a rockless system builds something ELSE rather than building nothing,
// so no system loses a station it would have had — only the outpost that could
// never have produced anything moves aside for one that can.
// What the faction holding this system does to one archetype's odds
// (Phase 13). Anything unstated — no bias table, a faction with no character,
// a lawless system, a row that does not reach this archetype — is 1.0, which
// is the pre-Phase-13 galaxy exactly.
[[nodiscard]] float factionBias(const GalaxyParams& params, std::uint32_t faction, std::uint32_t archetype)
{
    if (faction >= params.factionStationBias.size()) {
        return 1.0f;
    }
    const std::vector<float>& row = params.factionStationBias[faction];
    return archetype < row.size() ? row[archetype] : 1.0f;
}

// The weight one archetype carries in one system: its region tuning, tilted by
// its owner's character, vetoed to zero when it needs rock the system lacks.
[[nodiscard]] float archetypeWeight(const GalaxyParams& params,
                                    std::uint32_t archetype,
                                    std::size_t tier,
                                    std::uint32_t faction,
                                    std::uint32_t systemFieldCount,
                                    bool enforceFields)
{
    const StationRule& rule = params.stationRules[archetype];
    if (enforceFields && rule.requiresField && systemFieldCount == 0) {
        return 0.0f;
    }
    // ⚑ Multiply, never replace: the region tuning decides the baseline (ore in
    // the fringe, factories in the core) and the faction only tilts it, so a
    // character cannot quietly undo the economy's geography.
    return rule.weight[tier] * factionBias(params, faction, archetype);
}

[[nodiscard]] std::uint32_t pickArchetype(const GalaxyParams& params,
                                          core::Rng& rng,
                                          Region region,
                                          std::uint32_t faction,
                                          std::uint32_t systemFieldCount,
                                          bool enforceFields)
{
    if (params.stationRules.empty()) {
        return 0;
    }
    const std::uint32_t ruleCount = static_cast<std::uint32_t>(params.stationRules.size());
    const std::size_t tier = static_cast<std::size_t>(region);
    float total = 0.0f;
    for (std::uint32_t i = 0; i < ruleCount; ++i) {
        total += archetypeWeight(params, i, tier, faction, systemFieldCount, enforceFields);
    }
    // Every candidate vetoed — by the rock rule, or by a faction that zeroed
    // everything it could build here. Fall back to the unfiltered roll rather
    // than refusing to build: a system with no stations at all is a worse
    // world than an out-of-character one, and a mod can produce this.
    if (total <= 0.0f) {
        return enforceFields || faction != kNoFaction
                   ? pickArchetype(params, rng, region, kNoFaction, 0, false)
                   : 0;
    }
    float roll = rng.nextFloat01() * total;
    for (std::uint32_t i = 0; i < ruleCount; ++i) {
        roll -= archetypeWeight(params, i, tier, faction, systemFieldCount, enforceFields);
        if (roll <= 0.0f) {
            return i;
        }
    }
    return ruleCount - 1;
}

// Star, planets (AU-scale scenery; primary planet hosts the playfield),
// stations near the primary planet, gates toward each linked neighbor.
void populateSystem(const GalaxyParams& params,
                    std::uint32_t index,
                    SystemSpec& system,
                    const std::vector<SystemSpec>& systems,
                    core::Rng& rng,
                    const MiningParams* mining)
{
    // Asked once per system, before any station is placed. A field is a pure
    // function of the system's seed and region and never depends on what was
    // built there, so this is a read of content that already exists rather
    // than a decision that has to be sequenced against station placement.
    const std::uint32_t fieldCount = mining != nullptr ? fieldCountFor(system, *mining) : 0;
    const bool enforceFields = mining != nullptr;

    system.starRadius = 7.0e8 * (0.6 + 0.9 * rng.nextDouble01());

    const std::uint32_t planetCount = 1 + rng.range(4);
    double orbit = 4.0e10 * (1.0 + rng.nextDouble01()); // innermost 0.27-0.53 AU
    for (std::uint32_t p = 0; p < planetCount; ++p) {
        PlanetSpec planet;
        planet.name =
            system.name + " " + kPlanetNumerals[std::min<std::size_t>(p, std::size(kPlanetNumerals) - 1)];
        planet.radius = 2.5e6 + 4.5e7 * rng.nextDouble01();
        planet.position = randomPlayfieldDirection(rng) * orbit;
        orbit *= 1.6 + 0.8 * rng.nextDouble01();
        system.planets.push_back(std::move(planet));
    }
    // Hub: middle planet, so inner/outer scenery flanks the playfield.
    system.primaryPlanet = planetCount / 2;
    const core::DVec3 hub = system.planets[system.primaryPlanet].position;

    const std::size_t tier = static_cast<std::size_t>(system.region);
    const std::uint32_t minStations = params.stationCount[tier][0];
    const std::uint32_t maxStations = params.stationCount[tier][1];
    const std::uint32_t stationCount =
        minStations + (maxStations > minStations ? rng.range(maxStations - minStations + 1) : 0);
    for (std::uint32_t s = 0; s < stationCount; ++s) {
        StationSpec station;
        station.archetype =
            pickArchetype(params, rng, system.region, system.factionIndex, fieldCount, enforceFields);
        station.name =
            system.name + " " + kStationOrdinals[std::min<std::size_t>(s, std::size(kStationOrdinals) - 1)];
        const double distance = params.stationMinDistance +
                                (params.stationMaxDistance - params.stationMinDistance) * rng.nextDouble01();
        station.position = hub + randomPlayfieldDirection(rng) * distance;
        system.stations.push_back(std::move(station));
    }

    // Gates sit at the playfield edge in the map-space direction of their
    // destination, so the system layout reads like the galaxy map.
    for (GateSpec& gate : system.gates) {
        const core::Vec3 toNeighbor = systems[gate.toSystem].mapPosition - systems[index].mapPosition;
        core::DVec3 direction{toNeighbor.x, toNeighbor.y, toNeighbor.z};
        const double len = core::length(direction);
        direction = len > 0.0 ? direction * (1.0 / len) : core::DVec3{1.0, 0.0, 0.0};
        gate.position = hub + direction * params.gateDistance;
    }
}

} // namespace

core::DVec3 randomPlayfieldDirection(core::Rng& rng)
{
    constexpr double kTau = 6.283185307179586476925;
    const double theta = kTau * rng.nextDouble01();
    const double y = 0.25 * (rng.nextDouble01() * 2.0 - 1.0);
    return core::normalize(core::DVec3{std::cos(theta), y, std::sin(theta)});
}

core::DVec3 playfieldHub(const SystemSpec& spec)
{
    if (spec.planets.empty()) {
        return {};
    }
    const std::size_t index = std::min<std::size_t>(spec.primaryPlanet, spec.planets.size() - 1);
    return spec.planets[index].position;
}

Galaxy generateGalaxy(const GalaxyParams& params, const MiningParams* mining)
{
    SOL_ASSERT(params.systemCount >= 2);
    Galaxy galaxy;
    galaxy.seed = params.seed;
    galaxy.systems.reserve(params.systemCount);

    core::Rng positionRng(params.seed, kStreamPositions);
    scatterSystems(params, positionRng, galaxy.systems);
    assignRegions(params, galaxy.systems);
    buildGateGraph(params, galaxy.systems, galaxy.links);

    core::Rng nameRng(params.seed, kStreamNames);
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        std::string name = makeSystemName(nameRng);
        for (std::uint32_t j = 0; j < i;) { // linear uniqueness scan; n <= 150
            if (galaxy.systems[j].name == name) {
                name = makeSystemName(nameRng);
                j = 0;
                continue;
            }
            ++j;
        }
        galaxy.systems[i].name = std::move(name);
        galaxy.systems[i].seed = core::Rng(params.seed, kStreamContents + i).nextU64();
    }

    core::Rng factionRng(params.seed, kStreamFactions);
    claimTerritory(params, factionRng, galaxy.systems, galaxy.links);

    core::Rng clanRng(params.seed, kStreamClans);
    spawnClans(params, clanRng, galaxy.systems, galaxy.links, galaxy.clans);

    for (const GateLink& link : galaxy.links) {
        galaxy.systems[link.a].gates.push_back({link.b, {}});
        galaxy.systems[link.b].gates.push_back({link.a, {}});
    }
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        core::Rng contentRng(galaxy.systems[i].seed, kStreamContents);
        populateSystem(params, i, galaxy.systems[i], galaxy.systems, contentRng, mining);
    }
    return galaxy;
}

std::vector<std::uint32_t> routeBetween(const Galaxy& galaxy, std::uint32_t from, std::uint32_t to)
{
    const std::uint32_t count = static_cast<std::uint32_t>(galaxy.systems.size());
    if (from >= count || to >= count) {
        return {};
    }
    if (from == to) {
        return {from};
    }
    std::vector<std::uint32_t> previous(count, kInvalidIndex);
    std::vector<std::uint32_t> frontier{from};
    previous[from] = from;
    while (!frontier.empty() && previous[to] == kInvalidIndex) {
        std::vector<std::uint32_t> next;
        for (const std::uint32_t index : frontier) {
            for (const GateSpec& gate : galaxy.systems[index].gates) {
                if (previous[gate.toSystem] == kInvalidIndex) {
                    previous[gate.toSystem] = index;
                    next.push_back(gate.toSystem);
                }
            }
        }
        frontier = std::move(next);
    }
    if (previous[to] == kInvalidIndex) {
        return {};
    }
    std::vector<std::uint32_t> route;
    for (std::uint32_t index = to; index != from; index = previous[index]) {
        route.push_back(index);
    }
    route.push_back(from);
    std::reverse(route.begin(), route.end());
    return route;
}

} // namespace sol::sim
