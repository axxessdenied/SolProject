// T3, the construction tier, against the content that actually ships (engine
// plan Phase 33 stage E).
//
// ⚑⚑⚑ THE TWO CLAIMS HERE ARE THE TWO ENDS OF WHAT A TIER IS FOR, AND BOTH
// WERE WRITTEN BEFORE THE TIER EXISTED. Stage C put the hard bound in
// `haulWeight` - a module kit is freight for a hull built to carry freight, so
// no weighting makes it merely rare on a fighter, it has to be impossible - and
// then proved it against a SYNTHETIC commodity table, because no assembly good
// existed to prove it against. Its own comment says so: "Nothing reaches this
// today because no T3 good exists until stage E - it is the guard being in place
// BEFORE the tier it guards." This file is the other half of that sentence, and
// it is deliberately run against `game/data` rather than a fixture: a bound that
// holds for a fixture's five goods and not for the shipped nine would be a bound
// that guards nothing anybody flies through.
//
// ⚑⚑ THE SECOND CLAIM IS THE PLAYER-FACING ONE. `CatalogGate::requires` has sat
// on the SHARED gate since stage B, whose own comment reads "a ship, a weapon or
// a crew member could take one; nothing does". The Freighter takes one now, so
// the Shipyard tab is shorter at a station that has no hull sections - which is
// gdd.md §6's claim about T2 ("the gun you buy was manufactured somewhere by
// somebody") made about a 40-tonne hull, where it is most obviously true.

#include "space_world.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;

namespace {

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;
    bool ready = false;

    Fixture()
    {
        if (!loadShippedDefs(defs)) {
            return;
        }
        world.spawn(game::kDefaultUniverseSeed);
        // ⚑ Before `generateUniverse`, which is the order `content.cpp` boots
        // in. The loot roll reads a commodity's tier off the def database and
        // takes a uniform fallback when there is none, so the reversed order
        // gives a test that asserts the OLD behaviour while looking like it
        // asserts the new one. Stage C found that; stage D found the worse
        // version of it one file over.
        world.applyDefs(defs);
        ready = world.generateUniverse(defs);
    }
};

} // namespace

// The tier exists in shipped content, is spelled `assembly`, and is the only
// good in the game that is one. ⚑ Asserted by TIER rather than by id: the claim
// is about what the loot bound below is bounding, and an id would still pass the
// day somebody re-tiered the row underneath it.
SOL_TEST(the_shipped_tree_has_exactly_one_assembly_tier_good)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    std::size_t assemblies = 0;
    for (const sol::assets::CommodityDef& commodity : defs.commodities()) {
        if (commodity.hasTier && commodity.tier == sol::assets::CommodityTier::Assembly) {
            ++assemblies;
            SOL_CHECK(commodity.id == "sol.hull_section");
        }
    }
    if (assemblies != 1) {
        std::printf("  %zu assembly-tier good(s) in game/data\n", assemblies);
    }
    SOL_CHECK(assemblies == 1);
}

// ⚑⚑⚑ STAGE C's BOUND, NOW FIRING ON THE REAL TABLE. Six hundred seeds is far
// more than the roll needs to produce a section at its weight; the freighter
// half is what proves the bound is a bound and not an empty branch, and the
// interceptor half is what proves it is impossible rather than rare.
SOL_TEST(a_shipped_fighter_wreck_never_carries_a_hull_section_and_a_freighter_can)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.ready);

    const sol::assets::ShipDef* interceptor = fixture.defs.findShip("sol.interceptor");
    const sol::assets::ShipDef* freighter = fixture.defs.findShip("sol.freighter");
    SOL_REQUIRE(interceptor != nullptr && freighter != nullptr);
    // The bound is 25 t and these are the hulls it is drawn between. Stated
    // here so that re-massing a hull fails this test rather than silently
    // changing what wrecks pay out.
    SOL_REQUIRE(interceptor->mass < 25'000.0f);
    SOL_REQUIRE(freighter->mass >= 25'000.0f);

    const std::uint32_t section = fixture.world.commodityIndex("sol.hull_section");
    SOL_REQUIRE(section < fixture.defs.commodities().size());

    std::uint32_t fighterSections = 0;
    std::uint32_t freighterSections = 0;
    std::uint32_t fighterHauls = 0;
    for (std::uint64_t seed = 1; seed <= 600; ++seed) {
        const sol::sim::SignalLoot light = fixture.world.defaultWreckLoot(interceptor, seed);
        for (std::size_t i = 1; i < light.cargo.size(); ++i) { // [0] is always the hull scrap
            ++fighterHauls;
            fighterSections += light.cargo[i].commodity == section ? 1u : 0u;
        }
        const sol::sim::SignalLoot heavy = fixture.world.defaultWreckLoot(freighter, seed);
        for (std::size_t i = 1; i < heavy.cargo.size(); ++i) {
            freighterSections += heavy.cargo[i].commodity == section ? 1u : 0u;
        }
    }
    if (fighterSections != 0 || freighterSections == 0) {
        std::printf("  hull sections: %u off an %.0f t hull in %u hauls, %u off a %.0f t hull\n",
                    fighterSections,
                    interceptor->mass / 1000.0f,
                    fighterHauls,
                    freighterSections,
                    freighter->mass / 1000.0f);
    }
    SOL_REQUIRE(fighterHauls > 100); // the roll fires about half the time
    SOL_CHECK(fighterSections == 0);
    SOL_CHECK(freighterSections > 0);
}

// The gate is wired in shipped content, and only on the hull that should carry
// it. ⚑ The negative half matters more than the positive one: a sweep that put
// the requirement on every hull would strand a player with nothing to fly at a
// station that had run out, which is the restraint stage B showed at T2 and the
// reason `sol.armor_plating` is still the only gated fitting.
SOL_TEST(only_the_shipped_heavy_hull_is_made_of_hull_sections)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    std::string error;
    // A requirement naming a commodity no `[[commodity]]` row defines is a load
    // refusal, so this also says the section actually exists.
    SOL_CHECK(defs.validateCatalogGates(&error));

    const sol::assets::ShipDef* freighter = defs.findShip("sol.freighter");
    const sol::assets::ShipDef* shuttle = defs.findShip("sol.shuttle");
    const sol::assets::ShipDef* interceptor = defs.findShip("sol.interceptor");
    SOL_REQUIRE(freighter != nullptr && shuttle != nullptr && interceptor != nullptr);

    SOL_CHECK(freighter->gate.requiresCommodity == "sol.hull_section");
    SOL_CHECK(shuttle->gate.requiresCommodity.empty());
    SOL_CHECK(interceptor->gate.requiresCommodity.empty());
}

