#include "sol/sim/economy.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/math/math.hpp"
#include "sol/core/profiler.hpp"

#include <algorithm>
#include <cstddef>

namespace sol::sim {

namespace {

constexpr std::uint32_t kInvalid = 0xffff'ffffu;
constexpr std::uint64_t kEconomyStream = 101;
constexpr std::uint8_t kUnreachable = kUnreachableHops;

[[nodiscard]] float rateAt(const std::vector<float>& rates, std::uint32_t commodity)
{
    return commodity < rates.size() ? rates[commodity] : 0.0f;
}

} // namespace

void Economy::initialize(const Galaxy& galaxy, const EconomyParams& params, std::uint64_t seed)
{
    SOL_ASSERT(!params.commodities.empty());
    m_params = params;
    m_rng.seed(seed, kEconomyStream);
    m_accumulator = 0.0;
    m_systemCount = static_cast<std::uint32_t>(galaxy.systems.size());

    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_params.commodities.size());
    m_commodityCount = commodityCount;
    m_markets.clear();
    m_marketOffset.assign(m_systemCount, 0);
    m_marketCount.assign(m_systemCount, 0);
    for (std::uint32_t s = 0; s < m_systemCount; ++s) {
        const SystemSpec& system = galaxy.systems[s];
        m_marketOffset[s] = static_cast<std::uint32_t>(m_markets.size());
        m_marketCount[s] = static_cast<std::uint32_t>(system.stations.size());
        for (std::uint32_t i = 0; i < system.stations.size(); ++i) {
            StationMarket market;
            market.systemIndex = s;
            market.stationIndex = i;
            market.archetype = system.stations[i].archetype;
            // Start at half capacity: prices open neutral and drift from
            // production/consumption immediately.
            const float capacity = market.archetype < m_params.archetypes.size()
                                       ? m_params.archetypes[market.archetype].stockCapacity
                                       : 0.0f;
            market.stock.assign(commodityCount, capacity * 0.5f);
            m_markets.push_back(std::move(market));
        }
    }

    // Hop table: BFS from every system over the gate graph, capped at
    // maxTradeJumps (beyond that traders don't look, so kUnreachable).
    m_hops.assign(static_cast<std::size_t>(m_systemCount) * m_systemCount, kUnreachable);
    std::vector<std::uint32_t> frontier;
    std::vector<std::uint32_t> next;
    for (std::uint32_t from = 0; from < m_systemCount; ++from) {
        std::uint8_t* row = m_hops.data() + static_cast<std::size_t>(from) * m_systemCount;
        row[from] = 0;
        frontier.assign(1, from);
        for (std::uint8_t depth = 1; depth <= m_params.maxTradeJumps && !frontier.empty(); ++depth) {
            next.clear();
            for (const std::uint32_t index : frontier) {
                for (const GateSpec& gate : galaxy.systems[index].gates) {
                    if (row[gate.toSystem] == kUnreachable) {
                        row[gate.toSystem] = depth;
                        next.push_back(gate.toSystem);
                    }
                }
            }
            frontier.swap(next);
        }
    }

    m_traders.clear();
    if (!m_markets.empty()) {
        const auto marketCount = static_cast<std::uint32_t>(m_markets.size());
        for (std::uint32_t t = 0; t < m_params.traderCount; ++t) {
            EconomyTrader trader;
            trader.market = m_rng.range(marketCount);
            trader.origin = trader.market; // Idle: it is where it came from
            // ⚑ Scatter the fleet along its own clock (Phase 8x). A fleet that
            // all starts Idle all thinks on the same tick, so it all departs
            // together, all jumps together and all arrives together: every
            // system in the galaxy is either empty or holds forty-five
            // haulers, on one cycle they share. It decoheres eventually,
            // because legs differ by hop count — but not inside the first
            // minutes of a session, which is exactly where a player would
            // read a quiet sky as the feature not working.
            //
            // Each trader therefore starts somewhere along a leg it is already
            // flying, and starts it EMPTY: repositioning creates no goods, so
            // the galaxy's books open holding exactly what they always did.
            for (int attempt = 0; attempt < 8; ++attempt) {
                const std::uint32_t destination = m_rng.range(marketCount);
                if (destination == trader.market) {
                    continue;
                }
                const std::uint8_t hops =
                    hopCount(m_markets[trader.market].systemIndex, m_markets[destination].systemIndex);
                if (hops == kUnreachable) {
                    continue; // no route a trader would plan: leave it parked
                }
                beginTransit(trader, destination, hops);
                trader.travelRemaining = static_cast<double>(m_rng.nextFloat01()) * trader.legTotal;
                break;
            }
            m_traders.push_back(trader);
        }
    }

