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

// ---------------------------------------------------------------------------
// Authored systems (Phase 29). These cross the game/sim seam as plain structs
// exactly the way `StationRule` does: `sol::sim` never learns what a def is,
// and ids have already been resolved to indices by the time they arrive.
//
// ⚑⚑ EVERY OPTIONAL FIELD CARRIES ITS OWN `has…` FLAG, because two of the
// fields it writes into have no free sentinel: `kNoFaction` means both "unset"
// and "deliberately lawless", and `primaryPlanet == 0` means both "unset" and
// "the first planet". A generation stage asks the flag, never the value.
// ---------------------------------------------------------------------------

struct AuthoredPlanet
{
    std::string name;
    double radius = 0.0; // metres; unset lets the generator roll one
    bool hasRadius = false;
};

struct AuthoredStation
{
    std::string name;
    std::uint32_t archetype = 0; // index into GalaxyParams::stationRules
};

// How the generator chooses which node an authored system becomes (Phase 29).
//
// ⚑⚑⚑ THREE OF THE FOUR REPLACE AN EXISTING NODE AND ONE CREATES ONE, WHICH
// IS WHAT SPLITS PLACEMENT INTO TWO PASSES RATHER THAN ONE. `Anywhere` appends
// a node after `scatterSystems` and before the gate graph is built, so it is
// woven in like any other system; the other three pick a node that already
// exists, after `buildGateGraph`, and overwrite its spec.
enum class Placement : std::uint8_t
{
    Random = 0, // any ordinary node, drawn from the authored stream
    Anywhere,   // a new node at a new map position; grows the galaxy
    AtSystem,   // the capital of a named faction
    JumpsFrom,  // a ring of gate distance around an earlier authored system
};

struct AuthoredSystem
{
    // The author's own id. It is the only stable handle on a place in this
    // galaxy: procedural names are rolled AFTER placement, so they are a fact
    // about one seed at one systemCount rather than a name anything can rely on.
    std::string id;
    Placement placement = Placement::Random;
    // `AtSystem` only. The major-faction index whose capital to take, resolved
    // at the seam the way every other def id is.
    //
    // ⚑⚑ A FACTION CAPITAL IS THE ONLY STABLE PROCEDURAL LANDMARK IN THIS
    // GALAXY, AND IT IS THE ONLY THING `at_system` CAN LEGALLY NAME. The rule
    // was specified as naming an authored id, which cannot work: every
    // authored id belongs to a system that already occupies its own node, so
    // every legal argument was a contradiction. Procedural NAMES are worse -
    // they are rolled after placement and are a fact about one seed at one
    // system count. Capitals are chosen from map geometry, which an authored
    // replacement does not move.
    std::uint32_t atFactionCapital = kNoFaction;
    // `JumpsFrom` only: an EARLIER authored system's id, and a closed ring of
    // gate distance around the node it took. Def order is what makes this
    // resolvable - the anchor has an index by the time this row is read.
    std::string anchorId;
    std::uint32_t jumpsMin = 0;
    std::uint32_t jumpsMax = 0;
    std::string name;
    bool hasName = false;
    Region region = Region::Fringe;
    bool hasRegion = false;
    std::uint32_t factionIndex = kNoFaction;
    bool hasFaction = false; // true for a named owner AND for authored lawlessness
    std::uint32_t primaryPlanet = 0;
    bool hasPrimaryPlanet = false;
    // How hard this place is held, as a MAGNITUDE in [0, 1] (Phase 30 stage E).
    // Present, it replaces whichever curve `assignSecurity` would have run.
    //
    // ⚑⚑ THE SIGN IS DELIBERATELY NOT HERE, AND THAT IS DECISION 2 BEING TAKEN
    // SERIOUSLY RATHER THAN A SIMPLIFICATION. `SystemSpec::security` is signed
    // and the sign names WHO polices the place; who holds a system is settled
    // by `claimTerritory` and `spawnClans`, not by the author, so a signed
    // authored value could contradict the galaxy it was written into. The
    // generator signs the magnitude from the owner and the contradiction is
    // not expressible.
    float security = 0.0f;
    bool hasSecurity = false;
    bool secret = false;
    std::vector<AuthoredPlanet> planets;
    std::vector<AuthoredStation> stations;
};

