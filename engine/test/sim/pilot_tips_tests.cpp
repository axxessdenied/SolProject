#include <sol/sim/pilot_tips.hpp>

#include <sol/test/test.hpp>

#include <cstdint>
#include <vector>

using sol::core::DVec3;
using sol::sim::chooseMarketTip;
using sol::sim::choosePlaceTip;
using sol::sim::Galaxy;
using sol::sim::KnowledgeState;
using sol::sim::kTipDuplicateRange;
using sol::sim::Region;
using sol::sim::SignalSpec;
using sol::sim::StationMarket;
using sol::sim::SurveyParams;
using sol::sim::SurveySim;
using sol::sim::SystemSpec;
using sol::sim::TipSite;

namespace {

constexpr std::uint32_t kCommodities = 3;
constexpr std::uint32_t kMaxHops = 3;
// What the caller's BFS writes for "no route within the cap" (space_world.cpp
// kUnreachableHops). Restated here so the test fails if either side moves.
constexpr std::uint8_t kUnreachable = 0xff;

// Four systems in a line: 0 - 1 - 2 - 3, so hops from system 0 are 0,1,2,3 and
// a fifth system can be put out of reach. One station each, so market index
// equals system index and a failure names the system it came from.
Galaxy lineGalaxy()
{
    Galaxy galaxy;
    galaxy.seed = 90210;
    for (std::uint32_t i = 0; i < 4; ++i) {
        SystemSpec system;
        system.name = std::string("S") + static_cast<char>('0' + static_cast<char>(i));
        system.seed = 7000 + i;
        system.region = Region::Frontier;
        system.planets.push_back({.name = "I", .position = {1.0e10, 0.0, 0.0}, .radius = 1.0e6});
        system.primaryPlanet = 0;
        system.stations.push_back({.name = "Port", .archetype = 0, .position = {}});
        galaxy.systems.push_back(std::move(system));
    }
    for (std::uint32_t i = 0; i + 1 < 4; ++i) {
        galaxy.systems[i].gates.push_back({.toSystem = i + 1, .position = {}});
        galaxy.systems[i + 1].gates.push_back({.toSystem = i, .position = {}});
        galaxy.links.push_back({i, i + 1});
    }
    return galaxy;
}

SurveyParams lineParams()
{
    SurveyParams params;
    for (auto& tier : params.signalCount) { // exactly two sites per system
        tier[0] = 2;
        tier[1] = 2;
    }
    return params;
}

std::vector<StationMarket> lineMarkets()
{
    std::vector<StationMarket> markets;
    for (std::uint32_t i = 0; i < 4; ++i) {
        markets.push_back({.systemIndex = i, .stationIndex = 0, .archetype = 0, .stock = {}});
    }
    return markets;
}

std::vector<float> flatPrices(float value)
{
    return std::vector<float>(kCommodities, value);
}

} // namespace

SOL_TEST(pilot_tips_market_prefers_the_unknown_nearest_first)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey;
    survey.initialize(galaxy, lineParams(), kCommodities, 11);
    const std::vector<StationMarket> markets = lineMarkets();
    const std::vector<std::uint8_t> hops{0, 1, 2, 3};

    // Nothing remembered anywhere, and market 0 is in the system the player is
    // standing in — which is a window, not a tip. The nearest one ELSEWHERE is
    // what a pilot would name.
    std::uint32_t market = 0xffff'ffffu;
    SOL_CHECK(chooseMarketTip(markets, hops, kMaxHops, survey, 0.0, &market));
    SOL_CHECK(market == 1);

    // A tip writes the memory it just named, so the next pilot's best answer
    // moves along on its own. That self-advance is why the rule is not seeded.
    survey.recordMarket(1, flatPrices(10.0f), 0.0);
    SOL_CHECK(chooseMarketTip(markets, hops, kMaxHops, survey, 0.0, &market));
    SOL_CHECK(market == 2);

    survey.recordMarket(2, flatPrices(10.0f), 0.0);
    SOL_CHECK(chooseMarketTip(markets, hops, kMaxHops, survey, 0.0, &market));
    SOL_CHECK(market == 3);

    // ...and the market underfoot is never named, even when it is the only one
    // left unremembered. Spending a pilot's one answer on it would be worse
    // than the shrug they give instead.
    survey.recordMarket(3, flatPrices(10.0f), 0.0);
    SOL_CHECK(survey.remembered(0) == nullptr);
    SOL_CHECK(!chooseMarketTip(markets, hops, kMaxHops, survey, 0.0, &market));
}

