// Screens from modules (engine plan Phase 34 stage C): a station's tab strip is
// a filter over the module list stage B composed it from.
//
// ⚑⚑⚑⚑ THE CONTRACT THIS FILE HOLDS IS THE RULING, AND THE RULING IS ONE
// SENTENCE: *a tab is on the strip when the station is equipped for it, or when
// the player's own half of it has something in it.* The spec asked only for "a
// filter over the composition", and a plain union would have been wrong in a way
// nothing else in this repository could have reported - three of the eight tabs
// are a facility AND the only place the player can see something they own, and
// two more are not a facility at all. So the filter is tested as a rule (test 1),
// the composition it reads is tested against the defs (test 2), the galaxy it
// produces is measured (tests 3 and 4), and the remembered tab - which is where
// `UiContext::tabs` would silently substitute a DIFFERENT screen - is driven
// through the real frame (tests 5 and 6).
//
// ⚑⚑ AND THE PARALLEL PAIR IS PINNED AT COMPILE TIME, NOT HERE.
// `station_screen.cpp` carries eight `static_assert`s tying
// `StationScreenState::Tab` to `sol::assets::StationScreen` entry by entry, which
// is what makes one bit index serve both. A runtime test could only agree with
// whichever of the two it was written against.

#include "space_world.hpp"
#include "station_screen.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/synthetic_cooked_font.hpp>
#include <sol/test/test.hpp>
#include <sol/ui/context.hpp>

using game::StationScreenState;
using sol::assets::DefDatabase;
using sol::assets::ModuleDef;
using sol::assets::StationScreen;
using sol::ui::FleetRow;
using sol::ui::MissionRow;
using sol::ui::OutfitRow;
using sol::ui::StationPanel;
using sol::ui::UiContext;