    m_detained.assign(m_traders.size(), 0);
    m_arrivals.clear();
    m_tickPrices.assign(m_markets.size() * commodityCount, 0.0f);
    m_inbound.assign(m_markets.size() * commodityCount, 0.0f);
    m_satisfaction.assign(m_markets.size(), 1.0f);
    m_limiting.assign(m_markets.size(), kNoCommodity);
    refreshTickPrices();
}

std::uint32_t Economy::marketFor(std::uint32_t systemIndex, std::uint32_t stationIndex) const
{
    if (systemIndex >= m_systemCount || stationIndex >= m_marketCount[systemIndex]) {
        return kInvalid;
    }
    return m_marketOffset[systemIndex] + stationIndex;
}

std::uint8_t Economy::hopCount(std::uint32_t fromSystem, std::uint32_t toSystem) const
{
    if (fromSystem >= m_systemCount || toSystem >= m_systemCount) {
        return kUnreachable;
    }
    return m_hops[static_cast<std::size_t>(fromSystem) * m_systemCount + toSystem];
}

TraderRoute Economy::route(std::uint32_t traderIndex) const
{
    TraderRoute out;
    if (traderIndex >= m_traders.size()) {
        return out;
    }
    const EconomyTrader& trader = m_traders[traderIndex];
    if (trader.origin >= m_markets.size() || trader.market >= m_markets.size()) {
        return out;
    }
    const std::uint32_t fromSystem = m_markets[trader.origin].systemIndex;
    const std::uint32_t toSystem = m_markets[trader.market].systemIndex;
    out.fromMarket = trader.origin;
    out.toMarket = trader.market;
    const std::uint8_t hops = hopCount(fromSystem, toSystem);
    out.hops = hops == kUnreachable ? 0u : hops;

    if (trader.phase == TraderPhase::Idle) {
        out.system = toSystem; // parked, and `origin == market` says so
        return out;
    }

    // The clock, read back. Elapsed is clamped because a tick subtracts before
    // it checks, so the last instant of a leg is momentarily past its end.
    const double legSeconds = m_params.traderLegSeconds;
    const double total = trader.legTotal;
    const double elapsed = std::clamp(total - trader.travelRemaining, 0.0, std::max(total, 0.0));
    if (legSeconds <= 0.0) {
        // No in-system portion at all: the whole haul is gate transit, and
        // there is nowhere to draw a body.
        out.leg = TraderLeg::Jump;
        return out;
    }
    if (elapsed < legSeconds) {
        out.leg = TraderLeg::Depart;
        out.system = fromSystem;
        out.progress = static_cast<float>(elapsed / legSeconds);
        return out;
    }
    // A hopless haul has no middle: the arrive window opens exactly where the
    // depart window closes, so the trader never leaves the system.
    const double arriveStart = total - legSeconds;
    if (elapsed >= arriveStart) {
        out.leg = TraderLeg::Arrive;
        out.system = toSystem;
        out.progress = static_cast<float>((elapsed - arriveStart) / legSeconds);
        return out;
    }
    out.leg = TraderLeg::Jump; // system stays kNoSystem: it is between them
    out.progress = static_cast<float>((elapsed - legSeconds) / (arriveStart - legSeconds));
    return out;
}

float Economy::priceAtStock(std::uint32_t market, std::uint32_t commodity, float stockUnits) const
{
    if (market >= m_markets.size() || commodity >= m_params.commodities.size()) {
        return 0.0f;
    }
    const StationMarket& station = m_markets[market];
    const float capacity = station.archetype < m_params.archetypes.size()
                               ? m_params.archetypes[station.archetype].stockCapacity
                               : 0.0f;
    const float fraction = capacity > 0.0f ? core::clamp(stockUnits / capacity, 0.0f, 1.0f) : 1.0f;
    const float scale = core::lerp(m_params.maxPriceScale, m_params.minPriceScale, fraction);
    return m_params.commodities[commodity].basePrice * scale;
}

float Economy::price(std::uint32_t market, std::uint32_t commodity) const
{
    if (market >= m_markets.size() || commodity >= m_params.commodities.size()) {
        return 0.0f;
    }
    return priceAtStock(market, commodity, m_markets[market].stock[commodity]);
}

