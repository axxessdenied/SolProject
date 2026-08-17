#include <sol/sim/survey.hpp>
#include <sol/sim/universe.hpp>

#include <sol/core/serialize.hpp>
#include <sol/test/test.hpp>

#include <cstdint>
#include <vector>

using sol::sim::Galaxy;
using sol::sim::GalaxyParams;
using sol::sim::generateGalaxy;
using sol::sim::KnowledgeState;
using sol::sim::Region;
using sol::sim::SignalCargo;
using sol::sim::SignalLoot;
using sol::sim::SignalSpec;
using sol::sim::SurveyEntry;
using sol::sim::SurveyKind;
using sol::sim::SurveyParams;
using sol::sim::SurveySim;
using sol::sim::SystemSpec;

namespace {

// Three systems in a line, one of each region tier: 0 (core, settled) - 1
// (frontier, settled) - 2 (fringe, unsettled). Signal counts come from the
// params, so the fixture pins them per tier instead of trusting a seed.
Galaxy lineGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 4242;
    for (std::uint32_t i = 0; i < 3; ++i) {
        SystemSpec system;
        system.name = std::string("S") + static_cast<char>('0' + static_cast<char>(i));
        system.seed = 1000 + i;
        system.region = static_cast<Region>(i);
        system.planets.push_back({.name = "I", .position = {1.0e10, 0.0, 0.0}, .radius = 1.0e6});
        system.planets.push_back({.name = "II", .position = {2.0e10, 0.0, 0.0}, .radius = 1.0e6});
        system.primaryPlanet = 1;
        if (i < 2) { // the fringe system is unsettled: first-discovery pricing
            system.stations.push_back({.name = "Alpha", .archetype = 0, .position = {}});
        }
        galaxy.systems.push_back(std::move(system));
    }
    galaxy.systems[0].gates.push_back({.toSystem = 1, .position = {}});
    galaxy.systems[1].gates.push_back({.toSystem = 0, .position = {}});
    galaxy.systems[1].gates.push_back({.toSystem = 2, .position = {}});
    galaxy.systems[2].gates.push_back({.toSystem = 1, .position = {}});
    galaxy.links = {{0, 1}, {1, 2}};
    return galaxy;
}

SurveyParams lineParams()
{
    SurveyParams params;
    params.signalCount[0][0] = 1; // core: exactly 1
    params.signalCount[0][1] = 1;
    params.signalCount[1][0] = 2; // frontier: exactly 2
    params.signalCount[1][1] = 2;
    params.signalCount[2][0] = 3; // fringe: exactly 3
    params.signalCount[2][1] = 3;
    return params;
}

SurveySim lineSurvey(const Galaxy& galaxy)
{
    SurveySim survey;
    survey.initialize(galaxy, lineParams(), 2, 99);
    return survey;
}

} // namespace

SOL_TEST(survey_signals_are_deterministic_per_seed)
{
    const Galaxy galaxy = lineGalaxy();
    const SurveySim survey = lineSurvey(galaxy);

    std::vector<SignalSpec> first;
    std::vector<SignalSpec> second;
    survey.signalsFor(galaxy, 2, first);
    survey.signalsFor(galaxy, 2, second);
    SOL_REQUIRE(first.size() == 3);
    SOL_CHECK(survey.signalCount(2) == 3);
    SOL_CHECK(survey.signalCount(0) == 1);
    for (std::size_t i = 0; i < first.size(); ++i) {
        SOL_CHECK(first[i].kind == second[i].kind);
        SOL_CHECK(first[i].position.x == second[i].position.x);
        SOL_CHECK(first[i].position.y == second[i].position.y);
        SOL_CHECK(first[i].position.z == second[i].position.z);
        SOL_CHECK(first[i].seed == second[i].seed);
    }

    // A second SurveySim over the same galaxy sees the same sites, and the
    // sites sit in the playfield around the primary planet.
    const SurveySim other = lineSurvey(galaxy);
    std::vector<SignalSpec> otherSignals;
    other.signalsFor(galaxy, 2, otherSignals);
    SOL_REQUIRE(otherSignals.size() == first.size());
    const SurveyParams params = lineParams();
    for (std::size_t i = 0; i < first.size(); ++i) {
        SOL_CHECK(otherSignals[i].seed == first[i].seed);
        const sol::core::DVec3 offset =
            first[i].position - galaxy.systems[2].planets[1].position;
        const double distance = sol::core::length(offset);
        SOL_CHECK(distance >= params.signalMinDistance * 0.999);
        SOL_CHECK(distance <= params.signalMaxDistance * 1.001);
    }
}

