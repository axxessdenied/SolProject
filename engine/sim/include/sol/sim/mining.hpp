#pragma once

// Asteroid fields, wrecks, and the refinery queue on the coarse sim layer
// (engine plan Phase 8f). A fifth sibling of Economy/FactionSim/MissionSim/
// SurveySim, here for the same reasons: this is the part that must be
// deterministic, unit-testable, and saved.
//
// The three things it holds are deliberately three different *kinds* of
// state:
//   - Asteroid fields are content, a pure function of the system seed
//     (fieldsFor/rocksFor), so nothing about a rock's position or ore is
//     ever written to disk — the same trick signals and the galaxy use.
//   - Depletion is the state that is: a sparse record per rock the player
//     has actually cut, so an untouched galaxy costs zero bytes and a
//     mined-out field survives a jump and a reload.
//   - Wrecks are the opposite of content: they exist *because* something
//     happened, so they are stored outright. Their contents are a
//     SignalLoot — the struct Phase 8e already validates, saves, and fills
//     from Lua — so cutting a wreck and emptying a derelict pay out through
//     one path.
//
// Refine jobs live here rather than in Economy because they are the player's
// state, not the market's: the ore has left the hold, the metal is not in
// stock anywhere, and the job ticks on the same coarse clock whether the
// player is docked, flying, or three systems away (decisions/005).

