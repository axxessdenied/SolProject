// Contraband as a good (engine plan Phase 37 stage A): a fourth goods class
// that no legitimate module warehouses, and the black market's own supply
// chain hanging off the shadow modules Phase 34 stage E authored and left
// empty.
//
// ⚑⚑⚑⚑ THE CENSUS IS THE FIRST TEST BECAUSE THE RATES ARE TUNED AGAINST IT.
// Two shipped guards bind every commodity in this game - `shipped_content_
// makes_and_burns_every_commodity_somewhere` and `..._makes_no_commodity_much_
// faster_than_it_is_burnt` - and both read the COMPOSED galaxy, so a good whose
// producer sits on a module that rolls onto three stations and whose consumer
// sits on one that rolls onto eleven is out of band before anybody has typed a
// number. The counts are content, they move whenever a recipe moves, and a rate
// chosen without them is a guess.

#include "space_world.hpp"
#include "station_ui.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::ModuleFamily;

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

// `applyDefs` before `generateUniverse`, the order every galaxy-scoring suite in
// this directory uses: an unowned galaxy has no station bias, and the bias is
// most of what makes the real mix.
[[nodiscard]] bool buildShippedGalaxy(const DefDatabase& defs, game::SpaceWorld& world)
{
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    if (!world.generateUniverse(defs)) {
        std::printf("  generateUniverse refused the shipped defs\n");
        return false;
    }
    return true;
}

} // namespace

// ⚑⚑⚑ THE CENSUS. Not an assertion about a number - the numbers move with the
// content and pinning them here would be a second golden nobody asked for - but
// an assertion that the black market's supply chain STANDS UP: every shadow
// module carrying a rate line has to actually land somewhere, or the good it
// makes is made by nobody and the shipped economy guard fails two files away
// with no clue pointing back here.
SOL_TEST(the_shadow_modules_that_carry_the_black_markets_trade_are_actually_placed)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));
    SOL_REQUIRE(!defs.modules().empty());

    std::vector<std::size_t> placed(defs.modules().size(), 0);
    std::vector<std::size_t> archetypes(defs.stations().size(), 0);
    std::size_t stations = 0;
    std::size_t shadowStations = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const auto& list = world.galaxy().systems[s].stations;
        for (std::uint32_t t = 0; t < list.size(); ++t) {
            ++stations;
            if (list[t].archetype < archetypes.size()) {
                ++archetypes[list[t].archetype];
            }
            bool shadow = false;
            for (const std::uint32_t module : world.stationModules(s, t)) {
                if (module >= defs.modules().size()) {
                    continue;
                }
                ++placed[module];
                shadow = shadow || defs.modules()[module].family == ModuleFamily::Shadow;
            }
            shadowStations += shadow ? 1 : 0;
        }
    }

    std::printf("  %zu stations, %zu with a shadow module\n", stations, shadowStations);
    for (std::size_t a = 0; a < defs.stations().size(); ++a) {
        std::printf("    [arch] %-24s %zu\n", defs.stations()[a].id.c_str(), archetypes[a]);
    }
    for (std::size_t m = 0; m < defs.modules().size(); ++m) {
        if (defs.modules()[m].family != ModuleFamily::Shadow) {
            continue;
        }
        std::printf("    %-24s %zu\n", defs.modules()[m].id.c_str(), placed[m]);
    }

    SOL_CHECK(stations > 0);
    SOL_CHECK(shadowStations > 0);
}