SOL_TEST(survey_arrival_charts_neighbors_only)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey = lineSurvey(galaxy);
    SOL_CHECK(survey.knowledge(0) == KnowledgeState::Unknown);

    survey.notifyArrival(galaxy, 0);
    SOL_CHECK(survey.knowledge(0) == KnowledgeState::Visited);
    SOL_CHECK(survey.knowledge(1) == KnowledgeState::Charted); // one lane out
    SOL_CHECK(survey.knowledge(2) == KnowledgeState::Unknown); // two lanes out
    SOL_CHECK(survey.knownSystemCount() == 2);

    // Arriving again does not re-pay, and charting never demotes a visit.
    const double afterFirst = survey.ledgerValue();
    survey.notifyArrival(galaxy, 0);
    SOL_CHECK(survey.ledgerValue() == afterFirst);
    survey.notifyArrival(galaxy, 1);
    survey.notifyArrival(galaxy, 0);
    SOL_CHECK(survey.knowledge(1) == KnowledgeState::Visited);
    SOL_CHECK(survey.knowledge(2) == KnowledgeState::Charted);
}

SOL_TEST(survey_values_scale_with_region_and_settlement)
{
    const Galaxy galaxy = lineGalaxy();
    const SurveyParams params = lineParams();
    SurveySim core = lineSurvey(galaxy);
    SurveySim fringe = lineSurvey(galaxy);

    core.notifyArrival(galaxy, 0);
    fringe.notifyArrival(galaxy, 2);
    SOL_REQUIRE(core.ledger().size() == 1);
    SOL_REQUIRE(fringe.ledger().size() == 1);
    SOL_CHECK(core.ledger()[0].kind == SurveyKind::System);
    SOL_CHECK(!core.ledger()[0].firstDiscovery);   // a station means neighbors
    SOL_CHECK(fringe.ledger()[0].firstDiscovery);  // unsettled: nobody charted it

    const double expectedCore = params.valueSystem * params.regionMultiplier[0];
    const double expectedFringe =
        params.valueSystem * params.regionMultiplier[2] * params.firstDiscoveryBonus;
    SOL_CHECK(core.ledgerValue() == expectedCore);
    SOL_CHECK(fringe.ledgerValue() == expectedFringe);
    SOL_CHECK(fringe.ledgerValue() > core.ledgerValue() * 3.0); // the fringe pays

    // Selling clears the ledger and hands over exactly what it was worth.
    SOL_CHECK(fringe.sellLedger() == expectedFringe);
    SOL_CHECK(fringe.ledger().empty());
    SOL_CHECK(fringe.ledgerValue() == 0.0);
}