SOL_TEST(pilot_tips_market_falls_back_to_the_stalest_memory)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey;
    survey.initialize(galaxy, lineParams(), kCommodities, 11);
    const std::vector<StationMarket> markets = lineMarkets();
    const std::vector<std::uint8_t> hops{0, 1, 2, 3};

    // Every market remembered, at different times, and all three candidates
    // stale by `now` — so this picks the stalest rather than the only one.
    // Market 2 was seen longest ago, so its price has most likely moved.
    survey.recordMarket(0, flatPrices(10.0f), 500.0);
    survey.recordMarket(1, flatPrices(10.0f), 300.0);
    survey.recordMarket(2, flatPrices(10.0f), 100.0);
    survey.recordMarket(3, flatPrices(10.0f), 400.0);

    const double now = 400.0 + survey.params().intelStaleSeconds + 100.0;
    for (std::uint32_t i = 1; i < 4; ++i) {
        SOL_CHECK(survey.isStale(*survey.remembered(i), now));
    }
    std::uint32_t market = 0xffff'ffffu;
    SOL_CHECK(chooseMarketTip(markets, hops, kMaxHops, survey, now, &market));
    SOL_CHECK(market == 2);

    // ...and an unknown market beats every stale one, however old they are.
    std::vector<StationMarket> withNewPort = markets;
    withNewPort.push_back({.systemIndex = 3, .stationIndex = 1, .archetype = 0, .stock = {}});
    SOL_CHECK(chooseMarketTip(withNewPort, hops, kMaxHops, survey, now, &market));
    SOL_CHECK(market == 4);
}

SOL_TEST(pilot_tips_market_ignores_fresh_memories_and_distant_markets)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey;
    survey.initialize(galaxy, lineParams(), kCommodities, 11);
    const std::vector<StationMarket> markets = lineMarkets();

    // Everything the player has looked at recently: a pilot has nothing to add,
    // and saying so is a real answer rather than a failure. A game that always
    // has one more tip is a game where a tip means nothing.
    for (std::uint32_t i = 0; i < 4; ++i) {
        survey.recordMarket(i, flatPrices(10.0f), 50.0);
    }
    const std::vector<std::uint8_t> hops{0, 1, 2, 3};
    std::uint32_t market = 0xffff'ffffu;
    SOL_CHECK(!chooseMarketTip(markets, hops, kMaxHops, survey, 60.0, &market));

    // A pilot knows their neighbourhood, not everywhere their gates reach: an
    // unknown market past the horizon is still not something they can name.
    SurveySim blank;
    blank.initialize(galaxy, lineParams(), kCommodities, 11);
    const std::vector<std::uint8_t> farHops{kUnreachable, kUnreachable, kUnreachable, 2};
    SOL_CHECK(blank.remembered(0) == nullptr);
    SOL_CHECK(chooseMarketTip(markets, farHops, kMaxHops, blank, 0.0, &market));
    SOL_CHECK(market == 3); // the only one in reach
    const std::vector<std::uint8_t> noneInReach(4, kUnreachable);
    SOL_CHECK(!chooseMarketTip(markets, noneInReach, kMaxHops, blank, 0.0, &market));
}