// ⚑⚑⚑⚑ THE STAGE'S EXIT CRITERION, AND IT IS A NEGATIVE ABOUT 119 STATIONS
// RATHER THAN A POSITIVE ABOUT 8. An illicit good has capacity ZERO at every
// dock that did not roll a shadow module - not refused at a counter, not gated
// by standing, simply nowhere to put it. This is decisions/016's promise that
// "stock_capacity becomes per goods class, which is what lets a station be
// unable to store contraband" getting its first reader, and it is the whole
// reason the black market has a monopoly worth having.
//
// ⚑⚑ CHECKED IN BOTH DIRECTIONS, the shape `a_station_names_a_shadow_operator_
// exactly_when_it_has_a_shadow_module` established one phase earlier: "no
// lawful dock can hold it" and "every shadow dock can" are different failures -
// the first would mean a lawful hold leaked an illicit class, the second that
// the fence is a shopfront with no back room - and only writing both down tells
// them apart.
SOL_TEST(an_illicit_good_can_be_warehoused_only_where_a_shadow_module_is)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::vector<std::uint32_t> illicit;
    for (std::uint32_t c = 0; c < defs.commodities().size(); ++c) {
        if (defs.commodities()[c].goodsClass == sol::assets::GoodsClass::Illicit) {
            illicit.push_back(c);
        }
    }
    // ⚑⚑⚑⚑ THE SET IS PINNED BY NAME, AND A MUTATION IS WHY. Written as
    // "collect everything of class Illicit and check it", this test derives its
    // own subject from the thing under test: moving `sol.stims` to `hazardous`
    // shortened the list to one, every remaining assertion passed, and the good
    // became stockable at every dock with a hazmat hold with the whole suite
    // green. That is Phase 36 stage B's finding in a different costume - a
    // window computed from the parameter under test is not a test - and the fix
    // is the same one: say which goods are supposed to be here.
    SOL_REQUIRE(illicit.size() == 2);
    for (const char* id : {"sol.stims", "sol.hot_parts"}) {
        const sol::assets::CommodityDef* good = defs.findCommodity(id);
        SOL_REQUIRE(good != nullptr);
        if (good->goodsClass != sol::assets::GoodsClass::Illicit) {
            std::printf("  %s is %s, not illicit\n", id, sol::assets::goodsClassName(good->goodsClass));
        }
        SOL_CHECK(good->goodsClass == sol::assets::GoodsClass::Illicit);
    }

    std::size_t lawful = 0;
    std::size_t shadow = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const auto& list = world.galaxy().systems[s].stations;
        for (std::uint32_t t = 0; t < list.size(); ++t) {
            bool hasShadowModule = false;
            for (const std::uint32_t module : world.stationModules(s, t)) {
                hasShadowModule = hasShadowModule || (module < defs.modules().size() &&
                                                      defs.modules()[module].family == ModuleFamily::Shadow);
            }
            for (const std::uint32_t c : illicit) {
                const bool stocks = world.stationStocks(s, t, c);
                if (stocks != hasShadowModule) {
                    std::printf("  system %u station %u: %s stocked=%d shadow=%d\n",
                                s,
                                t,
                                defs.commodities()[c].id.c_str(),
                                stocks ? 1 : 0,
                                hasShadowModule ? 1 : 0);
                }
                SOL_CHECK(stocks == hasShadowModule);
            }
            (hasShadowModule ? shadow : lawful) += 1;
        }
    }
    std::printf("  %zu lawful dock(s) can hold none of it, %zu shadow dock(s) can\n", lawful, shadow);
    SOL_CHECK(lawful > 0);
    SOL_CHECK(shadow > 0);
}

// ⚑⚑⚑ THE CLASS IS A WAREHOUSE FACT AND THE LAW IS SOMETHING ELSE, AND THIS IS
// WHAT KEEPS THEM FROM DRIFTING INTO ONE IDEA. decisions/017 is explicit that
// "legality is a property of jurisdictions, never of cargo" - so an illicit
// goods class must NOT be what makes a good contraband, and sol.salvage must
// stay exactly what Phase 33 ruling 10 made it: hazardous, warehoused in the
// open at any breaker yard, and illegal only where the Hegemony's writ runs.
// ⚑ Without this, the obvious next edit is "move salvage to illicit", which
// would silently delete the phase's own best demonstration that jurisdiction is
// real.
SOL_TEST(the_illicit_class_is_a_warehouse_fact_and_not_a_law)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    const sol::assets::CommodityDef* salvage = defs.findCommodity("sol.salvage");
    SOL_REQUIRE(salvage != nullptr);
    SOL_CHECK(salvage->goodsClass == sol::assets::GoodsClass::Hazardous);

    // The Hegemony bans a good it can warehouse; nobody bans the illicit class,
    // because a jurisdiction has an opinion about cargo and not about holds.
    const sol::assets::FactionDef* hegemony = defs.findFaction("sol.hegemony");
    SOL_REQUIRE(hegemony != nullptr);
    bool bansSalvage = false;
    for (const std::string& id : hegemony->contraband) {
        bansSalvage = bansSalvage || id == "sol.salvage";
    }
    SOL_CHECK(bansSalvage);

    // And no faction's table names an illicit good, which is what makes stage
    // C's fence a market rather than a second legality mechanism.
    for (const sol::assets::FactionDef& faction : defs.factions()) {
        for (const std::string& id : faction.contraband) {
            const sol::assets::CommodityDef* good = defs.findCommodity(id.c_str());
            SOL_REQUIRE(good != nullptr);
            SOL_CHECK(good->goodsClass != sol::assets::GoodsClass::Illicit);
        }
    }
}

