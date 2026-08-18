#pragma once

// Mission/contract machinery on the coarse sim layer (engine plan Phase 8c).
// A sibling of Economy/FactionSim: missions are typed data (an ordered list
// of objectives) advanced by events the game reports — dock, delivery, kill,
// position — with deadlines on sim time. Offer *composition* belongs to Lua's
// mission_board hook (engine plan 2.8 — strategy in scripts): C++ enumerates
// candidates from real sim state (economy shortages, faction raid intensity),
// Lua posts concrete offers through a builder API, and postOffer validates
// procedural offers against the candidates it would have enumerated (the
// faction_think pattern). Authored campaign missions (decisions/008) skip
// candidate matching, may chain any objectives, and surface per-objective
// events so scripts add flavor without per-mission C++.
//
// Deterministic for (galaxy, params, seed, event sequence); save/load
// restores offers, journal, and campaign stage exactly, like the economy.

#include "sol/sim/economy.hpp"
#include "sol/sim/faction_sim.hpp"
#include "sol/sim/universe.hpp"

#include "sol/core/math/math.hpp"
#include "sol/core/random.hpp"
#include "sol/core/serialize.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sol::sim {

inline constexpr std::uint32_t kAnySystem = 0xffff'ffffu; // Kill: anywhere

enum class ObjectiveKind : std::uint32_t
{
    Dock = 0,   // dock at (system, station)
    Deliver,    // hand in `units` of `commodity` at (system, station)
    Kill,       // destroy `count` ships of `faction` in `system` (or anywhere)
    FlyTo,      // come within `radius` of `position` in `system`
    // `faction` still holds `system` when its contest resolves (Phase 8u).
    // One member covers both directions: a defence names the owner, an
    // assault contract names the attacker.
    Hold,
};

struct MissionObjective
{
    ObjectiveKind kind = ObjectiveKind::Dock;
    std::uint32_t system = 0;      // Dock/Deliver/FlyTo; Kill may use kAnySystem
    std::uint32_t station = 0;     // Dock/Deliver
    std::uint32_t commodity = 0;   // Deliver
    float units = 0.0f;            // Deliver: remaining to hand in
    std::uint32_t faction = 0;     // Kill: victim faction
    std::uint32_t kills = 0;       // Kill: remaining
    core::DVec3 position;          // FlyTo, system barycenter frame
    double radius = 0.0;           // FlyTo, meters
    std::string text;              // journal line
};

struct Mission
{
    std::string title;
    std::string campaignId;   // authored missions; empty = procedural contract
    std::uint32_t poster = kNoFaction;
    double rewardCredits = 0.0;
    float standingReward = 0.0f;
    float standingPenalty = 0.0f; // abandon/expiry, procedural only
    double deadline = 0.0;        // seconds remaining once active; 0 = none
    float minRep = -100.0f;       // board gate: standing with poster required
    std::vector<MissionObjective> objectives;
    std::uint32_t currentObjective = 0;

    [[nodiscard]] bool campaign() const { return !campaignId.empty(); }
};

// A market in candidateReach whose stock of one commodity sits below the
// shortage fraction of capacity — the raw material for a haul contract.
struct HaulCandidate
{
    std::uint32_t system = 0;
    std::uint32_t station = 0;
    std::uint32_t commodity = 0;
    float units = 0.0f;    // gap back to the neutral (half-capacity) stock
    float severity = 0.0f; // 1 - stock/capacity
    std::uint32_t jumps = 0;
};

// A system whose raid intensity is still warm, paired with the clan that hit
// it last — the raw material for a bounty contract.
struct BountyCandidate
{
    std::uint32_t system = 0;
    std::uint32_t clan = kNoFaction;
    float intensity = 0.0f;
    std::uint32_t jumps = 0;
};

// A system currently contested within reach, paired with the two factions
// fighting over it — the raw material for a defence contract (Phase 8u).
// Enumerated only where the board's owner is a party to the contest: a
// station will not pay a pilot to help the faction taking its system.
struct ContestCandidate
{
    std::uint32_t system = 0;
    std::uint32_t owner = kNoFaction;    // who holds it now
    std::uint32_t attacker = kNoFaction; // who is pressing the claim
    float pressure = 0.0f;
    std::uint32_t jumps = 0;
};

enum class MissionEventKind : std::uint32_t
{
    Accepted = 0,      // objective 0 is now current
    ObjectiveComplete, // `objective` finished; the next one (if any) started
    Completed,         // consume rewardCredits/standingReward
    Failed,            // deadline expired; consume standingPenalty
    Abandoned,         // player walked away; consume standingPenalty
    // A Hold objective's contest resolved the wrong way (Phase 8u). A
    // separate kind precisely so it can carry no standing penalty: losing a
    // battle you flew is not the same as letting a timer run out, which is
    // the rough edge Phase 8l recorded and could not fix in its own scope.
    Lost,
};

// Consequence queue entry: the game drains these each tick, applies payouts
// and penalties, and forwards campaign ones to Lua's mission_event hook.
struct MissionEvent
{
    MissionEventKind kind = MissionEventKind::Accepted;
    Mission mission; // snapshot at event time
    std::uint32_t objective = 0;
};

struct MissionParams
{
    std::uint32_t maxActive = 4;
    std::uint32_t maxOffers = 8;
    std::uint32_t maxBountyKills = 8;
    std::uint32_t candidateReach = 3;      // jumps from the board's system
    float shortageThreshold = 0.25f;       // stock/capacity below: shortage
    float bountyIntensityThreshold = 0.4f; // raid intensity above: bounty
    double boardRefreshInterval = 180.0;   // seconds docked between refreshes
};