namespace {

constexpr sol::core::Vec2 kScreen = {1280.0f, 720.0f};

[[nodiscard]] constexpr std::uint32_t bitOf(int tab)
{
    return 1u << static_cast<std::uint32_t>(tab);
}

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

// The shipped galaxy, built the way the game boots it - `applyDefs` before
// `generateUniverse`, for the reason `economy_content_tests.cpp` records: an
// unowned galaxy has no station bias, and the bias is most of what makes the
// real mix.
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

// Every station in the galaxy, so a measurement is over the thing that ships
// rather than over one system somebody picked.
template <typename Fn>
void forEachStation(const game::SpaceWorld& world, Fn&& fn)
{
    const sol::sim::Galaxy& galaxy = world.galaxy();
    for (std::uint32_t s = 0; s < galaxy.systems.size(); ++s) {
        for (std::uint32_t t = 0; t < galaxy.systems[s].stations.size(); ++t) {
            fn(s, t, galaxy.systems[s].stations[t]);
        }
    }
}

// The layout `buildStationScreen` uses, reproduced so a test can click a tab.
// ⚑ Brittle on purpose and safe because of it: if this drifts, the click lands
// on nothing and the test that depends on it FAILS rather than passing while
// proving nothing. Test 6 asserts the click changed something before it asserts
// what it changed to.
[[nodiscard]] sol::ui::Rect tabRect(const UiContext& ui, int index, int count)
{
    const float width = std::min(kScreen.x - 80.0f, 1180.0f);
    const float height = std::min(kScreen.y - 60.0f, 820.0f);
    const float left = (kScreen.x - width) * 0.5f;
    const float top = (kScreen.y - height) * 0.5f;
    const float padding = ui.theme().padding;
    const float spacing = ui.theme().spacing;
    // Column: padding, then the 44 px header, then spacing, then the 34 px strip.
    const float stripTop = top + padding + 44.0f + spacing;
    const float stripLeft = left + padding;
    const float stripWidth = width - padding * 2.0f;
    const float each = (stripWidth - spacing * static_cast<float>(count - 1)) / static_cast<float>(count);
    const float x = stripLeft + (each + spacing) * static_cast<float>(index);
    return {{x, stripTop}, {x + each, stripTop + 34.0f}};
}

// One click, which this UI completes on press-then-release over the same widget.
void clickTab(UiContext& ui, StationPanel& panel, StationScreenState& state, int index, int count)
{
    const sol::ui::Rect box = tabRect(ui, index, count);
    const sol::core::Vec2 at = {(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f};
    sol::ui::InputState input;
    input.mousePosition = at;
    input.mouseDown = true;
    input.mousePressed = true;
    ui.beginFrame(input, kScreen);
    (void)game::buildStationScreen(ui, panel, state);
    ui.endFrame();

    input.mouseDown = false;
    input.mousePressed = false;
    input.mouseReleased = true;
    ui.beginFrame(input, kScreen);
    (void)game::buildStationScreen(ui, panel, state);
    ui.endFrame();
}

void idleFrame(UiContext& ui, StationPanel& panel, StationScreenState& state)
{
    sol::ui::InputState input;
    input.mousePosition = {-100.0f, -100.0f};
    ui.beginFrame(input, kScreen);
    (void)game::buildStationScreen(ui, panel, state);
    ui.endFrame();
}

} // namespace

// ⚑⚑⚑⚑ TEST 1 IS THE RULING ITSELF, AND IT IS THE ONE TEST IN THIS FILE THAT
// WOULD HAVE CAUGHT THE MISTAKE THE SPEC WAS ABOUT TO MAKE. "The tab strip
// becomes a filter over the composition" is true of five tabs and false of
// three, and false in a direction that costs the player sight of things they
// own rather than sight of things the station has.
SOL_TEST(a_tab_is_on_the_strip_for_the_station_or_for_the_player_never_neither)
{
    StationPanel bare; // a station equipped for nothing, a pilot carrying nothing
    bare.screens = 0;

    // The two no station may withhold. Nothing composes them - no `[[module]]`
    // names either screen - so a plain union would have deleted both from the
    // game outright, which is what makes this a ruling rather than a filter.
    SOL_CHECK(game::stationTabOnStrip(bare, StationScreenState::Factions));
    SOL_CHECK(game::stationTabOnStrip(bare, StationScreenState::Survey));

    // Everything else is off at a station equipped for nothing.
    SOL_CHECK(!game::stationTabOnStrip(bare, StationScreenState::Trade));
    SOL_CHECK(!game::stationTabOnStrip(bare, StationScreenState::Outfitting));
    SOL_CHECK(!game::stationTabOnStrip(bare, StationScreenState::Shipyard));
    SOL_CHECK(!game::stationTabOnStrip(bare, StationScreenState::Crew));
    SOL_CHECK(!game::stationTabOnStrip(bare, StationScreenState::Missions));
    SOL_CHECK(!game::stationTabOnStrip(bare, StationScreenState::Refinery));

    // A module turns any of them on, one bit at a time.
    for (const int tab : {StationScreenState::Trade,
                          StationScreenState::Outfitting,
                          StationScreenState::Shipyard,
                          StationScreenState::Crew,
                          StationScreenState::Missions,
                          StationScreenState::Refinery}) {
        StationPanel equipped;
        equipped.screens = bitOf(tab);
        SOL_CHECK(game::stationTabOnStrip(equipped, tab));
    }

    // ⚑⚑ AND THE HALF THE SPEC DID NOT SEE: three tabs survive on what the
    // PLAYER is carrying, because each is the only place that thing is shown.
    const FleetRow fleet[] = {{.name = "Kestrel", .active = true}, {.name = "Mule", .active = false}};
    const OutfitRow crew[] = {{.id = "sol.crew_engineer", .name = "Engineer"}};
    const MissionRow journal[] = {{.title = "Haul to Erorea"}};

    StationPanel oneShip;
    oneShip.screens = 0;
    oneShip.fleet = std::span(fleet, 1); // the hull you are flying is not a fleet
    SOL_CHECK(!game::stationTabOnStrip(oneShip, StationScreenState::Shipyard));

    StationPanel spare;
    spare.screens = 0;
    spare.fleet = fleet; // a hull parked somewhere is a thing you own
    SOL_CHECK(game::stationTabOnStrip(spare, StationScreenState::Shipyard));

    StationPanel crewed;
    crewed.screens = 0;
    crewed.crewAboard = crew;
    SOL_CHECK(game::stationTabOnStrip(crewed, StationScreenState::Crew));
    SOL_CHECK(!game::stationTabOnStrip(crewed, StationScreenState::Shipyard));

    StationPanel underway;
    underway.screens = 0;
    underway.missionJournal = journal;
    SOL_CHECK(game::stationTabOnStrip(underway, StationScreenState::Missions));

    // ⚑ Outfitting is deliberately NOT on that list: every button on its mount
    // rows - Fit, and Remove for a refund - is work an outfitter does, so a
    // hull with mounts and no outfitter has nothing to offer but the reading.
    // A catalog the station is not selling from does not earn a tab either.
    StationPanel carrying;
    carrying.screens = 0;
    carrying.fleet = fleet;
    carrying.crewAboard = crew;
    carrying.missionJournal = journal;
    SOL_CHECK(!game::stationTabOnStrip(carrying, StationScreenState::Outfitting));
    SOL_CHECK(!game::stationTabOnStrip(carrying, StationScreenState::Trade));
    SOL_CHECK(!game::stationTabOnStrip(carrying, StationScreenState::Refinery));

    // Out of range is not a tab.
    SOL_CHECK(!game::stationTabOnStrip(bare, -1));
    SOL_CHECK(!game::stationTabOnStrip(bare, StationScreenState::TabCount));
}

// ⚑⚑⚑ TEST 2: THE MASK IS THE UNION OVER THE MODULES AND NOTHING ELSE, CHECKED
// AGAINST THE DEFS RATHER THAN AGAINST ITSELF. `stationScreens` reads a cache
// built at `generateUniverse` from `defs.modules()`; this recomputes it from the
// def database directly, so a cache that went stale, a module whose screen list
// was dropped, or an index off by one is a failure here.
SOL_TEST(a_stations_screen_mask_is_the_union_of_its_modules_screen_lists)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::vector<ModuleDef>& modules = defs.modules();
    SOL_REQUIRE(!modules.empty());

