// Storage by goods class (engine plan Phase 34 stage D): a station warehouses
// only what its holds admit, per commodity, and a good it has no hold for is
// not stocked, not delivered, not produced and not sold there.
//
// ⚑⚑⚑⚑ THE TEST THIS FILE EXISTS FOR IS THE SECOND ONE, AND IT IS THE ONE THAT
// WOULD HAVE CAUGHT EVERY DATA PROBLEM THIS STAGE HAD: *a station can hold
// everything its own modules touch.* Turning one capacity scalar into a vector
// is arithmetic; the danger is that a station ends up running a production line
// for a good it has nowhere to put, which is silent - `produce` simply taints to
// zero, the market sits empty, and the count-sheet guard reads it as a shortage
// somewhere else. Measured against the authored data before this stage, that
// invariant failed for 108 stations of 125 on Foodstuffs alone, because
// Foodstuffs is the only `cryo` good and only the Agricultural Station had a
// cryogenic hold. The fix was in the DATA - the larder rides with the habitat,
// the debris bunker with the breaker line and the reclaimer - and this test is
// what makes that a rule rather than a thing somebody noticed once.

#include "space_world.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::CommodityDef;
using sol::assets::DefDatabase;
using sol::assets::GoodsClass;
using sol::assets::ModuleDef;

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

template <typename Fn>
void forEachStation(const game::SpaceWorld& world, Fn&& fn)
{
    const sol::sim::Galaxy& galaxy = world.galaxy();
    for (std::uint32_t s = 0; s < galaxy.systems.size(); ++s) {
        for (std::uint32_t t = 0; t < galaxy.systems[s].stations.size(); ++t) {
            const std::uint32_t market = world.economy().marketFor(s, t);
            fn(s, t, market);
        }
    }
}

// Every commodity a module has a rate line for, in either direction.
void goodsTouchedBy(const ModuleDef& module, std::vector<std::string>& out)
{
    for (const auto* list : {&module.produces, &module.consumes, &module.feedstock}) {
        for (const sol::assets::StationRate& rate : *list) {
            if (std::find(out.begin(), out.end(), rate.commodityId) == out.end()) {
                out.push_back(rate.commodityId);
            }
        }
    }
}

} // namespace

// ⚑⚑⚑ TEST 1: THE CAPACITY A STATION HAS IS THE SUM OF ITS HOLDS, CHECKED
// AGAINST THE DEFS RATHER THAN AGAINST ITSELF. `m_commodityClass` and each
// module's resolved `storage` vector are caches built at `generateUniverse`;
// this recomputes both from the def database, so a stale cache, a goods class
// read off the wrong commodity, or a hold summed into the wrong column is a
// failure here.
SOL_TEST(a_stations_capacity_is_the_sum_of_the_holds_its_modules_carry)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const auto commodityCount = static_cast<std::uint32_t>(world.commodityIds().size());
    // 9 -> 11 (Phase 37 stage A: Combat Stims and Stripped Components). A count
    // pinned so the test notices the file moving under it, not an invariant.
    SOL_REQUIRE(commodityCount == 11);

    // The class of each commodity, straight from the file.
    std::vector<GoodsClass> classOf(commodityCount, GoodsClass::Bulk);
    for (std::uint32_t c = 0; c < commodityCount; ++c) {
        const CommodityDef* def = defs.findCommodity(world.commodityIds()[c].c_str());
        SOL_REQUIRE(def != nullptr);
        classOf[c] = def->goodsClass;
        SOL_CHECK(world.commodityClass(c) == def->goodsClass);
    }

    forEachStation(world, [&](std::uint32_t s, std::uint32_t t, std::uint32_t market) {
        for (std::uint32_t c = 0; c < commodityCount; ++c) {
            float expected = 0.0f;
            for (const std::uint32_t index : world.stationModules(s, t)) {
                SOL_REQUIRE(index < defs.modules().size());
                for (const sol::assets::ModuleStorage& hold : defs.modules()[index].stores) {
                    if (hold.goods == classOf[c]) {
                        expected += hold.capacity;
                    }
                }
            }
            SOL_CHECK(world.economy().capacityOf(market, c) == expected);
            // The accessor the trade fill asks, and the same fact.
            SOL_CHECK((expected > 0.0f) == (world.economy().capacityOf(market, c) > 0.0f));
        }
    });
}