class MissionSim
{
public:
    // Counts pin the layout the save format validates against (economy rule).
    void initialize(const Galaxy& galaxy, const MissionParams& params,
                    std::uint32_t factionCount, std::uint32_t commodityCount,
                    std::uint64_t seed);

    // Advances active-mission deadlines; expiries queue Failed events.
    void tick(double dt);
    // Advances the board-refresh clock; call only while docked. True when a
    // refresh is due (the caller re-opens the board and re-runs the hook).
    [[nodiscard]] bool tickBoard(double dt);

    // --- Candidates (enumerated in index order; deterministic) ---
    void haulCandidates(const Galaxy& galaxy, const Economy& economy,
                        std::uint32_t fromSystem, std::uint32_t fromStation,
                        std::vector<HaulCandidate>& out) const;
    void bountyCandidates(const Galaxy& galaxy, const FactionSim& factions,
                          std::uint32_t fromSystem,
                          std::vector<BountyCandidate>& out) const;
    // Contests in reach that `boardOwner` is a party to (Phase 8u). Pass
    // kNoFaction for an ownerless board, which enumerates nothing.
    void contestCandidates(const Galaxy& galaxy, const FactionSim& factions,
                           std::uint32_t fromSystem, std::uint32_t boardOwner,
                           std::vector<ContestCandidate>& out) const;

    // --- The board (offers at the docked station) ---
    // Clears the offers and rebinds the board to a station; the hook then
    // posts fresh offers. Campaign offers must be re-posted too.
    void openBoard(std::uint32_t system, std::uint32_t station);
    [[nodiscard]] std::uint32_t boardSystem() const { return m_boardSystem; }
    [[nodiscard]] std::uint32_t boardStation() const { return m_boardStation; }
    // One seeded roll per board composition — the only entropy the hook sees.
    [[nodiscard]] float boardRoll();
    // Validates and stores an offer. Procedural (non-campaign) offers must be
    // a single Deliver or Kill objective matching a current candidate;
    // campaign offers may chain anything in range. Duplicate campaignIds
    // (offered or active) and full boards are refused.
    bool postOffer(const Galaxy& galaxy, const Economy& economy,
                   const FactionSim& factions, Mission mission,
                   std::string* outError = nullptr);
    [[nodiscard]] const std::vector<Mission>& offers() const { return m_offers; }

    // --- The journal (active missions) ---
    // standingWithPoster enforces the offer's minRep gate.
    bool accept(std::uint32_t offerIndex, float standingWithPoster,
                std::string* outError = nullptr);
    bool abandon(std::uint32_t activeIndex);
    [[nodiscard]] const std::vector<Mission>& active() const { return m_active; }
    [[nodiscard]] std::uint32_t tracked() const { return m_tracked; }
    void setTracked(std::uint32_t activeIndex);

    // --- Events the game reports ---
    void notifyDock(std::uint32_t system, std::uint32_t station);
    void notifyKill(std::uint32_t victimFaction, std::uint32_t system);
    void notifyPosition(std::uint32_t system, const core::DVec3& position);
    // A contest ended (Phase 8u): a Hold objective on that system completes
    // when `winner` is the faction it named, and the mission is Lost when
    // it is not. Checked here rather than polled, so it costs nothing until
    // a border actually moves.
    void notifyContestResolved(std::uint32_t system, std::uint32_t winner);
    // Consumes up to `available` units toward the mission's current Deliver
    // objective at (system, station); returns what was consumed (the caller
    // removes it from cargo and feeds the market). Advances on completion.
    float recordDelivery(std::uint32_t activeIndex, std::uint32_t system,
                         std::uint32_t station, float available);

    // Moves the events queued since the last call into out (append).
    void takeEvents(std::vector<MissionEvent>& out);

    // --- Campaign spine (decisions/008): stage index scripts chain on ---
    [[nodiscard]] std::uint32_t campaignStage() const { return m_campaignStage; }
    void setCampaignStage(std::uint32_t stage) { m_campaignStage = stage; }

    [[nodiscard]] const MissionParams& params() const { return m_params; }

    // Dynamic state only (layout re-derives from galaxy + defs; load fails on
    // a count mismatch, same rule as the economy).
    void save(core::BinaryWriter& writer) const;
    [[nodiscard]] bool load(core::BinaryReader& reader);

private:
    [[nodiscard]] bool objectiveInRange(const Galaxy& galaxy,
                                        const MissionObjective& objective) const;
    // Completes the mission's current objective: queues the event, advances,
    // and returns true when the whole mission just completed (the caller
    // removes m_active[activeIndex]).
    [[nodiscard]] bool advanceObjective(std::uint32_t activeIndex);
    void removeActive(std::uint32_t activeIndex);
    // Jump distances from a source system, capped at candidateReach (0xff =
    // out of reach).
    void jumpDepths(const Galaxy& galaxy, std::uint32_t fromSystem,
                    std::vector<std::uint8_t>& out) const;

    MissionParams m_params;
    std::uint32_t m_systemCount = 0;
    std::uint32_t m_factionCount = 0;
    std::uint32_t m_commodityCount = 0;
    std::vector<Mission> m_offers;
    std::vector<Mission> m_active;
    std::vector<MissionEvent> m_events;
    std::uint32_t m_boardSystem = kNoFaction;
    std::uint32_t m_boardStation = kNoFaction;
    double m_boardAccumulator = 0.0;
    std::uint32_t m_tracked = 0;
    std::uint32_t m_campaignStage = 0;
    core::Rng m_rng;
};

} // namespace sol::sim
