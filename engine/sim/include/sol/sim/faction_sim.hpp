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
//
// Territory moves here too (engine plan Phase 8u). SystemSpec::factionIndex
// is the *founding claim* — part of the generated plan, regenerated from the
// seed on load, never mutated — while this sim holds who owns each system
// now. A raid feeds a per-system contest; sustained pressure flips the
// system to the attacker, and pressure nobody sustains decays until the
// contest lapses, which is the defender winning by attrition. A faction's
// home system cannot be contested, so no faction is ever erased.

#include "sol/core/random.hpp"
#include "sol/core/serialize.hpp"
#include "sol/sim/economy.hpp"
#include "sol/sim/universe.hpp"

#include <cstdint>
#include <vector>

namespace sol::sim {

struct FactionAgentParams
{
    float aggression = 0.5f;  // 0..1: appetite for raiding on a decision roll
    float forgiveness = 0.5f; // 0..1: scales relation drift back to baseline
    bool pirate = false;      // generated clan (raids on standing, not war)
    // ⚑⚑⚑⚑ DOES THIS AGENT CONTEST TERRITORY AT ALL, AND IT GATES THE DECISION
    // DRAW RATHER THAN THE DECISION (Phase 37 stage B). Every agent has taken a
    // roll every decision interval since Phase 8b, because every agent was a
    // claimant. A shadow faction holds no system, and `raidCandidates` seeds its
    // search from systems the faction HOLDS - so its frontier is empty, its
    // candidate list is empty, and its roll has exactly one possible outcome
    // however it lands.
    //
    // ⚑⚑⚑ THE POINT IS NOT THE WASTED ROLL, IT IS WHOSE ROLL IT DISPLACES. This
    // sim draws decision rolls and TRADER-LOSS rolls from one stream, so an
    // agent that can never act still moves every loss after it, forever - and
    // adding one to a shipped galaxy would silently re-run four phases of
    // economy measurements. Skipping the draw is what makes "a faction that
    // claims nothing changes nothing" true of the live simulation and not only
    // of the generated galaxy. ⚑ It is a STATIC property of the def, never a
    // live territory check: gating on "holds no systems right now" would mean a
    // major losing its last system re-seeded everybody else's fate.
    bool territorial = true;
};

struct FactionSimParams
{
    std::vector<FactionAgentParams> agents;
    // agents.size()^2, symmetric, zero diagonal: the authored starting
    // relations, and the values drift pulls back toward.
    std::vector<float> baselineRelations;
    std::vector<float> initialStandings;  // player standing per agent
    double decisionInterval = 60.0;       // seconds between decision rolls
    double stepInterval = 1.0;            // drift/decay integration step
    float warThreshold = -50.0f;          // at war when a relation falls to here...
    float peaceThreshold = -25.0f;        // ...until it recovers past here
    float hostileThreshold = -30.0f;      // relations/standings below: hostile
    float friendlyThreshold = 30.0f;      // player standing above: friendly
    std::uint32_t raidReach = 2;          // jumps from own territory
    float raidRelationHit = 4.0f;         // relation loss per raid, both ways
    float raidStockFraction = 0.25f;      // of victim-system station stock
    double raidIntensityHalfLife = 600.0; // seconds
    float driftPerSecond = 0.02f;         // points/s toward baseline, x pair mean forgiveness
    float killPenalty = 10.0f;            // standing lost with a victim's owner
    float killWebScale = 0.5f;            // x relation depth: enemies' gratitude
    float commerceRate = 0.001f;          // standing per credit traded
    // --- Territory (Phase 8u) ---
    float contestPerRaid = 0.2f;    // pressure a committed raid adds
    float contestPerKill = 0.05f;   // pressure the player's kill removes
    float contestThreshold = 0.25f; // at or above: publicly contested
    float contestFloor = 0.05f;     // below: the contest has lapsed
    double contestHalfLife = 900.0; // seconds, pressure decay
    // --- Attrition (Phase 8x) ---
    // What a system's danger is made of, and both halves already existed.
    // Raid intensity is "recent raids" (+1 each, decaying over
    // raidIntensityHalfLife); contest pressure is 0..1. A system carrying one
    // fresh raid and no contest therefore reads half dangerous.
    float dangerPerRaid = 0.5f;
    float dangerPerContest = 0.5f;
    // Chance per second that a trader flying an in-system leg through a fully
    // dangerous system is lost. ⚑ This number is guarded by
    // economy_shipped_rates_hold_a_steady_state, not by taste: 8g tuned the
    // galaxy's equilibrium against a fleet that never lost a haul, so raising
    // it is re-running that tuning whether or not anyone re-runs the test.
    float traderLossPerSecond = 0.002f;
    // Pressure a lost trader adds to a contest already running there.
    float contestPerTraderLoss = 0.02f;
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

// A live claim on a system someone else holds (Phase 8u). One attacker at a
// time: two would need a three-way resolution rule for a case the decision
// cadence makes rare. kNoFaction attacker means the system is quiet.
struct SystemContest
{
    std::uint32_t attacker = kNoFaction;
    float pressure = 0.0f; // 0..1; 1 flips the system

