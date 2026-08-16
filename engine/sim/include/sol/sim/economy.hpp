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

inline constexpr std::uint32_t kNoCommodity = 0xffff'ffffu;

struct EconomyCommodity
{
    float basePrice = 10.0f; // credits/unit at the neutral stock level
};

// Production profile of one station archetype; vectors are indexed by
// commodity and sized to the commodity count (shorter vectors read as 0).
//
// Inputs come in two kinds (Phase 8g), and the split is load-bearing rather
// than decorative: the production graph is a *cycle* (food -> ore -> metal ->
// machinery -> food), so gating output on every input would let a single
// empty link seize the whole galaxy permanently. Feedstock gates; upkeep
// does not, which leaves farms and mines as unconditional sources the chain
// can flow outward from.
struct EconomyArchetype
{
    std::vector<float> production;  // units/s added to stock
    std::vector<float> consumption; // upkeep: units/s burned regardless
    std::vector<float> feedstock;   // units/s of input that throttles output
    // Production is drawn from the world rather than from anyone's stock: a
    // mining outpost eats rock. Honoured only when a FeedstockSource is
    // installed; without one an extractor produces freely, which is what
    // keeps this change invisible to sim tests that don't care about it.
    bool extracts = false;
    float stockCapacity = 1'000.0f; // per commodity
};

// Where an extracting station's output actually comes from. The game
// installs one backed by MiningSim's asteroid fields; keeping it abstract is
// what lets sim::Economy stay a sibling of sim::MiningSim instead of
// depending on it.
class FeedstockSource
{
public:
    virtual ~FeedstockSource() = default;
    // Takes up to `units` for this market and returns what was actually
    // there. Called at most once per market per commodity per tick.
    virtual float draw(std::uint32_t market, std::uint32_t commodity, float units) = 0;
};

struct EconomyParams
{
    std::vector<EconomyCommodity> commodities;
    std::vector<EconomyArchetype> archetypes; // indexed by StationSpec::archetype
    // The fleet has to be able to actually carry the galaxy's demand
    // (Phase 8g). One trader delivers traderCargo per round trip of
    // ~traderLegSeconds*2 + hops*jumpSeconds, so at these numbers it moves
    // ~0.55 units/s and the fleet ~110 units/s against a galaxy that asks for
    // roughly 50 — the old 40x50 moved ~10, which is why consumers drained to
    // zero and stayed there whatever the archetype rates said.
    std::uint32_t traderCount = 200;
    float traderCargo = 120.0f;     // units per haul
    double traderLegSeconds = 90.0; // in-system travel per endpoint
    double jumpSeconds = 20.0;      // per gate transit
    std::uint32_t maxTradeJumps = 3;
    double tickInterval = 1.0;      // coarse tick, seconds
    // Price scale at empty (max) and full (min) stock; linear in between.
    float maxPriceScale = 2.0f;
    float minPriceScale = 0.5f;
    // Half-spread around that curve (Phase 8g): the player and the agents buy
    // above the mid price and sell below it. Without a spread a thin margin
    // is indistinguishable from a good one and a round trip at one station is
    // free, so nothing makes a route worth flying rather than any other.
    float priceSpread = 0.05f;
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

    // Advances real time; internally steps at params.tickInterval. `source`
    // supplies extracting archetypes (null: they produce freely).
    void tick(const Galaxy& galaxy, double dt, FeedstockSource* source = nullptr);

    [[nodiscard]] const std::vector<StationMarket>& markets() const { return m_markets; }
    [[nodiscard]] const std::vector<EconomyTrader>& traders() const { return m_traders; }
    [[nodiscard]] const EconomyParams& params() const { return m_params; }

    // Market index for a station, or kNoFaction-style invalid on bad input.
    [[nodiscard]] std::uint32_t marketFor(std::uint32_t systemIndex,
                                          std::uint32_t stationIndex) const;

