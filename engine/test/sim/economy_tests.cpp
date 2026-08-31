#include <cmath>
#include <cstdio>
#include <cstdint>
#include <span>

#include <sol/core/serialize.hpp>
#include <sol/sim/economy.hpp>
#include <sol/sim/mining.hpp>
#include <sol/sim/trade_route.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::sim::Economy;
using sol::sim::EconomyArchetype;
using sol::sim::EconomyCommodity;
using sol::sim::EconomyParams;
using sol::sim::EconomyTrader;
using sol::sim::Galaxy;
using sol::sim::GateSpec;
using sol::sim::StationSpec;
using sol::sim::SystemSpec;
using sol::sim::TradeResult;
using sol::sim::TraderLeg;
using sol::sim::TraderPhase;
using sol::sim::TraderRoute;

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

// One system, two stations, no gates: every haul a trader can find here is
// station-to-station, which is the hopless case the leg decomposition has to
// handle without inventing a gate stretch.
Galaxy soloGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 7;
    SystemSpec a;
    a.name = "A";
    a.planets.push_back({.name = "A I", .position = {}, .radius = 1.0e6});
    a.stations.push_back({.name = "A Alpha", .archetype = 0, .position = {}});
    a.stations.push_back({.name = "A Beta", .archetype = 1, .position = {}});
    galaxy.systems = {a};
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

// --- Phase 8x: trader routes ---

SOL_TEST(economy_trader_route_decomposes_a_haul)
{
    // The time model has quoted a haul as traderLegSeconds at each endpoint
    // plus jumpSeconds a gate since Phase 7. This asserts the boundaries land
    // exactly there and nowhere else, because that decomposition is the whole
    // basis on which a coarse trader can be given a position at all.
    const Galaxy galaxy = tinyGalaxy();
    const EconomyParams params = tinyParams(); // 5 s legs, 5 s jumps
    Economy economy;
    economy.initialize(galaxy, params, 9);
    SOL_REQUIRE(economy.traders().size() == 1);

    // The fleet starts scattered along its own clock now (Phase 8x stage 3),
    // so let this one land before watching it leave: everything below is a
    // haul observed from its first second, and the opening leg is already
    // half flown.
    int settle = 0;
    while (economy.traders()[0].phase != TraderPhase::Idle && settle++ < 400) {
        economy.tick(galaxy, 1.0);
    }
    SOL_REQUIRE(economy.traders()[0].phase == TraderPhase::Idle);

    // Parked: no leg, and the trader's origin is where it is standing.
    SOL_CHECK(economy.route(0).leg == TraderLeg::None);
    SOL_CHECK(economy.traders()[0].origin == economy.traders()[0].market);

    int guard = 0;
    while (economy.traders()[0].phase != TraderPhase::InTransit && guard++ < 400) {
        economy.tick(galaxy, 1.0);
    }
    SOL_REQUIRE(economy.traders()[0].phase == TraderPhase::InTransit);

    const TraderRoute start = economy.route(0);
    SOL_CHECK(start.hops == 1); // one station per system, so the gate is forced
    SOL_CHECK(start.fromMarket != start.toMarket);
    SOL_CHECK(economy.traders()[0].legTotal == 15.0); // 5 out + 5 gate + 5 in
    SOL_CHECK(start.leg == TraderLeg::Depart);
    SOL_CHECK(start.progress == 0.0f);
    SOL_CHECK(start.system == economy.markets()[start.fromMarket].systemIndex);

    const std::uint32_t fromSystem = economy.markets()[start.fromMarket].systemIndex;
    const std::uint32_t toSystem = economy.markets()[start.toMarket].systemIndex;
    for (int elapsed = 1; elapsed <= 14; ++elapsed) {
        economy.tick(galaxy, 1.0);
        SOL_REQUIRE(economy.traders()[0].phase == TraderPhase::InTransit);
        const TraderRoute route = economy.route(0);
        SOL_CHECK(route.fromMarket == start.fromMarket);
        SOL_CHECK(route.toMarket == start.toMarket);
        if (elapsed < 5) {
            SOL_CHECK(route.leg == TraderLeg::Depart);
            SOL_CHECK(route.system == fromSystem);
            SOL_CHECK(std::abs(route.progress - static_cast<float>(elapsed) / 5.0f) < 1.0e-5f);
        } else if (elapsed < 10) {
            // In the gate graph, so it is in no system — and that is the
            // answer rather than a gap in one.
            SOL_CHECK(route.leg == TraderLeg::Jump);
            SOL_CHECK(route.system == sol::sim::kNoSystem);
        } else {
            SOL_CHECK(route.leg == TraderLeg::Arrive);
            SOL_CHECK(route.system == toSystem);
            SOL_CHECK(std::abs(route.progress - static_cast<float>(elapsed - 10) / 5.0f) < 1.0e-5f);
        }
    }

    // The fifteenth second lands it, and a landed trader reads as parked.
    economy.tick(galaxy, 1.0);
    SOL_REQUIRE(economy.traders()[0].phase == TraderPhase::Idle);
    SOL_CHECK(economy.route(0).leg == TraderLeg::None);
    SOL_CHECK(economy.route(0).system == toSystem);
    SOL_CHECK(economy.traders()[0].origin == start.toMarket);
    SOL_CHECK(economy.traders()[0].legTotal == 0.0);
}

SOL_TEST(economy_a_hopless_haul_never_leaves_its_system)
{
    // Two stations in one system: the arrive window opens exactly where the
    // depart window closes, so there is no stretch of the trip the trader
    // spends nowhere. A body drawn for this one exists for the whole haul.
    const Galaxy galaxy = soloGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 3);
    SOL_REQUIRE(economy.markets().size() == 2);

    bool sawDepart = false;
    bool sawArrive = false;
    for (int i = 0; i < 400; ++i) {
        economy.tick(galaxy, 1.0);
        const TraderRoute route = economy.route(0);
        if (route.leg == TraderLeg::None) {
            continue;
        }
        SOL_REQUIRE(route.leg != TraderLeg::Jump);
        SOL_CHECK(route.hops == 0);
        SOL_CHECK(economy.traders()[0].legTotal == 10.0); // 5 out + no gate + 5 in
        SOL_CHECK(route.system == 0);
        sawDepart = sawDepart || route.leg == TraderLeg::Depart;
        sawArrive = sawArrive || route.leg == TraderLeg::Arrive;
    }
    SOL_CHECK(sawDepart);
    SOL_CHECK(sawArrive);
}