    [[nodiscard]] bool live() const { return attacker != kNoFaction; }
};

// A coarse trader lost since the last drain (Phase 8x). `system` is where it
// went — kNoSystem when it was between gates, which only the dev lever can
// reach, since attrition rolls nowhere but the two in-system legs.
struct TraderLoss
{
    std::uint32_t system = kNoSystem;
    std::uint32_t trader = 0;
};

// A contest that ended since the last drain. Either the attacker took the
// system (flipped) or the pressure lapsed and the holder kept it. The game
// announces these on comms and forwards them to MissionSim.
struct ContestResolution
{
    std::uint32_t system = 0;
    std::uint32_t winner = kNoFaction; // who holds the system now
    std::uint32_t loser = kNoFaction;  // the other party to the contest
    bool flipped = false;              // ownership actually changed hands
};

class FactionSim
{
public:
    // Agents must match the galaxy's faction table (majors + clans).
    // Deterministic for (galaxy, params, seed).
    void initialize(const Galaxy& galaxy, const FactionSimParams& params, std::uint64_t seed);

    // Advances real time: relation drift, raid-intensity decay, the decision
    // cadence (due decisions queue up for takeDueDecisions), and — when an
    // economy is handed in — trader attrition (Phase 8x).
    //
    // `economy` is optional exactly as it is on commitRaid: a caller with no
    // economy gets the war without the traffic. `shelteredSystem` is where the
    // player is standing, and nothing is lost there: losses happen where the
    // player is NOT, so in their own system a hauler dies by being shot, in
    // the open, leaving a wreck.
    void tick(double dt, Economy* economy = nullptr, std::uint32_t shelteredSystem = kNoSystem);

    // Moves the decisions queued since the last call into out (append).
    void takeDueDecisions(std::vector<FactionDecision>& out);

    // Raidable systems for a faction, in system-index order: held by a
    // faction it is at war with (or, for standing < hostileThreshold pairs,
    // deeply hostile to) within raidReach jumps of its own territory.
    void raidCandidates(const Galaxy& galaxy, std::uint32_t faction, std::vector<RaidCandidate>& out) const;

    // The scriptless policy for one due decision: raid the worst-relation
    // candidate when the roll clears the faction's aggression.
    void applyDefaultDecision(const Galaxy& galaxy, Economy* economy, const FactionDecision& decision);

