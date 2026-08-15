#pragma once

// Agent-based economy on the coarse sim layer (engine plan 2.6 / Phase 7):
// every station in the galaxy carries a market (per-commodity stock);
// archetype production/consumption rates move stock continuously, prices
// derive from stock vs. capacity, and NPC trader agents haul goods along
// the gate graph wherever the price gap pays for the trip. Runs galaxy-wide
// at a coarse tick, at 1x real time (decisions/005: no time compression),
// and is deterministic for a given seed + tick sequence.
//
// The game layers player trading on top through buy()/sell(), which move
// the same stock the agents see — prices react to both.

#include "sol/sim/universe.hpp"

#include "sol/core/random.hpp"
#include "sol/core/serialize.hpp"

#include <cstdint>
#include <vector>

namespace sol::sim {

struct EconomyCommodity
{
    float basePrice = 10.0f; // credits/unit at the neutral stock level
};

// Production profile of one station archetype; vectors are indexed by
// commodity and sized to the commodity count (shorter vectors read as 0).
struct EconomyArchetype
{
    std::vector<float> production;  // units/s added to stock
    std::vector<float> consumption; // units/s removed from stock
    float stockCapacity = 1'000.0f; // per commodity
};

struct EconomyParams
{
    std::vector<EconomyCommodity> commodities;
    std::vector<EconomyArchetype> archetypes; // indexed by StationSpec::archetype
    std::uint32_t traderCount = 40;
    float traderCargo = 50.0f;      // units per haul
    double traderLegSeconds = 90.0; // in-system travel per endpoint
    double jumpSeconds = 20.0;      // per gate transit
    std::uint32_t maxTradeJumps = 3;
    double tickInterval = 1.0;      // coarse tick, seconds
    // Price scale at empty (max) and full (min) stock; linear in between.
    float maxPriceScale = 2.0f;
    float minPriceScale = 0.5f;
};

// One station's market. Station identity is (system, station) in the galaxy;
// markets are stored flat in system-then-station order.
struct StationMarket
{
    std::uint32_t systemIndex = 0;
    std::uint32_t stationIndex = 0;
    std::uint32_t archetype = 0;
    std::vector<float> stock; // per commodity
};

enum class TraderPhase : std::uint8_t
{
    Idle = 0,  // at market, looking for a trade
    InTransit, // hauling cargo toward market
};

struct EconomyTrader
{
    std::uint32_t market = 0; // current market (Idle) or destination (InTransit)
    TraderPhase phase = TraderPhase::Idle;
    double travelRemaining = 0.0; // seconds, InTransit
    std::uint32_t commodity = 0;
    float cargo = 0.0f; // units aboard
};

struct TradeResult
{
    float units = 0.0f;   // actually moved
    float credits = 0.0f; // total price of those units
};

class Economy
{
public:
    // Builds one market per station and scatters traders across them.
    // Deterministic for (galaxy, params, seed).
    void initialize(const Galaxy& galaxy, const EconomyParams& params, std::uint64_t seed);

    // Advances real time; internally steps at params.tickInterval.
    void tick(const Galaxy& galaxy, double dt);

    [[nodiscard]] const std::vector<StationMarket>& markets() const { return m_markets; }
    [[nodiscard]] const std::vector<EconomyTrader>& traders() const { return m_traders; }
    [[nodiscard]] const EconomyParams& params() const { return m_params; }

    // Market index for a station, or kNoFaction-style invalid on bad input.
    [[nodiscard]] std::uint32_t marketFor(std::uint32_t systemIndex,
                                          std::uint32_t stationIndex) const;

    // Unit price at current stock (buys and sells share it for now).
    [[nodiscard]] float price(std::uint32_t market, std::uint32_t commodity) const;
    [[nodiscard]] float stock(std::uint32_t market, std::uint32_t commodity) const;

    // Player-side trades: move up to `units` against available stock/space;
    // the result reports what actually moved and its total price at the
    // marginal per-unit price (evaluated once per call, matching the agents).
    TradeResult buy(std::uint32_t market, std::uint32_t commodity, float units);
    TradeResult sell(std::uint32_t market, std::uint32_t commodity, float units);

    // Dynamic state only (stocks, traders, phase accumulator); the layout is
    // re-derived from galaxy+params, and load fails if they don't match.
    void save(core::BinaryWriter& writer) const;
    [[nodiscard]] bool load(core::BinaryReader& reader);

private:
    void step(const Galaxy& galaxy, double dt);
    void traderThink(const Galaxy& galaxy, EconomyTrader& trader);

    EconomyParams m_params;
    std::vector<StationMarket> m_markets;
    std::vector<EconomyTrader> m_traders;
    // Hop counts between systems capped at maxTradeJumps + 1 sentinel,
    // precomputed (BFS per system) for trader route evaluation.
    std::vector<std::uint8_t> m_hops; // [from * systemCount + to]
    std::uint32_t m_systemCount = 0;
    core::Rng m_rng;
    double m_accumulator = 0.0;
};

} // namespace sol::sim