    // Mid price at current stock: the curve itself, and what a price display
    // or a "is this dear or cheap" comparison wants.
    [[nodiscard]] float price(std::uint32_t market, std::uint32_t commodity) const;
    // The same curve evaluated at an arbitrary stock level. buy()/sell() use
    // it at the midpoint of what they move, which is the exact average price
    // of the block because the curve is linear in stock.
    [[nodiscard]] float priceAtStock(std::uint32_t market, std::uint32_t commodity,
                                     float stockUnits) const;
    // What it actually costs to take a unit out (above mid) and what a unit
    // handed over actually pays (below mid).
    [[nodiscard]] float buyPrice(std::uint32_t market, std::uint32_t commodity) const;
    [[nodiscard]] float sellPrice(std::uint32_t market, std::uint32_t commodity) const;
    [[nodiscard]] float stock(std::uint32_t market, std::uint32_t commodity) const;
    [[nodiscard]] float capacityOf(std::uint32_t market) const;

    // How much of its nominal output a station managed on the last tick, in
    // [0,1], and the commodity that held it back (kNoCommodity when nothing
    // did). Derived state: recomputed every tick, never saved.
    [[nodiscard]] float satisfaction(std::uint32_t market) const;
    [[nodiscard]] std::uint32_t limitingCommodity(std::uint32_t market) const;

    // Player-side trades: move up to `units` against available stock/space;
    // the result reports what actually moved and its total price at the
    // marginal per-unit price (evaluated once per call, matching the agents).
    TradeResult buy(std::uint32_t market, std::uint32_t commodity, float units);
    TradeResult sell(std::uint32_t market, std::uint32_t commodity, float units);

    // A mission hand-in (Phase 8c): units arrive in a station's stock without
    // a market trade — the haul contract pays instead, and the refilled stock
    // moves prices like any other supply.
    void deliver(std::uint32_t market, std::uint32_t commodity, float units);

    // A faction raid (Phase 8b): every market in the system loses
    // stockFraction of each commodity, and traders hauling toward the system
    // lose their cargo (they arrive empty) — piracy propagates into
    // shortages and prices per GDD 6.
    void raidSystem(std::uint32_t systemIndex, float stockFraction);

    // Dynamic state only (stocks, traders, phase accumulator); the layout is
    // re-derived from galaxy+params, and load fails if they don't match.
    void save(core::BinaryWriter& writer) const;
    [[nodiscard]] bool load(core::BinaryReader& reader);

private:
    void step(const Galaxy& galaxy, double dt, FeedstockSource* source);
    void produce(double dt, FeedstockSource* source);
    void refreshTickPrices();
    void traderThink(const Galaxy& galaxy, EconomyTrader& trader);
    [[nodiscard]] float tickPrice(std::uint32_t market, std::uint32_t commodity) const;

    EconomyParams m_params;
    std::vector<StationMarket> m_markets;
    std::vector<EconomyTrader> m_traders;
    // Hop counts between systems capped at maxTradeJumps + 1 sentinel,
    // precomputed (BFS per system) for trader route evaluation.
    std::vector<std::uint8_t> m_hops; // [from * systemCount + to]
    // Markets are built in system-then-station order, so a station's market
    // index is one addition rather than a scan over every market in the
    // galaxy — this is on per-frame UI paths, not just the sim tick.
    std::vector<std::uint32_t> m_marketOffset; // per system: first market
    std::vector<std::uint32_t> m_marketCount;  // per system: how many
    // Mid prices as of the start of the current tick. Agents decide against
    // this snapshot rather than calling price() inside an O(markets x
    // commodities) inner loop, which is what makes a fleet of a few hundred
    // traders affordable; their trades still clear at the live price.
    std::vector<float> m_tickPrices; // [market * commodityCount + commodity]
    std::vector<float> m_satisfaction;
    std::vector<std::uint32_t> m_limiting;
    std::uint32_t m_systemCount = 0;
    std::uint32_t m_commodityCount = 0;
    core::Rng m_rng;
    double m_accumulator = 0.0;
};

} // namespace sol::sim