SOL_TEST(economy_save_load_round_trips_a_route)
{
    // v12 added origin and legTotal, and they are what a body is placed from.
    // A load that dropped them would put a trader back in the galaxy with no
    // idea where it came from — which reads as a working save right up until
    // something tries to draw it.
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 9);
    int guard = 0;
    while (economy.route(0).leg != TraderLeg::Jump && guard++ < 400) {
        economy.tick(galaxy, 1.0);
    }
    SOL_REQUIRE(economy.route(0).leg == TraderLeg::Jump); // mid-haul, between systems

    sol::core::BinaryWriter writer;
    economy.save(writer);
    Economy restored;
    restored.initialize(galaxy, tinyParams(), 999); // wrong seed on purpose
    const std::span<const std::byte> bytes(writer.data());
    sol::core::BinaryReader reader(bytes);
    SOL_REQUIRE(restored.load(reader));

    const TraderRoute before = economy.route(0);
    const TraderRoute after = restored.route(0);
    SOL_CHECK(after.leg == before.leg);
    SOL_CHECK(after.fromMarket == before.fromMarket);
    SOL_CHECK(after.toMarket == before.toMarket);
    SOL_CHECK(after.hops == before.hops);
    SOL_CHECK(after.progress == before.progress);
    SOL_CHECK(restored.traders()[0].origin == economy.traders()[0].origin);
    SOL_CHECK(restored.traders()[0].legTotal == economy.traders()[0].legTotal);

    // And it carries on from where it was rather than restarting the leg.
    for (int i = 0; i < 4; ++i) {
        economy.tick(galaxy, 1.0);
        restored.tick(galaxy, 1.0);
    }
    SOL_CHECK(restored.route(0).leg == economy.route(0).leg);
    SOL_CHECK(restored.route(0).progress == economy.route(0).progress);
}

SOL_TEST(economy_trade_route_picks_the_gate_that_leads_home)
{
    // A hop count says how far the destination is, not which door leads
    // there. The rule is one step of the BFS the hop table was built with:
    // a gate is on a shortest path exactly when its far side is one hop
    // closer than here. Asserted against a hand-written graph, because the
    // way to get this wrong is to pick a gate that happens to exist.
    //
    //   0 -- 1 -- 2 -- 3        and a spur 0 -- 4 that leads nowhere useful
    const auto hops = [](std::uint32_t from, std::uint32_t to) -> std::uint32_t {
        static const std::uint32_t table[5][5] = {
            {0, 1, 2, 3, 1},
            {1, 0, 1, 2, 2},
            {2, 1, 0, 1, 3},
            {3, 2, 1, 0, 4},
            {1, 2, 3, 4, 0},
        };
        return table[from][to];
    };
    const std::vector<GateSpec> fromZero = {{.toSystem = 1, .position = {}}, {.toSystem = 4, .position = {}}};
    // Toward 3, only the gate to 1 shortens the trip; the spur to 4 lengthens
    // it, and a naive "take the first gate" would have taken either.
    SOL_CHECK(sol::sim::gateTowardSystem(std::span<const GateSpec>(fromZero), 0, 3, hops) == 0);
    SOL_CHECK(sol::sim::gateTowardSystem(std::span<const GateSpec>(fromZero), 0, 4, hops) == 1);
    // Standing in the destination, and an unreachable destination, both have
    // no answer rather than a wrong one.
    SOL_CHECK(sol::sim::gateTowardSystem(std::span<const GateSpec>(fromZero), 0, 0, hops) ==
              sol::sim::kNoGate);
    const std::vector<GateSpec> deadEnd = {{.toSystem = 0, .position = {}}};
    SOL_CHECK(sol::sim::gateTowardSystem(std::span<const GateSpec>(deadEnd), 4, 3, hops) == 0);
}

SOL_TEST(economy_a_hopless_leg_folds_into_one_straight_run)
{
    // Two stations in one system is a single run with no gate in it, and the
    // two leg windows are its two halves. Reading the arrive leg's progress
    // raw would snap the trader back to its origin the moment it crossed the
    // midpoint — a teleport in plain sight of anyone watching it cross.
    using sol::sim::hoplessProgress;
    SOL_CHECK(hoplessProgress(TraderLeg::Depart, 0.0f) == 0.0f);
    SOL_CHECK(hoplessProgress(TraderLeg::Depart, 1.0f) == 0.5f);
    SOL_CHECK(hoplessProgress(TraderLeg::Arrive, 0.0f) == 0.5f);
    SOL_CHECK(hoplessProgress(TraderLeg::Arrive, 1.0f) == 1.0f);
    // The seam is continuous: the end of one window and the start of the next
    // are the same point, which is the whole reason for the fold.
    SOL_CHECK(hoplessProgress(TraderLeg::Depart, 1.0f) == hoplessProgress(TraderLeg::Arrive, 0.0f));

    // And the point itself moves monotonically down the line.
    const sol::core::DVec3 from{0.0, 0.0, 0.0};
    const sol::core::DVec3 to{1000.0, 0.0, 0.0};
    SOL_CHECK(sol::sim::legPoint(from, to, 0.0f).x == 0.0);
    SOL_CHECK(sol::sim::legPoint(from, to, 0.5f).x == 500.0);
    SOL_CHECK(sol::sim::legPoint(from, to, 1.0f).x == 1000.0);
}

SOL_TEST(economy_the_schedule_brings_a_trader_in_on_time)
{
    // The two clocks do not agree and cannot: a leg is 90 s however far it
    // is, while a freighter tops out at 3,000 km/s and needs about 260 s for
    // the 600,000 km from a station to a gate. Left to fly freely it is still
    // mid-lane when its books say it arrived. So the record paces the middle
    // and the ship flies the ends, and this pins the shape of that.
    using sol::sim::scheduledLaneDistance;
    constexpr double kLeg = 90.0;
    constexpr double kLength = 6.0e8;    // station -> gate, the shipped figure
    constexpr double kApproach = 6000.0; // steerTravel's envelope distance
    constexpr double kWindow = 35.0;
    const auto at = [&](double remaining) {
        return scheduledLaneDistance(remaining, kLeg, kLength, kApproach, kWindow);
    };

    // It starts a leg away and ends on the destination, exactly.
    SOL_CHECK(std::abs(at(kLeg) - kLength) < 1.0);
    SOL_CHECK(at(0.0) == 0.0);
    // At the moment the ship takes over it is exactly at the approach
    // distance, so the handover is a continuation and not a jump.
    SOL_CHECK(std::abs(at(kWindow) - kApproach) < 1.0);
    // Monotonic: a trader with less time left is always nearer. A schedule
    // that ever backed up would drag a hauler the wrong way down its lane.
    double previous = -1.0;
    for (int i = 0; i <= 90; ++i) {
        const double distance = at(static_cast<double>(i));
        SOL_CHECK(distance >= previous);
        previous = distance;
    }
    // Degenerate legs answer rather than divide: a zero-length leg and a
    // zero-duration one both have to come back with something finite.
    SOL_CHECK(scheduledLaneDistance(10.0, 0.0, kLength, kApproach, kWindow) == 0.0);
    SOL_CHECK(std::isfinite(scheduledLaneDistance(10.0, kLeg, 0.0, kApproach, kWindow)));
    // A leg too short to hold the window still keeps both ends honest.
    SOL_CHECK(std::abs(scheduledLaneDistance(0.0, 10.0, 1000.0, kApproach, kWindow)) < 1.0e-9);
    SOL_CHECK(std::abs(scheduledLaneDistance(10.0, 10.0, 1000.0, kApproach, kWindow) - 1000.0) < 1.0e-9);
}