// One internal jump lane inside a constellation, by MEMBER index rather than
// by system index: the members do not have system indices until the group has
// been appended, and which indices they get is the generator's business.
//
// ⚑ Deliberately not `GateLink`, which is structurally identical and means
// something else. A reader who sees `GateLink` in an authored struct will read
// its fields as system indices, and they are not.
struct AuthoredConstellationLink
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

// A group of systems placed together, keeping the links their author drew
// between them (Phase 29 stage C, decisions/018).
//
// ⚑⚑⚑ A CONSTELLATION IS AN INSERTION AND ONLY AN INSERTION, AND THAT IS
// FORCED THE SAME WAY THE TWO PASSES WERE. Three of the four placement rules
// REPLACE an existing node; a group cannot replace one node as a unit, and
// making `random` mean "near a randomly chosen system" for a group while it
// means "become a randomly chosen system" for a system is two rules wearing
// one word. So the group is appended - which is also the only way its internal
// links can exist, since a replacement inherits the neighbours the generator
// already chose and declares none of its own.
//
// ⚑⚑ `AuthoredSystem::placement` IS UNREAD FOR A MEMBER. The group carries the
// placement; the def layer refuses a member that writes one, so a member never
// arrives here carrying a rule that would be silently ignored.
struct AuthoredConstellation
{
    std::string id; // the group's own id; names it in a refusal
    std::vector<AuthoredSystem> members;
    // Internal lanes, seeded into `Galaxy::links` before Prim runs. Empty is
    // not "no links": the generator chains the members in declaration order,
    // because a group with no lanes between its members is not a group. The
    // default lives there rather than in the def layer so that a hand-built
    // `GalaxyParams` means the same thing a hand-written file does.
    std::vector<AuthoredConstellationLink> links;
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
    // Places somebody put somewhere (Phase 29), in def order - which is the
    // order their placement rules resolve in. Empty is the pre-29 galaxy
    // exactly, which is what `game.unit`'s golden holds this to.
    std::vector<AuthoredSystem> authoredSystems;
    // Groups placed as a unit (Phase 29 stage C). Their members are appended
    // AFTER every `anywhere` system, contiguously and in def order, so a
    // constellation grows the galaxy exactly the way an insertion does.
    std::vector<AuthoredConstellation> constellations;

    // ---- System security baselines (Phase 30 stage A, decisions/019) -------
    // The gradient the GDD has promised since day one, finally a number. A
    // major's seat of power is its capital and a clan's is its home system, so
    // both bands are the same shape measured from two different chairs.
    //
    // What a major keeps in a system at its capital's own doorstep, by region.
    float securityByRegion[3] = {0.85f, 0.55f, 0.30f}; // core/frontier/fringe
    // What one gate hop from the capital costs, and the most the whole journey
    // can ever cost.
    //
    // ⚑⚑⚑ THE PENALTY SATURATES, AND THAT IS WHAT KEEPS THE THREE BANDS FROM
    // OVERLAPPING. Region and hop-distance are not independent - capitals are
    // drawn from the core and the fringe is by definition the outer ring - so a
    // plain `base - perJump * hops` DOUBLE-COUNTS the same distance twice and
    // flattens every fringe system onto whatever floor it is given. Capping the
    // tilt at a fraction of the gap between the region bands makes region the
    // headline and distance the detail, which is the order decisions/019 asked
    // for. With these numbers core lives in [0.73, 0.85], frontier in
    // [0.43, 0.55] and fringe in [0.18, 0.30]: DISJOINT, so a security number
    // names its region unambiguously and still says where in it you are.
    //
    // ⚑ Saturation is also why there is no separate floor parameter - the cap
    // is the floor, and one number cannot drift out of agreement with itself.
    float securityPerJump = 0.02f;
    float securityMaxJumpPenalty = 0.12f;
    // The clan band, written NEGATIVE onto the spec. Same curve, measured from
    // the clan's home system - the seat its component was seeded at, which is
    // its capital in all but name. A clan neighbourhood is a handful of systems
    // rather than a third of a galaxy, so its per-hop cost is steeper and the
    // band it spans, [-0.75, -0.30], stays clear of zero by a wide margin.
    float securityClanHome = 0.75f;
    float securityClanPerJump = 0.10f;
    float securityClanMaxJumpPenalty = 0.45f;
};