float Economy::buyPrice(std::uint32_t market, std::uint32_t commodity) const
{
    return price(market, commodity) * (1.0f + m_params.priceSpread);
}

float Economy::sellPrice(std::uint32_t market, std::uint32_t commodity) const
{
    return price(market, commodity) * (1.0f - m_params.priceSpread);
}

float Economy::stock(std::uint32_t market, std::uint32_t commodity) const
{
    if (market >= m_markets.size() || commodity >= m_params.commodities.size()) {
        return 0.0f;
    }
    return m_markets[market].stock[commodity];
}

float Economy::capacityOf(std::uint32_t market) const
{
    if (market >= m_markets.size()) {
        return 0.0f;
    }
    const std::uint32_t archetype = m_markets[market].archetype;
    return archetype < m_params.archetypes.size() ? m_params.archetypes[archetype].stockCapacity : 0.0f;
}

float Economy::satisfaction(std::uint32_t market) const
{
    return market < m_satisfaction.size() ? m_satisfaction[market] : 1.0f;
}

std::uint32_t Economy::limitingCommodity(std::uint32_t market) const
{
    return market < m_limiting.size() ? m_limiting[market] : kNoCommodity;
}

float Economy::tickPrice(std::uint32_t market, std::uint32_t commodity) const
{
    return m_tickPrices[static_cast<std::size_t>(market) * m_commodityCount + commodity];
}

void Economy::refreshMarketPrices(std::uint32_t market)
{
    float* row = m_tickPrices.data() + static_cast<std::size_t>(market) * m_commodityCount;
    for (std::uint32_t c = 0; c < m_commodityCount; ++c) {
        row[c] = price(market, c);
    }
}

float Economy::inbound(std::uint32_t market, std::uint32_t commodity) const
{
    return m_inbound[static_cast<std::size_t>(market) * m_commodityCount + commodity];
}

void Economy::refreshInbound()
{
    std::fill(m_inbound.begin(), m_inbound.end(), 0.0f);
    for (const EconomyTrader& trader : m_traders) {
        if (trader.phase == TraderPhase::InTransit && trader.cargo > 0.0f) {
            m_inbound[static_cast<std::size_t>(trader.market) * m_commodityCount + trader.commodity] +=
                trader.cargo;
        }
    }
}

void Economy::refreshTickPrices()
{
    for (std::uint32_t m = 0; m < m_markets.size(); ++m) {
        refreshMarketPrices(m);
    }
}

TradeResult Economy::buy(std::uint32_t market, std::uint32_t commodity, float units)
{
    TradeResult result;
    if (market >= m_markets.size() || commodity >= m_params.commodities.size() || units <= 0.0f) {
        return result;
    }
    StationMarket& station = m_markets[market];
    result.units = std::min(units, station.stock[commodity]);
    // Priced at the midpoint of the stock the trade actually moves, not at
    // the pre-trade marginal price. The curve is linear in stock, so the
    // midpoint *is* the exact average — and without it a big block clears at
    // a price that only moves afterwards, which let a buy-and-sell-back at
    // one station turn a profit that no spread was ever going to cover.
    result.credits = result.units *
                     priceAtStock(market, commodity, station.stock[commodity] - result.units * 0.5f) *
                     (1.0f + m_params.priceSpread);
    station.stock[commodity] -= result.units;
    return result;
}

TradeResult Economy::sell(std::uint32_t market, std::uint32_t commodity, float units)
{
    TradeResult result;
    if (market >= m_markets.size() || commodity >= m_params.commodities.size() || units <= 0.0f) {
        return result;
    }
    StationMarket& station = m_markets[market];
    const float capacity = station.archetype < m_params.archetypes.size()
                               ? m_params.archetypes[station.archetype].stockCapacity
                               : 0.0f;
    result.units = std::min(units, std::max(0.0f, capacity - station.stock[commodity]));
    result.credits = result.units *
                     priceAtStock(market, commodity, station.stock[commodity] + result.units * 0.5f) *
                     (1.0f - m_params.priceSpread);
    station.stock[commodity] += result.units;
    return result;
}