SOL_TEST(economy_lane_slots_keep_a_convoy_from_sharing_coordinates)
{
    // The failure this exists to stop was measured, not imagined: the coarse
    // fleet converges, so a dozen traders leave one station on one tick for
    // one destination and the record puts them all at the same progress along
    // the same line. Spawned there they share coordinates, the collision
    // resolver depenetrates a zero-distance pair along an arbitrary axis, and
    // the convoy kills itself in the first tick. Most of a 45-ship fleet was
    // lost that way before this existed.
    using sol::sim::laneSlotOffset;
    const sol::core::DVec3 lane{0.0, 0.0, 6.0e8}; // a real leg: station -> gate
    constexpr double kSpacing = 400.0;

    std::vector<sol::core::DVec3> slots;
    for (std::uint32_t i = 0; i < 120; ++i) { // the shipped fleet size
        slots.push_back(laneSlotOffset(i, lane, kSpacing));
    }
    double closest = 1.0e30;
    for (std::size_t a = 0; a < slots.size(); ++a) {
        for (std::size_t b = a + 1; b < slots.size(); ++b) {
            closest = std::min(closest, sol::core::length(slots[a] - slots[b]));
        }
    }
    // Comfortably past any hull: the largest is a 4x freighter, tens of metres.
    SOL_CHECK(closest > 200.0);

    // Every slot is square to the lane, so the fleet is spread across it
    // rather than strung out along it — offsetting along the lane would move
    // traders off the progress the record says they are at.
    for (std::uint32_t i = 0; i < 120; ++i) {
        SOL_CHECK(std::abs(sol::core::dot(slots[i], sol::core::normalize(lane))) < 1.0e-6);
    }
    // Stable: the same trader asks twice and gets the same slot, or a convoy
    // would shuffle every time the set is reconciled.
    SOL_CHECK(laneSlotOffset(7, lane, kSpacing) == slots[7]);
    // A degenerate lane must still answer, not divide by zero.
    const sol::core::DVec3 nowhere = laneSlotOffset(3, {}, kSpacing);
    SOL_CHECK(std::isfinite(nowhere.x) && std::isfinite(nowhere.y) && std::isfinite(nowhere.z));
}

