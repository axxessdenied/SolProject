#include <sol/sim/economy.hpp>
#include <sol/sim/universe.hpp>

#include <sol/core/serialize.hpp>
#include <sol/test/test.hpp>

#include <cmath>
#include <cstdint>
#include <span>

using sol::sim::Economy;
using sol::sim::EconomyArchetype;
using sol::sim::EconomyCommodity;
using sol::sim::EconomyParams;
using sol::sim::Galaxy;
using sol::sim::GateSpec;
using sol::sim::StationSpec;
using sol::sim::SystemSpec;
using sol::sim::TradeResult;
using sol::sim::TraderPhase;

namespace {

// Two systems, one lane: system 0 hosts a producer of commodity 0 (archetype
// 0), system 1 a consumer (archetype 1). One commodity, one trader.
Galaxy tinyGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 7;
    SystemSpec a;
    a.name = "A";
    a.planets.push_back({.name = "A I", .position = {}, .radius = 1.0e6});
    a.stations.push_back({.name = "A Alpha", .archetype = 0, .position = {}});
    a.gates.push_back({.toSystem = 1, .position = {}});
    SystemSpec b = a;
    b.name = "B";
    b.stations[0] = {.name = "B Alpha", .archetype = 1, .position = {}};
    b.gates[0] = {.toSystem = 0, .position = {}};
    galaxy.systems = {a, b};
    galaxy.links = {{0, 1}};
    return galaxy;
}

EconomyParams tinyParams()
{
    EconomyParams params;
    params.commodities = {EconomyCommodity{.basePrice = 10.0f}};
    EconomyArchetype producer;
    producer.production = {2.0f};
    producer.stockCapacity = 1'000.0f;
    EconomyArchetype consumer;
    consumer.consumption = {2.0f};
    consumer.stockCapacity = 1'000.0f;
    params.archetypes = {producer, consumer};
    params.traderCount = 1;
    params.traderCargo = 50.0f;
    params.traderLegSeconds = 5.0;
    params.jumpSeconds = 5.0;
    return params;
}

} // namespace

SOL_TEST(economy_markets_match_galaxy_and_seed_is_deterministic)
{
    const Galaxy galaxy = tinyGalaxy();
    Economy first;
    first.initialize(galaxy, tinyParams(), 42);
    SOL_REQUIRE(first.markets().size() == 2);
    SOL_CHECK(first.markets()[0].systemIndex == 0);
    SOL_CHECK(first.markets()[1].systemIndex == 1);
    SOL_CHECK(first.marketFor(1, 0) == 1);
    SOL_CHECK(first.marketFor(2, 0) == 0xffff'ffffu);

    Economy second;
    second.initialize(galaxy, tinyParams(), 42);
    for (int i = 0; i < 500; ++i) {
        first.tick(galaxy, 1.0);
        second.tick(galaxy, 1.0);
    }
    SOL_REQUIRE(second.markets().size() == first.markets().size());
    for (std::size_t m = 0; m < first.markets().size(); ++m) {
        SOL_CHECK(first.markets()[m].stock[0] == second.markets()[m].stock[0]);
    }
    SOL_CHECK(first.traders()[0].market == second.traders()[0].market);
}

SOL_TEST(economy_price_falls_as_stock_rises)
{
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 1);
    const float neutral = economy.price(0, 0); // half stock
    (void)economy.sell(0, 0, 400.0f);          // stock 500 -> 900
    const float glutted = economy.price(0, 0);
    (void)economy.buy(0, 0, 800.0f); // 900 -> 100
    const float scarce = economy.price(0, 0);
    SOL_CHECK(glutted < neutral);
    SOL_CHECK(scarce > neutral);
    // Bounds: empty/full stock pin to the scale limits.
    (void)economy.buy(0, 0, 1.0e9f);
    SOL_CHECK(std::abs(economy.price(0, 0) - 20.0f) < 1.0e-3f); // base * maxScale
}

