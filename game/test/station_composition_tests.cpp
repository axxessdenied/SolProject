// Station composition (engine plan Phase 34 stage B): the generator rolls a
// module list per station and the game reduces it to an economy archetype of
// its own.
//
// ⚑⚑⚑⚑ THE CONTRACT THIS FILE EXISTS TO HOLD IS ONE SENTENCE: *the expected
// composition of an archetype reproduces that archetype's own rate list.* Every
// number in `stations.toml` was searched rather than argued - three stages of
// Phase 33 generated the whole galaxy per candidate to find them - so a
// decomposition that lands anywhere else is not a new balance, it is the old
// one thrown away. The second test below is that sentence, checked line by line
// against the file it is about.
//
// ⚑⚑ AND THE FALLBACK IS TESTED BY EVERY OTHER SUITE IN THE PROJECT, WHICH IS
// WORTH SAYING RATHER THAN DUPLICATING: a `StationSpec` with `kNoComposition`
// reads its archetype's rates, and `sim.unit` builds nothing else - so the
// whole economy suite is the negative case, and this file only has to prove the
// positive one (test 4).

#include "space_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::ModuleDef;
using sol::assets::StationDef;
using sol::assets::StationModuleEntry;
using sol::assets::StationRate;

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

// The shipped galaxy, built the way the game boots it. `applyDefs` first, for
// the reason `economy_content_tests.cpp` records: an unowned galaxy has no
// station bias to apply, and the bias is most of what makes the real mix.
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

[[nodiscard]] double rateOf(const std::vector<StationRate>& list, const std::string& commodityId)
{
    for (const StationRate& rate : list) {
        if (rate.commodityId == commodityId) {
            return rate.rate;
        }
    }
    return 0.0;
}

} // namespace

// ⚑⚑⚑ THE RULING, MEASURED ON THE GALAXY THE GAME BUILDS: power is the
// constraint the composer satisfies. Nothing in the sim reads a station's
// budget and nothing ever will unless somebody gives it a reader - so this test
// is what makes the figures in `modules.toml` mean anything at all.
SOL_TEST(every_composed_station_makes_the_power_it_draws)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));
    SOL_REQUIRE(!defs.modules().empty());

    std::size_t composed = 0;
    std::size_t plants = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const auto& stations = world.galaxy().systems[s].stations;
        for (std::uint32_t i = 0; i < stations.size(); ++i) {
            const std::span<const std::uint32_t> modules = world.stationModules(s, i);
            if (modules.empty()) {
                continue;
            }
            ++composed;
            float draw = 0.0f;
            float output = 0.0f;
            std::size_t powerRows = 0;
            for (const std::uint32_t module : modules) {
                SOL_REQUIRE(module < defs.modules().size());
                draw += defs.modules()[module].powerDraw;
                output += defs.modules()[module].powerOutput;
                if (defs.modules()[module].family == sol::assets::ModuleFamily::Power) {
                    ++powerRows;
                }
            }
            if (output < draw) {
                std::printf("  %s draws %.1f and makes %.1f\n", stations[i].name.c_str(), draw, output);
            }
            SOL_CHECK(output >= draw);
            // A plant is FITTED rather than authored, so every composed station
            // has at least one and no recipe may name one (the def validator
            // refuses that at load).
            SOL_CHECK(powerRows >= 1);
            plants += powerRows;
        }
    }
    std::printf("  %zu composed station(s), %zu plant(s), %zu distinct composition(s)\n",
                composed,
                plants,
                world.compositionCount());
    SOL_CHECK(composed > 0);
}

// ⚑⚑⚑⚑ THE ONE THAT MATTERS. A recipe is a bag of chances, and the arithmetic
// that has to hold is that the bag's EXPECTED rate list is the archetype's
// authored one - the numbers Phase 33 searched for, which every economy guard in
// the repository is still scored against.
SOL_TEST(every_recipe_reproduces_its_archetype_rate_list_in_expectation)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    SOL_REQUIRE(!defs.stations().empty());

    std::size_t withRecipes = 0;
    for (const StationDef& station : defs.stations()) {
        if (station.modules.empty()) {
            continue;
        }
        ++withRecipes;
        for (const sol::assets::CommodityDef& commodity : defs.commodities()) {
            double produces = 0.0;
            double consumes = 0.0;
            double feedstock = 0.0;
            for (const StationModuleEntry& entry : station.modules) {
                const ModuleDef* module = defs.findModule(entry.moduleId.c_str());
                SOL_REQUIRE(module != nullptr);
                const double chance = entry.chance;
                produces += chance * rateOf(module->produces, commodity.id);
                consumes += chance * rateOf(module->consumes, commodity.id);
                feedstock += chance * rateOf(module->feedstock, commodity.id);
            }
            struct Line
            {
                const char* key;
                double expected;
                double declared;
            };
            const Line lines[] = {
                {"produces", produces, rateOf(station.produces, commodity.id)},
                {"consumes", consumes, rateOf(station.consumes, commodity.id)},
                {"feedstock", feedstock, rateOf(station.feedstock, commodity.id)},
            };
            for (const Line& line : lines) {
                // A tenth of a percent of the rate, or a millionth of a unit for
                // the lines that should be flat zero. Nothing here is a rounding
                // question: the chances are authored to make these exact, and a
                // real drift is a whole module.
                const double tolerance = std::max(1.0e-6, std::abs(line.declared) * 1.0e-3);
                if (std::abs(line.expected - line.declared) > tolerance) {
                    std::printf("  %s %s %s: modules expect %.5f, the archetype declares %.5f\n",
                                station.id.c_str(),
                                line.key,
                                commodity.id.c_str(),
                                line.expected,
                                line.declared);
                }
                SOL_CHECK(std::abs(line.expected - line.declared) <= tolerance);
            }
        }
    }
    std::printf("  %zu of %zu archetype(s) carry a recipe\n", withRecipes, defs.stations().size());
    // Every shipped archetype is composed. A mod's may not be, and that is the
    // fallback the whole sim suite exercises - but nothing in the base game may
    // be half decomposed, or the galaxy would run on two different models at
    // once and the count sheet would score neither.
    SOL_CHECK(withRecipes == defs.stations().size());
}