    // Commits a raid chosen by Lua (or the default policy). Validates the
    // target against raidCandidates; economy may be null (tests).
    bool
    commitRaid(const Galaxy& galaxy, Economy* economy, std::uint32_t faction, std::uint32_t targetSystem);

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
    // The same, one system over (Phase 39 stage D): pins how raided a system
    // has recently been, which is the larger half of what `danger` is made of.
    // ⚑ A cheat rather than a test-only entry point, on `setStanding`'s own
    // precedent - the coarse hazard a captain is exposed to is a thing a DRIVE
    // has to be able to cause too, because the alternative is flying around
    // waiting for a clan to raid the system you left somebody in.
    void setRaidIntensity(std::uint32_t system, float value);
    // Clamped relative change (mission rewards/penalties, Phase 8c).
    void addStanding(std::uint32_t faction, float delta);
    // The player destroyed one of victimFaction's ships: standing falls with
    // the owner and rises with its enemies, scaled by how deep the enmity is.
    void recordShipKill(std::uint32_t victimFaction);
    // The player traded credits' worth of goods at a faction's station.
    void recordTrade(std::uint32_t faction, double credits);

    // --- Bounties (Phase 36 stage A, decisions/017) ------------------------
    //
    // ⚑⚑⚑ PER FACTION, NEVER GLOBAL, AND `017` REJECTED THE ALTERNATIVE BY
    // NAME: "a global wanted level (one number, all factions) ... incompatible
    // with the reputation web already built: bounties are per faction because
    // standing is per faction, and one number would flatten the system's best
    // feature." So this is a vector exactly parallel to `m_standings`, written
    // and read the same way, saved beside it.
    //
    // ⚑⚑ A BOUNTY IS NOT NEGATIVE STANDING AND THE TWO MUST NOT BE FOLDED.
    // Standing is what a faction THINKS of you and it moves both ways on its
    // own; a bounty is a PRICE somebody has posted, it only goes up until it is
    // settled, and it is the thing another pilot can collect. Phase 34's
    // `BountyCandidate` is a different noun again - a warm raid paired with the
    // clan that made it, raw material for a contract - and nothing here is
    // related to it beyond the word.
    //
    // ⚑ Credits, so it can be printed, paid and compared against a fine
    // without a second unit. Zero means "not wanted", which is the state every
    // faction starts in and the only one this stage can produce: stage D is the
    // first writer that is not a dev lever.
    [[nodiscard]] float bounty(std::uint32_t faction) const;

    // True when any faction has posted a price at all - the cheap question the
    // HUD and the bar both want, without walking the vector at every call site.
    [[nodiscard]] bool wanted() const;

    // Adds to the posted price (negative settles it); clamped at zero, because
    // paying off more than you owe does not leave a faction owing you.
    void addBounty(std::uint32_t faction, float credits);

    // Dev/test override, the mirror of setStanding.
    void setBounty(std::uint32_t faction, float credits);

    [[nodiscard]] float raidIntensity(std::uint32_t system) const;
    [[nodiscard]] std::uint32_t lastRaider(std::uint32_t system) const;

    // --- Attrition (Phase 8x) ---
    // How dangerous a system is to fly a haul through, 0..1. Composed from
    // state that already existed — recent raids and a live contest's pressure
    // — so nothing here is a second model of who is fighting whom.
    [[nodiscard]] float danger(std::uint32_t system) const;
    // A coarse trader was lost. Called by attrition and by the game when a
    // trader's body is destroyed in the bubble, so both roads to the same
    // event have one consequence.
    void recordTraderLoss(std::uint32_t system, std::uint32_t trader);
    // Moves the losses queued since the last call into out (append).
    void takeTraderLosses(std::vector<TraderLoss>& out);