void Economy::deliver(std::uint32_t market, std::uint32_t commodity, float units)
{
    if (market >= m_markets.size() || commodity >= m_params.commodities.size() || units <= 0.0f) {
        return;
    }
    StationMarket& station = m_markets[market];
    const float capacity = station.archetype < m_params.archetypes.size()
                               ? m_params.archetypes[station.archetype].stockCapacity
                               : 0.0f;
    station.stock[commodity] = std::min(station.stock[commodity] + units, capacity);
}

void Economy::raidSystem(std::uint32_t systemIndex, float stockFraction)
{
    const float keep = 1.0f - core::clamp(stockFraction, 0.0f, 1.0f);
    for (StationMarket& market : m_markets) {
        if (market.systemIndex != systemIndex) {
            continue;
        }
        for (float& stockUnits : market.stock) {
            stockUnits *= keep;
        }
    }
    for (EconomyTrader& trader : m_traders) {
        if (trader.phase == TraderPhase::InTransit && m_markets[trader.market].systemIndex == systemIndex) {
            trader.cargo = 0.0f;
        }
    }
}

bool Economy::loseTrader(std::uint32_t traderIndex)
{
    if (traderIndex >= m_traders.size()) {
        return false;
    }
    EconomyTrader& trader = m_traders[traderIndex];
    if (trader.phase != TraderPhase::InTransit) {
        return false; // parked at a market: there is no haul to lose
    }
    // Back where it started, empty-handed. `market` is the destination while
    // in transit, so returning to `origin` is the whole of it: the goods never
    // arrived, and the fleet slot re-enters the pool at the station it left.
    trader.market = trader.origin;
    trader.phase = TraderPhase::Idle;
    trader.travelRemaining = 0.0;
    trader.legTotal = 0.0;
    trader.cargo = 0.0f;
    // m_inbound is derived from the trader list and rebuilt every step, so the
    // destination stops counting on this delivery without being told.
    return true;
}

void Economy::clearDetained()
{
    m_detained.assign(m_traders.size(), 0);
}

void Economy::detainTrader(std::uint32_t traderIndex)
{
    if (traderIndex < m_detained.size()) {
        m_detained[traderIndex] = 1;
    }
}

bool Economy::detained(std::uint32_t traderIndex) const
{
    return traderIndex < m_detained.size() && m_detained[traderIndex] != 0;
}

void Economy::tick(const Galaxy& galaxy, double dt, FeedstockSource* source)
{
    // Cleared here rather than by whoever reads it: an arrival is a fact about
    // the tick it happened in, so the list belongs to this call and cannot
    // outlive it. A headless test that never asks therefore accumulates
    // nothing across four sim hours.
    m_arrivals.clear();
    m_accumulator += dt;
    while (m_accumulator >= m_params.tickInterval) {
        m_accumulator -= m_params.tickInterval;
        step(galaxy, m_params.tickInterval, source);
    }
}

