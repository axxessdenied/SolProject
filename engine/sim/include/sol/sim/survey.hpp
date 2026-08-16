#pragma once

// Exploration knowledge, scannable signals, and the survey ledger on the
// coarse sim layer (engine plan Phase 8e). A fourth sibling of
// Economy/FactionSim/MissionSim, and it sits here for the same reasons they
// do: this is the part that must be deterministic, unit-testable, and saved.
//
// What a system's signals *are* is a pure function of its seed
// (signalsFor()), exactly like the galaxy generator's content pass, so the
// save carries per-signal state bits and never the content itself. What the
// player *knows* is a one-way ladder per system — Unknown -> Charted ->
// Visited -> Surveyed — advanced only by events the game reports.
//
// Loot is the one exception to "content regenerates": it is composed once at
// resolution (game-side, through Lua) and held here until the site is
// emptied, so a script edit between sessions cannot rewrite a wreck the
// player has already scanned.

#include "sol/sim/universe.hpp"

#include "sol/core/math/math.hpp"
#include "sol/core/random.hpp"
#include "sol/core/serialize.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sol::sim {

inline constexpr std::uint32_t kNoSystem = 0xffff'ffffu;

// Per-system knowledge ladder. Charted is what a gate tells you for free:
// the destination has a name and a place on the map, and nothing else.
enum class KnowledgeState : std::uint8_t
{
    Unknown = 0,
    Charted,  // named by a gate you have stood at; contents blank
    Visited,  // you have been here: owner, stations, and bodies are known
    Surveyed, // every body scanned and every signal resolved
};

enum class SignalKind : std::uint32_t
{
    Derelict = 0, // a dead hull: cargo, sometimes a module
    Cache,        // a stash: cargo plus credits
    Count,
};

[[nodiscard]] const char* signalKindName(SignalKind kind);

// A scannable site. Generated on demand from the system seed; `seed` is the
// per-signal roll the loot hook consumes, so loot is fixed by the world seed
// rather than by when the player happened to scan it.
struct SignalSpec
{
    SignalKind kind = SignalKind::Derelict;
    core::DVec3 position; // system barycenter frame, meters
    std::uint64_t seed = 0;
};

// What a scan produced, for the ledger.
enum class SurveyKind : std::uint32_t
{
    System = 0, // first arrival in a system
    Body,       // a star or planet resolved by a target scan
    Site,       // a signal resolved by a target scan
    Completion, // a system reaching Surveyed, paid once
    Count,
};

// One unsold line of survey data. Value is fixed when the entry is written,
// so a params edit cannot retroactively repay what is already in the hold.
struct SurveyEntry
{
    std::uint32_t system = 0;
    SurveyKind kind = SurveyKind::System;
    Region region = Region::Core;
    bool firstDiscovery = false; // nobody lives here: no station in the system
    double value = 0.0;          // credits
};

// One cargo stack inside a site.
struct SignalCargo
{
    std::uint32_t commodity = 0;
    float units = 0.0f;
};

// A resolved site's contents, held until the site is emptied.
struct SignalLoot
{
    std::vector<SignalCargo> cargo;
    double credits = 0.0;
    std::string moduleId; // empty = none
};

// Shared loot validation: known commodities, positive unit amounts,
// non-negative credits, and a stack ceiling. Phase 8f wrecks are composed
// game-side exactly as signal loot is, so they answer to the same rule.
[[nodiscard]] bool validSignalLoot(const SignalLoot& loot, std::uint32_t commodityCount,
                                   std::uint32_t maxCargoStacks);

// What the player remembers about one market's prices, and when they looked
// (Phase 8g). This lives here rather than in Economy because Economy holds
// no player state at all — buy()/sell() are pure market operations — and
// that property is worth keeping; SurveySim is already the home of "what the
// player knows about the galaxy".
struct MarketMemory
{
    std::uint32_t market = 0;
    std::vector<float> prices; // mid price per commodity, as seen
    double takenAt = 0.0;      // sim seconds when the snapshot was taken
};