SOL_TEST(survey_completion_needs_every_body_and_site)
{
    const Galaxy galaxy = lineGalaxy();
    const SurveyParams params = lineParams();
    SurveySim survey = lineSurvey(galaxy);
    survey.notifyArrival(galaxy, 0);

    SOL_CHECK(survey.bodyCount(galaxy, 0) == 3); // star + two planets
    SOL_CHECK(survey.notifyBodyScanned(galaxy, 0, 0));
    SOL_CHECK(!survey.notifyBodyScanned(galaxy, 0, 0)); // once each
    SOL_CHECK(!survey.notifyBodyScanned(galaxy, 0, 3)); // out of range
    SOL_CHECK(survey.bodyScanned(0, 0));
    SOL_CHECK(survey.notifyBodyScanned(galaxy, 0, 1));
    SOL_CHECK(survey.notifyBodyScanned(galaxy, 0, 2));
    SOL_CHECK(survey.knowledge(0) == KnowledgeState::Visited); // site still open

    SOL_CHECK(survey.notifySignalDiscovered(0, 0));
    SOL_CHECK(!survey.notifySignalDiscovered(0, 1)); // core system holds one
    SOL_CHECK(survey.notifySignalResolved(galaxy, 0, 0));
    SOL_CHECK(survey.knowledge(0) == KnowledgeState::Surveyed);

    const double expected = params.valueSystem * params.regionMultiplier[0]
                            + 3.0 * params.valueBody * params.regionMultiplier[0]
                            + params.valueSite * params.regionMultiplier[0]
                            + params.valueCompletion * params.regionMultiplier[0];
    SOL_CHECK(survey.ledgerValue() == expected);

    // Completion pays once, and a scan in a system you have never visited is
    // refused (knowledge is earned by being there).
    SOL_CHECK(!survey.notifySignalResolved(galaxy, 0, 0));
    SOL_CHECK(survey.ledgerValue() == expected);
    SOL_CHECK(!survey.notifyBodyScanned(galaxy, 2, 0));
}

SOL_TEST(survey_loot_is_held_until_the_site_is_emptied)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey = lineSurvey(galaxy);
    survey.notifyArrival(galaxy, 0);

    SignalLoot loot;
    loot.cargo.push_back({.commodity = 1, .units = 12.0f});
    loot.credits = 500.0;
    SOL_CHECK(!survey.setLoot(0, 0, loot)); // not resolved yet

    SOL_CHECK(survey.notifySignalResolved(galaxy, 0, 0));
    SOL_CHECK(survey.setLoot(0, 0, loot));
    SOL_REQUIRE(survey.loot(0, 0) != nullptr);
    SOL_CHECK(survey.loot(0, 0)->credits == 500.0);
    SOL_REQUIRE(survey.loot(0, 0)->cargo.size() == 1);
    SOL_CHECK(survey.loot(0, 0)->cargo[0].units == 12.0f);

    SignalLoot bad;
    bad.cargo.push_back({.commodity = 9, .units = 1.0f}); // commodityCount is 2
    SOL_CHECK(!survey.setLoot(0, 0, bad));
    SOL_CHECK(survey.loot(0, 0)->credits == 500.0); // rejected, not merged

    SOL_CHECK(survey.notifySignalEmptied(0, 0));
    SOL_CHECK(survey.signalEmptied(0, 0));
    SOL_CHECK(survey.loot(0, 0) == nullptr);
    SOL_CHECK(!survey.notifySignalEmptied(0, 0));
    SOL_CHECK(!survey.setLoot(0, 0, loot)); // an emptied site stays empty
}

SOL_TEST(survey_route_advances_and_drops)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey = lineSurvey(galaxy);

    survey.setRoute({0});
    SOL_CHECK(survey.route().empty()); // a route to where you stand is none
    survey.setRoute({0, 1, 7});
    SOL_CHECK(survey.route().empty()); // out-of-galaxy plot leaves nothing

    survey.setRoute({0, 1, 2});
    SOL_CHECK(survey.nextHop() == 1);
    survey.notifyArrival(galaxy, 1);
    SOL_REQUIRE(survey.route().size() == 2);
    SOL_CHECK(survey.route()[0] == 1);
    SOL_CHECK(survey.nextHop() == 2);
    survey.notifyArrival(galaxy, 2);
    SOL_CHECK(survey.route().empty()); // arrived
    SOL_CHECK(survey.nextHop() == sol::sim::kNoSystem);

    survey.setRoute({2, 1, 0});
    survey.notifyArrival(galaxy, 2); // still at the head: route intact
    SOL_CHECK(survey.route().size() == 3);
    survey.notifyArrival(galaxy, 0); // jumped off the plot entirely
    SOL_CHECK(survey.route().empty());
}