// ⚑⚑⚑⚑ TEST 2: A STATION CAN HOLD EVERYTHING ITS OWN MODULES TOUCH. This is the
// rule the data was re-authored to satisfy and the reason three module rows
// gained a `stores` line they did not have: a production line with nowhere to
// put its output does not fail loudly, it tapers to zero and reads as a shortage
// at the other end of the chain.
//
// ⚑ It is checked per STATION rather than per archetype, because that is where
// it can actually break - a recipe rolls its modules independently, so an
// archetype whose hold sits at chance 0.5 satisfies "the archetype has a hold"
// and still leaves half the stations of that kind without one.
SOL_TEST(every_station_can_hold_what_its_own_modules_make_and_burn)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::vector<std::string> touched;
    int offenders = 0;
    forEachStation(world, [&](std::uint32_t s, std::uint32_t t, std::uint32_t market) {
        touched.clear();
        for (const std::uint32_t index : world.stationModules(s, t)) {
            goodsTouchedBy(defs.modules()[index], touched);
        }
        for (const std::string& id : touched) {
            const std::uint32_t c = world.commodityIndex(id.c_str());
            if (c >= world.commodityIds().size()) {
                continue; // an unknown commodity is the composer's warning, not this test's
            }
            if (world.economy().capacityOf(market, c) > 0.0f) {
                continue;
            }
            if (offenders < 8) {
                std::printf("  '%s' runs a line for '%s' and has no hold for it\n",
                            world.galaxy().systems[s].stations[t].name.c_str(),
                            id.c_str());
            }
            ++offenders;
        }
    });
    if (offenders > 0) {
        std::printf("  %d station/good pair(s) with a line and no hold\n", offenders);
    }
    SOL_CHECK(offenders == 0);
}

// ⚑⚑⚑ TEST 3: FOOD IS EXACTLY THE STATIONS WITH PEOPLE, IN BOTH DIRECTIONS. The
// larder rides with the habitat, and habitat modules are what carry the food
// upkeep line (stage B's ruling), so the two are the same set by construction.
// Stating it as a test is what stops a later edit putting a cryogenic hold on
// something with nobody in it, or - far worse - moving the food line off the
// habitat and leaving 125 stations eating out of a warehouse they do not have.
SOL_TEST(a_station_holds_exactly_the_food_it_has_people_to_eat)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::uint32_t food = world.commodityIndex("sol.food");
    SOL_REQUIRE(food < world.commodityIds().size());
    SOL_CHECK(world.commodityClass(food) == GoodsClass::Cryo);

    int withPeople = 0;
    int withoutPeople = 0;
    forEachStation(world, [&](std::uint32_t s, std::uint32_t t, std::uint32_t market) {
        bool habitat = false;
        for (const std::uint32_t index : world.stationModules(s, t)) {
            habitat = habitat || defs.modules()[index].family == sol::assets::ModuleFamily::Habitat;
        }
        const bool holdsFood = world.economy().capacityOf(market, food) > 0.0f;
        SOL_CHECK(habitat == holdsFood);
        (habitat ? withPeople : withoutPeople)++;
    });
    std::printf(
        "  %d station(s) with people hold food; %d with nobody hold none\n", withPeople, withoutPeople);
    // Both halves are real, or the invariant is true because one side is empty.
    SOL_CHECK(withPeople > 0);
    SOL_CHECK(withoutPeople > 0);
}