// ⚑⚑ A RATE LINE WITHOUT A HOLD IS A PRODUCTION LINE WITH NOWHERE TO PUT ITS
// OUTPUT, AND IT FAILS QUIETLY. `every_station_can_hold_what_its_own_modules_
// make_and_burn` says this for stations; this says it one layer up, for the
// four shadow modules specifically, because they roll INDEPENDENTLY and a
// clinic that landed without a fence beside it is a real composition. Stated
// here so the failure names the module rather than a station index.
SOL_TEST(every_shadow_module_that_trades_an_illicit_good_carries_a_hold_for_it)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    std::size_t traders = 0;
    for (const sol::assets::ModuleDef& module : defs.modules()) {
        if (module.family != ModuleFamily::Shadow) {
            continue;
        }
        bool touchesIllicit = false;
        const std::vector<const std::vector<sol::assets::StationRate>*> lists = {
            &module.produces, &module.consumes, &module.feedstock};
        for (const std::vector<sol::assets::StationRate>* list : lists) {
            for (const sol::assets::StationRate& rate : *list) {
                const sol::assets::CommodityDef* good = defs.findCommodity(rate.commodityId.c_str());
                SOL_REQUIRE(good != nullptr);
                touchesIllicit = touchesIllicit || good->goodsClass == sol::assets::GoodsClass::Illicit;
            }
        }
        if (!touchesIllicit) {
            continue;
        }
        ++traders;
        float capacity = 0.0f;
        for (const sol::assets::ModuleStorage& hold : module.stores) {
            if (hold.goods == sol::assets::GoodsClass::Illicit) {
                capacity += hold.capacity;
            }
        }
        if (capacity <= 0.0f) {
            std::printf("  %s trades an illicit good and has no illicit hold\n", module.id.c_str());
        }
        SOL_CHECK(capacity > 0.0f);
    }
    SOL_CHECK(traders > 0);
}

// ⚑⚑⚑⚑ AND THE HOUSE DOES NOT RECITE THE BLACK MARKET'S CATALOGUE. The bar's
// warehouse topic names what a dock has no hold for, and every illicit good is
// unstockable at all 117 lawful docks - so without a filter the barkeep at
// nearly every station in the galaxy would volunteer "no hold here for Combat
// Stims, Stripped Components" to a stranger who had never heard of either.
//
// ⚑⚑ THE FAILURE THIS GUARDS AGAINST IS A LORE LEAK, NOT A CRASH, WHICH IS
// EXACTLY THE KIND THAT SHIPS. Phase 34 stage D wrote that line to answer "why
// did my cargo not appear on the board", and it was correct for nine goods that
// were all ordinary freight. The eleventh and tenth are not, and the line had
// no way to know.
SOL_TEST(a_lawful_dock_never_names_an_illicit_good_in_its_warehouse_line)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::vector<std::string> illicitNames;
    for (const sol::assets::CommodityDef& good : defs.commodities()) {
        if (good.goodsClass == sol::assets::GoodsClass::Illicit) {
            illicitNames.push_back(good.name);
        }
    }
    SOL_REQUIRE(!illicitNames.empty());

    std::vector<game::BarLine> house;
    int checked = 0;
    int warehouseLines = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (game::stationRoom(world, defs, s, t) == nullptr) {
                continue;
            }
            SOL_REQUIRE(world.enterSystem(s));
            SOL_REQUIRE(world.warpToStationOffset(t, {100.0, 0.0, 0.0}));
            SOL_REQUIRE(world.tryDockNearestStation(1000.0));
            house.clear();
            game::composeHouseTalk(world, defs, s, t, house);
            ++checked;
            for (const game::BarLine& line : house) {
                if (line.topic != std::string("The warehouse")) {
                    continue;
                }
                ++warehouseLines;
                for (const std::string& name : illicitNames) {
                    if (line.text.find(name) != std::string::npos) {
                        std::printf("  system %u station %u recites '%s': %s\n",
                                    s,
                                    t,
                                    name.c_str(),
                                    line.text.c_str());
                    }
                    SOL_CHECK(line.text.find(name) == std::string::npos);
                }
            }
        }
    }
    std::printf(
        "  %d dock(s) with a room, %d warehouse line(s), none naming contraband\n", checked, warehouseLines);
    SOL_CHECK(checked > 0);
    SOL_CHECK(warehouseLines > 0);
}