    // --- Territory (Phase 8u) ---
    // Who holds the system now, which is not necessarily who was generated
    // holding it. kNoFaction for a system nobody claims.
    [[nodiscard]] std::uint32_t systemOwner(std::uint32_t system) const;
    // The founding claim from the generated plan: what systemOwner started
    // as, kept so a caller can tell a moved border from an original one.
    [[nodiscard]] std::uint32_t foundingClaim(std::uint32_t system) const;
    // The one system a faction can never lose: the lowest-index system
    // holding its founding claim, the rule ClanSpec::homeSystem states. A
    // faction with no claim at all has none (kNoFaction).
    [[nodiscard]] std::uint32_t homeSystem(std::uint32_t faction) const;
    [[nodiscard]] SystemContest contestOf(std::uint32_t system) const;
    // Publicly contested: pressure at or above contestThreshold. Below it
    // the pressure is weather, and nothing in the UI or the board sees it.
    [[nodiscard]] bool contested(std::uint32_t system) const;
    // The player destroyed one of the attacker's ships in a contested
    // system: pressure falls by contestPerKill.
    //
    // ⚑ This used to say that only the player's kills may move a contest,
    // because nothing simulated a war's attrition and crediting ambient
    // dogfights would have invented state the sim did not have. Phase 8x
    // built that state: traders can now be lost, so recordTraderLoss moves
    // pressure the other way without the player firing a shot. This is still
    // the player's own half of it, and it is still the only thing that pushes
    // a claim back.
    void recordContestKill(std::uint32_t system, std::uint32_t victimFaction);
    // Dev/test lever: open, move or (with kNoFaction) clear a contest
    // outright. Resolves immediately if the value is already decisive.
    void setContest(std::uint32_t system, std::uint32_t attacker, float pressure);
    // Dev/test lever: hand a system to a faction, ending any contest.
    bool flipSystem(std::uint32_t system, std::uint32_t owner);
    // Moves the resolutions queued since the last call into out (append).
    void takeResolutions(std::vector<ContestResolution>& out);

    // Dynamic state only (layout re-derives from galaxy + params; load fails
    // on a mismatch, same rule as the economy).
    void save(core::BinaryWriter& writer) const;
    [[nodiscard]] bool load(core::BinaryReader& reader);

private:
    void step(double dt, Economy* economy, std::uint32_t shelteredSystem);
    // Rolls one step's losses over the fleet (Phase 8x §C). Rides the same
    // step accumulator as drift and decay, so the war and its cost share one
    // clock and one rng, and a save restores both tick-for-tick.
    void attrition(Economy& economy, double dt, std::uint32_t shelteredSystem);
    void refreshWar(std::uint32_t a, std::uint32_t b);
    // Opens a contest on a system or feeds the standing attacker's. A rival
    // cannot hijack a live contest; it can only claim one that has lapsed.
    void pressSystem(std::uint32_t system, std::uint32_t attacker, float amount);
    // Applies the decisive outcome for a system whose pressure has reached
    // 1 (flip) or fallen through contestFloor (lapse), and queues it.
    void resolveContest(std::uint32_t system, bool flipped);

    [[nodiscard]] std::size_t pairIndex(std::uint32_t a, std::uint32_t b) const
    {
        return static_cast<std::size_t>(a) * m_count + b;
    }

    FactionSimParams m_params;
    std::uint32_t m_count = 0;
    std::uint32_t m_systemCount = 0;
    std::vector<float> m_relations;          // count^2, kept symmetric
    std::vector<std::uint8_t> m_atWar;       // count^2, hysteresis state
    std::vector<float> m_standings;          // player, per faction
    // Phase 36 stage A: parallel to m_standings, in credits, 0 = not wanted.
    std::vector<float> m_bounty;
    std::vector<float> m_raidIntensity;      // per system, decaying
    std::vector<std::uint32_t> m_lastRaider; // per system, kNoFaction = none
    // Territory (Phase 8u). m_foundingClaim mirrors the generated plan and
    // never moves; m_systemOwner is who holds it now. m_homeSystem is
    // derived per faction in initialize and so is not serialized.
    std::vector<std::uint32_t> m_foundingClaim;
    std::vector<std::uint32_t> m_systemOwner;
    std::vector<std::uint32_t> m_contestAttacker;
    std::vector<float> m_contestPressure;
    std::vector<std::uint32_t> m_homeSystem;
    std::vector<ContestResolution> m_resolutions;
    std::vector<TraderLoss> m_traderLosses;
    std::vector<FactionDecision> m_dueDecisions;
    double m_stepAccumulator = 0.0;
    double m_decisionAccumulator = 0.0;
    core::Rng m_rng;
};

} // namespace sol::sim
