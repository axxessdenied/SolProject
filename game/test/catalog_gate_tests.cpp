// The material tree's last hop (engine plan Phase 33 stage B): a station will
// not sell a fitting it has none of the material for.
//
// ⚑⚑⚑ THIS IS THE ONLY PLACE THE ECONOMY AND THE OUTFITTING SCREEN MEET, AND
// UNTIL THIS STAGE THEY NEVER DID. gdd.md §6 puts T2 components in the material
// tree because "the gun you buy was manufactured somewhere by somebody out of
// things somebody mined" — and a chain that runs from a rock to a warehouse and
// stops there is a chain no player ever meets. Before `CatalogGate::requires`, a
// fitting was for sale at every station the player's standing let them dock at,
// whatever the galaxy had actually built.
//
// ⚑⚑ THE TEST DRAINS A REAL MARKET RATHER THAN SETTING A FLAG. The station
// archetype below burns hull plate as upkeep out of a ten-unit warehouse, so
// ticking the world empties it the way a shortage would — through
// `Economy::produce`, at the market `dockedMarket()` names. Anything less than
// that would be asserting that the branch is reachable rather than that the
// mechanism works.

#include "space_world.hpp"

#include <cstring>
#include <string>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;

namespace {

// ⚑ Two components, identical but for the one key, because "the gated one is
// absent" is only evidence if something ungated is present beside it. A screen
// that had gone empty for an unrelated reason would pass half this test.
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
tier = "consumer"
base_price = 8.0

[[commodity]]
id = "sol.hull_plate"
name = "Hull Plate"
tier = "component"
base_price = 64.0

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0

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
consumes = ["sol.hull_plate:5.0"]
stock_capacity = 10

[[component]]
id = "sol.armor_plating"
name = "Armor Plating"
mount = "utility"
size = "small"
price = 1200.0
requires = "sol.hull_plate"

[[component]]
id = "sol.cargo_pod"
name = "Cargo Pod"
mount = "utility"
size = "small"
price = 400.0
)";

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    Fixture()
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        SOL_CHECK(defs.validateCatalogGates(&error));
        world.spawn(1701);
        SOL_CHECK(world.generateUniverse(defs));
    }

    [[nodiscard]] bool dockAtFirstStation()
    {
        for (std::size_t i = 0; i < world.navTargets().size(); ++i) {
            if (world.navTargetKind(i) != game::SpaceWorld::NavKind::Station) {
                continue;
            }
            if (!world.warpToStationOffset(world.navTargetStation(i), {100.0, 0.0, 0.0})) {
                return false;
            }
            return world.tryDockNearestStation(1000.0);
        }
        return false;
    }
};

} // namespace

SOL_TEST(a_station_with_no_hull_plate_does_not_sell_hull_plating)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.dockAtFirstStation());
    SOL_REQUIRE(fixture.world.isDocked());

    const sol::assets::ComponentDef* gated = fixture.defs.findComponent("sol.armor_plating");
    const sol::assets::ComponentDef* ungated = fixture.defs.findComponent("sol.cargo_pod");
    SOL_REQUIRE(gated != nullptr && ungated != nullptr);
    SOL_REQUIRE(gated->gate.requiresCommodity == "sol.hull_plate");
    SOL_REQUIRE(ungated->gate.requiresCommodity.empty());

    const std::uint32_t plate = fixture.world.commodityIndex("sol.hull_plate");
    SOL_REQUIRE(plate < fixture.defs.commodities().size());
    const std::uint32_t market = fixture.world.dockedMarket();

    // Every market opens at half its capacity of everything, so the fitting is
    // on the shelf before a single unit has been burnt.
    SOL_REQUIRE(fixture.world.economy().stock(market, plate) > 0.0f);
    SOL_CHECK(fixture.world.stationSells(gated->gate));
    SOL_CHECK(fixture.world.stationSells(ungated->gate));

    // Five units at five a second, through the same `consumes` path a real
    // shortage runs through. Ten seconds of world time is eight times what it
    // needs and still costs a fraction of a second here.
    for (int step = 0; step < 600; ++step) {
        fixture.world.tick(1.0 / 60.0);
    }
    SOL_REQUIRE(fixture.world.isDocked()); // ticking must not have undocked us
    SOL_CHECK(fixture.world.economy().stock(market, plate) == 0.0f);

    // ⚑ The whole claim in two lines: one fitting left the catalogue because
    // the galaxy stopped making what it is made of, and the one beside it did
    // not move.
    SOL_CHECK(!fixture.world.stationSells(gated->gate));
    SOL_CHECK(fixture.world.stationSells(ungated->gate));
}

SOL_TEST(a_requirement_is_asked_of_the_station_you_are_standing_in)
{
    // ⚑⚑ NOT DOCKED IS NOT "NO REQUIREMENT" — it is "no station to ask", and
    // the answer has to be no. `stationSells` already refused everything while
    // flying, and this pins that the commodity half did not quietly invent a
    // path around it: a gate that answered `true` in space would put the
    // requirement's whole meaning at the mercy of whoever called it first.
    Fixture fixture;
    SOL_REQUIRE(!fixture.world.isDocked());

    const sol::assets::ComponentDef* gated = fixture.defs.findComponent("sol.armor_plating");
    SOL_REQUIRE(gated != nullptr);
    SOL_CHECK(!fixture.world.stationSells(gated->gate));
    SOL_CHECK(!fixture.world.stationStocksRequirement(gated->gate));

    // And an empty requirement is answerable anywhere, because it asks nothing.
    sol::assets::CatalogGate open;
    SOL_CHECK(fixture.world.stationStocksRequirement(open));
}
