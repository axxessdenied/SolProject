// What a dead ship turns out to have been carrying (engine plan Phase 33
// stage C).
//
// ⚑⚑⚑ THE ROLL DID NOT BREAK — THE TABLE GREW UNDERNEATH IT. Both loot
// composers picked `rng.range(commodityCount)`, a uniform draw over every good
// in the game, and at four goods that reads as "a hauler's cargo" and nobody
// ever looked at it twice. The material tree is what turns it into a defect:
// over gdd.md §6's forty goods the same line puts a station module kit in an
// interceptor's hold one time in forty, and the only reader who would ever have
// noticed is a player who thought it was funny.
//
// ⚑⚑ THE TWO ASSERTIONS BELOW ARE DELIBERATELY DIFFERENT IN KIND. The scrap
// re-point is a fact about ONE roll and is checked as one. The tier bound is a
// statement about a DISTRIBUTION, so it is checked over enough seeds that a
// weighting which merely made module kits rare would fail it — "rare" and
// "impossible" are the whole difference between the fix and the appearance of
// one, and a single seed cannot tell them apart.

#include "space_world.hpp"

#include <cstring>
#include <string>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;

namespace {

// One good per tier, so a roll can be attributed to a tier by its index alone,
// plus the two hulls the bound is drawn between. The masses are the shipped
// interceptor's and freighter's: this test is about `ships.toml`'s real spread
// and not about a threshold picked to make it pass.
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.salvage"
name = "Salvage"
tier = "raw"
base_price = 11.0

[[commodity]]
id = "sol.food"
name = "Foodstuffs"
tier = "consumer"
base_price = 8.0

[[commodity]]
id = "sol.alloy"
name = "Structural Alloy"
tier = "refined"
base_price = 38.0

[[commodity]]
id = "sol.hull_plate"
name = "Hull Plate"
tier = "component"
base_price = 64.0

[[commodity]]
id = "sol.module_kit"
name = "Station Module Kit"
tier = "assembly"
base_price = 900.0

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0
mass = 10000.0

[[ship]]
id = "sol.interceptor"
name = "Interceptor"
model = "ship"
max_speed = 260.0
mass = 8000.0

[[ship]]
id = "sol.freighter"
name = "Freighter"
model = "ship"
max_speed = 160.0
mass = 40000.0

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5
produces = ["sol.food:0.26"]
stock_capacity = 2500
)";

enum Commodity : std::uint32_t
{
    Salvage = 0,
    Food,
    Alloy,
    HullPlate,
    ModuleKit,
};

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    Fixture()
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        world.spawn(1701);
        // ⚑ `generateUniverse` builds the commodity TABLE but does not hand the
        // world its def database - `applyDefs` is what does that, and the loot
        // roll reads a commodity's tier off the defs rather than off the table.
        // Without it the roll takes its own nullptr fallback and answers the
        // uniform draw this stage exists to replace, which is a test that
        // asserts the old behaviour while looking like it asserts the new one.
        //
        // ⚑⚑ AND IT GOES FIRST, WHICH IS THE ORDER THE GAME BOOTS IN
        // (`content.cpp`) AND IS LOAD-BEARING RATHER THAN TIDY: this call after
        // `generateUniverse` still fixes the loot roll, but `initializeFactions`
        // has already run and found no defs, so the faction table is empty and
        // every system in the galaxy is unowned. Stage D found that the hard
        // way one test file over.
        world.applyDefs(defs);
        SOL_CHECK(world.generateUniverse(defs));
    }
};

} // namespace

// ⚑ The line's own comment always read "Scrap: the hull itself, as ore". It was
// right about what it was doing and wrong about what to call it, because until
// this stage there was no T0 good that came off a ship rather than out of a
// rock — so the first stack out of every wreck in the galaxy was raw ore, and a
// mining outpost and a battlefield paid out the same commodity.
SOL_TEST(a_wrecks_scrap_is_salvage_and_not_ore)
{
    Fixture fixture;
    const sol::assets::ShipDef* interceptor = fixture.defs.findShip("sol.interceptor");
    SOL_REQUIRE(interceptor != nullptr);

    const std::uint32_t salvage = fixture.world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage == Salvage);

    // The hull scrap is the FIRST stack, always present and never rolled for,
    // which is what makes it checkable seed by seed.
    for (std::uint64_t seed = 1; seed <= 32; ++seed) {
        const sol::sim::SignalLoot loot = fixture.world.defaultWreckLoot(interceptor, seed);
        SOL_REQUIRE(!loot.cargo.empty());
        SOL_CHECK(loot.cargo[0].commodity == Salvage);
        SOL_CHECK(loot.cargo[0].units > 0.0f);
    }
}

