// The room (engine plan Phase 35 stage A): a recreation module puts a bar on a
// station, and what the house has to say about the dock it is standing on.
//
// ⚑⚑⚑⚑ THIS FILE EXISTS BECAUSE THE INSTRUMENT THAT WATCHES STATION DATA IS
// BLIND TO WHAT THIS STAGE CHANGED, AND THAT WAS FORESEEABLE RATHER THAN
// DISCOVERED. `kGoldenComposition` hashes module IDS and the list a station was
// composed from - deliberately, so that inserting a row in `modules.toml` is not
// a false positive. Adding `screens = ["bar"]` to five modules changes what a
// module OFFERS without changing what any station IS composed of, so the
// composition digest does not move, the structure digest reads a galaxy
// `composeStations` never touched, and both geometry digests are untouched. Four
// green digests and a whole feature they cannot see.
//
// Phase 34 stage D paid for the general form of this: the composition line went
// blind for a whole stage because the digest moved for the wrong reason and
// nobody looked again. Its remedy is the rule this file follows - the claim and
// the proof that the claim measured something live in ONE place - so every count
// below is printed and floored, and nothing here can pass by measuring nothing.

#include "space_world.hpp"
#include "station_screen.hpp"
#include "station_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using game::StationScreenState;
using sol::assets::DefDatabase;
using sol::assets::ModuleDef;
using sol::assets::ModuleFamily;
using sol::assets::StationScreen;
using sol::ui::InfoRow;
using sol::ui::StationPanel;

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

[[nodiscard]] constexpr std::uint32_t barBit()
{
    return 1u << static_cast<std::uint32_t>(StationScreen::Bar);
}

} // namespace

// ⚑⚑⚑ THE RULING, ON THE DEFS, WHERE THE RULING LIVES. Ruled 2026-08-31: the
// WHOLE recreation family offers the screen. The alternative - only
// `sol.mod_bar` - would have left four authored rows as vocabulary with no
// reader, which is the failure `modules.toml`'s own header warns about in
// capitals and which this project has now shipped twice (Phase 32's ship `role`,
// Phase 33 stage A's inert commodity). It would also have put a station with a
// casino and no bar in the position of having a room full of people and no way
// into it.
SOL_TEST(every_recreation_module_offers_the_bar_and_nothing_else_does)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    int rooms = 0;
    int offenders = 0;
    for (const ModuleDef& module : defs.modules()) {
        const bool offersBar = std::find(module.screens.begin(), module.screens.end(), StationScreen::Bar) !=
                               module.screens.end();
        if (module.family == ModuleFamily::Recreation) {
            ++rooms;
            if (!offersBar) {
                std::printf("  '%s' is recreation and offers no bar\n", module.id.c_str());
                ++offenders;
            }
            continue;
        }
        if (offersBar) {
            std::printf("  '%s' is not recreation and offers a bar\n", module.id.c_str());
            ++offenders;
        }
    }
    std::printf("  %d recreation module(s), %d offender(s)\n", rooms, offenders);
    // gdd.md §12 names five rooms. The floor is the anti-vacuity guard: a file
    // with no recreation rows in it would otherwise pass this test perfectly.
    SOL_REQUIRE(rooms >= 5);
    SOL_CHECK(offenders == 0);
}

// ⚑⚑⚑ AND THE LADDER, HANDED THE CASES THE GALAXY DECLINES TO PRODUCE. The room
// is the recreation module with the largest `power_draw`, which is a decision
// about a ladder that is already authored (2, 3, 4, 5, 8) rather than a second
// number that can drift out of step with the first. Stage 34-E's lesson is why
// this is a def-level test and not only a galaxy one: at the shipped seed the
// bigger rooms are rare, so a galaxy-level assertion about which room wins would
// be measuring almost nothing.
SOL_TEST(the_room_ladder_is_the_authored_power_draw_and_it_is_strictly_ordered)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    // The ladder gdd.md §12 lists, smallest room first.
    const char* const ladder[] = {
        "sol.mod_bar", "sol.mod_restaurant", "sol.mod_concourse", "sol.mod_casino", "sol.mod_resort"};
    const ModuleDef* previous = nullptr;
    for (const char* id : ladder) {
        const ModuleDef* module = defs.findModule(id);
        SOL_REQUIRE(module != nullptr);
        SOL_CHECK(module->family == ModuleFamily::Recreation);
        if (previous != nullptr) {
            // Strictly increasing, or "the largest draw" stops naming one room:
            // two rooms at the same figure would make the answer depend on the
            // order the composer happened to roll them in.
            std::printf("  %-22s draw %.1f\n", module->id.c_str(), static_cast<double>(module->powerDraw));
            SOL_CHECK(module->powerDraw > previous->powerDraw);
        }
        previous = module;
    }
}