SOL_TEST(survey_market_memory_ages_and_finds_the_best_price)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey = lineSurvey(galaxy);
    SOL_CHECK(survey.marketMemory().empty());
    SOL_CHECK(survey.remembered(0) == nullptr);

    survey.recordMarket(0, {10.0f, 30.0f}, 0.0);
    survey.recordMarket(2, {14.0f, 25.0f}, 100.0);
    survey.recordMarket(1, {9.0f, 44.0f}, 200.0);
    SOL_REQUIRE(survey.marketMemory().size() == 3);
    // Kept in market order so the save is stable and lookups can bisect.
    SOL_CHECK(survey.marketMemory()[0].market == 0);
    SOL_CHECK(survey.marketMemory()[1].market == 1);
    SOL_CHECK(survey.marketMemory()[2].market == 2);

    // Looking again refreshes in place rather than piling up readings.
    survey.recordMarket(2, {13.0f, 26.0f}, 300.0);
    SOL_CHECK(survey.marketMemory().size() == 3);
    SOL_REQUIRE(survey.remembered(2) != nullptr);
    SOL_CHECK(survey.remembered(2)->takenAt == 300.0);
    SOL_CHECK(survey.remembered(2)->prices[0] == 13.0f);

    // A snapshot that doesn't match the commodity table is not a snapshot.
    survey.recordMarket(4, {1.0f}, 300.0);
    SOL_CHECK(survey.remembered(4) == nullptr);

    // Best price elsewhere, and how old that reading is: this is what the
    // Trade tab's "elsewhere" column and the map's trade overlay both read.
    std::uint32_t market = 0;
    float price = 0.0f;
    double age = 0.0;
    SOL_REQUIRE(survey.bestRemembered(1, 0, 400.0, &market, &price, &age));
    SOL_CHECK(market == 1);
    SOL_CHECK(price == 44.0f);
    SOL_CHECK(age == 200.0);
    // Excluding the winner falls through to the next best.
    SOL_REQUIRE(survey.bestRemembered(1, 1, 400.0, &market, &price, &age));
    SOL_CHECK(market == 0);
    SOL_CHECK(price == 30.0f);
    SOL_CHECK(!survey.bestRemembered(9, 0, 400.0, &market, &price, &age));

    // Staleness is a fact about the reading, not about the market.
    SOL_CHECK(!survey.isStale(*survey.remembered(2), 400.0));
    SOL_CHECK(survey.isStale(*survey.remembered(2), 300.0 + survey.params().intelStaleSeconds + 1.0));
}

