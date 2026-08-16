#include "sol/sim/economy.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/math/math.hpp"

#include <algorithm>
#include <cstddef>

namespace sol::sim {

namespace {

constexpr std::uint32_t kInvalid = 0xffff'ffffu;
constexpr std::uint64_t kEconomyStream = 101;
constexpr std::uint8_t kUnreachable = 0xff;

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

    const std::uint32_t commodityCount =
        static_cast<std::uint32_t>(m_params.commodities.size());
    m_markets.clear();
    for (std::uint32_t s = 0; s < m_systemCount; ++s) {
        const SystemSpec& system = galaxy.systems[s];
        for (std::uint32_t i = 0; i < system.stations.size(); ++i) {
            StationMarket market;
            market.systemIndex = s;
            market.stationIndex = i;
            market.archetype = system.stations[i].archetype;
            // Start at half capacity: prices open neutral and drift from
            // production/consumption immediately.
            const float capacity =
                market.archetype < m_params.archetypes.size()
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
        for (std::uint8_t depth = 1;
             depth <= m_params.maxTradeJumps && !frontier.empty(); ++depth) {
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
        for (std::uint32_t t = 0; t < m_params.traderCount; ++t) {
            EconomyTrader trader;
            trader.market = m_rng.range(static_cast<std::uint32_t>(m_markets.size()));
            m_traders.push_back(trader);
        }
    }
}

std::uint32_t Economy::marketFor(std::uint32_t systemIndex, std::uint32_t stationIndex) const
{
    for (std::uint32_t i = 0; i < m_markets.size(); ++i) {
        if (m_markets[i].systemIndex == systemIndex &&
            m_markets[i].stationIndex == stationIndex) {
            return i;
        }
    }
    return kInvalid;
}

float Economy::price(std::uint32_t market, std::uint32_t commodity) const
{
    if (market >= m_markets.size() || commodity >= m_params.commodities.size()) {
        return 0.0f;
    }
    const StationMarket& station = m_markets[market];
    const float capacity = station.archetype < m_params.archetypes.size()
                               ? m_params.archetypes[station.archetype].stockCapacity
                               : 0.0f;
    const float fraction =
        capacity > 0.0f ? core::clamp(station.stock[commodity] / capacity, 0.0f, 1.0f) : 1.0f;
    const float scale =
        core::lerp(m_params.maxPriceScale, m_params.minPriceScale, fraction);
    return m_params.commodities[commodity].basePrice * scale;
}

float Economy::stock(std::uint32_t market, std::uint32_t commodity) const
{
    if (market >= m_markets.size() || commodity >= m_params.commodities.size()) {
        return 0.0f;
    }
    return m_markets[market].stock[commodity];
}

TradeResult Economy::buy(std::uint32_t market, std::uint32_t commodity, float units)
{
    TradeResult result;
    if (market >= m_markets.size() || commodity >= m_params.commodities.size() ||
        units <= 0.0f) {
        return result;
    }
    StationMarket& station = m_markets[market];
    result.units = std::min(units, station.stock[commodity]);
    result.credits = result.units * price(market, commodity);
    station.stock[commodity] -= result.units;
    return result;
}

TradeResult Economy::sell(std::uint32_t market, std::uint32_t commodity, float units)
{
    TradeResult result;
    if (market >= m_markets.size() || commodity >= m_params.commodities.size() ||
        units <= 0.0f) {
        return result;
    }
    StationMarket& station = m_markets[market];
    const float capacity = station.archetype < m_params.archetypes.size()
                               ? m_params.archetypes[station.archetype].stockCapacity
                               : 0.0f;
    result.units = std::min(units, std::max(0.0f, capacity - station.stock[commodity]));
    result.credits = result.units * price(market, commodity);
    station.stock[commodity] += result.units;
    return result;
}

void Economy::deliver(std::uint32_t market, std::uint32_t commodity, float units)
{
    if (market >= m_markets.size() || commodity >= m_params.commodities.size() ||
        units <= 0.0f) {
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
        if (trader.phase == TraderPhase::InTransit &&
            m_markets[trader.market].systemIndex == systemIndex) {
            trader.cargo = 0.0f;
        }
    }
}

void Economy::tick(const Galaxy& galaxy, double dt)
{
    m_accumulator += dt;
    while (m_accumulator >= m_params.tickInterval) {
        m_accumulator -= m_params.tickInterval;
        step(galaxy, m_params.tickInterval);
    }
}

void Economy::step(const Galaxy& galaxy, double dt)
{
    // Production/consumption, clamped to [0, capacity].
    for (StationMarket& market : m_markets) {
        if (market.archetype >= m_params.archetypes.size()) {
            continue;
        }
        const EconomyArchetype& archetype = m_params.archetypes[market.archetype];
        for (std::uint32_t c = 0; c < market.stock.size(); ++c) {
            const float delta = (rateAt(archetype.production, c) -
                                 rateAt(archetype.consumption, c)) *
                                static_cast<float>(dt);
            market.stock[c] =
                core::clamp(market.stock[c] + delta, 0.0f, archetype.stockCapacity);
        }
    }

    for (EconomyTrader& trader : m_traders) {
        switch (trader.phase) {
        case TraderPhase::Idle:
            traderThink(galaxy, trader);
            break;
        case TraderPhase::InTransit:
            trader.travelRemaining -= dt;
            if (trader.travelRemaining <= 0.0) {
                // Arrive and sell the haul (whatever the market can absorb;
                // any overflow is jettisoned — cheap and rare).
                (void)sell(trader.market, trader.commodity, trader.cargo);
                trader.cargo = 0.0f;
                trader.phase = TraderPhase::Idle;
            }
            break;
        }
    }
}

void Economy::traderThink(const Galaxy& galaxy, EconomyTrader& trader)
{
    (void)galaxy;
    const StationMarket& here = m_markets[trader.market];
    const std::uint8_t* hopRow =
        m_hops.data() + static_cast<std::size_t>(here.systemIndex) * m_systemCount;

    // Best profit-per-second over every commodity and reachable market.
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
            m_params.traderLegSeconds * 2.0 +
            static_cast<double>(hops) * m_params.jumpSeconds;
        for (std::uint32_t c = 0; c < m_params.commodities.size(); ++c) {
            const float available = std::min(m_params.traderCargo, here.stock[c]);
            if (available <= 1.0f) {
                continue;
            }
            const float profit = (price(m, c) - price(trader.market, c)) * available;
            const float rate = profit / static_cast<float>(travelSeconds);
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
                    price(trader.market, c) > 0.0f ? price(m, c) / price(trader.market, c)
                                                   : 1.0f;
                if (bestMarket == kInvalid || ratio < cheapest) {
                    cheapest = ratio;
                    bestMarket = m;
                }
            }
        }
        if (bestMarket == kInvalid || cheapest >= 1.0f) {
            return; // nowhere better; look again next tick
        }
        const std::uint8_t hops = hopRow[m_markets[bestMarket].systemIndex];
        trader.cargo = 0.0f;
        trader.market = bestMarket;
        trader.phase = TraderPhase::InTransit;
        trader.travelRemaining = m_params.traderLegSeconds * 2.0 +
                                 static_cast<double>(hops) * m_params.jumpSeconds;
        return;
    }