SOL_TEST(economy_a_hauler_flies_a_hull_that_can_carry_its_load)
{
    // Stage 6's whole idea: the sky is readable because the hull fits the
    // cargo. A laden haul is a freighter and a deadhead is a shuttle, so a
    // raider, an escort and the player all read the same thing off a silhouette
    // without being told it.
    using sol::sim::chooseTraderHull;
    const std::vector<float> shipped = {200.0f, 50.0f}; // ships_trader, as data
    const std::span<const float> roster(shipped);

    // The shipped fleet hauls 150 units, which only the freighter can hold.
    SOL_CHECK(chooseTraderHull(roster, 150.0f, 0) == 0);
    // A part load fits the small hull, and so does a deadhead.
    SOL_CHECK(chooseTraderHull(roster, 40.0f, 0) == 1);
    SOL_CHECK(chooseTraderHull(roster, 0.0f, 0) == 1);
    // Exactly full still fits: the rule is "can carry", not "has room to spare".
    SOL_CHECK(chooseTraderHull(roster, 50.0f, 0) == 1);
    // The smallest hull that COVERS it, not the biggest available.
    const std::vector<float> three = {500.0f, 200.0f, 50.0f};
    SOL_CHECK(chooseTraderHull(std::span<const float>(three), 60.0f, 0) == 1);
    // Nothing covers it: a roster is data, and data that cannot carry a shipped
    // cargo must still answer with a ship rather than with nothing.
    SOL_CHECK(chooseTraderHull(roster, 5'000.0f, 0) == 0);
    SOL_CHECK(chooseTraderHull({}, 10.0f, 3) == 0);

    // Equal-capacity hulls are shared out by trader, so a faction listing two
    // of the same size flies both — and one hauler keeps its own answer, which
    // is what stops a body changing ship under the player mid-leg.
    const std::vector<float> twins = {100.0f, 100.0f, 20.0f};
    const std::span<const float> pair(twins);
    SOL_CHECK(chooseTraderHull(pair, 90.0f, 0) == 0);
    SOL_CHECK(chooseTraderHull(pair, 90.0f, 1) == 1);
    SOL_CHECK(chooseTraderHull(pair, 90.0f, 2) == 0);
    SOL_CHECK(chooseTraderHull(pair, 90.0f, 7) == chooseTraderHull(pair, 90.0f, 7));
    // And the tie-break never reaches past what fits: 20 cannot take 90.
    for (std::uint32_t t = 0; t < 16; ++t) {
        SOL_CHECK(chooseTraderHull(pair, 90.0f, t) != 2);
    }
}

SOL_TEST(economy_hop_count_agrees_with_the_leg_it_quoted)
{
    // hopCount is public now because placing a body needs it, and it has to
    // be the same table that priced the trip — otherwise a puppet would fly
    // toward a gate on a route the economy never planned.
    const Galaxy galaxy = tinyGalaxy();
    const EconomyParams params = tinyParams();
    Economy economy;
    economy.initialize(galaxy, params, 9);
    SOL_CHECK(economy.hopCount(0, 0) == 0);
    SOL_CHECK(economy.hopCount(0, 1) == 1);
    SOL_CHECK(economy.hopCount(99, 0) == sol::sim::kUnreachableHops);

    int guard = 0;
    while (economy.traders()[0].phase != TraderPhase::InTransit && guard++ < 400) {
        economy.tick(galaxy, 1.0);
    }
    SOL_REQUIRE(economy.traders()[0].phase == TraderPhase::InTransit);
    const TraderRoute route = economy.route(0);
    const double quoted =
        params.traderLegSeconds * 2.0 + static_cast<double>(route.hops) * params.jumpSeconds;
    SOL_CHECK(economy.traders()[0].legTotal == quoted);
}

// --- Phase 8g: feedstock gating, extraction, and the spread ---

namespace {

// Same two systems, but now commodity 0 is ore and commodity 1 is metal:
// market 0 mines ore, market 1 refines it. No traders — these tests are
// about one station's books, not about logistics.
EconomyParams chainParams()
{
    EconomyParams params;
    params.commodities = {EconomyCommodity{.basePrice = 10.0f}, EconomyCommodity{.basePrice = 30.0f}};
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

namespace {

// The numbers game/data actually ships, mirrored here on purpose: this test
// is the exit criterion for Phase 8g, and an exit criterion that reads its
// own inputs from wherever the game happens to have moved them is not one.
// If stations.toml or commodities.toml change, this changes with them and
// the band below says whether the change was survivable.
constexpr float kFoodPrice = 8.0f;
constexpr float kOrePrice = 12.0f;
constexpr float kMetalPrice = 32.0f;
constexpr float kMachineryPrice = 52.0f;
// Phase 33 stage B's ferrous chain.
constexpr float kFerrousPrice = 14.0f;
constexpr float kAlloyPrice = 38.0f;
constexpr float kHullPlatePrice = 64.0f;

// ⚑⚑ THE ORDER IS `commodities.toml`'S ORDER AND HAS TO STAY THAT WAY. The sim
// knows a commodity only as an index into the table the game built by walking
// that file top to bottom, so a row inserted anywhere but the end renumbers
// every good after it - and this mirror would then be tuning a different
// economy from the one that ships while agreeing with it perfectly by name.
enum Commodity : std::uint32_t
{
    Food = 0,
    Ore,
    Metal,
    Machinery,
    OreFerrous,
    Alloy,
    HullPlate,
    CommodityCount,
};

// Likewise `stations.toml`'s order, for `stationRules` below as much as for
// this: the generator picks an archetype by index into that list.
enum Archetype : std::uint32_t
{
    Agri = 0,
    Mine,
    Refinery,
    Factory,
    FerrousMine,
    Foundry,
    FabWorks,
};

[[nodiscard]] Galaxy shippedGalaxy()
{
    sol::sim::GalaxyParams params;
    params.seed = 1701; // the seed every phase has been verified against
    params.factionCount = 5;
    // ⚑⚑⚑ THE FIRST FOUR WEIGHTS ARE UNTOUCHED BY PHASE 33, AND THAT IS NOT
    // THE SAME THING AS THE OLD MIX SURVIVING. Station COUNT is fixed by the
    // generator at 124 whatever this list says, so three new archetypes take
    // stations away from the old four. Leaving those four weights alone keeps
    // their proportions PER REGION TIER - and the aggregate still moved, because
    // the three new rows sit outward rather than evenly: 22:33:43:26 became
    // 18:17:22:18, measured. Touching one of the old four would re-tune the
    // whole galaxy on top of that, which is why none of them is touched; what
    // holds the result together is
    // `economy_no_commodity_is_made_much_faster_than_it_is_burnt` below.
    //
    // ⚑ `requiresField` is left false here as it always has been, so this galaxy
    // sites extractors over rockless systems where the game's would not (Phase 13
    // set the flag from `produces_from = "field"`). It makes the test HARDER than
    // the game - some mines draw from nothing - which is the safe direction for a
    // stability check, and it is a difference from the shipped galaxy worth
    // knowing about rather than a mirror error to fix under cover of this stage.
    params.stationRules = {
        {{1.0f, 1.5f, 0.5f}},   // agri
        {{0.5f, 1.5f, 2.0f}},   // mine
        {{0.4f, 1.6f, 1.7f}},   // refinery: sited out where the rock is
        {{2.0f, 1.0f, 0.3f}},   // factory
        {{0.3f, 0.9f, 1.2f}},   // ferrous mine: further out still
        {{0.25f, 1.0f, 1.0f}},  // foundry: smelt at the pit
        {{1.2f, 0.6f, 0.2f}},   // fabrication works: near the hardpoints
    };
    return sol::sim::generateGalaxy(params);
}

[[nodiscard]] EconomyParams shippedParams()
{
    EconomyParams params;
    params.commodities.resize(CommodityCount);
    params.commodities[Food].basePrice = kFoodPrice;
    params.commodities[Ore].basePrice = kOrePrice;
    params.commodities[Metal].basePrice = kMetalPrice;
    params.commodities[Machinery].basePrice = kMachineryPrice;
    params.commodities[OreFerrous].basePrice = kFerrousPrice;
    params.commodities[Alloy].basePrice = kAlloyPrice;
    params.commodities[HullPlate].basePrice = kHullPlatePrice;

    const auto archetype = [](std::initializer_list<float> production,
                              std::initializer_list<float> consumption,
                              std::initializer_list<float> feedstock,
                              bool extracts) {
        EconomyArchetype out;
        out.production = production;
        out.consumption = consumption;
        out.feedstock = feedstock;
        // Sized to the commodity table so callers can index any of them; the
        // sim itself reads short vectors as zero, but the test's reporting
        // subscripts them directly.
        out.production.resize(CommodityCount, 0.0f);
        out.consumption.resize(CommodityCount, 0.0f);
        out.feedstock.resize(CommodityCount, 0.0f);
        out.extracts = extracts;
        out.stockCapacity = 2'500.0f;
        return out;
    };
    params.archetypes.resize(7);
    // Indices, in the order of the enum above:
    //                            food   ore  metal  mach  ferr alloy plate
    constexpr float P = 0.013f; // the hull-plate upkeep heavy industry burns
    params.archetypes[Agri] = archetype({0.26f}, {0, 0, 0, 0.11f}, {}, false);
    params.archetypes[Mine] = archetype({0, 0.28f}, {0.05f, 0, 0, 0, 0, 0, P}, {}, true);
    params.archetypes[Refinery] =
        archetype({0, 0, 0.11f}, {0.035f, 0, 0, 0, 0, 0, P}, {0, 0.17f}, false);
    params.archetypes[Factory] =
        archetype({0, 0, 0, 0.12f}, {0.05f, 0, 0, 0, 0, 0, P}, {0, 0, 0.15f}, false);
    params.archetypes[FerrousMine] =
        archetype({0, 0, 0, 0, 0.20f}, {0.015f, 0, 0, 0, 0, 0, P}, {}, true);
    params.archetypes[Foundry] =
        archetype({0, 0, 0, 0, 0, 0.085f}, {0.015f, 0, 0, 0, 0, 0, P}, {0, 0, 0, 0, 0.13f}, false);
    params.archetypes[FabWorks] =
        archetype({0, 0, 0, 0, 0, 0, 0.12f}, {0.015f}, {0, 0, 0, 0, 0, 0.15f}, false);
    return params;
}

// Mines draw from the rock in their own system, exactly as the game wires it.
struct FieldSource final : sol::sim::FeedstockSource
{
    sol::sim::MiningSim* mining = nullptr;
    const Galaxy* galaxy = nullptr;
    const Economy* economy = nullptr;

    float draw(std::uint32_t market, std::uint32_t commodity, float units) override
    {
        const std::uint32_t system = economy->markets()[market].systemIndex;
        return mining->drawFromSystem(*galaxy, system, commodity, units);
    }
};

} // namespace

// ⚑⚑⚑ PHASE 33 STAGE A MEASURED THIS TEST, BECAUSE THE ARC HAD A STANDING RISK
// SAYING THE MATERIAL TREE WOULD MAKE IT UNSHIPPABLE. IT WILL NOT, AND THE
// NUMBERS ARE WORTH KEEPING BESIDE THE TEST THEY DESCRIBE.
//
// The risk read: the economy is dense in commodity count, `m_tickPrices` and
// `m_inbound` are `[market * commodityCount + commodity]`, the agent loop is
// `O(markets x commodities)`, and GDD 6's tree is ten times the four commodities
// that ship - so Phase 33 must choose between a sparse market and a shorter
// horizon. Every structural claim there is true. Measured on this galaxy (80
// systems, 124 markets, 120 traders), over exactly the four sim hours below,
// padded with inert commodities so only array width and loop bounds move:
//
//    4 commodities  43.2 s      40 commodities  47.9 s
//    4, no source    0.61 s     40, no source    4.99 s
//
// ⚑⚑ TEN TIMES THE TABLE COSTS ELEVEN PERCENT, because 98.6% of this test is
// the `FeedstockSource` below - `MiningSim::drawFromSystem` walking every rock
// in a mine's own system every tick, plus the depletion walk aging what has
// been cut. Both are `O(rock records)` and neither has ever heard of the
// commodity table. The commodity-proportional work IS linear as predicted
// (0.61 -> 4.99 s is 8.2x for a 10x widening); it is linear on a 0.6-second
// base. ⚑ *If this test's runtime ever needs to come down, the lever is the
// mining draw and not the width of anything.*
//
// ⚑⚑⚑ AND THE MEASUREMENT FOUND A HOLE IN THE ASSERTIONS BELOW THAT MATTERS
// MORE THAN THE RUNTIME DID: THIS TEST CANNOT TELL A MATERIAL TREE FROM
// SCENERY. A commodity nobody produces and nobody consumes is stocked at half
// capacity by `initialize` and stays there forever, so its `fill` is 0.500 -
// dead centre of the 0.15/0.85 band checked below - while `starved` and
// `glutted` are both zero BECAUSE `wants` and `makes` are zero. Thirty-six
// inert goods would pass this test as comfortably as four working ones. Any
// commodity Phase 33 stage B adds has to reach an assertion that knows it is
// supposed to be MOVING, or the tree lands green and decorative.
SOL_TEST(economy_shipped_rates_hold_a_steady_state)
{
    // The Phase 8g exit criterion, as arithmetic rather than an impression:
    // left alone for hours, nothing may be pinned empty or pinned full
    // galaxy-wide and the total quantity of goods must hold steady. The 8c
    // finding ("the hour-old economy runs ore and food dry near the core")
    // was exactly this test failing, before there was one to fail.
    //
    // What it turns out to be sensitive to, in rough order of how much it
    // hurt to learn: warehouse size against how often a hauler calls (a
    // buffer smaller than the gap between visits starves the station and
    // throttles its neighbours), fleet size from *above* as well as below
    // (surplus haulers hoard rather than idle), and production run a little
    // ahead of consumption so a glut cannot ratchet the whole chain down.
    const Galaxy galaxy = shippedGalaxy();
    const EconomyParams params = shippedParams();
    Economy economy;
    economy.initialize(galaxy, params, 1701);

    sol::sim::MiningParams miningParams;
    miningParams.ores = {sol::sim::OreEntry{.commodity = Ore, .weight = {1.0f, 1.0f, 1.0f}},
                         sol::sim::OreEntry{.commodity = OreFerrous, .weight = {0.35f, 1.0f, 1.6f}}};
    sol::sim::MiningSim mining;
    mining.initialize(galaxy, miningParams, CommodityCount, 1701);

    FieldSource source;
    source.mining = &mining;
    source.galaxy = &galaxy;
    source.economy = &economy;

    // Total goods held anywhere — in markets and in transit. If the economy
    // is stable this is flat; the failure mode this whole test exists to
    // catch is a slow ratchet downward, which no snapshot of one moment
    // would ever show.
    const auto goodsInTheGalaxy = [&]() {
        double total = 0.0;
        for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
            for (std::uint32_t c = 0; c < CommodityCount; ++c) {
                total += economy.stock(m, c);
            }
        }
        for (const EconomyTrader& trader : economy.traders()) {
            total += trader.cargo;
        }
        return total;
    };

    double afterFirstHour = 0.0;
    // Four hours, not one: every arrangement of these numbers looks healthy
    // for the first hour, and the ones that are actually broken only show it
    // over the next several. Four is where every failure met during tuning
    // had become unmistakable, and it keeps this (a debug-build sim of a
    // whole galaxy) from dominating the suite's runtime.
    for (int second = 0; second < 14'400; ++second) {
        economy.tick(galaxy, 1.0, &source);
        mining.tick(1.0);
        if (second == 3'600) {
            afterFirstHour = goodsInTheGalaxy();
        }
    }
    const double afterFourHours = goodsInTheGalaxy();

    // Per-commodity fill across every market in the galaxy. "Pinned" is
    // counted only where it means something: a station starved of an input it
    // actually needs, or a producer with nowhere left to put its output. A
    // Fabricator Yard holding no ore is not a symptom — it is a station with
    // no interest in ore, and counting it would drown the real signal.
    for (std::uint32_t c = 0; c < CommodityCount; ++c) {
        double stock = 0.0;
        double capacity = 0.0;
        std::uint32_t starved = 0;
        std::uint32_t wants = 0;
        std::uint32_t glutted = 0;
        std::uint32_t makes = 0;
        for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
            const float units = economy.stock(m, c);
            const float cap = economy.capacityOf(m);
            stock += units;
            capacity += cap;
            const EconomyArchetype& archetype = params.archetypes[economy.markets()[m].archetype];
            const bool needs = archetype.feedstock[c] > 0.0f || archetype.consumption[c] > 0.0f;
            const bool produces = archetype.production[c] > 0.0f;
            wants += needs ? 1u : 0u;
            makes += produces ? 1u : 0u;
            starved += needs && units <= cap * 0.01f ? 1u : 0u;
            glutted += produces && units >= cap * 0.99f ? 1u : 0u;
        }
        const double fill = capacity > 0.0 ? stock / capacity : 0.0;
        // ⚑⚑ THE ROW IS PRINTED WHENEVER ONE OF THE FOUR BOUNDS BREAKS, BECAUSE
        // A BARE `fill > 0.15` TELLS YOU NOTHING ABOUT WHICH GOOD OR WHY. Seven
        // commodities across four checks is twenty-eight ways for this test to
        // say "the economy is broken" and no way to say where, which is the
        // difference between a failure you can act on and one you re-run.
        const bool ok = fill > 0.15 && fill < 0.85 && starved <= wants / 4 && glutted <= makes / 4;
        if (!ok) {
            std::printf("  commodity %u: fill %.3f, starved %u/%u, glutted %u/%u\n",
                        c,
                        fill,
                        starved,
                        wants,
                        glutted,
                        makes);
        }
        SOL_CHECK(fill > 0.15);
        SOL_CHECK(fill < 0.85);
        // Local extremes are the economy working; most of the galaxy at a
        // stop is the economy broken.
        SOL_CHECK(starved <= wants / 4);
        SOL_CHECK(glutted <= makes / 4);
    }

    // No slow leak. Everything the traders are carrying counts as still in
    // the galaxy, so this only moves when goods are made or consumed — and
    // a healthy economy makes about as much as it burns.
    SOL_CHECK(afterFourHours > afterFirstHour * 0.9);
    SOL_CHECK(afterFourHours < afterFirstHour * 1.1);

    // The chain has to have actually run: metal and machinery exist only if
    // ore reached a refinery and metal reached a factory.
    double metal = 0.0;
    double machinery = 0.0;
    for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
        metal += economy.stock(m, Metal);
        machinery += economy.stock(m, Machinery);
    }
    SOL_CHECK(metal > 0.0);
    SOL_CHECK(machinery > 0.0);

    // ⚑⚑⚑ AND THE FERROUS CHAIN THE SAME WAY, BUT PROVED RATHER THAN
    // ASSERTED - BECAUSE PHASE 33 STAGE A SHOWED "IT EXISTS" IS NOT EVIDENCE.
    // Every market is stocked at half capacity by `initialize`, so a good that
    // is never made and never moved still reads a comfortable 1250 units four
    // hours later. `metal > 0` above has that weakness and is kept only for
    // continuity; the two checks below do not have it.
    //
    // A Fabrication Works produces no alloy and burns 0.15/s of it, which over
    // 14,400 seconds is 2,160 units against a 1,250-unit start. So a Works with
    // ANY alloy left has been delivered some - and alloy exists only where a
    // Foundry smelted ferrous ore, which exists only where a Ferrous Mine drew
    // it out of real rock. One non-zero number, the whole chain behind it.
    float alloyAtWorks = 0.0f;
    double worksSatisfaction = 0.0;
    double foundrySatisfaction = 0.0;
    std::uint32_t works = 0;
    std::uint32_t foundries = 0;
    for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
        const std::uint32_t which = economy.markets()[m].archetype;
        if (which == FabWorks) {
            alloyAtWorks += economy.stock(m, Alloy);
            worksSatisfaction += economy.satisfaction(m);
            ++works;
        } else if (which == Foundry) {
            foundrySatisfaction += economy.satisfaction(m);
            ++foundries;
        }
    }
    SOL_REQUIRE(works > 0);
    SOL_REQUIRE(foundries > 0);
    SOL_CHECK(alloyAtWorks > 0.0f);

    // ⚑⚑ THE SECOND HALF, AND IT IS THE ONE THAT WOULD CATCH A CHAIN THAT
    // TRICKLES. Satisfaction IS the production ratio the sim throttled the
    // station to, so a Works averaging near zero is a Works standing idle beside
    // a full warehouse of nothing. Half is a low bar deliberately - this is not
    // a tuning target, it is the line between "the chain runs" and "the chain is
    // a diagram" - and the same is asked of the Foundries one step upstream, so
    // a failure says WHICH link went.
    SOL_CHECK(worksSatisfaction / works > 0.5);
    SOL_CHECK(foundrySatisfaction / foundries > 0.5);
    if (worksSatisfaction / works <= 0.5 || foundrySatisfaction / foundries <= 0.5) {
        std::printf("  ferrous chain: %u foundries at %.3f, %u works at %.3f, alloy at works %.1f\n",
                    foundries,
                    foundrySatisfaction / foundries,
                    works,
                    worksSatisfaction / works,
                    static_cast<double>(alloyAtWorks));
    }

    // And the rock is still there: an hour of NPC mining must not strip the
    // galaxy, which is what regrowth is for.
    float ground = 0.0f;
    for (std::uint32_t s = 0; s < galaxy.systems.size(); ++s) {
        ground += mining.systemStock(galaxy, s, Ore);
    }
    SOL_CHECK(ground > 0.0f);
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

SOL_TEST(economy_a_lost_haul_returns_the_trader_without_shrinking_the_fleet)
{
    // Phase 8x §B. The cargo is gone and the hauler is back where it started;
    // what must NOT happen is the fleet getting smaller, because traderCount
    // is the number 8g tuned the whole galaxy against from both directions.
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 5);
    const std::size_t fleet = economy.traders().size();
    SOL_REQUIRE(fleet == 1);

    // Run until it is actually hauling something, which is the only state a
    // loss means anything in.
    for (int second = 0; second < 200 && economy.traders()[0].cargo <= 0.0f; ++second) {
        economy.tick(galaxy, 1.0);
    }
    SOL_REQUIRE(economy.traders()[0].phase == TraderPhase::InTransit);
    SOL_REQUIRE(economy.traders()[0].cargo > 0.0f);
    const std::uint32_t origin = economy.traders()[0].origin;
    const std::uint32_t destination = economy.traders()[0].market;
    SOL_REQUIRE(origin != destination);

    SOL_CHECK(economy.loseTrader(0));
    const EconomyTrader& trader = economy.traders()[0];
    SOL_CHECK(economy.traders().size() == fleet);
    SOL_CHECK(trader.phase == TraderPhase::Idle);
    SOL_CHECK(trader.market == origin); // it never arrived
    SOL_CHECK(trader.origin == origin);
    SOL_CHECK(trader.cargo == 0.0f);
    SOL_CHECK(trader.legTotal == 0.0);
    SOL_CHECK(trader.travelRemaining == 0.0);
    // A parked trader has no haul to lose, so a second call changes nothing —
    // which is what keeps one cargo from being destroyed twice when the body
    // and the record both report the same death.
    SOL_CHECK(!economy.loseTrader(0));
    SOL_CHECK(!economy.loseTrader(99));
    // And it reads as parked rather than as a haul with a stopped clock.
    SOL_CHECK(economy.route(0).leg == TraderLeg::None);
}