// ⚑⚑⚑ THE PHASE'S EXIT CRITERION, ASSERTED THE DAY THE MECHANISM LANDS RATHER
// THAN AT THE END: *two stations of the same archetype differ in what they
// offer.* Before stage B this was unsayable - two stations of one archetype were
// the same row of the same table - so this test could not have been written at
// all, which is the clearest statement of what the second index bought.
SOL_TEST(two_stations_of_one_archetype_differ_in_the_shipped_galaxy)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    // archetype -> the compositions seen for it
    std::vector<std::vector<std::vector<std::uint32_t>>> seen(defs.stations().size());
    std::size_t stations = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const auto& rows = world.galaxy().systems[s].stations;
        for (std::uint32_t i = 0; i < rows.size(); ++i) {
            const std::span<const std::uint32_t> modules = world.stationModules(s, i);
            if (modules.empty() || rows[i].archetype >= seen.size()) {
                continue;
            }
            ++stations;
            std::vector<std::uint32_t> list(modules.begin(), modules.end());
            auto& bucket = seen[rows[i].archetype];
            if (std::find(bucket.begin(), bucket.end(), list) == bucket.end()) {
                bucket.push_back(std::move(list));
            }
        }
    }
    SOL_REQUIRE(stations > 0);

    std::size_t archetypesWithVariety = 0;
    for (std::size_t a = 0; a < seen.size(); ++a) {
        if (seen[a].size() > 1) {
            ++archetypesWithVariety;
        } else if (!seen[a].empty()) {
            std::printf("  every %s in the galaxy is identical\n", defs.stations()[a].name.c_str());
        }
    }
    std::printf("  %zu archetype(s) place more than one composition\n", archetypesWithVariety);
    // Every archetype in the shipped galaxy has optional modules in its recipe
    // and enough stations to roll them differently, so anything less than all of
    // them means the roll is not doing what it looks like it is doing.
    SOL_CHECK(archetypesWithVariety == 11);
}

// ⚑⚑ THE INDEX ITSELF, END TO END. `Economy::initialize` reads
// `StationSpec::composition` when it is set - so if it were quietly reading the
// archetype instead, every rate in the galaxy would still be plausible and only
// the stations that DIFFER from their archetype would give it away. This walks
// the markets and checks each one against the sum of its own modules.
SOL_TEST(every_market_runs_on_the_rates_its_own_modules_add_up_to)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));
    SOL_REQUIRE(!world.economy().markets().empty());

    const std::vector<std::string>& ids = world.commodityIds();
    std::size_t differsFromArchetype = 0;
    for (const sol::sim::StationMarket& market : world.economy().markets()) {
        const std::span<const std::uint32_t> modules =
            world.stationModules(market.systemIndex, market.stationIndex);
        SOL_REQUIRE(!modules.empty());
        SOL_REQUIRE(market.archetype < world.economy().params().archetypes.size());
        const sol::sim::EconomyArchetype& rates = world.economy().params().archetypes[market.archetype];
        const std::uint32_t archetypeIndex =
            world.galaxy().systems[market.systemIndex].stations[market.stationIndex].archetype;
        const sol::sim::EconomyArchetype& declared = world.economy().params().archetypes[archetypeIndex];

        bool differs = false;
        for (std::uint32_t c = 0; c < ids.size(); ++c) {
            double produces = 0.0;
            double consumes = 0.0;
            double feedstock = 0.0;
            for (const std::uint32_t module : modules) {
                const ModuleDef& def = defs.modules()[module];
                produces += rateOf(def.produces, ids[c]);
                consumes += rateOf(def.consumes, ids[c]);
                feedstock += rateOf(def.feedstock, ids[c]);
            }
            SOL_CHECK(std::abs(static_cast<double>(rates.production[c]) - produces) < 1.0e-5);
            SOL_CHECK(std::abs(static_cast<double>(rates.consumption[c]) - consumes) < 1.0e-5);
            SOL_CHECK(std::abs(static_cast<double>(rates.feedstock[c]) - feedstock) < 1.0e-5);
            differs = differs || std::abs(static_cast<double>(declared.consumption[c]) - consumes) > 1.0e-5;
        }
        if (differs) {
            ++differsFromArchetype;
        }
    }
    // ⚑ The half that proves the index is live: if `initialize` were reading the
    // archetype, every market here would match the archetype's flat food line
    // exactly and this count would be zero.
    std::printf("  %zu of %zu market(s) eat something other than their archetype's flat line\n",
                differsFromArchetype,
                world.economy().markets().size());
    SOL_CHECK(differsFromArchetype > 0);
}