// One tick of every station's books. The order matters: the satisfaction
// ratio is read off the stock the station started the tick with, so a
// station cannot spend the same units twice, and every delta is applied
// afterwards.
void Economy::produce(double dt, FeedstockSource* source)
{
    const float step = static_cast<float>(dt);
    for (std::uint32_t m = 0; m < m_markets.size(); ++m) {
        StationMarket& market = m_markets[m];
        if (market.archetype >= m_params.archetypes.size()) {
            m_satisfaction[m] = 0.0f;
            m_limiting[m] = kNoCommodity;
            continue;
        }
        const EconomyArchetype& archetype = m_params.archetypes[market.archetype];

        // How much of its nominal output the station can actually make. Two
        // things hold it back and both scale the *whole* station, because a
        // production line runs or it doesn't:
        //   - the scarcest feedstock (a station with none runs flat out), and
        //   - the fullest warehouse. A station with nowhere to put its output
        //     throttles back rather than making it and throwing it away —
        //     without which a glutted mining outpost would go on tearing rock
        //     out of the ground forever, and a glutted refinery would go on
        //     burning ore for nothing.
        float ratio = 1.0f;
        std::uint32_t limiting = kNoCommodity;
        for (std::uint32_t c = 0; c < market.stock.size(); ++c) {
            const float wanted = rateAt(archetype.feedstock, c) * step;
            if (wanted > 0.0f) {
                const float have = std::min(1.0f, market.stock[c] / wanted);
                if (have < ratio) {
                    ratio = have;
                    limiting = c;
                }
            }
            const float makes = rateAt(archetype.production, c) * step;
            if (makes > 0.0f) {
                const float headroom = std::max(0.0f, archetype.stockCapacity - market.stock[c]);
                // Ease off across the top of the warehouse rather than
                // slamming shut when it is exactly full.
                const float band = archetype.stockCapacity * m_params.outputTaperFraction;
                const float room =
                    band > 0.0f ? core::clamp(headroom / band, 0.0f, 1.0f) : std::min(1.0f, headroom / makes);
                if (room < ratio) {
                    ratio = room;
                    limiting = c;
                }
            }
        }

        for (std::uint32_t c = 0; c < market.stock.size(); ++c) {
            float delta = 0.0f;

            const float output = rateAt(archetype.production, c) * step * ratio;
            if (output > 0.0f) {
                if (archetype.extracts && source != nullptr) {
                    // An extractor's output is limited by what is left in the
                    // ground, and extraction is 1:1, so what came back *is*
                    // what gets made — nothing is drawn and then wasted.
                    const float drawn = source->draw(m, c, output);
                    delta += drawn;
                    if (drawn < output) {
                        const float rockRatio = output > 0.0f ? drawn / output : 0.0f;
                        if (rockRatio < ratio) {
                            ratio = rockRatio;
                            limiting = c;
                        }
                    }
                } else {
                    delta += output;
                }
            }

            // Feedstock is spent in proportion to what it actually made.
            delta -= rateAt(archetype.feedstock, c) * step * ratio;
            // Upkeep burns whether or not anything was produced, and simply
            // runs out when it runs out (the clamp below).
            delta -= rateAt(archetype.consumption, c) * step;

            market.stock[c] = core::clamp(market.stock[c] + delta, 0.0f, archetype.stockCapacity);
        }

        m_satisfaction[m] = ratio;
        m_limiting[m] = limiting;
    }
}

void Economy::step(const Galaxy& galaxy, double dt, FeedstockSource* source)
{
    {
        SOL_PROFILE_ZONE_NAMED(produceZone, "economy.produce");
        SOL_PROFILE_COUNT(produceZone, m_markets.size());
        produce(dt, source);
    }
    // Agents decide against the prices this tick opened with, which keeps
    // their inner loop a flat array read instead of a price() call per
    // market per commodity.
    {
        SOL_PROFILE_ZONE("economy.prices");
        refreshTickPrices();
        refreshInbound();
    }

    SOL_PROFILE_ZONE_NAMED(traderZone, "economy.traders");
    SOL_PROFILE_COUNT(traderZone, m_traders.size());
    for (std::size_t index = 0; index < m_traders.size(); ++index) {
        EconomyTrader& trader = m_traders[index];
        switch (trader.phase) {
        case TraderPhase::Idle:
            traderThink(galaxy, trader);
            break;
        case TraderPhase::InTransit:
            if (index < m_detained.size() && m_detained[index] != 0) {
                break; // being shot at: the delivery waits, the fight does not
            }
            trader.travelRemaining -= dt;
            if (trader.travelRemaining <= 0.0) {
                // Arrive and sell what the market can absorb. Whatever will
                // not fit stays in the hold: a full warehouse is a reason to
                // carry on somewhere else, not a reason to tip the cargo into
                // space. (It used to be jettisoned, on the theory that this
                // was rare. With a fleet this size it is not rare at all —
                // traders converge on the same starved market, the first few
                // fill it, and the rest destroyed their entire hold on
                // arrival. That is a galaxy-wide leak of goods, and it drains
                // every market in the game given a few hours.)
                const TradeResult sold = sell(trader.market, trader.commodity, trader.cargo);
                refreshMarketPrices(trader.market); // same reason as the buy side
                trader.cargo = std::max(0.0f, trader.cargo - sold.units);
                trader.phase = TraderPhase::Idle;
                // Parked: the leg it just flew is over, so the clock reads
                // zero and its origin is where it is standing.
                trader.origin = trader.market;
                trader.travelRemaining = 0.0;
                trader.legTotal = 0.0;
                // It got there. The one thing an escort contract is waiting to
                // hear, reported from the one place a haul can end well —
                // loseTrader() is the other end and reports the other answer.
                m_arrivals.push_back({.system = m_markets[trader.market].systemIndex,
                                      .trader = static_cast<std::uint32_t>(index)});
            }
            break;
        }
    }
}

