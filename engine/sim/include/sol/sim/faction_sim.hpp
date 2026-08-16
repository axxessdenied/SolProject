#pragma once

// Faction relations, raids, and player reputation on the coarse sim layer
// (engine plan Phase 8b). A sibling of Economy: one agent per faction
// (authored majors then generated pirate clans, in galaxy order), a
// symmetric relations matrix with war/peace hysteresis, and a slow decision
// cadence. Each decision tick the sim rolls once per faction in index order;
// the *choice* of action belongs to Lua's faction_think (engine plan 2.8 —
// strategy in scripts), with applyDefaultDecision as the scriptless policy.
// A committed raid drains the victim system's markets through
// Economy::raidSystem, pushes the pair's relations down, and bumps a
// per-system raid intensity that decays over minutes.
//
// Player reputation lives here too: standings move on kill and trade events
// the game reports, with kill credit propagating to the victim's enemies
// (the GDD web). Deterministic for (galaxy, params, seed, event sequence);
// save/load restores tick-for-tick like the economy.

#include "sol/sim/economy.hpp"
#include "sol/sim/universe.hpp"

#include "sol/core/random.hpp"
#include "sol/core/serialize.hpp"

#include <cstdint>
#include <vector>

namespace sol::sim {

struct FactionAgentParams
{
    float aggression = 0.5f;  // 0..1: appetite for raiding on a decision roll
    float forgiveness = 0.5f; // 0..1: scales relation drift back to baseline
    bool pirate = false;      // generated clan (raids on standing, not war)
};

struct FactionSimParams
{
    std::vector<FactionAgentParams> agents;
    // agents.size()^2, symmetric, zero diagonal: the authored starting
    // relations, and the values drift pulls back toward.
    std::vector<float> baselineRelations;
    std::vector<float> initialStandings; // player standing per agent
    double decisionInterval = 60.0;      // seconds between decision rolls
    double stepInterval = 1.0;           // drift/decay integration step
    float warThreshold = -50.0f;   // at war when a relation falls to here...
    float peaceThreshold = -25.0f; // ...until it recovers past here
    float hostileThreshold = -30.0f;  // relations/standings below: hostile
    float friendlyThreshold = 30.0f;  // player standing above: friendly
    std::uint32_t raidReach = 2;      // jumps from own territory
    float raidRelationHit = 4.0f;     // relation loss per raid, both ways
    float raidStockFraction = 0.25f;  // of victim-system station stock
    double raidIntensityHalfLife = 600.0; // seconds
    float driftPerSecond = 0.02f;     // points/s toward baseline, x pair mean forgiveness
    float killPenalty = 10.0f;        // standing lost with a victim's owner
    float killWebScale = 0.5f;        // x relation depth: enemies' gratitude
    float commerceRate = 0.001f;      // standing per credit traded
};

// A raid a faction could commit this decision: an enemy-held system in reach.
struct RaidCandidate
{
    std::uint32_t system = 0;
    std::uint32_t owner = kNoFaction;
    float relation = 0.0f; // acting faction vs owner
};

// One due decision from a decision tick: the acting faction and its roll
// (the only entropy Lua sees; strategy stays a pure function of both).
struct FactionDecision
{
    std::uint32_t faction = 0;
    float roll = 0.0f;
};

class FactionSim
{
public:
    // Agents must match the galaxy's faction table (majors + clans).
    // Deterministic for (galaxy, params, seed).
    void initialize(const Galaxy& galaxy, const FactionSimParams& params, std::uint64_t seed);

    // Advances real time: relation drift, raid-intensity decay, and the
    // decision cadence (due decisions queue up for takeDueDecisions).
    void tick(double dt);

    // Moves the decisions queued since the last call into out (append).
    void takeDueDecisions(std::vector<FactionDecision>& out);

    // Raidable systems for a faction, in system-index order: held by a
    // faction it is at war with (or, for standing < hostileThreshold pairs,
    // deeply hostile to) within raidReach jumps of its own territory.
    void raidCandidates(const Galaxy& galaxy, std::uint32_t faction,
                        std::vector<RaidCandidate>& out) const;

    // The scriptless policy for one due decision: raid the worst-relation
    // candidate when the roll clears the faction's aggression.
    void applyDefaultDecision(const Galaxy& galaxy, Economy* economy,
                              const FactionDecision& decision);

    // Commits a raid chosen by Lua (or the default policy). Validates the
    // target against raidCandidates; economy may be null (tests).
    bool commitRaid(const Galaxy& galaxy, Economy* economy, std::uint32_t faction,
                    std::uint32_t targetSystem);

    [[nodiscard]] std::uint32_t factionCount() const { return m_count; }
    [[nodiscard]] const FactionSimParams& params() const { return m_params; }

    [[nodiscard]] float relation(std::uint32_t a, std::uint32_t b) const;
    [[nodiscard]] bool atWar(std::uint32_t a, std::uint32_t b) const;
    // Dev/test override; refreshes the war flag with the same hysteresis.
    void setRelation(std::uint32_t a, std::uint32_t b, float value);

    // --- Player reputation ---
    [[nodiscard]] float standing(std::uint32_t faction) const;
    [[nodiscard]] bool playerHostile(std::uint32_t faction) const
    {
        return standing(faction) < m_params.hostileThreshold;
    }
    [[nodiscard]] bool playerFriendly(std::uint32_t faction) const
    {
        return standing(faction) > m_params.friendlyThreshold;
    }
    void setStanding(std::uint32_t faction, float value); // dev cheat
    // The player destroyed one of victimFaction's ships: standing falls with
    // the owner and rises with its enemies, scaled by how deep the enmity is.
    void recordShipKill(std::uint32_t victimFaction);
    // The player traded credits' worth of goods at a faction's station.
    void recordTrade(std::uint32_t faction, double credits);

    [[nodiscard]] float raidIntensity(std::uint32_t system) const;
    [[nodiscard]] std::uint32_t lastRaider(std::uint32_t system) const;

    // Dynamic state only (layout re-derives from galaxy + params; load fails
    // on a mismatch, same rule as the economy).
    void save(core::BinaryWriter& writer) const;
    [[nodiscard]] bool load(core::BinaryReader& reader);

private:
    void step(double dt);
    void refreshWar(std::uint32_t a, std::uint32_t b);
    [[nodiscard]] std::size_t pairIndex(std::uint32_t a, std::uint32_t b) const
    {
        return static_cast<std::size_t>(a) * m_count + b;
    }

    FactionSimParams m_params;
    std::uint32_t m_count = 0;
    std::uint32_t m_systemCount = 0;
    std::vector<float> m_relations;      // count^2, kept symmetric
    std::vector<std::uint8_t> m_atWar;   // count^2, hysteresis state
    std::vector<float> m_standings;      // player, per faction
    std::vector<float> m_raidIntensity;  // per system, decaying
    std::vector<std::uint32_t> m_lastRaider; // per system, kNoFaction = none
    std::vector<FactionDecision> m_dueDecisions;
    double m_stepAccumulator = 0.0;
    double m_decisionAccumulator = 0.0;
    core::Rng m_rng;
};

} // namespace sol::sim