    std::uint32_t composed = 0;
    forEachStation(world, [&](std::uint32_t s, std::uint32_t t, const sol::sim::StationSpec&) {
        const std::span<const std::uint32_t> owned = world.stationModules(s, t);
        SOL_REQUIRE(!owned.empty()); // every shipped station is composed (stage B)
        ++composed;
        std::uint32_t expected = 0;
        for (const std::uint32_t module : owned) {
            SOL_REQUIRE(module < modules.size());
            for (const StationScreen screen : modules[module].screens) {
                expected |= 1u << static_cast<std::uint32_t>(screen);
            }
        }
        SOL_CHECK(world.stationScreens(s, t) == expected);
    });
    SOL_CHECK(composed == 125);

    // ⚑ The fallback, which is the whole of what a galaxy with no `[[module]]`
    // content gets: an index that names no station offers EVERY screen, the
    // same shape `Economy::initialize` takes when it finds no composition.
    constexpr std::uint32_t kEveryScreen = (1u << sol::assets::kStationScreenCount) - 1u;
    SOL_CHECK(world.stationScreens(0xffffffffu, 0) == kEveryScreen);
    SOL_CHECK(world.stationScreens(0, 0xffffffffu) == kEveryScreen);
    // Not docked is not "every screen": it is no station at all.
    SOL_CHECK(world.dockedStationScreens() == 0u);
}