void Economy::traderThink(const Galaxy& galaxy, EconomyTrader& trader)
{
    (void)galaxy;
    const StationMarket& here = m_markets[trader.market];
    const std::uint8_t* hopRow = m_hops.data() + static_cast<std::size_t>(here.systemIndex) * m_systemCount;

    // Still holding something the last stop could not take: shift it before
    // thinking about anything else. Room may have opened here since arrival;
    // if not, carry it to the best-paying market in reach.
    if (trader.cargo > 0.0f) {
        const TradeResult sold = sell(trader.market, trader.commodity, trader.cargo);
        if (sold.units > 0.0f) {
            refreshMarketPrices(trader.market);
            trader.cargo -= sold.units;
        }
        if (trader.cargo > 0.0f) {
            float bestPrice = tickPrice(trader.market, trader.commodity);
            std::uint32_t bestDestination = kInvalid;
            for (std::uint32_t m = 0; m < m_markets.size(); ++m) {
                if (m == trader.market || hopRow[m_markets[m].systemIndex] == kUnreachable) {
                    continue;
                }
                const float value = tickPrice(m, trader.commodity);
                if (value > bestPrice) {
                    bestPrice = value;
                    bestDestination = m;
                }
            }
            if (bestDestination != kInvalid) {
                beginTransit(trader, bestDestination, hopRow[m_markets[bestDestination].systemIndex]);
            }
            return; // laden either way: no room aboard for a new haul
        }
    }

    // Best profit-per-second over every commodity and reachable market. The
    // margin is measured across the spread — buy high, sell low — so a haul
    // has to clear the round-trip cost to look worth taking at all.
    const float buyScale = 1.0f + m_params.priceSpread;
    const float sellScale = 1.0f - m_params.priceSpread;
    float bestRate = 0.0f;
    std::uint32_t bestMarket = kInvalid;
    std::uint32_t bestCommodity = 0;
    for (std::uint32_t m = 0; m < m_markets.size(); ++m) {
        if (m == trader.market) {
            continue;
        }
        const std::uint8_t hops = hopRow[m_markets[m].systemIndex];
        if (hops == kUnreachable) {
            continue;
        }
        const double travelSeconds =
            m_params.traderLegSeconds * 2.0 + static_cast<double>(hops) * m_params.jumpSeconds;
        const float destinationCapacity = capacityOf(m);
        for (std::uint32_t c = 0; c < m_params.commodities.size(); ++c) {
            const float available = std::min(m_params.traderCargo, here.stock[c]);
            if (available <= 1.0f) {
                continue;
            }
            // Room for this load once everything already flying there has
            // landed. Without this the whole fleet answers the same shortage
            // at once and most of them arrive to a warehouse that filled up
            // while they were in the air.
            const float room = destinationCapacity - m_markets[m].stock[c] - inbound(m, c);
            if (room < available) {
                continue;
            }
            const float cost = tickPrice(trader.market, c) * buyScale * available;
            if (!(cost > 0.0f)) {
                continue;
            }
            const float profit =
                (tickPrice(m, c) * sellScale - tickPrice(trader.market, c) * buyScale) * available;
            // Return on capital per second, not credits per second. A hauler
            // has to *buy* what it carries, so a hold of machinery ties up
            // roughly ten times the money a hold of ore does — ranking by
            // absolute profit quietly means nobody ever hauls anything cheap,
            // however badly it is needed. That is what left mines sitting at
            // capacity while refineries three jumps away sat empty.
            const float rate = profit / (cost * static_cast<float>(travelSeconds));
            if (rate > bestRate) {
                bestRate = rate;
                bestMarket = m;
                bestCommodity = c;
            }
        }
    }
    if (bestMarket == kInvalid) {
        // Deadhead: no profitable haul starts here, so reposition (empty) at
        // the cheapest stocked market in range — the glut is where the next
        // job is. Without this a trader idles forever at a drained consumer.
        float cheapest = 0.0f;
        for (std::uint32_t m = 0; m < m_markets.size(); ++m) {
            if (m == trader.market || hopRow[m_markets[m].systemIndex] == kUnreachable) {
                continue;
            }
            for (std::uint32_t c = 0; c < m_params.commodities.size(); ++c) {
                if (m_markets[m].stock[c] <= m_params.traderCargo) {
                    continue;
                }
                const float ratio =
                    tickPrice(trader.market, c) > 0.0f ? tickPrice(m, c) / tickPrice(trader.market, c) : 1.0f;
                if (bestMarket == kInvalid || ratio < cheapest) {
                    cheapest = ratio;
                    bestMarket = m;
                }
            }
        }
        if (bestMarket == kInvalid || cheapest >= 1.0f) {
            return; // nowhere better; look again next tick
        }
        trader.cargo = 0.0f;
        beginTransit(trader, bestMarket, hopRow[m_markets[bestMarket].systemIndex]);
        return;
    }

    const TradeResult bought = buy(trader.market, bestCommodity, m_params.traderCargo);
    // The next trader to think this tick has to see what this one just did,
    // or the whole fleet reads one stale snapshot and piles onto a single
    // route together — two hundred traders behaving like one. Refreshing the
    // row that moved keeps the inner loop a flat array read and restores the
    // feedback that spreads them out.
    refreshMarketPrices(trader.market);
    if (bought.units <= 0.0f) {
        return;
    }
    trader.commodity = bestCommodity;
    trader.cargo = bought.units;
    // Visible to every trader that thinks after this one, this same tick.
    m_inbound[static_cast<std::size_t>(bestMarket) * m_commodityCount + bestCommodity] += bought.units;
    beginTransit(trader, bestMarket, hopRow[m_markets[bestMarket].systemIndex]);
}