#include "sol/core/math/math.hpp"
#include "sol/core/serialize.hpp"
#include "sol/sim/survey.hpp"
#include "sol/sim/universe.hpp"
#include "sol/sim/weapons.hpp" // segmentHitsSphere: a miner's path through a field

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sol::sim {

// One mineable rock. Content: generated on demand, never saved.
struct RockSpec
{
    core::DVec3 position; // system barycenter frame, meters
    double radius = 0.0;  // meters
    core::Vec3 tumbleAxis{0.0f, 1.0f, 0.0f};
    float tumbleRate = 0.0f;     // rad/s
    std::uint32_t commodity = 0; // economy commodity index
    float yieldUnits = 0.0f;     // total units in an untouched rock
    std::uint64_t seed = 0;
};

// A cluster of rocks in the playfield. Content: generated on demand.
struct AsteroidFieldSpec
{
    core::DVec3 center; // system barycenter frame, meters
    double radius = 0.0;
    std::uint32_t rockCount = 0;
    std::uint64_t seed = 0;
};

// What a rock can be made of, and how likely that is per region tier. The
// game supplies these (it owns the commodity table), so a rare-mineral tier
// later is a data change rather than a sim change.
struct OreEntry
{
    std::uint32_t commodity = 0;
    float weight[3] = {1.0f, 1.0f, 1.0f}; // Core, Frontier, Fringe
};

// A ship that died where the player could see it. Unlike a signal, a wreck
// is not derivable from any seed — it is the record of an event.
struct WreckRecord
{
    std::uint32_t id = 0;
    std::uint32_t system = 0;
    core::DVec3 position;   // where it fell
    std::string defId;      // the victim's ship def (name and hull size)
    std::string name;       // display name at the time of death
    std::uint64_t seed = 0; // the loot roll handed to the Lua hook
    SignalLoot contents;
    double decayRemaining = 0.0;
    bool contentsSet = false; // false until the loot hook (or default) answers
    bool opened = false;      // the beam has been into it; contents are frozen
};

// One outstanding refinery order. Output is fixed when the job starts, so a
// params edit cannot retroactively change what the player is owed — the same
// rule SurveyEntry::value follows.
struct RefineJob
{
    std::uint32_t market = 0; // economy market index; collect at that station
    std::uint32_t inputCommodity = 0;
    std::uint32_t outputCommodity = 0;
    float inputUnits = 0.0f;
    float outputUnits = 0.0f;
    double secondsRemaining = 0.0; // <= 0: waiting to be collected
};

struct MiningParams
{
    // Fields per system, [min, max] by region tier (Core, Frontier, Fringe).
    // The core is picked over; the fringe is where the rock is.
    std::uint32_t fieldCount[3][2] = {{0, 1}, {1, 2}, {1, 3}};
    std::uint32_t rockCount[3][2] = {{18, 28}, {24, 38}, {30, 48}};
    // Fields sit in the same playfield band as stations, gates, and signals,
    // so reaching one stays inside the decisions/005 cruise budget.
    double fieldMinDistance = 8.0e7;
    double fieldMaxDistance = 4.0e8;
    // A field is small enough to fly across in well under a minute at normal
    // thrust: it is a place you are *inside*, not another cruise leg. At this
    // radius a few dozen rocks sit roughly a kilometre apart, so arriving at
    // the field puts several of them in front of you.
    double fieldRadiusMin = 2.0e3;
    double fieldRadiusMax = 6.0e3;
    double rockRadiusMin = 45.0;
    double rockRadiusMax = 240.0;
    float yieldMin = 25.0f;
    float yieldMax = 140.0f;
    float regionYieldMultiplier[3] = {1.0f, 1.35f, 1.8f};
    std::vector<OreEntry> ores; // empty => nothing is mineable

    // Rocks regrow (Phase 8g), which 8f recorded as a known gap and named
    // this as the fix for. It is not optional once mining outposts eat real
    // rock: a field measured live holds ~2,200 units, so without regrowth
    // NPC mining is just a countdown to a dead galaxy.
    // How fast an outpost may draw is not a knob here: it is the archetype's
    // own production rate in stations.toml, capped by what the rock can
    // sustain, which is regen x the number of rocks being worked.
    float rockRegenPerSecond = 0.02f; // units/s returned to a cut rock

    // Wrecks: they age out on the coarse clock so a long save is not a
    // junkyard, and the store is capped (oldest evicted first).
    double wreckDecaySeconds = 3'600.0;
    std::uint32_t maxWrecks = 64;

    // Refining: a fee per unit and a wait that scales with the order, so a
    // big job is a real round trip rather than a pause at the counter.
    double refineSecondsBase = 60.0;
    double refineSecondsPerUnit = 2.0;
    float refineRatio = 0.55f;     // output units per input unit
    float refineFeePerUnit = 2.0f; // credits
    std::uint32_t maxRefineJobs = 16;

    std::uint32_t maxCargoStacks = 3; // wreck-loot validation (as SurveyParams)
};

// How many asteroid fields a system has, without building any of them
// (Phase 13). Free rather than a member because the galaxy GENERATOR has to
// ask it — a Mining Outpost in a system with no rock produces nothing forever
// (stations.toml's produces_from = "field"), so placement has to know — and
// generation runs long before any MiningSim exists.
//
// ⚑ This is the ONE definition of the rule: MiningSim::fieldsFor calls it for
// its own count rather than repeating the draw. Copying MiningParams::
// fieldCount into GalaxyParams was the obvious alternative and is exactly how
// two answers to one question start. A field depends only on the system's seed
// and region — never on what was built there — which is what makes asking this
// during generation acyclic.
[[nodiscard]] std::uint32_t fieldCountFor(const SystemSpec& spec, const MiningParams& params);

// One rock as a miner sees it: somewhere to be, and something to fly into.
struct MiningRock
{
    core::DVec3 position;
    double radius = 0.0;
};

inline constexpr std::uint32_t kNoRock = 0xffff'ffffu;

// The rock a miner moves to next: the nearest one, other than the one it is
// leaving, that it can reach in a STRAIGHT LINE without flying into another.
// kNoRock when there is no such rock, which means it stays where it is.
//
// ⚑ The straight line is the whole rule, and it was written after watching a
// miner die. Rocks are solid statics and nothing in the game steers around
// them: `m_obstacles` holds stations and planets only, so a ship crossing a
// field has no avoidance at all. The first version picked the next rock by
// generation order, which sent a freighter 5,377 m across a field at 207 m/s
// — measured, in that order, one sample before `'sol.freighter' destroyed` —
// and the outpost lost its miner every sixty seconds forever. Nearest keeps
// the hop short; the clearance test is what makes even a short one safe.
//
// The test is conservative on both ends: it aims at the rock's centre, while
// the ship actually stops a hold point short of it, and it counts the rock
// being LEFT as an obstacle, because a new rock on the far side of the old
// one is exactly the hop that would clip it.
[[nodiscard]] inline std::uint32_t chooseWorkRock(const core::DVec3& from,
                                                  std::uint32_t avoid,
                                                  std::span<const MiningRock> rocks,
                                                  double clearance)
{
    std::uint32_t best = kNoRock;
    double bestDistance = 0.0;
    for (std::uint32_t i = 0; i < rocks.size(); ++i) {
        if (i == avoid) {
            continue;
        }
        const double distance = length(rocks[i].position - from);
        if (best != kNoRock && distance >= bestDistance) {
            continue;
        }
        bool blocked = false;
        for (std::uint32_t j = 0; j < rocks.size() && !blocked; ++j) {
            double t = 0.0;
            blocked =
                j != i &&
                segmentHitsSphere(from, rocks[i].position, rocks[j].position, rocks[j].radius + clearance, t);
        }
        if (!blocked) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

// Where a mining ship holds while it works a rock (engine plan Phase 8x,
// stage 6).
//
// An extractor station's ore comes out of the ground through drawFromSystem
// whether or not anyone is watching; a miner is that draw given a body, so it
// parks off a rock rather than flying a lane. The clearance is the whole rule
// and it is easy to get wrong in a way a screenshot will not show: rocks run
// from 45 m to 240 m of radius, so a fixed hold distance that looks generous
// at the small end puts the ship inside the big one — and a rock is solid.
// Measured from the rock's own surface, never from its centre.
//
// `towards` is the direction the ship should sit on (the game passes the
// station's side, so a miner is between the rock and the dock it feeds, which
// is where anyone flying in from the station will see it). A degenerate
// direction still has to answer with a point outside the rock.
[[nodiscard]] inline core::DVec3
minerHoldPoint(const core::DVec3& rock, double rockRadius, const core::DVec3& towards, double clearance)
{
    const double distance = length(towards);
    const core::DVec3 direction = distance > 1.0e-6 ? towards * (1.0 / distance) : core::DVec3{0.0, 0.0, 1.0};
    const double radius = rockRadius > 0.0 ? rockRadius : 0.0;
    return rock + direction * (radius + (clearance > 0.0 ? clearance : 0.0));
}

class MiningSim
{
public:
    // Sizes the per-system tables and precomputes field counts.
    // Deterministic for (galaxy, params, seed).
    void initialize(const Galaxy& galaxy,
                    const MiningParams& params,
                    std::uint32_t commodityCount,
                    std::uint64_t seed);

    // --- Content (pure functions of the system seed) ---
    void fieldsFor(const Galaxy& galaxy, std::uint32_t system, std::vector<AsteroidFieldSpec>& out) const;
    // Expands one field into its rocks. Out is cleared even on bad input.
    void rocksFor(const Galaxy& galaxy,
                  std::uint32_t system,
                  std::uint32_t field,
                  std::vector<RockSpec>& out) const;
    [[nodiscard]] std::uint32_t fieldCount(std::uint32_t system) const;

    // --- Depletion (sparse: only rocks that have been cut) ---
    [[nodiscard]] float unitsTaken(std::uint32_t system, std::uint32_t field, std::uint32_t rock) const;
    [[nodiscard]] float
    unitsLeft(std::uint32_t system, std::uint32_t field, std::uint32_t rock, float totalUnits) const;
    // Cuts up to `units` out of a rock holding `totalUnits` when untouched;
    // returns what actually came out (0 once the rock is empty).
    float
    mineRock(std::uint32_t system, std::uint32_t field, std::uint32_t rock, float totalUnits, float units);

    [[nodiscard]] std::size_t depletionRecordCount() const { return m_depletion.size(); }

    // Takes up to `units` of one commodity out of a system's fields, working
    // rock by rock, and reports what was actually there. This is the path a
    // mining outpost's production runs through, so an NPC and the player's
    // beam deplete the same rock through the same accounting — the first
    // time the sandbox and the economy have touched one finite resource.
    float drawFromSystem(const Galaxy& galaxy, std::uint32_t system, std::uint32_t commodity, float units);
    // Units of a commodity still in the ground across a whole system.
    [[nodiscard]] float
    systemStock(const Galaxy& galaxy, std::uint32_t system, std::uint32_t commodity) const;

    // --- Wrecks ---
    // Records a kill; returns the new wreck id, or 0 on bad input (ids start
    // at 1, so 0 is always "no wreck").
    std::uint32_t addWreck(std::uint32_t system,
                           const core::DVec3& position,
                           std::string defId,
                           std::string name,
                           std::uint64_t seed);

    [[nodiscard]] const std::vector<WreckRecord>& wrecks() const { return m_wrecks; }

    [[nodiscard]] const WreckRecord* wreck(std::uint32_t id) const;
    // Contents composed game-side (Lua or the built-in default), validated
    // exactly like signal loot. The default is written at death and the Lua
    // hook may replace it, but once the beam has been into the hull nothing
    // can rewrite what is inside it.
    bool setWreckContents(std::uint32_t id, SignalLoot contents);
    // Cuts up to `units` of cargo out of a wreck, drawing from one stack at a
    // time; reports what came out and which commodity it was. 0 means the
    // hull has no loose cargo left (the caller then takes the valuables and
    // removes it). Opening a wreck freezes its contents.
    float cutWreckCargo(std::uint32_t id, float units, std::uint32_t* outCommodity);
    // The player took it: the wreck leaves the store (a cut-open hull has
    // nothing left worth remembering).
    bool removeWreck(std::uint32_t id);
    void wrecksIn(std::uint32_t system, std::vector<std::uint32_t>& out) const;

    // --- Refining ---
    [[nodiscard]] double refineFee(float units) const;
    [[nodiscard]] double refineDuration(float units) const;
    [[nodiscard]] float refineOutput(float units) const;
    // Queues an order. False when the queue is full or the input is invalid;
    // the caller has already taken the ore and the fee.
    bool startRefineJob(std::uint32_t market,
                        std::uint32_t inputCommodity,
                        float units,
                        std::uint32_t outputCommodity);

    [[nodiscard]] const std::vector<RefineJob>& refineJobs() const { return m_refineJobs; }

    // Finished output waiting at a market (0 while jobs are still running).
    [[nodiscard]] float readyAt(std::uint32_t market, std::uint32_t commodity) const;
    // Time until the earliest unfinished job at a market completes, or a
    // negative value when nothing is pending there.
    [[nodiscard]] double soonestAt(std::uint32_t market) const;
    // Hands over up to maxUnits of finished output; emptied jobs are dropped.
    float collectAt(std::uint32_t market, std::uint32_t commodity, float maxUnits);

    // Ages wrecks and advances refine jobs on the coarse clock.
    void tick(double dt);

    [[nodiscard]] const MiningParams& params() const { return m_params; }

    // Dynamic state only; the layout re-derives from galaxy + defs and load
    // fails on a count mismatch (the rule every sibling sim follows).
    void save(core::BinaryWriter& writer) const;
    [[nodiscard]] bool load(core::BinaryReader& reader);

private:
    // Sparse depletion, sorted by a packed (system, field, rock) key so a
    // lookup is a binary search and the save order is stable.
    struct RockDepletion
    {
        std::uint64_t key = 0;
        float unitsTaken = 0.0f;
    };

    [[nodiscard]] static std::uint64_t rockKey(std::uint32_t system, std::uint32_t field, std::uint32_t rock);
    [[nodiscard]] std::size_t findDepletion(std::uint64_t key) const;

    // Adds `takes` (ascending key order, unique) in one linear merge. An
    // outpost's draw is spread across every rock in its system holding the
    // commodity, so the one-sorted-insert-per-rock path mineRock uses costs
    // O(rocks * records) of memmove per economy step and gets worse as the
    // galaxy is worked — the whole of the 3.4 ms spike Phase 8n measured.
    // Same resulting table, same key order, so the save is unaffected.
    void mergeDepletion(const std::vector<RockDepletion>& takes);

    // rocksFor is a pure function of the system seed, and the coarse economy
    // asks for the same handful of extractor systems once a second forever —
    // so regenerating them costs an RNG walk per rock for an answer that
    // cannot have changed. Memoized by (system, field); derived state, never
    // saved, dropped whenever the galaxy it came from is replaced.
    [[nodiscard]] const std::vector<RockSpec>&
    cachedRocks(const Galaxy& galaxy, std::uint32_t system, std::uint32_t field) const;
    // The generator behind the memo. Every caller goes through cachedRocks.
    void generateRocks(const Galaxy& galaxy,
                       std::uint32_t system,
                       std::uint32_t field,
                       std::vector<RockSpec>& out) const;

    MiningParams m_params;
    std::uint32_t m_systemCount = 0;
    std::uint32_t m_commodityCount = 0;
    std::vector<std::uint8_t> m_fieldCounts; // per system, from the seed
    std::vector<RockDepletion> m_depletion;
    std::vector<WreckRecord> m_wrecks;
    std::vector<RefineJob> m_refineJobs;
    std::uint32_t m_nextWreckId = 1;
    std::uint64_t m_seed = 0;

    // Scratch held so the once-a-second galaxy step does not allocate, and
    // the rock memo. All derived state: never saved.
    std::vector<RockDepletion> m_drawTakes;
    std::vector<RockDepletion> m_depletionScratch;
    mutable std::unordered_map<std::uint64_t, std::vector<RockSpec>> m_rockCache;
};

} // namespace sol::sim