// ⚑⚑⚑⚑ TEST 3 IS THIS STAGE'S HALF OF THE PHASE'S EXIT CRITERION - *two stations
// of the same archetype differ in what they offer* - read as SCREENS rather than
// as rates. Stage B proved it of the rate lists; until this stage every dock in
// the galaxy drew the same eight tabs, including a Shipyard tab on a mining
// outpost, which is the thing gdd.md §12 asks to be got rid of.
// ⚑⚑⚑⚑ THIS TEST USED TO ASSERT THE RULE ON THE GALAXY AND WAS REWRITTEN
// IN PHASE 35 STAGE A, BECAUSE THE GALAXY IS A SAMPLE AND THE RULE IS ABOUT THE
// RECIPES. It read: *for every archetype with two or more stations, two of them
// offer different screens* - and its own comment named the real claim, "every
// recipe carries at least one screen-bearing module below chance 1.0". Those are
// not the same statement. The first is the second plus a coin toss, and the
// smallest archetypes toss the fewest coins: `sol.station_shipyard` places 5
// stations and `sol.station_assembly` 6.
//
// ⚑⚑⚑ IT WAS NOT A HYPOTHETICAL. Stage A added ONE recipe row - a resort,
// so that a module with no placement anywhere in the tree could appear at all -
// and a recipe row adds a draw, so every station composed after it re-rolled.
// On the resampled galaxy BOTH of those archetypes came out uniform, with the
// recipes completely unchanged. MEASURED, not reasoned: reverting the stage's
// screen lists and leaving the resampled recipes still failed both, which is
// what proves it was the resample and not the new screen.
//
// So the rule is asserted where the rule lives, and the galaxy is measured
// beside it as a consequence with a band rather than as the claim itself. Same
// move stage 34-E made for `shadowOperatorFor` and stage 34-C for
// `stationTabOnStrip`: *a galaxy-level assertion is only a guard for the cases
// that galaxy actually contains.*
SOL_TEST(every_archetype_recipe_can_produce_two_stations_with_different_screens)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    // The rule, on the defs: an archetype can differ only if it has a recipe
    // row that offers a screen and does not always land.
    int archetypesChecked = 0;
    for (const sol::assets::StationDef& station : defs.stations()) {
        bool canVary = false;
        for (const sol::assets::StationModuleEntry& entry : station.modules) {
            const ModuleDef* module = defs.findModule(entry.moduleId.c_str());
            if (module != nullptr && !module->screens.empty() && entry.chance < 1.0f) {
                canVary = true;
                break;
            }
        }
        if (!canVary) {
            std::printf("  '%s' has no screen-bearing module below chance 1.0\n", station.id.c_str());
        }
        SOL_CHECK(canVary);
        ++archetypesChecked;
    }
    SOL_CHECK(archetypesChecked == 11);
}

// And the consequence, measured on the galaxy the rule produces. A BAND, because
// which archetypes happen to vary is resampled by every recipe row anybody adds -
// which is exactly how the version of this test that pinned all eleven broke.
SOL_TEST(most_archetypes_do_produce_different_screens_in_the_shipped_galaxy)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    // Screen masks seen per archetype.
    std::vector<std::vector<std::uint32_t>> seen(defs.stations().size());
    forEachStation(world, [&](std::uint32_t s, std::uint32_t t, const sol::sim::StationSpec& spec) {
        if (spec.archetype < seen.size()) {
            seen[spec.archetype].push_back(world.stationScreens(s, t));
        }
    });

    int withStations = 0;
    int archetypesThatVary = 0;
    for (std::size_t a = 0; a < seen.size(); ++a) {
        std::vector<std::uint32_t> masks = seen[a];
        if (masks.size() < 2) {
            continue;
        }
        ++withStations;
        std::sort(masks.begin(), masks.end());
        const bool varies = std::unique(masks.begin(), masks.end()) != masks.end();
        std::printf("  %-24s %2zu station(s), screens %s\n",
                    defs.stations()[a].id.c_str(),
                    masks.size(),
                    varies ? "VARY" : "all identical");
        archetypesThatVary += varies ? 1 : 0;
    }
    SOL_CHECK(withStations == 11);
    // The dividend has to be visible, not merely possible: most of the galaxy's
    // archetypes place two docks that offer different things. The floor is what
    // makes this a guard - if a resample ever drops it here, the recipes have
    // stopped carrying enough optional screens and the test above will say so.
    SOL_CHECK(archetypesThatVary >= 8);
}