SOL_TEST(pilot_tips_place_prefers_this_system_then_the_nearest)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey;
    survey.initialize(galaxy, lineParams(), kCommodities, 11);
    for (std::uint32_t i = 0; i < 4; ++i) {
        survey.setKnowledge(galaxy, i, KnowledgeState::Visited);
    }
    const std::vector<std::uint8_t> hops{0, 1, 2, 3};
    std::vector<SignalSpec> scratch;
    TipSite site;

    SOL_CHECK(choosePlaceTip(galaxy, survey, hops, kMaxHops, scratch, &site));
    SOL_CHECK(site.system == 0);

    // The site named must be a real one, at the position the generator puts it.
    survey.signalsFor(galaxy, site.system, scratch);
    SOL_CHECK(site.signal < scratch.size());
    SOL_CHECK(length(site.position - scratch[site.signal].position) < 1.0e-9);

    // Standing somewhere else, the nearest system with an unresolved site wins
    // rather than the lowest-numbered one.
    const std::vector<std::uint8_t> fromThree{3, 2, 1, 0};
    SOL_CHECK(choosePlaceTip(galaxy, survey, fromThree, kMaxHops, scratch, &site));
    SOL_CHECK(site.system == 3);
}

SOL_TEST(pilot_tips_place_skips_sites_already_known_or_marked)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey;
    survey.initialize(galaxy, lineParams(), kCommodities, 11);
    // Only system 1 is placeable at all: an Unknown system cannot be pointed
    // at, because the player has nowhere to put it.
    survey.setKnowledge(galaxy, 1, KnowledgeState::Charted);
    const std::vector<std::uint8_t> hops{0, 1, 2, 3};
    std::vector<SignalSpec> scratch;
    TipSite site;

    SOL_CHECK(choosePlaceTip(galaxy, survey, hops, kMaxHops, scratch, &site));
    SOL_CHECK(site.system == 1);
    SOL_CHECK(site.signal == 0);

    // Found by the player's own pulse: naming a contact already on their HUD
    // tells them nothing and would read as a bug rather than a shrug.
    SOL_CHECK(survey.notifySignalDiscovered(1, 0));
    SOL_CHECK(choosePlaceTip(galaxy, survey, hops, kMaxHops, scratch, &site));
    SOL_CHECK(site.system == 1);
    SOL_CHECK(site.signal == 1);

    // Already marked — by an earlier tip or by the player — so two pilots do
    // not send you to the same rock.
    survey.signalsFor(galaxy, 1, scratch);
    const std::uint32_t marked =
        survey.addBookmark(1, scratch[1].position, "rumour", sol::sim::kTipLabel, 0.0);
    SOL_CHECK(marked != 0);
    SOL_CHECK(!choosePlaceTip(galaxy, survey, hops, kMaxHops, scratch, &site));

    // A bookmark just outside the dedupe range does not suppress the site.
    SurveySim other;
    other.initialize(galaxy, lineParams(), kCommodities, 11);
    other.setKnowledge(galaxy, 1, KnowledgeState::Charted);
    other.signalsFor(galaxy, 1, scratch);
    const DVec3 offset = scratch[0].position + DVec3{kTipDuplicateRange * 2.0, 0.0, 0.0};
    const std::uint32_t elsewhere = other.addBookmark(1, offset, "elsewhere", 0, 0.0);
    SOL_CHECK(elsewhere != 0);
    SOL_CHECK(choosePlaceTip(galaxy, other, hops, kMaxHops, scratch, &site));
    SOL_CHECK(site.system == 1);
    SOL_CHECK(site.signal == 0);
}

SOL_TEST(pilot_tips_place_runs_out_when_the_neighbourhood_is_known)
{
    const Galaxy galaxy = lineGalaxy();
    SurveySim survey;
    survey.initialize(galaxy, lineParams(), kCommodities, 11);
    std::vector<SignalSpec> scratch;
    TipSite site;

    // Nothing charted: nobody can tell you about anywhere.
    const std::vector<std::uint8_t> hops{0, 1, 2, 3};
    SOL_CHECK(!choosePlaceTip(galaxy, survey, hops, kMaxHops, scratch, &site));

    // Everything visited and every site resolved: the rumours are used up, and
    // that is the answer rather than an error.
    for (std::uint32_t system = 0; system < 4; ++system) {
        survey.setKnowledge(galaxy, system, KnowledgeState::Visited);
        survey.signalsFor(galaxy, system, scratch);
        for (std::uint32_t signal = 0; signal < scratch.size(); ++signal) {
            (void)survey.notifySignalResolved(galaxy, system, signal);
        }
    }
    SOL_CHECK(!choosePlaceTip(galaxy, survey, hops, kMaxHops, scratch, &site));
}