SOL_TEST(economy_a_detained_trader_holds_its_clock_and_then_carries_on)
{
    // Phase 8x §D. A hauler being shot at is not flying its leg. ⚑ Without
    // this the record wins every fight: travelRemaining counts down whatever
    // is happening in the bubble, so a hauler under fire arrives on schedule,
    // starts its next leg, and its body is rebuilt somewhere else in the
    // system - which a drive watched happen twice, at 2 km, with the raider
    // already shooting.
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 5);
    SOL_REQUIRE(economy.traders().size() == 1);
    for (int second = 0; second < 200 && economy.traders()[0].phase != TraderPhase::InTransit; ++second) {
        economy.tick(galaxy, 1.0);
    }
    SOL_REQUIRE(economy.traders()[0].phase == TraderPhase::InTransit);

    // Held: the clock does not move, however long the fight lasts.
    const double held = economy.traders()[0].travelRemaining;
    for (int second = 0; second < 5; ++second) {
        economy.clearDetained();
        economy.detainTrader(0);
        SOL_CHECK(economy.detained(0));
        economy.tick(galaxy, 1.0);
    }
    SOL_CHECK(economy.traders()[0].travelRemaining == held);
    SOL_CHECK(economy.traders()[0].phase == TraderPhase::InTransit);

    // Released, and the haul carries on from exactly where it stopped rather
    // than catching up - a fight costs the delivery the time it took.
    economy.clearDetained();
    SOL_CHECK(!economy.detained(0));
    economy.tick(galaxy, 1.0);
    SOL_CHECK(economy.traders()[0].travelRemaining < held);
    SOL_CHECK(economy.traders()[0].travelRemaining >= held - 1.5);

    // Out-of-range indices are ignored rather than resizing anything, so the
    // bubble handing over a stale trader index cannot detain trader zero.
    economy.detainTrader(99);
    SOL_CHECK(!economy.detained(99));
    SOL_CHECK(!economy.detained(0));
}