SOL_TEST(economy_player_trades_move_stock_and_clamp)
{
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 1);
    const float before = economy.stock(0, 0);
    const TradeResult bought = economy.buy(0, 0, 100.0f);
    SOL_CHECK(bought.units == 100.0f);
    SOL_CHECK(bought.credits > 0.0f);
    SOL_CHECK(economy.stock(0, 0) == before - 100.0f);
    // Can't buy more than stock; can't sell past capacity.
    const TradeResult drained = economy.buy(0, 0, 1.0e9f);
    SOL_CHECK(economy.stock(0, 0) == 0.0f);
    SOL_CHECK(drained.units == before - 100.0f);
    const TradeResult refilled = economy.sell(0, 0, 1.0e9f);
    SOL_CHECK(refilled.units == 1'000.0f); // capacity
    SOL_CHECK(economy.stock(0, 0) == 1'000.0f);
}

SOL_TEST(economy_traders_haul_from_producer_to_consumer)
{
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 9);

    // Run long enough for several hauls. The consumer burns 2/s and starts
    // with 500: without imports it would be empty after 250 s.
    double consumerLow = 1.0e9;
    for (int i = 0; i < 600; ++i) {
        economy.tick(galaxy, 1.0);
        consumerLow = std::min(consumerLow, static_cast<double>(economy.stock(1, 0)));
    }
    SOL_CHECK(economy.stock(1, 0) > 0.0f); // imports kept the consumer alive
    // The producer's glut was exported: stock stayed below the cap.
    SOL_CHECK(economy.stock(0, 0) < 1'000.0f);
}

SOL_TEST(economy_save_load_round_trips_and_stays_deterministic)
{
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 5);
    for (int i = 0; i < 137; ++i) {
        economy.tick(galaxy, 1.0);
    }
    sol::core::BinaryWriter writer;
    economy.save(writer);

    Economy restored;
    restored.initialize(galaxy, tinyParams(), 999); // wrong seed on purpose
    const std::span<const std::byte> bytes(writer.data());
    sol::core::BinaryReader reader(bytes);
    SOL_REQUIRE(restored.load(reader));

    // Same trajectory after restore as the original continuing.
    for (int i = 0; i < 200; ++i) {
        economy.tick(galaxy, 1.0);
        restored.tick(galaxy, 1.0);
    }
    for (std::size_t m = 0; m < economy.markets().size(); ++m) {
        SOL_CHECK(economy.markets()[m].stock[0] == restored.markets()[m].stock[0]);
    }
    SOL_CHECK(economy.traders()[0].cargo == restored.traders()[0].cargo);
    SOL_CHECK(economy.traders()[0].market == restored.traders()[0].market);

    // A truncated buffer fails cleanly.
    Economy broken;
    broken.initialize(galaxy, tinyParams(), 5);
    sol::core::BinaryReader truncated(bytes.subspan(0, 10));
    SOL_CHECK(!broken.load(truncated));
}

// --- Phase 8g: feedstock gating, extraction, and the spread ---

namespace {

// Same two systems, but now commodity 0 is ore and commodity 1 is metal:
// market 0 mines ore, market 1 refines it. No traders — these tests are
// about one station's books, not about logistics.
EconomyParams chainParams()
{
    EconomyParams params;
    params.commodities = {EconomyCommodity{.basePrice = 10.0f},
                          EconomyCommodity{.basePrice = 30.0f}};
    EconomyArchetype mine;
    mine.production = {1.0f, 0.0f};
    mine.stockCapacity = 1'000.0f;
    EconomyArchetype refinery;
    refinery.production = {0.0f, 1.0f};
    refinery.feedstock = {2.0f, 0.0f};
    refinery.stockCapacity = 1'000.0f;
    params.archetypes = {mine, refinery};
    params.traderCount = 0;
    return params;
}

// Stands in for the asteroid fields: hands out what it has and no more.
struct StubSource final : sol::sim::FeedstockSource
{
    float available = 0.0f;
    float lastAsked = 0.0f;
    int calls = 0;

    float draw(std::uint32_t /*market*/, std::uint32_t /*commodity*/, float units) override
    {
        ++calls;
        lastAsked = units;
        const float got = units < available ? units : available;
        available -= got;
        return got;
    }
};

} // namespace

SOL_TEST(economy_feedstock_gates_production_and_upkeep_does_not)
{
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, chainParams(), 3);

    // Fed: the refinery makes its full rate and eats feedstock to match.
    const float oreBefore = economy.stock(1, 0);
    const float metalBefore = economy.stock(1, 1);
    economy.tick(galaxy, 1.0);
    SOL_CHECK(std::abs(economy.stock(1, 1) - (metalBefore + 1.0f)) < 1.0e-3f);
    SOL_CHECK(std::abs(economy.stock(1, 0) - (oreBefore - 2.0f)) < 1.0e-3f);
    SOL_CHECK(economy.satisfaction(1) == 1.0f);

    // Starved: no ore, so no metal — and the sim says which commodity did it.
    (void)economy.buy(1, 0, 1.0e9f);
    const float metalStarved = economy.stock(1, 1);
    economy.tick(galaxy, 1.0);
    SOL_CHECK(economy.stock(1, 1) == metalStarved);
    SOL_CHECK(economy.satisfaction(1) == 0.0f);
    SOL_CHECK(economy.limitingCommodity(1) == 0);

    // Half fed: half rate, and only half the feedstock is spent.
    (void)economy.sell(1, 0, 1.0f); // one unit against a 2.0/s appetite
    economy.tick(galaxy, 1.0);
    SOL_CHECK(std::abs(economy.stock(1, 1) - (metalStarved + 0.5f)) < 1.0e-3f);
    SOL_CHECK(std::abs(economy.satisfaction(1) - 0.5f) < 1.0e-3f);

    // Upkeep is the other kind of input: the mine has none of the machinery
    // it burns, and goes right on mining.
    EconomyParams params = chainParams();
    params.archetypes[0].consumption = {0.0f, 5.0f};
    Economy hungry;
    hungry.initialize(galaxy, params, 3);
    (void)hungry.buy(0, 1, 1.0e9f); // strip its upkeep stock
    const float oreStart = hungry.stock(0, 0);
    hungry.tick(galaxy, 1.0);
    SOL_CHECK(std::abs(hungry.stock(0, 0) - (oreStart + 1.0f)) < 1.0e-3f);
    SOL_CHECK(hungry.satisfaction(0) == 1.0f);
}