void Economy::beginTransit(EconomyTrader& trader, std::uint32_t destination, std::uint8_t hops)
{
    trader.origin = trader.market;
    trader.market = destination;
    trader.phase = TraderPhase::InTransit;
    // The quote a leg is flown against, and the number route() decomposes.
    trader.legTotal = m_params.traderLegSeconds * 2.0 + static_cast<double>(hops) * m_params.jumpSeconds;
    trader.travelRemaining = trader.legTotal;
}

void Economy::save(core::BinaryWriter& writer) const
{
    writer.write(static_cast<std::uint32_t>(m_markets.size()));
    for (const StationMarket& market : m_markets) {
        for (const float stockUnits : market.stock) {
            writer.write(stockUnits);
        }
    }
    writer.write(static_cast<std::uint32_t>(m_traders.size()));
    for (const EconomyTrader& trader : m_traders) {
        writer.write(trader.market);
        writer.write(trader.origin); // v12: the route half of a trader
        writer.write(static_cast<std::uint8_t>(trader.phase));
        writer.write(trader.travelRemaining);
        writer.write(trader.legTotal); // v12
        writer.write(trader.commodity);
        writer.write(trader.cargo);
    }
    writer.write(m_accumulator);
    // The wandering rng state is part of determinism across save/load.
    const core::Rng::RawState rngState = m_rng.rawState();
    writer.write(rngState.state);
    writer.write(rngState.inc);
}

bool Economy::load(core::BinaryReader& reader)
{
    std::uint32_t marketCount = 0;
    if (!reader.read(marketCount) || marketCount != m_markets.size()) {
        return false; // galaxy/params mismatch: initialize() first
    }
    for (StationMarket& market : m_markets) {
        for (float& stockUnits : market.stock) {
            if (!reader.read(stockUnits)) {
                return false;
            }
        }
    }
    std::uint32_t traderCount = 0;
    if (!reader.read(traderCount) || traderCount != m_traders.size()) {
        return false;
    }
    for (EconomyTrader& trader : m_traders) {
        std::uint8_t phase = 0;
        if (!reader.read(trader.market) || !reader.read(trader.origin) || !reader.read(phase) ||
            !reader.read(trader.travelRemaining) || !reader.read(trader.legTotal) ||
            !reader.read(trader.commodity) || !reader.read(trader.cargo) ||
            trader.market >= m_markets.size() ||
            // A route with an origin off the end of the market list would be a
            // body drawn at a station that does not exist, so it is checked
            // exactly as hard as the destination always has been.
            trader.origin >= m_markets.size() || trader.legTotal < 0.0 ||
            phase > static_cast<std::uint8_t>(TraderPhase::InTransit)) {
            return false;
        }
        trader.phase = static_cast<TraderPhase>(phase);
    }
    core::Rng::RawState rngState;
    if (!reader.read(m_accumulator) || !reader.read(rngState.state) || !reader.read(rngState.inc)) {
        return false;
    }
    m_rng.setRawState(rngState);
    // The agents' price snapshot is derived, so it is rebuilt rather than
    // saved — but it has to exist before the first tick after a load.
    refreshTickPrices();
    return true;
}

} // namespace sol::sim