SOL_TEST(survey_save_load_round_trips)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey = lineSurvey(galaxy);
    survey.notifyArrival(galaxy, 1);
    survey.notifyArrival(galaxy, 2);
    SOL_CHECK(survey.notifyBodyScanned(galaxy, 2, 0));
    SOL_CHECK(survey.notifySignalDiscovered(2, 1));
    SOL_CHECK(survey.notifySignalResolved(galaxy, 2, 0));
    SignalLoot loot;
    loot.cargo.push_back({.commodity = 0, .units = 7.5f});
    loot.credits = 120.0;
    loot.moduleId = "sol.scanner_mk2";
    SOL_CHECK(survey.setLoot(2, 0, loot));
    survey.setRoute({2, 1, 0});
    survey.recordMarket(3, {11.0f, 42.0f}, 640.0);
    const std::uint32_t bookmarkId =
        survey.addBookmark(2, {1.5e8, -2.0e7, 9.0e7}, "Good rock", 3, 812.5);
    SOL_CHECK(bookmarkId != 0);

    sol::core::BinaryWriter writer;
    survey.save(writer);

    SurveySim restored = lineSurvey(galaxy);
    sol::core::BinaryReader reader(writer.data());
    SOL_REQUIRE(restored.load(reader));

    SOL_CHECK(restored.knowledge(0) == survey.knowledge(0));
    SOL_CHECK(restored.knowledge(1) == survey.knowledge(1));
    SOL_CHECK(restored.knowledge(2) == survey.knowledge(2));
    SOL_CHECK(restored.bodyScanned(2, 0));
    SOL_CHECK(restored.signalDiscovered(2, 1));
    SOL_CHECK(restored.signalResolved(2, 0));
    SOL_CHECK(restored.ledgerValue() == survey.ledgerValue());
    SOL_REQUIRE(restored.ledger().size() == survey.ledger().size());
    for (std::size_t i = 0; i < survey.ledger().size(); ++i) {
        SOL_CHECK(restored.ledger()[i].system == survey.ledger()[i].system);
        SOL_CHECK(restored.ledger()[i].kind == survey.ledger()[i].kind);
        SOL_CHECK(restored.ledger()[i].value == survey.ledger()[i].value);
        SOL_CHECK(restored.ledger()[i].firstDiscovery == survey.ledger()[i].firstDiscovery);
    }
    SOL_REQUIRE(restored.loot(2, 0) != nullptr);
    SOL_CHECK(restored.loot(2, 0)->credits == 120.0);
    SOL_CHECK(restored.loot(2, 0)->moduleId == "sol.scanner_mk2");
    SOL_REQUIRE(restored.loot(2, 0)->cargo.size() == 1);
    SOL_CHECK(restored.loot(2, 0)->cargo[0].units == 7.5f);
    SOL_REQUIRE(restored.route().size() == 3);
    SOL_CHECK(restored.nextHop() == 1);
    SOL_REQUIRE(restored.remembered(3) != nullptr);
    SOL_CHECK(restored.remembered(3)->takenAt == 640.0);
    SOL_REQUIRE(restored.remembered(3)->prices.size() == 2);
    SOL_CHECK(restored.remembered(3)->prices[1] == 42.0f);
    // Bookmarks come back with their name, position, label and timestamp
    // exactly - a waypoint that moves on reload is worse than none.
    SOL_REQUIRE(restored.bookmark(bookmarkId) != nullptr);
    SOL_CHECK(restored.bookmark(bookmarkId)->name == "Good rock");
    SOL_CHECK(restored.bookmark(bookmarkId)->position.x == 1.5e8);
    SOL_CHECK(restored.bookmark(bookmarkId)->position.y == -2.0e7);
    SOL_CHECK(restored.bookmark(bookmarkId)->position.z == 9.0e7);
    SOL_CHECK(restored.bookmark(bookmarkId)->label == 3);
    SOL_CHECK(restored.bookmark(bookmarkId)->createdAt == 812.5);
    SOL_CHECK(restored.bookmark(bookmarkId)->system == 2);
    // And a new bookmark after a reload must not reuse a live id.
    SOL_CHECK(restored.addBookmark(2, {}, "Another", 0, 900.0) != bookmarkId);

    // A mismatched layout is rejected rather than half-read (economy rule).
    GalaxyParams biggerParams;
    biggerParams.seed = 5;
    biggerParams.systemCount = 8;
    const Galaxy bigger = generateGalaxy(biggerParams);
    SurveySim mismatched;
    mismatched.initialize(bigger, lineParams(), 2, 99);
    sol::core::BinaryReader replay(writer.data());
    SOL_CHECK(!mismatched.load(replay));
}

SOL_TEST(survey_generated_galaxy_signals_follow_the_region_gradient)
{
    GalaxyParams params;
    params.seed = 1701;
    params.systemCount = 40;
    params.factionCount = 3;
    const Galaxy galaxy = generateGalaxy(params);

    SurveySim survey;
    survey.initialize(galaxy, SurveyParams{}, 4, params.seed);

    std::uint32_t coreSignals = 0;
    std::uint32_t coreSystems = 0;
    std::uint32_t fringeSignals = 0;
    std::uint32_t fringeSystems = 0;
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        if (galaxy.systems[i].region == Region::Core) {
            coreSignals += survey.signalCount(i);
            ++coreSystems;
        } else if (galaxy.systems[i].region == Region::Fringe) {
            fringeSignals += survey.signalCount(i);
            ++fringeSystems;
        }
    }
    SOL_REQUIRE(coreSystems > 0);
    SOL_REQUIRE(fringeSystems > 0);
    const double corePer = static_cast<double>(coreSignals) / coreSystems;
    const double fringePer = static_cast<double>(fringeSignals) / fringeSystems;
    SOL_CHECK(fringePer > corePer); // opportunity rises toward the fringe

    // The same seed regenerates the same signals for a fresh sim.
    SurveySim again;
    again.initialize(galaxy, SurveyParams{}, 4, params.seed);
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        SOL_CHECK(again.signalCount(i) == survey.signalCount(i));
    }
}

