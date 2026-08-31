// The economy's count-sheet guards, run against the content the game actually
// ships and the galaxy it actually generates (engine plan Phase 33 stage E).
//
// ⚑⚑⚑⚑ THIS FILE EXISTS BECAUSE THE GUARD IN `engine/test/sim/economy_tests.cpp`
// WAS SCORING A GALAXY THE GAME DOES NOT BUILD, AND HAD BEEN SINCE IT WAS
// WRITTEN. That test is the better instrument in every way but one: `sim.unit`
// links `sol::sim` alone, so it cannot see `game/data` and has to reach the
// content by HAND TRANSCRIPTION. Stage B and stage C both kept that
// transcription faithfully in step with the two files it is a mirror of -
// `commodities.toml` and `stations.toml` - and neither of them was the file
// that broke it. `factions.toml` has carried `station_bias` since Phase 13, it
// multiplies an archetype's placement weight wherever a faction holds
// territory, and it changes how many of each station the galaxy contains. It
// was in nobody's mirror.
//
// What that cost: refined metal ran at 0.561 made-to-burnt in the shipped
// galaxy - 17 Fabricator Yards against 13 Refineries, every one of them
// permanently feedstock-throttled - while the sim-side guard read 1.015 and
// passed, and food and raw ore were over the ceiling for the same reason.
//
// ⚑⚑⚑ SO THE FIX IS NOT A WIDER MIRROR, IT IS NO MIRROR. These two tests read
// the defs off disk and generate the galaxy through `SpaceWorld` exactly as the
// game boots it, so there is nothing to keep in step and no list of files to
// remember. The sim-side pair stay where they are and are still worth having -
// they guard the mirror's own arithmetic, and when these two fail while those
// two pass, the difference IS the diagnosis.
//
// ⚑⚑ AND THE THIRD FILE IS NOW LOAD-BEARING FOR THESE TESTS: editing a
// `station_bias` in `factions.toml` will fail them, which is the entire point.
// That is not a fragile test, it is the coupling being visible for the first
// time.

#include "space_world.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::StationDef;
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

// How many of each archetype the shipped galaxy contains, at the seed every
// phase is verified against. ⚑ Through `SpaceWorld` rather than through
// `generateGalaxy` directly, because everything this file is about - the
// faction bias, the pirate templates, the authored systems - is applied by
// `generateUniverse` and by nothing below it.
struct Mix
{
    std::vector<std::uint32_t> counts;
    std::size_t stations = 0;
};

[[nodiscard]] Mix shippedMix(const DefDatabase& defs, game::SpaceWorld& world)
{
    world.spawn(game::kDefaultUniverseSeed);
    // ⚑ Before `generateUniverse`, which is the order the game boots in and is
    // load-bearing: `initializeFactions` returns immediately when there is no
    // def database, leaving every system unowned - and an unowned galaxy has no
    // station bias to apply, which is the exact difference this file exists to
    // measure. Stage D found the same inversion the hard way.
    world.applyDefs(defs);
    Mix mix;
    if (!world.generateUniverse(defs)) {
        std::printf("  generateUniverse refused the shipped defs\n");
        return mix;
    }
    mix.counts.assign(defs.stations().size(), 0);
    for (const sol::sim::SystemSpec& system : world.galaxy().systems) {
        for (const sol::sim::StationSpec& station : system.stations) {
            if (station.archetype < mix.counts.size()) {
                ++mix.counts[station.archetype];
            }
            ++mix.stations;
        }
    }
    return mix;
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

struct Flow
{
    double made = 0.0;
    double burnt = 0.0;
};

// What the ARCHETYPES say the galaxy does: one count times one rate list, which
// is what this file measured before Phase 34 stage B and is now the DECLARED
// half of the comparison rather than the answer.
[[nodiscard]] Flow declaredFlowOf(const DefDatabase& defs, const Mix& mix, const std::string& commodityId)
{
    Flow flow;
    for (std::size_t a = 0; a < defs.stations().size(); ++a) {
        const double count = mix.counts[a];
        const StationDef& station = defs.stations()[a];
        flow.made += count * rateOf(station.produces, commodityId);
        // Feedstock and upkeep both count as burning it - they differ in
        // whether they gate production, not in whether the units go away.
        flow.burnt += count * (rateOf(station.feedstock, commodityId) +
                               rateOf(station.consumes, commodityId));
    }
    return flow;
}

// ⚑⚑⚑ WHAT THE GALAXY ACTUALLY DOES, WHICH SINCE PHASE 34 STAGE B IS NOT THE
// SAME QUESTION. Every station is composed of modules and runs on the rates its
// own module list adds up to, so an archetype-level sum is an EXPECTATION and
// this is the sample that was drawn. `station_composition_tests.cpp` proves the
// two agree in the arithmetic; this walks the markets the sim will actually
// tick and adds up what they will actually do.
[[nodiscard]] Flow composedFlowOf(const game::SpaceWorld& world, std::uint32_t commodity)
{
    Flow flow;
    const std::vector<sol::sim::EconomyArchetype>& rates = world.economy().params().archetypes;
    for (const sol::sim::StationMarket& market : world.economy().markets()) {
        if (market.archetype >= rates.size()) {
            continue;
        }
        const sol::sim::EconomyArchetype& row = rates[market.archetype];
        if (commodity < row.production.size()) {
            flow.made += row.production[commodity];
            flow.burnt += row.feedstock[commodity] + row.consumption[commodity];
        }
    }
    return flow;
}

} // namespace