// ⚑⚑⚑ TEST 4 MEASURES THE GALAXY THE RULING PRODUCES, BECAUSE A FILTER THAT IS
// CORRECT AND USELESS LOOKS EXACTLY LIKE ONE THAT WORKS. Bands rather than exact
// counts: the mix is resampled by every archetype anybody adds, and this stage
// must not become a second count sheet that goes stale one commit later. What is
// pinned is the SHAPE - a market floor is common, a drydock is rare, and neither
// is universal.
SOL_TEST(the_shipped_galaxy_offers_screens_that_are_worth_flying_to)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    int total = 0;
    int offering[sol::assets::kStationScreenCount] = {};
    int bareStations = 0; // equipped for nothing at all
    forEachStation(world, [&](std::uint32_t s, std::uint32_t t, const sol::sim::StationSpec&) {
        const std::uint32_t mask = world.stationScreens(s, t);
        ++total;
        if (mask == 0u) {
            ++bareStations;
        }
        for (std::size_t i = 0; i < sol::assets::kStationScreenCount; ++i) {
            if ((mask & (1u << i)) != 0u) {
                ++offering[i];
            }
        }
    });
    SOL_REQUIRE(total == 125);

    for (std::size_t i = 0; i < sol::assets::kStationScreenCount; ++i) {
        std::printf("  %-11s %3d of %d\n",
                    sol::assets::stationScreenName(static_cast<StationScreen>(i)),
                    offering[i],
                    total);
    }
    std::printf("  equipped for nothing: %d\n", bareStations);

    // Trade is the dividend gdd.md §12 names by hand - "a mining outpost with no
    // market floor has no Trade tab, and finding one that does is worth flying
    // to". Common enough to trade with, scarce enough to notice.
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Trade)] >= 70);
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Trade)] <= 115);
    // The three that should be genuinely worth crossing space for.
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Shipyard)] >= 4);
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Shipyard)] <= 35);
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Refinery)] >= 6);
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Refinery)] <= 40);
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Outfitting)] >= 15);
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Outfitting)] <= 75);
    // ⚑⚑ THE BAR (Phase 35 stage A), AND ITS BAND IS WIDE ON PURPOSE. Every
    // one of the eleven recipes carries a bar between 0.45 and 0.55, so about
    // half the galaxy has a room - common enough that a player meets one early,
    // scarce enough that a dock without one is a different kind of place. The
    // band is not a count sheet: five recreation rows roll independently and
    // the mix is resampled by every recipe row anybody adds.
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Bar)] >= 40);
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Bar)] <= 100);
    // Neither is universal, or the filter has stopped filtering.
    for (const StationScreen screen : {StationScreen::Trade,
                                       StationScreen::Outfitting,
                                       StationScreen::Shipyard,
                                       StationScreen::Crew,
                                       StationScreen::Missions,
                                       StationScreen::Refinery,
                                       StationScreen::Bar}) {
        SOL_CHECK(offering[static_cast<std::size_t>(screen)] < total);
    }
    // Nothing composes these two, which is what the ruling is FOR.
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Factions)] == 0);
    SOL_CHECK(offering[static_cast<std::size_t>(StationScreen::Survey)] == 0);

    // ⚑⚑ AND THE REASON THE RULING IS NOT A MATTER OF TASTE: a plain union
    // leaves stations equipped for nothing at all. Measured rather than argued
    // and every one of them is a dock that would open on an empty strip if
    // Factions and Survey were gated too.
    //
    // ⚑⚑⚑ PHASE 35 STAGE A HALVED THIS FIGURE AND THAT IS THE POINT OF IT.
    // A room is equipment, so a dock that was equipped for nothing but happens
    // to have a bar is now equipped for something: measured, the count fell
    // from about six of 125 to about three. The Bar is the single biggest
    // widener of the NARROWEST strips in the galaxy - two-tab docks fall from
    // five to two and three-tab docks from thirty to twelve - which is a
    // partial answer to the question Phase 34's exit flight is still owed,
    // whether a three-tab dock reads as a small station or as a broken one.
    std::printf("  a plain union would leave %d of %d docks with no tabs\n", bareStations, total);
    SOL_CHECK(bareStations > 0);
    // Every station still has a strip, because two tabs are never withheld.
    forEachStation(world, [&](std::uint32_t s, std::uint32_t t, const sol::sim::StationSpec&) {
        StationPanel panel;
        panel.screens = world.stationScreens(s, t);
        int tabs = 0;
        for (int tab = 0; tab < StationScreenState::TabCount; ++tab) {
            tabs += game::stationTabOnStrip(panel, tab) ? 1 : 0;
        }
        SOL_CHECK(tabs >= 2);
    });
}

