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