SOL_TEST(economy_shortage_propagates_down_the_chain)
{
    // Cutting the refinery's ore off has to show up in its metal output, not
    // just in its own books: this is the mechanism GDD 6 always claimed.
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, chainParams(), 11);
    for (int i = 0; i < 10; ++i) {
        economy.tick(galaxy, 1.0);
    }
    const float healthy = economy.stock(1, 1);
    economy.tick(galaxy, 1.0);
    const float healthyRate = economy.stock(1, 1) - healthy;
    SOL_CHECK(healthyRate > 0.5f);

    economy.raidSystem(1, 1.0f); // the ore in that system is gone
    const float starved = economy.stock(1, 1);
    economy.tick(galaxy, 1.0);
    SOL_CHECK(economy.stock(1, 1) - starved < healthyRate * 0.5f);
    // And the price of what it can no longer make has risen.
    SOL_CHECK(economy.price(1, 1) > economy.params().commodities[1].basePrice);
}

SOL_TEST(economy_extraction_draws_from_the_source)
{
    const Galaxy galaxy = tinyGalaxy();
    EconomyParams params = chainParams();
    params.archetypes[0].extracts = true;

    // With no source installed an extractor produces freely — which is what
    // keeps this invisible to every test that does not care about it.
    Economy ungated;
    ungated.initialize(galaxy, params, 4);
    const float freeStart = ungated.stock(0, 0);
    ungated.tick(galaxy, 1.0);
    SOL_CHECK(std::abs(ungated.stock(0, 0) - (freeStart + 1.0f)) < 1.0e-3f);

    // With one, output is exactly what the ground gave up.
    Economy economy;
    economy.initialize(galaxy, params, 4);
    StubSource source;
    source.available = 2.5f;
    const float start = economy.stock(0, 0);
    economy.tick(galaxy, 1.0, &source);
    SOL_CHECK(source.calls == 1);
    SOL_CHECK(std::abs(source.lastAsked - 1.0f) < 1.0e-3f);
    SOL_CHECK(std::abs(economy.stock(0, 0) - (start + 1.0f)) < 1.0e-3f);
    economy.tick(galaxy, 1.0, &source);
    economy.tick(galaxy, 1.0, &source); // only 0.5 left for this one
    SOL_CHECK(std::abs(economy.stock(0, 0) - (start + 2.5f)) < 1.0e-3f);
    SOL_CHECK(source.available == 0.0f);

    // Mined out: nothing comes, and the sim reports the station as starved.
    economy.tick(galaxy, 1.0, &source);
    SOL_CHECK(std::abs(economy.stock(0, 0) - (start + 2.5f)) < 1.0e-3f);
    SOL_CHECK(economy.satisfaction(0) == 0.0f);
}

SOL_TEST(economy_spread_costs_a_round_trip)
{
    const Galaxy galaxy = tinyGalaxy();
    EconomyParams params = chainParams();
    params.priceSpread = 0.05f;
    Economy economy;
    economy.initialize(galaxy, params, 6);

    SOL_CHECK(economy.buyPrice(0, 0) > economy.price(0, 0));
    SOL_CHECK(economy.sellPrice(0, 0) < economy.price(0, 0));

    // Buying a block and handing it straight back has to lose money, or
    // nothing distinguishes a good route from a pointless one.
    const TradeResult bought = economy.buy(0, 0, 100.0f);
    const TradeResult sold = economy.sell(0, 0, bought.units);
    SOL_CHECK(sold.units == bought.units);
    SOL_CHECK(sold.credits < bought.credits);

    // With no spread it is exactly break-even, which is the old behaviour.
    params.priceSpread = 0.0f;
    Economy flat;
    flat.initialize(galaxy, params, 6);
    const TradeResult flatBought = flat.buy(0, 0, 100.0f);
    const TradeResult flatSold = flat.sell(0, 0, flatBought.units);
    SOL_CHECK(std::abs(flatSold.credits - flatBought.credits) < 1.0e-2f);
}

SOL_TEST(economy_market_lookup_matches_a_scan)
{
    // marketFor() went from a linear scan to an offset table; it has to agree
    // with the thing it replaced for every station in the galaxy.
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, chainParams(), 2);
    for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
        const auto& market = economy.markets()[m];
        SOL_CHECK(economy.marketFor(market.systemIndex, market.stationIndex) == m);
    }
    SOL_CHECK(economy.marketFor(0, 5) == 0xffff'ffffu);
    SOL_CHECK(economy.marketFor(99, 0) == 0xffff'ffffu);
}