struct PlanetSpec
{
    std::string name;
    core::DVec3 position; // system barycenter frame, meters
    double radius = 0.0;  // meters
};

// A station with no composition: its rates are its archetype's, which is what
// every station was before Phase 34 stage B and what a `[[station]]` with no
// recipe still is.
inline constexpr std::uint32_t kNoComposition = 0xFFFF'FFFFu;

struct StationSpec
{
    std::string name;
    std::uint32_t archetype = 0; // index into GalaxyParams::stationRules
    // Which composed rate list this station runs on (Phase 34 stage B), or
    // `kNoComposition`.
    //
    // ⚑⚑⚑ THE SECOND INDEX, AND THE WHOLE REASON IT EXISTS: `archetype` is one
    // integer doing three jobs - it is the placement rule, it is every rate the
    // station runs at, and it is what the golden structure digest hashes - so
    // two stations of one archetype were NECESSARILY identical, being the same
    // row of the same table. A composition is a generated archetype: the
    // generator rolls a module list per station, the game reduces it to rate
    // lists, and `Economy::initialize` reads THIS index instead when it is set.
    //
    // ⚑⚑ NOTHING IN `sol::sim` WRITES IT AND NOTHING HERE KNOWS WHAT A MODULE
    // IS. `generateGalaxy` leaves it `kNoComposition`; the game composes after
    // the galaxy is built, out of its own Rng stream, so no draw in this file
    // moves and no station, planet or gate shifts by a metre.
    std::uint32_t composition = kNoComposition;
    // Who runs the black-market module on this dock (Phase 34 stage E), in the
    // faction index space `SystemSpec::factionIndex` already uses, or
    // `kNoFaction` where nobody does. Written by the same game-side pass that
    // composes, out of its own stream, and never serialized.
    //
    // ⚑⚑⚑⚑ IT STORES *WHO*, NOT *WHETHER IT IS A SHADOW PRESENCE*, AND THAT
    // DISTINCTION IS THE WHOLE STAGE. The plan's phrasing is "a module present
    // on a station whose owner is not the station's owner" - a comparison, and
    // its right-hand side MOVES: since Phase 8u who holds a system is dynamic
    // (`FactionSim::systemOwner`), and the shipped galaxy changes hands several
    // times a minute. A stored "this is shadow" bit would be a fact about the
    // FOUNDING claim, would rot inside a minute of play, and **no test could
    // see it, because at t=0 the founding claim and the current owner agree**.
    // This project has already met that trap twice - the garrison sign three
    // fields down ("the sign is not stored, and stage F is why") and the
    // legality table Phase 33 stage D had to re-point at the live owner. So the
    // operator is stored and the shadowness is DERIVED, per read, against
    // whoever holds the place now.
    //
    // ⚑⚑ THE CONSEQUENCE IS CONTENT RATHER THAN BOOKKEEPING: when the clan that
    // runs a station's fence takes the system it sits in, the fence stops being
    // a shadow presence and becomes the local boss's own shop. Nothing has to
    // notice for that to be true - it falls out of the comparison.
    //
    // ⚑ NOTHING IN `sol::sim` WRITES IT, for the reason `composition` above is
    // not written here either: this layer does not know what a module is, and
    // the Shadow family is a `[[module]]` fact.
    std::uint32_t shadowOwner = kNoFaction;
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
    // The `[[system]]` id that claimed this node, or empty for one the seed
    // produced. This is what `sol.system_by_id` will look up, and what every
    // "was this authored?" question in the generator asks.
    std::string authoredId;
    core::Vec3 mapPosition; // light-years, galaxy map space (not sim space)
    Region region = Region::Fringe;
    std::uint32_t factionIndex = kNoFaction;
    std::uint64_t seed = 0;          // per-system stream, for later instantiation needs
    double starRadius = 0.0;         // meters
    std::uint32_t primaryPlanet = 0; // index into planets; hub of the playfield
    std::vector<PlanetSpec> planets;
    std::vector<StationSpec> stations;
    std::vector<GateSpec> gates; // one per link touching this system
    // A placement flag and nothing else in this phase (Phase 29): nothing hides
    // a system from the map yet.
    bool secret = false;
    // How much force this place is worth to whoever holds it, as a STATIC
    // BASELINE and as a MAGNITUDE in [0, 1] (Phase 30 stage A; unsigned since
    // stage F). Zero means nobody holds it, which is the one reading a
    // magnitude can carry on its own.
    //
    // ⚑⚑⚑⚑ THE SIGN IS NOT STORED, AND STAGE F IS WHY. decisions/019 decision
    // 2 says the sign names WHO POLICES THIS PLACE - and since Phase 8u who
    // holds a system is dynamic, so a stored sign is a fact about whoever
    // FOUNDED it. Stages B and D both read it to say something about the
    // CURRENT owner and both were wrong the moment a system changed hands,
    // which the shipped galaxy does several times a minute: a clan that took a
    // core system garrisoned it with nothing, and the map called that clan
    // "Policed by". So the sign is a VIEW, computed where both halves are in
    // hand - `SpaceWorld::systemSecurityBaseline` - and the whole class of bug
    // is unrepresentable rather than fixed twice.
    //
    // ⚑ An author may replace it outright (Phase 30 stage E), and what they
    // write is exactly this: how hard the place is held, never by whom.
    //
    // ⚑⚑⚑ IT IS A PURE FUNCTION OF OWNER, REGION AND GATE DISTANCE, WITH NO
    // DRAW IN IT AT ALL. Every shared stream in this file is order-sensitive -
    // the name loop's comment is a monument to what one shifted draw costs - so
    // taking even one for this would reshape the galaxy at the shipped seed.
    // Consuming nothing makes `shipped_seed_galaxy_keeps_its_recorded_structure`
    // hold by construction rather than by luck.
    float security = 0.0f;
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
// An authored system whose placement rule no node satisfied (Phase 29 stage B).
//
// ⚑⚑⚑ THIS IS A RETURN VALUE RATHER THAN A LOG LINE, AND THE SEAM IS WHY.
// There is not one `SOL_LOG` in all of `engine/sim/src`, and `sol::sim` has
// never known what a file is - so it cannot write the refusal decision 3 asks
// for, which names the FILE, the id and the rule. It reports what failed and
// why; the game layer already holds `SystemDef::source` and matches on `id` to
// name the file. That keeps the refusal complete without teaching the
// generator about defs.
//
// ⚑⚑ AND SATISFIABILITY IS A PER-SEED FACT, WHICH IS WHY THERE IS NO
// LOAD-TIME CHECK THAT COULD HAVE REPLACED THIS. A `jumps_from` ring is a
// claim about a gate graph, and the gate graph is built from the seed. A ring
// that holds at the shipped seed can be empty at another one, so the check has
// to live where the graph does.
struct AuthoredPlacementFailure
{
    std::string id;     // the authored system's own id
    std::string rule;   // the placement rule it asked for, spelled as an author wrote it
    std::string reason; // why no node satisfied it
};

// `outFailures`, when given, is CLEARED and then filled with one entry per
// authored system that could not be placed. Those systems are absent from the
// returned galaxy rather than placed somewhere plausible (decision 3). Null
// keeps the pre-Phase-29-stage-B signature working for every caller that has
// no authored input to fail.
[[nodiscard]] Galaxy generateGalaxy(const GalaxyParams& params,
                                    const MiningParams* mining = nullptr,
                                    std::vector<AuthoredPlacementFailure>* outFailures = nullptr);

// Fewest-jumps route through the gate graph, inclusive of endpoints; empty
// if unreachable (cannot happen for generateGalaxy output) or on bad input.
[[nodiscard]] std::vector<std::uint32_t>
routeBetween(const Galaxy& galaxy, std::uint32_t from, std::uint32_t to);

} // namespace sol::sim
