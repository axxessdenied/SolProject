#pragma once

// Seeded procedural galaxy generation (engine plan Phase 7). The generator
// produces a *plan* — star systems on a galaxy map, a connected jump-gate
// graph, region tiers, faction territory claims, and per-system content
// specs (star, planets, stations, gates) — which the game instantiates into
// entities using its data defs. Same seed + same params => same galaxy, so
// everything here draws from sol::core::Rng streams and iterates in index
// order; no unordered containers, no wall clock.
//
// Layout honors decisions/004 (gates are the inter-system baseline) and
// decisions/005 (no time compression): each system's visitable content —
// stations and gates — clusters in a playfield around a primary planet so
// cruise legs stay within a real-time budget, while the star and outer
// planets remain AU-scale scenery.

#include "sol/core/math/math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sol::sim {

// Difficulty/opportunity gradient from the GDD: civilized core, contested
// frontier, lawless fringe, assigned by galaxy-map radius.
enum class Region : std::uint8_t
{
    Core = 0,
    Frontier,
    Fringe,
};

inline constexpr std::uint32_t kNoFaction = 0xffff'ffffu; // lawless system

// How often one station archetype appears per region; the game supplies one
// rule per station def, and specs refer back to it by index.
struct StationRule
{
    float weight[3] = {1.0f, 1.0f, 1.0f}; // Core, Frontier, Fringe
};

struct GalaxyParams
{
    std::uint64_t seed = 0;
    std::uint32_t systemCount = 80; // GDD target band is 50-150
    float galaxyRadius = 60.0f;     // light-years, map space
    // Region thresholds as fractions of galaxyRadius.
    float coreRadiusFraction = 0.35f;
    float frontierRadiusFraction = 0.70f;
    std::uint32_t factionCount = 0;    // territory claimants (capitals in the core)
    float fringeLawlessChance = 0.6f;  // fringe systems that stay unclaimed
    std::uint32_t extraGatesPerSystem = 1; // nearest-neighbor links beyond the MST
    // In-system playfield (meters, sim space). Stations sit within
    // [stationMinDistance, stationMaxDistance] of the primary planet; gates
    // at gateDistance. 2*gateDistance bounds a full system transit, which is
    // what the decisions/005 cruise-leg budget tunes against.
    double stationMinDistance = 1.0e8;
    double stationMaxDistance = 4.0e8;
    double gateDistance = 6.0e8;
    // Station count ranges [min, max] per region tier.
    std::uint32_t stationCount[3][2] = {{2, 4}, {1, 3}, {0, 2}};
    std::vector<StationRule> stationRules; // empty => every station archetype 0
};

struct PlanetSpec
{
    std::string name;
    core::DVec3 position; // system barycenter frame, meters
    double radius = 0.0;  // meters
};

struct StationSpec
{
    std::string name;
    std::uint32_t archetype = 0; // index into GalaxyParams::stationRules
    core::DVec3 position;
};

struct GateSpec
{
    std::uint32_t toSystem = 0; // destination system index
    core::DVec3 position;
};

struct SystemSpec
{
    std::string name;
    core::Vec3 mapPosition; // light-years, galaxy map space (not sim space)
    Region region = Region::Fringe;
    std::uint32_t factionIndex = kNoFaction;
    std::uint64_t seed = 0;    // per-system stream, for later instantiation needs
    double starRadius = 0.0;   // meters
    std::uint32_t primaryPlanet = 0; // index into planets; hub of the playfield
    std::vector<PlanetSpec> planets;
    std::vector<StationSpec> stations;
    std::vector<GateSpec> gates; // one per link touching this system
};

// Undirected jump lane between two system indices (a < b).
struct GateLink
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

struct Galaxy
{
    std::uint64_t seed = 0;
    std::vector<SystemSpec> systems;
    std::vector<GateLink> links;
};

// Generates the full galaxy plan. The gate graph is always connected.
[[nodiscard]] Galaxy generateGalaxy(const GalaxyParams& params);

// Fewest-jumps route through the gate graph, inclusive of endpoints; empty
// if unreachable (cannot happen for generateGalaxy output) or on bad input.
[[nodiscard]] std::vector<std::uint32_t> routeBetween(const Galaxy& galaxy, std::uint32_t from,
                                                      std::uint32_t to);

} // namespace sol::sim