// ⚑⚑ AND THE MECHANISM, END TO END: dock, confirm the forecourt carries the
// gated hull because a market opens holding some of everything, run the station
// out of hull sections through `Economy::produce`, and watch that hull leave the
// catalogue while the one beside it stays. `stationSells` is what the Shipyard
// tab is built from (`station_ui.cpp:194`) and what `buyShip` asks at the
// counter, so this is the screen and the till agreeing.
//
// ⚑ A SYNTHETIC ARCHETYPE, LIKE `catalog_gate_tests`, AND FOR THE SAME REASON
// TURNED ROUND. That file drains a market by burning upkeep out of a ten-unit
// warehouse; a shipped Shipyard eats 0.0175 sections a second out of 1250, so
// running one dry takes twenty sim hours and would buy nothing this does not.
// The three tests above are the ones that have to be about shipped content -
// what the tier IS, what wrecks drop, and which hull is gated. This one is
// about the code path, and the code path does not know whose defs it has.
namespace {

constexpr const char* const kGateDefs = R"(
[[commodity]]
id = "sol.hull_section"
name = "Hull Section"
tier = "assembly"
base_price = 200.0

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[station]]
id = "sol.station_yard"
name = "Shipyard"
weight_core = 1.0
weight_frontier = 1.0
weight_fringe = 1.0
consumes = ["sol.hull_section:5.0"]
stock_capacity = 10

[[ship]]
id = "sol.freighter"
name = "Freighter"
model = "ship"
max_speed = 160.0
mass = 40000.0
price = 60000.0
requires = "sol.hull_section"

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0
mass = 10000.0
price = 8000.0
)";

} // namespace

SOL_TEST(a_dock_with_no_hull_sections_stops_selling_the_hull_made_of_them)
{
    DefDatabase defs;
    game::SpaceWorld world;
    std::string error;
    SOL_REQUIRE(defs.mergeToml(kGateDefs, std::strlen(kGateDefs), "test_defs.toml", &error));
    SOL_REQUIRE(defs.validateCatalogGates(&error));
    world.spawn(1701);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    bool docked = false;
    for (std::size_t i = 0; i < world.navTargets().size() && !docked; ++i) {
        if (world.navTargetKind(i) != game::SpaceWorld::NavKind::Station) {
            continue;
        }
        if (!world.warpToStationOffset(world.navTargetStation(i), {100.0, 0.0, 0.0})) {
            continue;
        }
        docked = world.tryDockNearestStation(1000.0);
    }
    SOL_REQUIRE(docked);

    const sol::assets::ShipDef* gated = defs.findShip("sol.freighter");
    const sol::assets::ShipDef* ungated = defs.findShip("sol.shuttle");
    SOL_REQUIRE(gated != nullptr && ungated != nullptr);
    SOL_REQUIRE(gated->gate.requiresCommodity == "sol.hull_section");
    SOL_REQUIRE(ungated->gate.requiresCommodity.empty());

    const std::uint32_t section = world.commodityIndex("sol.hull_section");
    SOL_REQUIRE(section < defs.commodities().size());
    const std::uint32_t market = world.dockedMarket();

    // Every market opens at half capacity of everything
    // (`Economy::initialize`), so the hull is on the forecourt before the tree
    // has moved a unit. That is Phase 34 stage D's to overturn, not this
    // stage's.
    SOL_REQUIRE(world.economy().stock(market, section) > 0.0f);
    SOL_CHECK(world.stationSells(gated->gate));
    SOL_CHECK(world.stationSells(ungated->gate));

    // Five units at five a second, through the same `consumes` path a real
    // shortage runs through.
    for (int step = 0; step < 600; ++step) {
        world.tick(1.0 / 60.0);
    }
    SOL_REQUIRE(world.isDocked()); // ticking must not have undocked us
    SOL_REQUIRE(world.economy().stock(market, section) == 0.0f);

    // ⚑ The claim in two lines: one hull left the forecourt because the galaxy
    // has none of what it is welded out of, and the one beside it did not move.
    SOL_CHECK(!world.stationSells(gated->gate));
    SOL_CHECK(world.stationSells(ungated->gate));
}