SOL_TEST(economy_a_lost_haul_destroys_exactly_its_own_cargo)
{
    // The goods leave the galaxy once. Everything a trader carries counts as
    // still in the galaxy (the steady-state test's own accounting), so this is
    // the only way a loss is allowed to move that total.
    const Galaxy galaxy = tinyGalaxy();
    Economy economy;
    economy.initialize(galaxy, tinyParams(), 9);
    const auto goods = [&]() {
        double total = 0.0;
        for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
            total += static_cast<double>(economy.stock(m, 0));
        }
        for (const EconomyTrader& trader : economy.traders()) {
            total += static_cast<double>(trader.cargo);
        }
        return total;
    };
    for (int second = 0; second < 200 && economy.traders()[0].cargo <= 0.0f; ++second) {
        economy.tick(galaxy, 1.0);
    }
    const double before = goods();
    const double hold = static_cast<double>(economy.traders()[0].cargo);
    SOL_REQUIRE(hold > 0.0);
    SOL_CHECK(economy.loseTrader(0));
    SOL_CHECK(std::abs((before - hold) - goods()) < 1.0e-4);
}

SOL_TEST(economy_initialize_scatters_the_fleet_instead_of_starting_it_in_step)
{
    // Phase 8x. A fleet that all starts Idle all thinks on the same tick, so
    // it departs, jumps and arrives as one body: every system is either empty
    // or holds forty-five haulers, on a cycle the whole galaxy shares. Stage 2
    // measured that as a nearest-body track reading 42599 / 77552 / 641276 km
    // and then no bodies at all for minutes.
    const Galaxy galaxy = shippedGalaxy();
    const EconomyParams params = shippedParams();
    Economy economy;
    economy.initialize(galaxy, params, 1701);
    SOL_REQUIRE(economy.traders().size() == params.traderCount);

    std::uint32_t byLeg[4] = {0, 0, 0, 0};
    double cargo = 0.0;
    for (std::uint32_t t = 0; t < economy.traders().size(); ++t) {
        byLeg[static_cast<std::size_t>(economy.route(t).leg)] += 1;
        cargo += static_cast<double>(economy.traders()[t].cargo);
    }
    // Every leg of a haul is represented on the very first tick, which is the
    // whole point: somebody is already arriving somewhere.
    SOL_CHECK(byLeg[static_cast<std::size_t>(TraderLeg::Depart)] > 0);
    SOL_CHECK(byLeg[static_cast<std::size_t>(TraderLeg::Jump)] > 0);
    SOL_CHECK(byLeg[static_cast<std::size_t>(TraderLeg::Arrive)] > 0);
    // And the fleet is not bunched: the old behaviour put all of it in Idle.
    SOL_CHECK(byLeg[static_cast<std::size_t>(TraderLeg::None)] < economy.traders().size() / 2);
    // ⚑ It also creates no goods. The scattered legs are deadheads, so the
    // galaxy's books open holding exactly what they always did — without which
    // this would be a quiet change to the equilibrium 8g tuned.
    SOL_CHECK(cargo == 0.0);
}