struct SurveyParams
{
    // Signals per system, [min, max] by region tier (Core, Frontier, Fringe):
    // the GDD's difficulty-and-opportunity gradient, in content terms.
    std::uint32_t signalCount[3][2] = {{0, 2}, {2, 4}, {3, 6}};
    // Sites sit in the playfield around the primary planet, like stations and
    // gates, so a sweep stays inside the decisions/005 cruise budget. The
    // band is deliberately the station band: a pulse fired from the inhabited
    // part of a system has to be able to reach them, or exploration is a
    // needle in a volume no player will ever sweep.
    double signalMinDistance = 8.0e7;
    double signalMaxDistance = 4.0e8;
    // Data values, before the region multiplier.
    double valueSystem = 150.0;
    double valueBody = 80.0;
    double valueSite = 260.0;
    double valueCompletion = 600.0;
    float regionMultiplier[3] = {1.0f, 2.0f, 3.5f};
    float firstDiscoveryBonus = 1.6f; // systems with no station at all
    std::uint32_t maxCargoStacks = 3; // loot validation cap
    // Market intel (Phase 8g). Past this age a remembered price is shown
    // greyed: it is still the best guess anyone has, but the market has moved
    // since and saying so is what keeps bought intel worth re-buying instead
    // of being a one-time unlock.
    double intelStaleSeconds = 900.0;
};

class SurveySim
{
public:
    // Sizes the knowledge table to the galaxy and precomputes signal counts.
    // Deterministic for (galaxy, params, seed).
    void initialize(const Galaxy& galaxy, const SurveyParams& params, std::uint32_t commodityCount,
                    std::uint64_t seed);

    // --- Content (pure functions of the system seed) ---
    void signalsFor(const Galaxy& galaxy, std::uint32_t system,
                    std::vector<SignalSpec>& out) const;
    [[nodiscard]] std::uint32_t signalCount(std::uint32_t system) const;
    // 1 star + the system's planets; what "every body scanned" counts.
    [[nodiscard]] std::uint32_t bodyCount(const Galaxy& galaxy, std::uint32_t system) const;

    // --- Knowledge ---
    [[nodiscard]] KnowledgeState knowledge(std::uint32_t system) const;
    [[nodiscard]] std::uint32_t knownSystemCount() const;
    // Arrival: the system becomes Visited (at least) and every system its
    // gates reach becomes Charted. Writes a System entry the first time.
    void notifyArrival(const Galaxy& galaxy, std::uint32_t system);
    // Dev/console path: chart or visit without flying there.
    void setKnowledge(const Galaxy& galaxy, std::uint32_t system, KnowledgeState state);

    // --- Bodies (index 0 is the star, 1.. are planets in spec order) ---
    [[nodiscard]] bool bodyScanned(std::uint32_t system, std::uint32_t body) const;
    // False when out of range or already scanned; otherwise writes a Body
    // entry and may complete the system.
    bool notifyBodyScanned(const Galaxy& galaxy, std::uint32_t system, std::uint32_t body);

    // --- Signals ---
    [[nodiscard]] bool signalDiscovered(std::uint32_t system, std::uint32_t signal) const;
    [[nodiscard]] bool signalResolved(std::uint32_t system, std::uint32_t signal) const;
    [[nodiscard]] bool signalEmptied(std::uint32_t system, std::uint32_t signal) const;
    // A pulse found it: it becomes a contact on the HUD, still unidentified.
    bool notifySignalDiscovered(std::uint32_t system, std::uint32_t signal);
    // A target scan identified it: writes a Site entry, may complete the
    // system. Discovers it first if a scan somehow beat the pulse to it.
    bool notifySignalResolved(const Galaxy& galaxy, std::uint32_t system, std::uint32_t signal);
    // The player took what was inside; drops the stored loot.
    bool notifySignalEmptied(std::uint32_t system, std::uint32_t signal);