// ⚑⚑⚑ TEST 5: THE REFINING SERVICE FOLLOWS THE MODULE, AND THE TAB AND THE
// SERVICE CAN NEVER DISAGREE. `sol.mod_refinery_service` sits in the Refinery's
// recipe at chance 0.85, so roughly one refinery in seven has the production
// line and not the counter - the first time two stations of one archetype differ
// in a SERVICE rather than in a rate. The invariant matters more than the count:
// stage C deleted the "(this station refines nothing)" note, so a Refining tab
// with no service behind it would now draw an uninitialised panel.
SOL_TEST(the_refining_service_and_the_refining_tab_come_from_the_same_module)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    const std::vector<ModuleDef>& modules = defs.modules();
    int refines = 0;
    int refineryArchetypesWithout = 0;
    const sol::assets::StationDef* refineryDef = defs.findStation("sol.station_refinery");
    SOL_REQUIRE(refineryDef != nullptr);
    const auto refineryIndex = static_cast<std::uint32_t>(refineryDef - defs.stations().data());

    forEachStation(world, [&](std::uint32_t s, std::uint32_t t, const sol::sim::StationSpec& spec) {
        const bool tab = (world.stationScreens(s, t) & bitOf(StationScreenState::Refinery)) != 0u;
        bool service = false;
        for (const std::uint32_t module : world.stationModules(s, t)) {
            if (!modules[module].refineInput.empty() && !modules[module].refineOutput.empty()) {
                service = true;
            }
        }
        // The parser refuses a module that says one without the other, so the
        // two can only disagree if a reader here lost one of them.
        SOL_CHECK(tab == service);
        refines += tab ? 1 : 0;
        if (spec.archetype == refineryIndex && !tab) {
            ++refineryArchetypesWithout;
        }
    });

    std::printf("  %d stations refine; %d Refineries have the line but not the counter\n",
                refines,
                refineryArchetypesWithout);
    SOL_CHECK(refines > 0);
    // The whole point of authoring that chance at 0.85 rather than 1.0.
    SOL_CHECK(refineryArchetypesWithout > 0);
}

// ⚑⚑⚑⚑ TEST 6 IS THE BUG THE SPEC SAW AND THE STAGE HAD TO NOT SHIP.
// `UiContext::tabs` ends with `clamp(selected, 0, count - 1)` because it was
// written for a fixed strip. Hand it a remembered index against a per-station
// strip and a remembered *Refinery* becomes whatever happens to be fourth at the
// next dock - the player's place silently traded for a different screen. So the
// remembered value is the tab's IDENTITY and the strip index is derived per
// station. Driven through the real frame, because the substitution happens
// inside a call this cannot be reasoned about from outside.
SOL_TEST(a_remembered_tab_survives_a_station_that_does_not_have_it)
{
    sol::assets::Font font;
    SOL_REQUIRE(font.loadFromMemory(sol::test::buildSyntheticCookedFont()));
    UiContext ui;
    ui.setFont(&font, 1);

    // A refinery: Trade, Missions and Refining. The player is reading Refining.
    StationPanel refinery;
    refinery.screens = bitOf(StationScreenState::Trade) | bitOf(StationScreenState::Missions) |
                       bitOf(StationScreenState::Refinery);
    refinery.refinery.refines = true;
    refinery.refinery.inputName = "Ore";
    refinery.refinery.outputName = "Metal";

    StationScreenState state;
    state.tab = StationScreenState::Refinery;
    idleFrame(ui, refinery, state);
    SOL_CHECK(state.tab == StationScreenState::Refinery);

    // Undock, dock at a mining outpost that offers only Trade. Its strip is
    // [Trade, Factions, Survey]: three labels, and the remembered 7 is past the
    // end of it. Before this stage `tabs()` would have clamped 7 to 2 and drawn
    // SURVEY while remembering it as Survey; what must happen is that the strip
    // falls back to its first tab and the memory is left alone.
    StationPanel outpost;
    outpost.screens = bitOf(StationScreenState::Trade);
    idleFrame(ui, outpost, state);
    SOL_CHECK(state.tab == StationScreenState::Refinery);

    // Back at a refinery, the player is where they left off.
    idleFrame(ui, refinery, state);
    SOL_CHECK(state.tab == StationScreenState::Refinery);

    // ⚑⚑ And the other half: what is written back is an identity, not a place.
    // The outpost's strip is [Trade, Factions, Survey], so clicking the SECOND
    // tab must remember `Factions` (4) - a stage that stored the strip index
    // would remember 1, which is Outfitting, a tab this station does not have.
    state.tab = StationScreenState::Trade;
    clickTab(ui, outpost, state, 1, 3);
    SOL_CHECK(state.tab != StationScreenState::Trade); // the click landed at all
    SOL_CHECK(state.tab == StationScreenState::Factions);
    SOL_CHECK(state.tab != StationScreenState::Outfitting);

    // The third tab is Survey (6), not Shipyard (2).
    clickTab(ui, outpost, state, 2, 3);
    SOL_CHECK(state.tab == StationScreenState::Survey);
}