// ⚑⚑⚑⚑ TEST 4 IS THE STAGE'S EXIT CRITERION AND THE FLIGHT'S COMPLAINT ANSWERED
// IN ONE. Phase 33's exit flight found that "every market opens holding every
// good", so the catalogue gates refused nothing in a fresh galaxy; this measures
// that a fresh galaxy is no longer like that. It also measures the contraband
// half: the Ironstar Hegemony declares `sol.salvage` contraband, and there is a
// station in its space that could not stock salvage if it wanted to.
//
// ⚑ Bands rather than counts, for `station_screen_tests`' reason: the station
// mix is resampled by every archetype anybody adds, and a count sheet in a test
// is a measurement with no test behind it.
SOL_TEST(a_fresh_galaxy_does_not_open_with_every_good_on_every_dock)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const auto commodityCount = static_cast<std::uint32_t>(world.commodityIds().size());
    std::vector<int> stocks(commodityCount, 0);
    int stations = 0;
    forEachStation(world, [&](std::uint32_t, std::uint32_t, std::uint32_t market) {
        ++stations;
        for (std::uint32_t c = 0; c < commodityCount; ++c) {
            if (world.economy().capacityOf(market, c) > 0.0f) {
                ++stocks[c];
            }
        }
    });
    SOL_REQUIRE(stations == 125);
    for (std::uint32_t c = 0; c < commodityCount; ++c) {
        std::printf("  %-18s stocked at %3d of %d\n", world.commodityIds()[c].c_str(), stocks[c], stations);
    }

    // ⚑⚑⚑⚑ AND HERE IS THE HONEST LIMIT OF WHAT THIS STAGE CAN DO, MEASURED
    // RATHER THAN CLAIMED. Seven of the eleven goods are `bulk`, every station
    // touches at least one bulk good, and a station must be able to hold what
    // its own modules touch (test 2) - so **every station has a bulk hold and
    // therefore stocks all seven bulk goods**. With three goods classes and a
    // commodity tree this shape, storage cannot make a bulk good scarce; it
    // makes FOOD and SALVAGE scarce. The flight's complaint - "every market
    // opens holding every good" - is narrowed from nine goods to seven, and the
    // two it removes are exactly the two that carry population and legality.
    //
    // ⚑ Asserted in this direction on purpose. A later stage that gives some
    // station a bulk good it cannot hold has either split the bulk class or
    // broken test 2, and both deserve to be noticed here rather than absorbed.
    for (std::uint32_t c = 0; c < commodityCount; ++c) {
        SOL_CHECK(stocks[c] > 0); // no good has vanished from the galaxy
        if (world.commodityClass(c) == GoodsClass::Bulk) {
            SOL_CHECK(stocks[c] == stations);
        } else {
            SOL_CHECK(stocks[c] < stations);
        }
    }
    // Salvage is the scarce one: hazardous, and most stations have no bunker.
    const std::uint32_t salvage = world.commodityIndex("sol.salvage");
    SOL_REQUIRE(salvage < commodityCount);
    SOL_CHECK(world.commodityClass(salvage) == GoodsClass::Hazardous);
    SOL_CHECK(stocks[salvage] >= 15);
    SOL_CHECK(stocks[salvage] <= 60);

    // ⚑⚑ THE EXIT CRITERION: somewhere under a law that forbids a good, a
    // station that could not have stocked it anyway. The Hegemony is the only
    // faction in `factions.toml` declaring any contraband at all, so if this
    // ever reads zero the sentence has quietly stopped being demonstrable.
    int refusals = 0;
    forEachStation(world, [&](std::uint32_t s, std::uint32_t, std::uint32_t market) {
        if (world.commodityLegality(s, salvage) != sol::assets::Legality::Contraband) {
            return;
        }
        if (world.economy().capacityOf(market, salvage) <= 0.0f) {
            ++refusals;
        }
    });
    std::printf("  %d station(s) under a law banning salvage cannot stock it\n", refusals);
    SOL_CHECK(refusals > 0);
}