// ⚑⚑⚑⚑ THE GUARD PHASE 33 STAGE A's MEASUREMENT ASKED FOR, AND IT COSTS
// NOTHING BECAUSE IT RUNS NO SIM AT ALL.
//
// Stage A padded this economy out to forty commodities to price the material
// tree and found something worse than a slow test: `economy_shipped_rates_hold_
// a_steady_state` CANNOT TELL A MATERIAL TREE FROM SCENERY. `Economy::initialize`
// stocks every market with half a warehouse of every good that exists, so a
// commodity nobody produces and nobody consumes sits at exactly 0.500 fill
// forever - dead centre of that test's own 0.15/0.85 band - while its `starved`
// and `glutted` counts are zero BECAUSE its `wants` and `makes` are zero. Thirty
// six inert goods would have passed the economy's exit criterion as comfortably
// as four working ones, and the only thing that would ever have noticed is a
// player wondering why nothing is ever short of anything.
//
// ⚑⚑ SO THE RULE IS STATED HERE INSTEAD OF BEING LEFT TO A FOUR-HOUR SIM: a
// good in this game is MADE by somebody and BURNT by somebody. Feedstock and
// upkeep both count as burning it - they differ in whether they gate production
// (see `EconomyArchetype`), not in whether the units go away. Anything that
// cannot answer both halves is decoration, and Phase 33 stage B is the phase
// where this file learns to grow.
//
// ⚑ It asserts against `shippedParams()`, which is this file's hand-kept mirror
// of `commodities.toml` and `stations.toml` - so it catches a good added to the
// game with nowhere to go only once somebody mirrors it here, which is the same
// contract every other assertion in this file has had since 8g.
SOL_TEST(economy_every_commodity_is_made_and_burnt_somewhere)
{
    const EconomyParams params = shippedParams();
    SOL_REQUIRE(params.commodities.size() == CommodityCount);

    for (std::uint32_t c = 0; c < CommodityCount; ++c) {
        bool made = false;
        bool burnt = false;
        for (const EconomyArchetype& archetype : params.archetypes) {
            made = made || (c < archetype.production.size() && archetype.production[c] > 0.0f);
            burnt = burnt || (c < archetype.feedstock.size() && archetype.feedstock[c] > 0.0f) ||
                    (c < archetype.consumption.size() && archetype.consumption[c] > 0.0f);
        }
        if (!made || !burnt) {
            std::printf("  commodity %u is %s and %s\n",
                        c,
                        made ? "made" : "MADE BY NOBODY",
                        burnt ? "burnt" : "BURNT BY NOBODY");
        }
        SOL_CHECK(made);
        SOL_CHECK(burnt);
    }
}