// ⚑⚑ THE GALAXY THE RULING PRODUCES, MEASURED. Not a count sheet - the mix is
// resampled by every recipe row anybody adds, which is exactly what this stage
// did - so what is pinned is the SHAPE: common enough to meet early, scarce
// enough that a dock without one is a different kind of place, and the screen
// mask and the module list can never disagree.
SOL_TEST(the_shipped_galaxy_puts_a_room_on_about_half_its_docks)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    int stations = 0;
    int withRoom = 0;
    int offersBar = 0;
    int disagreements = 0;
    int withChoice = 0; // docks with more than one room, where the ladder decides
    int byRoom[5] = {};
    const char* const ladder[] = {
        "sol.mod_bar", "sol.mod_restaurant", "sol.mod_concourse", "sol.mod_casino", "sol.mod_resort"};
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            ++stations;
            const ModuleDef* room = game::stationRoom(world, defs, s, t);
            const bool masked = (world.stationScreens(s, t) & barBit()) != 0u;
            if (room != nullptr) {
                ++withRoom;
                for (int i = 0; i < 5; ++i) {
                    if (room->id == ladder[i]) {
                        ++byRoom[i];
                    }
                }
            }
            if (masked) {
                ++offersBar;
            }
            // ⚑⚑⚑ AND THE LADDER, ON THE DOCKS THAT ACTUALLY HAVE A CHOICE. The
            // def-level test above proves the ladder is ordered; this proves the
            // fill WALKS it in the right direction. Without this, changing
            // `stationRoom` from largest to SMALLEST leaves every count in this
            // file identical and the whole suite green - a station with a bar and
            // a restaurant would simply be called the wrong room, and nothing
            // else here could see it.
            if (room != nullptr) {
                float biggest = 0.0f;
                int roomsHere = 0;
                for (const std::uint32_t index : world.stationModules(s, t)) {
                    if (index < defs.modules().size() &&
                        defs.modules()[index].family == ModuleFamily::Recreation) {
                        ++roomsHere;
                        biggest = std::max(biggest, defs.modules()[index].powerDraw);
                    }
                }
                withChoice += roomsHere > 1 ? 1 : 0;
                SOL_CHECK(room->powerDraw == biggest);
            }
            // ⚑⚑ THE INVARIANT THAT MATTERS MORE THAN ANY COUNT HERE, and it is
            // the shape stage 34-C's refining guard already holds: the tab is on
            // the strip because of the mask, and the panel is filled from the
            // module list. If those two ever disagree, a player clicks Bar and
            // gets a screen with no room behind it - or a station with a casino
            // in it has no way in.
            if (masked != (room != nullptr)) {
                ++disagreements;
            }
        }
    }
    std::printf("  %d of %d dock(s) have a room; the mask says %d; %d have more than one\n",
                withRoom,
                stations,
                offersBar,
                withChoice);
    for (int i = 0; i < 5; ++i) {
        std::printf("    %-22s is the room at %3d dock(s)\n", ladder[i], byRoom[i]);
    }
    SOL_REQUIRE(stations == 125);
    // The anti-vacuity guard, in the same file as the claim (stage 34-D's rule).
    SOL_REQUIRE(withRoom > 0);
    // Anti-vacuity for the ladder check above: with no two-room dock anywhere,
    // `stationRoom` is never asked to choose and that assertion is true of
    // nothing. Stage 34-E's lesson, applied before it costs anything.
    SOL_REQUIRE(withChoice > 0);
    SOL_CHECK(disagreements == 0);
    SOL_CHECK(withRoom == offersBar);
    // Common, and not universal.
    SOL_CHECK(withRoom >= 40);
    SOL_CHECK(withRoom <= 100);
    SOL_CHECK(withRoom < stations);
}