// The sim-side `economy_every_commodity_is_made_and_burnt_somewhere`, asked of
// the real thing. A commodity nobody produces and nobody consumes sits at
// exactly half capacity forever, which is dead centre of every band the
// four-hour steady-state test checks - so scenery passes that test and only
// this shape of assertion can tell a material tree from a decoration.
SOL_TEST(shipped_content_makes_and_burns_every_commodity_somewhere)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    const Mix mix = shippedMix(defs, world);
    SOL_REQUIRE(mix.stations > 0);
    SOL_REQUIRE(!defs.commodities().empty());

    for (std::uint32_t c = 0; c < defs.commodities().size(); ++c) {
        const sol::assets::CommodityDef& commodity = defs.commodities()[c];
        const Flow flow = composedFlowOf(world, c);
        if (flow.made <= 0.0 || flow.burnt <= 0.0) {
            std::printf("  %s is %s and %s\n",
                        commodity.id.c_str(),
                        flow.made > 0.0 ? "made" : "MADE BY NOBODY",
                        flow.burnt > 0.0 ? "burnt" : "BURNT BY NOBODY");
        }
        SOL_CHECK(flow.made > 0.0);
        SOL_CHECK(flow.burnt > 0.0);
    }
}

// ⚑⚑⚑ THE ONE THAT WOULD HAVE CAUGHT IT. Same band as the sim-side guard
// ([0.85, 1.6], wide and asymmetric because `stations.toml` runs every producer
// deliberately ahead of its customers), same arithmetic, and a station count
// taken from the galaxy the player flies rather than from a transcription of
// two of the three files that decide it.
//
// ⚑⚑ IT PRINTS THE MIX ON FAILURE, NOT JUST THE RATIO. The failure mode this
// whole stage came out of is a ratio moving because the station COUNT moved,
// with every rate in the game untouched - so a bare ratio sends the reader to
// `stations.toml` to look for an edit that is not there. The counts are the
// diagnosis.
SOL_TEST(shipped_content_makes_no_commodity_much_faster_than_it_is_burnt)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    const Mix mix = shippedMix(defs, world);
    SOL_REQUIRE(mix.stations > 0);

    bool anyOutOfBand = false;
    for (std::uint32_t c = 0; c < defs.commodities().size(); ++c) {
        const sol::assets::CommodityDef& commodity = defs.commodities()[c];
        const Flow flow = composedFlowOf(world, c);
        SOL_REQUIRE(flow.burnt > 0.0); // the test above already said somebody burns it
        const double ratio = flow.made / flow.burnt;
        if (ratio <= 0.85 || ratio >= 1.6) {
            anyOutOfBand = true;
            std::printf("  %s: %.4f/s made, %.4f/s burnt, ratio %.3f\n",
                        commodity.id.c_str(),
                        flow.made,
                        flow.burnt,
                        ratio);
        }
        SOL_CHECK(ratio > 0.85);
        SOL_CHECK(ratio < 1.6);

        // ⚑⚑ AND THE SAMPLE AGAINST ITS OWN EXPECTATION (Phase 34 stage B). The
        // archetype rate lists are what the recipes were authored to reproduce,
        // so a composed galaxy that drifts far from them is a recipe bug rather
        // than a balance decision - and it would otherwise hide inside a band
        // wide enough to swallow it. Five percent is generous for the hundred and
        // twenty-five draws a galaxy takes, and still catches a whole module
        // sitting in the wrong recipe.
        const Flow declared = declaredFlowOf(defs, mix, commodity.id);
        for (int half = 0; half < 2; ++half) {
            const double sampled = half == 0 ? flow.made : flow.burnt;
            const double expected = half == 0 ? declared.made : declared.burnt;
            if (expected > 0.0 && std::abs(sampled - expected) > expected * 0.05) {
                std::printf("  %s %s: the composed galaxy does %.4f/s against an expected %.4f/s\n",
                            commodity.id.c_str(),
                            half == 0 ? "made" : "burnt",
                            sampled,
                            expected);
                anyOutOfBand = true;
            }
            SOL_CHECK(expected <= 0.0 || std::abs(sampled - expected) <= expected * 0.05);
        }
    }
    if (anyOutOfBand) {
        std::printf("  the mix that produced those numbers (%zu stations):\n   ", mix.stations);
        for (std::size_t a = 0; a < defs.stations().size(); ++a) {
            std::printf(" %s %u |", defs.stations()[a].name.c_str(), mix.counts[a]);
        }
        std::printf("\n  a ratio can move because a RATE moved or because a COUNT did;"
                    " `station_bias` in factions.toml moves counts.\n");
    }
}
