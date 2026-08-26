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
#include "sol/core/random.hpp"

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
// "No system at all", as opposed to system 0. Lives here rather than in the
// first sibling that wanted it (survey.hpp, Phase 8e) because a second one
// now does: Phase 8x asks where a coarse trader is, and a trader between
// gates is honestly nowhere.
inline constexpr std::uint32_t kNoSystem = 0xffff'ffffu;

// Mining tuning, needed here only so station placement can ask whether a
// system has rock (Phase 13). Forward-declared rather than included: mining.hpp
// includes THIS header, so taking it by name keeps generation free of the
// mining layer while letting universe.cpp reach fieldCountFor.
struct MiningParams;

// How often one station archetype appears per region; the game supplies one
// rule per station def, and specs refer back to it by index.
struct StationRule
{
    float weight[3] = {1.0f, 1.0f, 1.0f}; // Core, Frontier, Fringe
    // This archetype's output comes out of the ground (Phase 13), so it needs
    // asteroid fields in its own system to produce anything at all. Set from
    // StationDef::producesFrom == "field", whose own comment has stated the
    // rule since 8g: "a system with no rock supports no mine". Generation
    // ignored it until now, which is what put outposts over empty systems.
    bool requiresField = false;
};

struct GalaxyParams
{
    std::uint64_t seed = 0;
    std::uint32_t systemCount = 80; // GDD target band is 50-150
    float galaxyRadius = 60.0f;     // light-years, map space
    // Region thresholds as fractions of galaxyRadius.
    float coreRadiusFraction = 0.35f;
    float frontierRadiusFraction = 0.70f;
    std::uint32_t factionCount = 0;   // territory claimants (capitals in the core)
    float fringeLawlessChance = 0.6f; // fringe systems that stay unclaimed
    // Pirate clan templates available (Phase 8b): > 0 turns each connected
    // neighborhood of lawless systems into a generated clan whose faction
    // index continues past the majors (factionCount + clan index). 0 keeps
    // lawless systems at kNoFaction (pre-8b behavior).
    std::uint32_t pirateTemplateCount = 0;
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
    // What each faction prefers to build, [faction][archetype] (Phase 13), as
    // a multiplier on the region weight above. Empty — the default — means no
    // faction has a character and every system builds to the region baseline,
    // which is what the galaxy did before this existed. A row shorter than
    // stationRules, or an index past the end, reads as 1.0.
    //
    // ⚑ Indexed by the faction that HOLDS the system, so a system that changed
    // hands is not still building to its founder's taste.
    std::vector<std::vector<float>> factionStationBias;
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
    std::uint64_t seed = 0;          // per-system stream, for later instantiation needs
    double starRadius = 0.0;         // meters
    std::uint32_t primaryPlanet = 0; // index into planets; hub of the playfield
    std::vector<PlanetSpec> planets;
    std::vector<StationSpec> stations;
    std::vector<GateSpec> gates; // one per link touching this system
};

// A unit direction biased to the orbital plane (y small): the playfield is
// flat-ish per the GDD, and every in-system placement — planets, stations,
// gates, signals, asteroid fields — is scattered with this so a system reads
// as a disc rather than a ball.
[[nodiscard]] core::DVec3 randomPlayfieldDirection(core::Rng& rng);

// The primary planet's position: the hub the visitable playfield clusters
// around. Zero for a system with no planets.
[[nodiscard]] core::DVec3 playfieldHub(const SystemSpec& spec);

// Undirected jump lane between two system indices (a < b).
struct GateLink
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

// A generated pirate clan (Phase 8b): one per connected neighborhood of
// lawless fringe systems. Its faction index is params.factionCount + its
// position in Galaxy::clans; personality/color jitter derives from seed
// against the template (game side, where the defs live).
struct ClanSpec
{
    std::string name;
    std::uint32_t templateIndex = 0; // pirate template def, game-side order
    std::uint64_t seed = 0;
    std::uint32_t homeSystem = 0; // lowest-index member system
};

struct Galaxy
{
    std::uint64_t seed = 0;
    std::vector<SystemSpec> systems;
    std::vector<GateLink> links;
    std::vector<ClanSpec> clans;
};

// Generates the full galaxy plan. The gate graph is always connected.
//
// `mining` is optional and opt-in (Phase 13): supplied, station placement
// consults each system's asteroid field count and will not site an archetype
// whose output comes out of the ground where there is none. Null keeps the
// pre-Phase-13 behaviour exactly, which is why every caller that does not care
// about rock — and every test written before this rule existed — is unchanged.
[[nodiscard]] Galaxy generateGalaxy(const GalaxyParams& params, const MiningParams* mining = nullptr);

// Fewest-jumps route through the gate graph, inclusive of endpoints; empty
// if unreachable (cannot happen for generateGalaxy output) or on bad input.
[[nodiscard]] std::vector<std::uint32_t>
routeBetween(const Galaxy& galaxy, std::uint32_t from, std::uint32_t to);

} // namespace sol::sim