// ⚑⚑⚑⚑ AND THE REASON THIS SOURCE SHIPS BEFORE THE LIVE-GALAXY ONE, ASSERTED
// RATHER THAN ARGUED. Stage B reads real shortages, real raids and real fronts.
// MEASURED over two sim hours at this seed: at t=0 the shortage, bounty and
// contest enumerators are ALL EMPTY - a fresh galaxy stocks every market at half
// capacity, so nothing is short, and nobody has raided anybody yet - and t=0 is
// exactly when a new player first docks. A room built only on stage B's sources
// would be silent at the one moment it is most likely to be walked into.
//
// So every line the house has is a fact about the dock, true the instant the
// galaxy exists, and this test docks at every station in the galaxy with a room
// and proves the screen is never empty at t=0.
SOL_TEST(no_room_in_the_shipped_galaxy_is_silent_on_the_day_it_opens)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::deque<std::string> text;
    std::vector<InfoRow> talk;
    StationPanel panel;
    int docked = 0;
    int silent = 0;
    int fewest = 1000;
    int most = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (game::stationRoom(world, defs, s, t) == nullptr) {
                continue;
            }
            // There is no dockAt(index): the only ways in are the player's
            // request and the dev shortcut, so this parks 100 m off the station
            // and takes the shortcut, exactly as catalog_gate_tests does.
            SOL_REQUIRE(world.enterSystem(s));
            SOL_REQUIRE(world.warpToStationOffset(t, {100.0, 0.0, 0.0}));
            SOL_REQUIRE(world.tryDockNearestStation(1000.0));
            SOL_REQUIRE(world.dockedStationIndex() == t);
            text.clear();
            game::fillStationBar(world, defs, text, panel, talk);
            ++docked;
            const int lines = static_cast<int>(panel.barTalk.size());
            silent += lines == 0 ? 1 : 0;
            fewest = std::min(fewest, lines);
            most = std::max(most, lines);
            // The heading names the room, always: a screen about a room whose
            // header says nothing is the empty-tab failure stage 34-C deleted.
            SOL_CHECK(panel.barRoom[0] != '\0');
            // ⚑⚑⚑⚑ THE TOPICS BY NAME, BECAUSE A LINE COUNT IS NOT A
            // CONTENT CHECK - AND THIS IS NOT A HYPOTHETICAL. This test first
            // shipped asserting only "at least three lines", and deleting the
            // warehouse line outright left the whole suite GREEN: the count fell
            // from four to three and sat exactly on the floor, because the plant
            // line took its place at most docks. A conserved total is not a
            // checksum. So the three answerable-everywhere topics are checked by
            // name, and the two conditional ones deliberately are not.
            for (const char* topic : {"The room", "The law", "The warehouse"}) {
                bool said = false;
                for (const InfoRow& line : panel.barTalk) {
                    said = said || std::strcmp(line.label, topic) == 0;
                }
                if (!said) {
                    std::printf("  %s says nothing about '%s'\n", panel.barRoom, topic);
                }
                SOL_CHECK(said);
                // And the topic is never a heading with nothing under it.
                for (const InfoRow& line : panel.barTalk) {
                    SOL_CHECK(line.value != nullptr && line.value[0] != '\0');
                }
            }
        }
    }
    std::printf("  docked at %d room(s): %d to %d line(s) each, %d silent\n", docked, fewest, most, silent);
    // Anti-vacuity again: a galaxy with no rooms would pass every check below.
    SOL_REQUIRE(docked > 0);
    SOL_CHECK(silent == 0);
    // The floor is kept beside the topic check above rather than instead of it:
    // it catches a room that loses lines it never named, and the loop above
    // catches a room that loses one it did. Neither is sufficient alone.
    SOL_CHECK(fewest >= 3);
}