// ⚑⚑ THE ONE HARD BOUND. A weighting alone would have made this "rare", and
// rare is not the fix — a fighter has nowhere to put a module kit at all. Six
// hundred seeds is far more than the roll needs to produce one at any plausible
// weight; the freighter half of the test is what proves the bound is a bound
// and not an empty branch.
SOL_TEST(a_fighter_wreck_never_yields_a_station_module_kit_and_a_freighter_can)
{
    Fixture fixture;
    const sol::assets::ShipDef* interceptor = fixture.defs.findShip("sol.interceptor");
    const sol::assets::ShipDef* freighter = fixture.defs.findShip("sol.freighter");
    SOL_REQUIRE(interceptor != nullptr && freighter != nullptr);

    std::uint32_t fighterKits = 0;
    std::uint32_t freighterKits = 0;
    std::uint32_t fighterHauls = 0;
    for (std::uint64_t seed = 1; seed <= 600; ++seed) {
        const sol::sim::SignalLoot light = fixture.world.defaultWreckLoot(interceptor, seed);
        for (std::size_t i = 1; i < light.cargo.size(); ++i) {
            ++fighterHauls;
            fighterKits += light.cargo[i].commodity == ModuleKit ? 1u : 0u;
        }
        const sol::sim::SignalLoot heavy = fixture.world.defaultWreckLoot(freighter, seed);
        for (std::size_t i = 1; i < heavy.cargo.size(); ++i) {
            freighterKits += heavy.cargo[i].commodity == ModuleKit ? 1u : 0u;
        }
    }
    if (fighterKits != 0 || freighterKits == 0) {
        std::printf("  kits: %u off an 8 t hull in %u hauls, %u off a 40 t hull\n",
                    fighterKits,
                    fighterHauls,
                    freighterKits);
    }
    SOL_REQUIRE(fighterHauls > 100); // the roll fires about half the time
    SOL_CHECK(fighterKits == 0);
    SOL_CHECK(freighterKits > 0);
}

// ⚑⚑ AND THE WEIGHTING, WHICH IS THE HALF THAT IS ABOUT WHAT FREIGHT IS RATHER
// THAN ABOUT WHAT A HULL CAN HOLD. Bulk goods move constantly and in quantity;
// components move rarely. This is not a tuning target — the bar is only that
// the distribution is no longer flat, because flat is exactly what it was and
// what nobody could see.
SOL_TEST(a_wrecks_haul_is_bulk_goods_far_more_often_than_components)
{
    Fixture fixture;
    const sol::assets::ShipDef* freighter = fixture.defs.findShip("sol.freighter");
    SOL_REQUIRE(freighter != nullptr);

    std::uint32_t bulk = 0;    // raw + consumer
    std::uint32_t refined = 0; // refined + component
    for (std::uint64_t seed = 1; seed <= 600; ++seed) {
        const sol::sim::SignalLoot loot = fixture.world.defaultWreckLoot(freighter, seed);
        for (std::size_t i = 1; i < loot.cargo.size(); ++i) {
            const std::uint32_t c = loot.cargo[i].commodity;
            bulk += (c == Salvage || c == Food) ? 1u : 0u;
            refined += (c == Alloy || c == HullPlate) ? 1u : 0u;
        }
    }
    // Weights 4 + 3 against 2 + 1, so a little over twice as often. Asserted as
    // "more" rather than as the ratio: the numbers are a design choice and this
    // test is a statement that the roll reads the tier at all.
    if (bulk <= refined) {
        std::printf("  haul mix: %u bulk, %u refined-or-component\n", bulk, refined);
    }
    SOL_REQUIRE(bulk + refined > 100);
    SOL_CHECK(bulk > refined);
}