// ⚑⚑⚑⚑ AND THE SECOND HALF OF THAT RULE, WHICH IS THE ONE STAGE B ACTUALLY
// NEEDED: MADE AND BURNT IS NOT ENOUGH - IT HAS TO BE MADE AT ABOUT THE RATE IT
// IS BURNT. A commodity produced half again as fast as it is consumed does not
// fail the four-hour test; it drifts, and four hours is not long enough to see
// it. Phase 33 stage B measured its own first cut over NINETY-SIX sim hours and
// watched structural alloy climb from 0.494 to 0.645 fill while every assertion
// in the test above stayed comfortably green.
//
// ⚑⚑⚑ IT RUNS NO ECONOMY AT ALL - IT COUNTS STATIONS AND MULTIPLIES. The
// galaxy generator is deterministic at seed 1701, so how many of each archetype
// exist is a fact available in milliseconds, and the aggregate flow of a good is
// just (stations x rate) summed. That is the whole diagnosis the ninety-six-hour
// run produced, for none of its ten minutes.
//
// ⚑⚑ THE BAND IS WIDE AND ASYMMETRIC ON PURPOSE. `stations.toml` runs every
// producer deliberately ahead of its customers - "that slack is not waste, it is
// stability" - so a ratio comfortably above 1 is the design and not a defect;
// what the ceiling catches is a surplus so large that warehouses fill faster
// than the output taper can absorb. Below 1 is survivable too, because feedstock
// gating throttles the consumer rather than letting it drain to nothing - that
// is what `satisfaction` is for - but not by much, and refined metal at 0.90 is
// the tightest thing this galaxy ships.
//
// ⚑ Station COUNTS, not weights: a weight is an intention and a count is what
// the generator actually did with it. Phase 33 stage B assumed the four existing
// archetypes would keep their proportions when three more were added, because
// their weights were untouched - and they did not, because the new rows are not
// spread evenly across the three region tiers. 22:33:43:26 became 18:17:22:18.
SOL_TEST(economy_no_commodity_is_made_much_faster_than_it_is_burnt)
{
    const Galaxy galaxy = shippedGalaxy();
    const EconomyParams params = shippedParams();
    std::vector<std::uint32_t> stations(params.archetypes.size(), 0);
    for (const SystemSpec& system : galaxy.systems) {
        for (const StationSpec& station : system.stations) {
            SOL_REQUIRE(station.archetype < stations.size());
            ++stations[station.archetype];
        }
    }

    for (std::uint32_t c = 0; c < CommodityCount; ++c) {
        double made = 0.0;
        double burnt = 0.0;
        for (std::size_t a = 0; a < params.archetypes.size(); ++a) {
            const EconomyArchetype& archetype = params.archetypes[a];
            const double count = stations[a];
            made += count * archetype.production[c];
            burnt += count * (archetype.feedstock[c] + archetype.consumption[c]);
        }
        SOL_REQUIRE(burnt > 0.0); // the test above already said somebody burns it
        const double ratio = made / burnt;
        if (ratio < 0.85 || ratio > 1.6) {
            std::printf("  commodity %u: %.3f/s made, %.3f/s burnt, ratio %.2f\n", c, made, burnt, ratio);
        }
        SOL_CHECK(ratio > 0.85);
        SOL_CHECK(ratio < 1.6);
    }
}

SOL_TEST(economy_holds_a_steady_state_while_losing_traders)
{
    // ⚑ The guard rail Phase 8x's spec names for the whole item. 8g tuned the
    // fleet at 120 against a galaxy where a haul never failed, and found that
    // BOTH directions break the economy. Attrition changes trader lifetimes,
    // so it changes that equilibrium — and this is where that has to be found,
    // rather than in a play session three phases later.
    //
    // It is deliberately far harsher than the shipped rate. Attrition rolls
    // traderLossPerSecond (0.002) against a system's danger, and only in the
    // handful of systems being raided or contested at any moment. This kills a
    // hauler every twenty seconds, galaxy-wide, for four hours — around 27,000
    // units of cargo destroyed per hour. If the galaxy holds through that, the
    // shipped rate has a very wide margin.
    const Galaxy galaxy = shippedGalaxy();
    const EconomyParams params = shippedParams();
    Economy economy;
    economy.initialize(galaxy, params, 1701);

    sol::sim::MiningParams miningParams;
    miningParams.ores = {sol::sim::OreEntry{.commodity = Ore, .weight = {1.0f, 1.0f, 1.0f}},
                         sol::sim::OreEntry{.commodity = OreFerrous, .weight = {0.35f, 1.0f, 1.6f}}};
    sol::sim::MiningSim mining;
    mining.initialize(galaxy, miningParams, CommodityCount, 1701);

    FieldSource source;
    source.mining = &mining;
    source.galaxy = &galaxy;
    source.economy = &economy;

    const auto goodsInTheGalaxy = [&]() {
        double total = 0.0;
        for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
            for (std::uint32_t c = 0; c < CommodityCount; ++c) {
                total += economy.stock(m, c);
            }
        }
        for (const EconomyTrader& trader : economy.traders()) {
            total += trader.cargo;
        }
        return total;
    };

    double afterFirstHour = 0.0;
    std::uint32_t losses = 0;
    std::uint32_t victim = 0;
    const auto fleetSize = static_cast<std::uint32_t>(economy.traders().size());
    for (int second = 0; second < 14'400; ++second) {
        economy.tick(galaxy, 1.0, &source);
        mining.tick(1.0);
        if (second % 20 == 0) {
            // Walk the fleet rather than always picking one trader, so losses
            // are spread over the galaxy the way real danger spreads them.
            for (std::uint32_t attempt = 0; attempt < fleetSize; ++attempt) {
                victim = (victim + 1) % fleetSize;
                if (economy.loseTrader(victim)) {
                    ++losses;
                    break;
                }
            }
        }
        if (second == 3'600) {
            afterFirstHour = goodsInTheGalaxy();
        }
    }
    const double afterFourHours = goodsInTheGalaxy();

    // The instrument has to have actually fired, or this passes by testing
    // nothing — which is the failure 8n's BODIES=0 taught to check for.
    SOL_CHECK(losses > 600);
    // The fleet is exactly the size it started. This is the invariant
    // attrition must never break, whatever the rate.
    SOL_CHECK(economy.traders().size() == params.traderCount);

    for (std::uint32_t c = 0; c < CommodityCount; ++c) {
        double stock = 0.0;
        double capacity = 0.0;
        std::uint32_t starved = 0;
        std::uint32_t wants = 0;
        std::uint32_t glutted = 0;
        std::uint32_t makes = 0;
        for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
            const float units = economy.stock(m, c);
            const float cap = economy.capacityOf(m);
            stock += units;
            capacity += cap;
            const EconomyArchetype& archetype = params.archetypes[economy.markets()[m].archetype];
            const bool needs = archetype.feedstock[c] > 0.0f || archetype.consumption[c] > 0.0f;
            const bool produces = archetype.production[c] > 0.0f;
            wants += needs ? 1u : 0u;
            makes += produces ? 1u : 0u;
            starved += needs && units <= cap * 0.01f ? 1u : 0u;
            glutted += produces && units >= cap * 0.99f ? 1u : 0u;
        }
        const double fill = capacity > 0.0 ? stock / capacity : 0.0;
        SOL_CHECK(fill > 0.15);
        SOL_CHECK(fill < 0.85);
        SOL_CHECK(starved <= wants / 4);
        SOL_CHECK(glutted <= makes / 4);
    }

    // Goods destroyed by attrition are goods leaving the galaxy, so this may
    // sag where the untouched economy's total may not — but it must not
    // ratchet, which is the failure the whole four-hour run exists to catch.
    SOL_CHECK(afterFourHours > afterFirstHour * 0.9);
    SOL_CHECK(afterFourHours < afterFirstHour * 1.1);
}