    // --- Loot (composed game-side at resolution, held until emptied) ---
    // Rejects unknown commodities, negative amounts, too many stacks, and
    // signals that are not resolved-and-still-full.
    bool setLoot(std::uint32_t system, std::uint32_t signal, SignalLoot loot);
    [[nodiscard]] const SignalLoot* loot(std::uint32_t system, std::uint32_t signal) const;

    // --- Survey ledger ---
    [[nodiscard]] const std::vector<SurveyEntry>& ledger() const { return m_ledger; }
    [[nodiscard]] double ledgerValue() const;
    // Sells everything and returns the credits (the caller pays the player).
    double sellLedger();

    // --- Market intel (Phase 8g) ---
    // Writes (or refreshes) what the player knows about a market's prices.
    void recordMarket(std::uint32_t market, const std::vector<float>& prices, double now);
    [[nodiscard]] const std::vector<MarketMemory>& marketMemory() const { return m_marketMemory; }
    [[nodiscard]] const MarketMemory* remembered(std::uint32_t market) const;
    [[nodiscard]] bool isStale(const MarketMemory& memory, double now) const;
    // The best remembered price for a commodity anywhere but `excludeMarket`,
    // with the market it was seen at and how old that reading is. Returns
    // false when nothing is remembered. This is what the station Trade tab's
    // "elsewhere" column and the galaxy map's trade overlay both read.
    bool bestRemembered(std::uint32_t commodity, std::uint32_t excludeMarket, double now,
                        std::uint32_t* outMarket, float* outPrice, double* outAge) const;

    // --- Plotted route (galaxy map; no auto-jump, decisions/005) ---
    // Stored as the remaining systems including the current one at index 0.
    void setRoute(std::vector<std::uint32_t> route);
    void clearRoute() { m_route.clear(); }
    [[nodiscard]] const std::vector<std::uint32_t>& route() const { return m_route; }
    // The system the player should jump to next, or kNoSystem.
    [[nodiscard]] std::uint32_t nextHop() const;

    [[nodiscard]] const SurveyParams& params() const { return m_params; }

    // Dynamic state only; the layout re-derives from the galaxy and load
    // fails on a count mismatch (the rule every sibling sim follows).
    void save(core::BinaryWriter& writer) const;
    [[nodiscard]] bool load(core::BinaryReader& reader);

private:
    // Per-system state. Bodies and signals are bitmasks: a system holds at
    // most 5 bodies and 6 signals, and initialize() asserts the ceiling.
    struct SystemSurvey
    {
        KnowledgeState state = KnowledgeState::Unknown;
        std::uint32_t bodiesScanned = 0;
        std::uint32_t signalsDiscovered = 0;
        std::uint32_t signalsResolved = 0;
        std::uint32_t signalsEmptied = 0;
    };

    struct LootRecord
    {
        std::uint32_t system = 0;
        std::uint32_t signal = 0;
        SignalLoot loot;
    };

    void addEntry(const Galaxy& galaxy, std::uint32_t system, SurveyKind kind);
    // Promotes to Surveyed (and pays the completion entry) once every body is
    // scanned and every signal resolved.
    void checkSurveyed(const Galaxy& galaxy, std::uint32_t system);
    [[nodiscard]] std::size_t findLoot(std::uint32_t system, std::uint32_t signal) const;

    SurveyParams m_params;
    std::uint32_t m_systemCount = 0;
    std::uint32_t m_commodityCount = 0;
    std::vector<SystemSurvey> m_systems;
    std::vector<std::uint8_t> m_signalCounts; // per system, from the seed
    std::vector<SurveyEntry> m_ledger;
    std::vector<LootRecord> m_loot;
    std::vector<MarketMemory> m_marketMemory; // sorted by market index
    std::vector<std::uint32_t> m_route;
    std::uint64_t m_seed = 0;
};

} // namespace sol::sim