SOL_TEST(survey_bookmark_ids_are_stable_across_deletions)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey = lineSurvey(galaxy);

    const std::uint32_t first = survey.addBookmark(0, {1.0e8, 0.0, 0.0}, "One", 0, 10.0);
    const std::uint32_t second = survey.addBookmark(2, {2.0e8, 0.0, 0.0}, "Two", 0, 20.0);
    const std::uint32_t third = survey.addBookmark(0, {3.0e8, 0.0, 0.0}, "Three", 0, 30.0);
    SOL_CHECK(first != 0 && second != 0 && third != 0);
    SOL_CHECK(first != second && second != third);

    // Deleting one in ANOTHER system must not renumber the rest. This is the
    // whole reason bookmarks carry an id instead of being addressed by their
    // ledger index: SpaceWorld's nav-target tail points at them, and a live
    // selection or an in-flight scan indexes into that tail.
    SOL_CHECK(survey.removeBookmark(second));
    SOL_REQUIRE(survey.bookmark(first) != nullptr);
    SOL_REQUIRE(survey.bookmark(third) != nullptr);
    SOL_CHECK(survey.bookmark(first)->name == "One");
    SOL_CHECK(survey.bookmark(third)->name == "Three");
    SOL_CHECK(survey.bookmark(second) == nullptr);

    // A fresh bookmark never takes a retired id, so a stale slot pointing at
    // a deleted one cannot silently become a different place.
    const std::uint32_t fourth = survey.addBookmark(0, {}, "Four", 0, 40.0);
    SOL_CHECK(fourth != second);
    SOL_CHECK(fourth != first && fourth != third);

    // Per-system listing is by system, in creation order.
    std::vector<std::uint32_t> inZero;
    survey.bookmarksIn(0, inZero);
    SOL_REQUIRE(inZero.size() == 3);
    SOL_CHECK(inZero[0] == first);
    SOL_CHECK(inZero[1] == third);
    SOL_CHECK(inZero[2] == fourth);
    SOL_CHECK(survey.bookmarkCountIn(2) == 0);

    SOL_CHECK(survey.renameBookmark(first, "Renamed"));
    SOL_CHECK(survey.bookmark(first)->name == "Renamed");
    SOL_CHECK(!survey.renameBookmark(second, "Gone"));
    SOL_CHECK(!survey.removeBookmark(second));
}

SOL_TEST(survey_bookmarks_are_capped_per_system)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey;
    SurveyParams params = lineParams();
    params.maxBookmarksPerSystem = 3;
    survey.initialize(galaxy, params, 2, 99);

    for (std::uint32_t i = 0; i < 3; ++i) {
        SOL_CHECK(survey.addBookmark(1, {}, "x", 0, 0.0) != 0);
    }
    // Past the cap the attempt fails rather than pushing the oldest out: the
    // nav cycle and the radar disc both surface these, and silently losing a
    // waypoint the player wrote down would be worse than refusing a new one.
    SOL_CHECK(survey.addBookmark(1, {}, "overflow", 0, 0.0) == 0);
    SOL_CHECK(survey.bookmarkCountIn(1) == 3);
    // The cap is per system, so another system is unaffected.
    SOL_CHECK(survey.addBookmark(2, {}, "elsewhere", 0, 0.0) != 0);
}