    const TradeResult bought =
        buy(trader.market, bestCommodity, m_params.traderCargo);
    if (bought.units <= 0.0f) {
        return;
    }
    const std::uint8_t hops = hopRow[m_markets[bestMarket].systemIndex];
    trader.commodity = bestCommodity;
    trader.cargo = bought.units;
    trader.market = bestMarket;
    trader.phase = TraderPhase::InTransit;
    trader.travelRemaining = m_params.traderLegSeconds * 2.0 +
                             static_cast<double>(hops) * m_params.jumpSeconds;
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
        writer.write(static_cast<std::uint8_t>(trader.phase));
        writer.write(trader.travelRemaining);
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
        if (!reader.read(trader.market) || !reader.read(phase) ||
            !reader.read(trader.travelRemaining) || !reader.read(trader.commodity) ||
            !reader.read(trader.cargo) || trader.market >= m_markets.size() ||
            phase > static_cast<std::uint8_t>(TraderPhase::InTransit)) {
            return false;
        }
        trader.phase = static_cast<TraderPhase>(phase);
    }
    core::Rng::RawState rngState;
    if (!reader.read(m_accumulator) || !reader.read(rngState.state) ||
        !reader.read(rngState.inc)) {
        return false;
    }
    m_rng.setRawState(rngState);
    return true;
}

} // namespace sol::sim
