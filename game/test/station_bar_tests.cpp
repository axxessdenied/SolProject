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

#include "content.hpp"
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
#include <sol/sim/missions.hpp>
#include <sol/sim/pilot_tips.hpp>
#include <sol/test/test.hpp>

using game::GameContent;
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
//
// ⚑⚑ STAGE B MOVED THE SEAM THIS TEST DRIVES AND DID NOT WEAKEN THE CLAIM.
// `fillStationBar` is a presenter now, over talk `GameContent` composed when the
// player walked in, so the house's own lines are reached through
// `composeRoomLine` + `composeHouseTalk` - which is the same code the game runs,
// one call earlier. What it deliberately does NOT include is stage B's talk
// about the wider galaxy: at t=0 that is empty at 23 of the 62 rooms, and this
// test's whole subject is what a room has to say WITHOUT it.
SOL_TEST(no_room_in_the_shipped_galaxy_is_silent_on_the_day_it_opens)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    std::deque<std::string> text;
    std::vector<InfoRow> talk;
    std::vector<game::BarLine> house;
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
            house.clear();
            game::composeRoomLine(world, defs, s, t, house);
            game::composeHouseTalk(world, defs, s, t, house);
            game::fillStationBar(house,
                                 game::stationRoom(world, defs, s, t)->name.c_str(),
                                 world.stationCast(s, t)->name.c_str(),
                                 text,
                                 panel,
                                 talk);
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

// ---------------------------------------------------------------------------
// Stage B: what the room has to say about the galaxy around it.
//
// ⚑⚑⚑⚑ THE CLAIMS BELOW ARE MEASURED AGAINST THE SHIPPED GALAXY AND NOT
// AGAINST A FIXTURE, AND THAT IS A CORRECTION MADE DURING THE STAGE RATHER THAN
// A PREFERENCE. The first version of the war-front check lived in `sim.unit`
// over a synthetic four-system galaxy, and it printed `fronts in reach: 0`: the
// faction roster there is empty, nothing is ever contested, and every assertion
// in it ran over an empty list. It would have passed with the ungated and gated
// enumerators SWAPPED. Same shape as stage 34-E's mutation that did not fail -
// a galaxy-level assertion is only a guard for the cases that galaxy contains -
// so the claims moved to the galaxy that actually has wars in it, and every one
// of them carries its own floor.
// ---------------------------------------------------------------------------

namespace {

// Five sim minutes with the faction sim's decisions DRAINED. ⚑⚑ The drain is
// not optional and leaving it out is standing risk 6 in its exact shape:
// `SpaceWorld::tick` runs the faction sim's drift and decay and does NOT
// dispatch its decisions - `GameContent` drains `takeDueDecisions` and applies
// them - so a test that ticks the world alone reports zero raids and zero
// contests forever, which is precisely what this phase's first probe did for
// two sim hours before the drain was added.
void warmTheGalaxy(game::SpaceWorld& world, double seconds)
{
    std::vector<sol::sim::FactionDecision> decisions;
    for (double t = 0.0; t < seconds; t += 1.0 / 30.0) {
        world.tick(1.0 / 30.0);
        decisions.clear();
        world.factionSim().takeDueDecisions(decisions);
        for (const sol::sim::FactionDecision& decision : decisions) {
            world.applyDefaultFactionDecision(decision);
        }
    }
}

} // namespace

// ⚑⚑⚑ THE ONE THING A BAR CAN TELL YOU THAT A MISSION BOARD STRUCTURALLY
// CANNOT, WITH A NUMBER ON IT. `contestCandidates` only enumerates a fight the
// board's OWNER is a party to, because a station will not pay a pilot to help
// the faction taking its own system - a rule about who will PAY. A barkeep two
// jumps from a war needs nobody's permission to mention it, so stage B split
// the enumeration (`frontCandidates`) from the gate, and `contestCandidates` is
// now the first plus one filter: one definition of "a contest in reach", with
// the gate visible AS a gate.
//
// The floor below is what makes this a guard rather than a sentence: with no
// war anywhere in the galaxy every check here is true of nothing.
SOL_TEST(a_room_can_name_a_war_the_board_beside_it_would_have_gated_away)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));
    warmTheGalaxy(world, 300.0);

    std::vector<sol::sim::ContestCandidate> all;
    std::vector<sol::sim::ContestCandidate> gated;
    int rooms = 0;
    int roomsWithAFront = 0;
    int roomsTheBoardWouldSilence = 0;
    int totalFronts = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (game::stationRoom(world, defs, s, t) == nullptr) {
                continue;
            }
            ++rooms;
            world.missionSim().frontCandidates(world.galaxy(), world.factionSim(), s, all);
            world.missionSim().contestCandidates(
                world.galaxy(), world.factionSim(), s, world.systemOwnerFaction(s), gated);
            totalFronts += static_cast<int>(all.size());
            // The gate only ever narrows, and every row it keeps is a fight the
            // holder of this system is actually in.
            SOL_CHECK(gated.size() <= all.size());
            for (const sol::sim::ContestCandidate& c : gated) {
                SOL_CHECK(c.owner == world.systemOwnerFaction(s) ||
                          c.attacker == world.systemOwnerFaction(s));
            }
            roomsWithAFront += all.empty() ? 0 : 1;
            roomsTheBoardWouldSilence += (!all.empty() && gated.empty()) ? 1 : 0;
        }
    }
    std::printf("  %d room(s): %d can name a war, %d of those name one the board would gate away "
                "(%d front candidates in total)\n",
                rooms,
                roomsWithAFront,
                roomsTheBoardWouldSilence,
                totalFronts);
    SOL_REQUIRE(rooms > 0);
    // ⚑ THE FLOOR THAT MAKES THIS A GUARD. Without it the whole test is true of
    // an empty list, which is exactly how its first draft passed while proving
    // nothing at all.
    SOL_REQUIRE(totalFronts > 0);
    SOL_REQUIRE(roomsWithAFront > 0);
    // The dividend itself: rooms where the split is the difference between a
    // war being mentioned and not. Measured at 9-16 over five minutes to two
    // sim hours; asserted as a band, because a count sheet in a test is the
    // thing that goes stale one commit after it is written.
    SOL_CHECK(roomsTheBoardWouldSilence > 0);
    SOL_CHECK(roomsTheBoardWouldSilence < rooms);
}

// ⚑⚑ THE LADDER BUYS SENTENCES, NOT DISTANCE - which is a correction to stage
// A's own note, made when the re-read found `MissionParams::candidateReach`
// capping every candidate enumerator at three jumps for the mission board's
// reasons. A five-rung ladder cannot map onto three rungs of distance without
// being crushed, and cannot widen past the cap without moving a number the
// board also reads.
//
// ⚑ AND THE HONEST NUMBER, ASSERTED RATHER THAN GLOSSED: the shipped galaxy
// barely exercises this. 60 of the 62 rooms are a bar or a restaurant and get
// one line each. That is a fact about what this seed rolled, not about the
// rule, so the rule is checked against the DEFS - where the whole ladder
// exists - and the galaxy is only measured.
SOL_TEST(the_room_ladder_buys_lines_and_the_shipped_galaxy_barely_climbs_it)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    // Every rung, in the def data, in order. A bigger room is never worth
    // fewer lines than a smaller one, and the smallest is still worth one.
    int rungs = 0;
    int previous = 0;
    float previousDraw = 0.0f;
    std::vector<const ModuleDef*> family;
    for (const ModuleDef& module : defs.modules()) {
        if (module.family == ModuleFamily::Recreation) {
            family.push_back(&module);
        }
    }
    std::sort(family.begin(), family.end(), [](const ModuleDef* a, const ModuleDef* b) {
        return a->powerDraw < b->powerDraw;
    });
    for (const ModuleDef* room : family) {
        const int lines = game::roomTalkLines(*room);
        std::printf("    %-22s draw %4.1f -> %d line(s)\n",
                    room->name.c_str(),
                    static_cast<double>(room->powerDraw),
                    lines);
        SOL_CHECK(lines >= 1);
        SOL_CHECK(lines >= previous); // monotone in the authored ladder
        if (rungs > 0) {
            SOL_CHECK(room->powerDraw >= previousDraw);
        }
        previous = lines;
        previousDraw = room->powerDraw;
        ++rungs;
    }
    SOL_REQUIRE(rungs >= 5); // anti-vacuity: the whole family, not a survivor
    // The ladder is a ladder: the top rung buys strictly more than the bottom.
    SOL_CHECK(game::roomTalkLines(*family.back()) > game::roomTalkLines(*family.front()));

    // And what the galaxy actually rolled, printed rather than pinned.
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));
    int byLines[8] = {};
    int rooms = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const ModuleDef* room = game::stationRoom(world, defs, s, t);
            if (room == nullptr) {
                continue;
            }
            ++rooms;
            const int lines = game::roomTalkLines(*room);
            SOL_REQUIRE(lines >= 1 && lines < 8);
            ++byLines[lines];
        }
    }
    std::printf("  %d room(s) in the galaxy: 1 line at %d, 2 at %d, 3 at %d, 4 at %d\n",
                rooms,
                byLines[1],
                byLines[2],
                byLines[3],
                byLines[4]);
    SOL_REQUIRE(rooms > 0);
    // Every room is worth at least one line, which is what makes the budget a
    // budget rather than a gate a room can fail.
    SOL_CHECK(byLines[0] == 0);
}

// ⚑⚑⚑⚑ THE WHOLE PATH, THROUGH THE REAL `GameContent` AND THE SHIPPED
// `init.lua`, BECAUSE THE SEAM MOVED AND THE OLD TESTS CANNOT SEE IT. Stage A's
// tests drive the composers directly, which is right for the house's own lines
// and blind to everything stage B added: the Lua hook, the line budget, the
// fallback, and the rule about WHEN the talk is composed.
//
// ⚑⚑ AND THE CASE THE BINDING EXISTS FOR IS THE SECOND HALF OF THIS TEST. A
// docked LOAD clears `m_dockEventPending` on purpose - board offers came back
// with the save and are not re-rolled - so a bar armed by the dock event alone
// would open EMPTY on every loaded save. Arming on the BINDING self-heals that,
// and this proves it by consuming the event before the first tick, which is the
// state a load leaves behind.
SOL_TEST(the_room_composes_when_you_walk_in_and_does_not_move_while_you_stand_there)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed); // registers the component storages
    GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    // Find a dock with a room and stand in it.
    bool docked = false;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size() && !docked; ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (game::stationRoom(world, content.defs(), s, t) == nullptr) {
                continue;
            }
            SOL_REQUIRE(world.enterSystem(s));
            SOL_REQUIRE(world.warpToStationOffset(t, {100.0, 0.0, 0.0}));
            SOL_REQUIRE(world.tryDockNearestStation(1000.0));
            docked = true;
            break;
        }
    }
    SOL_REQUIRE(docked);

    // ⚑ The state a docked LOAD leaves: docked, and no dock event pending.
    // Under an event-driven rule the room would never speak.
    (void)world.consumeDockEvent();
    SOL_CHECK(content.barTalk().empty()); // nothing composed yet
    content.tick(1.0 / 30.0);
    SOL_REQUIRE(!content.barTalk().empty());
    SOL_CHECK(content.barRoom()[0] != '\0');

    // The house's own lines are still there, after stage B's talk rather than
    // instead of it - the establishing line first, the news, then the dock.
    SOL_CHECK(content.barTalk().front().topic == "The room");
    bool law = false;
    for (const game::BarLine& line : content.barTalk()) {
        law = law || line.topic == "The law";
        SOL_CHECK(!line.topic.empty());
        SOL_CHECK(!line.text.empty());
    }
    SOL_CHECK(law);

    // ⚑⚑⚑ RULED BY THE USER: composed when you walk in, and not moved again
    // until you undock. A conversation is not a readout, and re-opening the tab
    // is not a re-roll.
    //
    // ⚑⚑ AND THE GALAXY IS RUN UNDERNEATH IT, WHICH IS WHAT MAKES THIS A GUARD
    // RATHER THAN A TAUTOLOGY. The selection rules are deterministic, so with a
    // FROZEN world a bar recomposed on every single frame would produce exactly
    // the same sentences and this check would pass over the defect it exists to
    // catch. Five sim minutes is the interval that settles it: at t=0 the
    // shortage pool is empty galaxy-wide and five minutes later it is in the
    // thousands, so anything that recomposes will grow a "Short" line that was
    // not there when the player walked in.
    std::vector<game::BarLine> first(content.barTalk().begin(), content.barTalk().end());
    std::vector<sol::sim::FactionDecision> decisions;
    for (double elapsed = 0.0; elapsed < 300.0; elapsed += 1.0 / 30.0) {
        world.tick(1.0 / 30.0);
        decisions.clear();
        world.factionSim().takeDueDecisions(decisions);
        for (const sol::sim::FactionDecision& decision : decisions) {
            world.applyDefaultFactionDecision(decision);
        }
        content.tick(1.0 / 30.0);
    }
    // The galaxy really did move, or the paragraph above is describing nothing.
    {
        std::vector<sol::sim::HaulCandidate> shortages;
        world.missionSim().haulCandidates(world.galaxy(),
                                          world.economy(),
                                          world.currentSystemIndex(),
                                          world.dockedStationIndex(),
                                          shortages);
        std::printf("  five sim minutes later the dock stands on %d shortage(s)\n",
                    static_cast<int>(shortages.size()));
        SOL_REQUIRE(!shortages.empty());
    }
    SOL_REQUIRE(content.barTalk().size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        SOL_CHECK(content.barTalk()[i].topic == first[i].topic);
        SOL_CHECK(content.barTalk()[i].text == first[i].text);
    }
    std::printf("  %s said %d line(s) and held them through five sim minutes\n",
                content.barRoom(),
                static_cast<int>(first.size()));

    // ⚑⚑⚑⚑ AND THE LINE COUNT IS NOT THE CHECK, WHICH THIS FILE HAS
    // ALREADY PAID TO LEARN ONCE. Stage A measured the house's own lines at 4 to
    // 5 per room, so "5 lines" is exactly what a room with NO stage B talk at
    // all also looks like: the guard would be sitting on a total that another
    // line had quietly taken the place of. A conserved total is not a checksum.
    // So the stage's own topics are counted BY NAME, at every room in the
    // galaxy, and the invariant is exact rather than a floor - the room spends
    // its whole budget, and `bar_talk`'s own fallback is why even a room with
    // nothing live to report still says one thing.
    int rooms = 0;
    int roomsWithTalk = 0;
    int overBudget = 0;
    int quietNights = 0;
    int spoke = 0; // rooms where an authored character said something (stage C)
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const ModuleDef* room = game::stationRoom(world, content.defs(), s, t);
            if (room == nullptr) {
                continue;
            }
            SOL_REQUIRE(world.enterSystem(s));
            SOL_REQUIRE(world.warpToStationOffset(t, {100.0, 0.0, 0.0}));
            SOL_REQUIRE(world.tryDockNearestStation(1000.0));
            content.tick(1.0 / 30.0);
            ++rooms;
            int said = 0;
            int fromTheSpeaker = 0;
            for (const game::BarLine& line : content.barTalk()) {
                // ⚑ "Ask for" JOINED THIS LIST IN STAGE C and "They say" did
                // not, and the difference is the budget. The first four topics
                // and the fifth are lines about the wider galaxy, spent out of
                // `roomTalkLines`; a character's own line is a person
                // introducing themselves and is not billed to that ladder, so
                // counting it here would make every room with somebody written
                // in it look one line over budget.
                for (const char* topic : {"Short", "Trouble", "The war", "Leaving", "Ask for", "Talk"}) {
                    said += line.topic == topic ? 1 : 0;
                }
                quietNights += line.topic == "Talk" ? 1 : 0;
                fromTheSpeaker += line.topic == "They say" ? 1 : 0;
            }
            // At most one, and only where a `[[character]]` is seated: the hook
            // is armed with exactly one line and `cast.lua` returns for a
            // regular.
            SOL_CHECK(fromTheSpeaker <= 1);
            spoke += fromTheSpeaker;
            if (said == 0) {
                std::printf("  %s (%s) said nothing about the galaxy at all\n",
                            system.stations[t].name.c_str(),
                            room->name.c_str());
            }
            roomsWithTalk += said > 0 ? 1 : 0;
            overBudget += said > game::roomTalkLines(*room) ? 1 : 0;
            // The establishing line is always first, and the house's own lines
            // are always underneath - stage B inserts, it does not replace.
            SOL_CHECK(content.barTalk().front().topic == "The room");
            SOL_REQUIRE(world.undock());
        }
    }
    std::printf("  %d room(s) walked into: %d had something to say, %d over budget, "
                "%d quiet nights, %d with an authored voice in them\n",
                rooms,
                roomsWithTalk,
                overBudget,
                quietNights,
                spoke);
    SOL_REQUIRE(rooms > 0);
    // Every room says something about the galaxy, and none says more than the
    // room is worth. A budget nothing ever reaches is not a budget.
    SOL_CHECK(roomsWithTalk == rooms);
    SOL_CHECK(overBudget == 0);
    // ⚑ AND THE STAGE C FLOOR, HERE RATHER THAN IN ITS OWN FILE, because this
    // is the only test in the tree that walks the player into all 62 rooms
    // through the real hook: `characters.toml` and `cast.lua` are two files that
    // have to agree on six ids, and nothing else would notice if they stopped.
    SOL_CHECK(spoke > 0);
    SOL_CHECK(spoke < rooms); // and a regular is still a regular

    // Walking out empties the room, so nothing another dock said is left on the
    // screen - the same reason `openBoard` runs even where nothing is posted.
    content.tick(1.0 / 30.0);
    SOL_CHECK(content.barTalk().empty());
    SOL_CHECK(content.barRoom()[0] == '\0');
}

// ⚑⚑ THE BUILDERS REFUSE OUTSIDE THE HOOK, AND ONE NUMBER HOLDS BOTH REFUSALS.
// `pilot_hail`'s trio are guarded by `answeringHail()`, which the first answer
// closes, so "only inside the hook" and "answer with exactly one" are one fact.
// A room is the same shape with a budget instead of a bool: a script calling
// `sol.bar_shortage` from `on_tick` and a room that has already said its piece
// are the same refusal, so there is no second flag to leave out of step.
SOL_TEST(a_script_cannot_talk_its_way_into_a_free_rumour_from_outside_the_room)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed); // registers the component storages
    GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    SOL_CHECK(!content.composingBarTalk());
    SOL_CHECK(!content.sayBarLine(GameContent::BarFact::None, "Nice weather."));
    SOL_CHECK(!content.sayBarLine(GameContent::BarFact::Shortage, "They're short of"));
    SOL_CHECK(!content.sayBarLine(GameContent::BarFact::Raid, "Word is"));
    SOL_CHECK(!content.sayBarLine(GameContent::BarFact::Front, "There's fighting over"));
    SOL_CHECK(!content.sayBarLine(GameContent::BarFact::Hauler, "Somebody's taking"));
    SOL_CHECK(content.barTalk().empty());
}

// ⚑⚑⚑ THE STAGE'S HEADLINE MEASUREMENT: with the galaxy warm, what can a room
// actually say? The bands are asserted rather than the counts, deliberately -
// a count sheet in a test is the thing that went stale in `stations.toml` one
// commit after it was written - but the FLOORS are the point, because a
// selection rule that quietly stops selecting produces no error anywhere.
SOL_TEST(day_one_has_one_live_source_and_a_warm_galaxy_gives_every_room_something)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    SOL_REQUIRE(buildShippedGalaxy(defs, world));

    // ⚑⚑⚑⚑ THE DAY IT OPENS, FIRST, BECAUSE IT IS THE MEASUREMENT TWO RULINGS
    // REST ON AND NOTHING ELSE IN THE TREE ASSERTS IT. At t=0 the shortage, raid
    // and war enumerators are EMPTY galaxy-wide - a fresh galaxy stocks every
    // market at half capacity so nothing is short, and nobody has raided anybody
    // - so the departing hauler is the ONLY live source a brand-new game has.
    // And not one of those haulers is carrying anything yet, which is why
    // `chooseHaulerTalk` prefers a loaded run instead of requiring one: required,
    // it would have returned false at every room in the galaxy on day one.
    {
        std::vector<sol::sim::EscortCandidate> runs;
        std::vector<sol::sim::HaulCandidate> shortages;
        std::vector<sol::sim::BountyCandidate> warm;
        std::vector<sol::sim::ContestCandidate> wars;
        int rooms = 0, withARun = 0, withALoadedRun = 0, live = 0;
        std::uint32_t pick = 0;
        for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
            const sol::sim::SystemSpec& system = world.galaxy().systems[s];
            for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
                if (game::stationRoom(world, defs, s, t) == nullptr) {
                    continue;
                }
                ++rooms;
                const sol::sim::MissionSim& missions = world.missionSim();
                missions.haulCandidates(world.galaxy(), world.economy(), s, t, shortages);
                missions.bountyCandidates(world.galaxy(), world.factionSim(), s, warm);
                missions.frontCandidates(world.galaxy(), world.factionSim(), s, wars);
                missions.escortCandidates(world.galaxy(), world.economy(), world.factionSim(), s, runs);
                SOL_CHECK(shortages.empty());
                SOL_CHECK(warm.empty());
                SOL_CHECK(wars.empty());
                withARun += sol::sim::chooseHaulerTalk(runs, &pick) ? 1 : 0;
                for (const sol::sim::EscortCandidate& run : runs) {
                    withALoadedRun += run.cargo > 0.0f ? 1 : 0;
                }
                live += runs.empty() ? 0 : 1;
            }
        }
        std::printf("  day one: %d room(s), %d can point at a departing hauler, %d of those runs "
                    "are carrying anything, %d rooms with any live source at all\n",
                    rooms,
                    withARun,
                    withALoadedRun,
                    live);
        SOL_REQUIRE(rooms > 0);
        // The hauler is the only source with anything, and it says something.
        SOL_CHECK(withARun > 0);
        SOL_CHECK(withARun == live);
        // ⚑ And nothing is loaded yet, which is the whole reason laden is a
        // preference. If this ever becomes non-zero the rule can be tightened -
        // and this is the line that will say so.
        SOL_CHECK(withALoadedRun == 0);
        // A brand-new game leaves most rooms with nothing live at all, which is
        // why stage A's house lines ship underneath all of this.
        SOL_CHECK(live < rooms);
    }

    warmTheGalaxy(world, 300.0);

    std::vector<sol::sim::HaulCandidate> hauls;
    std::vector<sol::sim::BountyCandidate> raids;
    std::vector<sol::sim::ContestCandidate> fronts;
    std::vector<sol::sim::EscortCandidate> runs;
    int rooms = 0, canShort = 0, canRaid = 0, canFront = 0, canHauler = 0, mute = 0;
    std::uint32_t pick = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (game::stationRoom(world, defs, s, t) == nullptr) {
                continue;
            }
            ++rooms;
            const sol::sim::MissionSim& missions = world.missionSim();
            missions.haulCandidates(world.galaxy(), world.economy(), s, t, hauls);
            missions.bountyCandidates(world.galaxy(), world.factionSim(), s, raids);
            missions.frontCandidates(world.galaxy(), world.factionSim(), s, fronts);
            missions.escortCandidates(world.galaxy(), world.economy(), world.factionSim(), s, runs);
            const bool a = sol::sim::chooseShortageTalk(
                hauls, world.economy().markets(), world.survey(), world.worldSeconds(), &pick);
            const bool b = sol::sim::chooseRaidTalk(raids, world.survey(), &pick);
            const bool c = sol::sim::chooseFrontTalk(fronts, world.survey(), &pick);
            const bool d = sol::sim::chooseHaulerTalk(runs, &pick);
            canShort += a ? 1 : 0;
            canRaid += b ? 1 : 0;
            canFront += c ? 1 : 0;
            canHauler += d ? 1 : 0;
            mute += (a || b || c || d) ? 0 : 1;
        }
    }
    std::printf("  %d room(s) after five sim minutes: shortage %d, raid %d, war %d, hauler %d, "
                "nothing live %d\n",
                rooms,
                canShort,
                canRaid,
                canFront,
                canHauler,
                mute);
    SOL_REQUIRE(rooms > 0);
    // Every source has SOMETHING, which is what a floor is for: a rule that
    // quietly stops selecting looks exactly like a quiet galaxy.
    SOL_CHECK(canShort > 0);
    SOL_CHECK(canRaid > 0);
    SOL_CHECK(canFront > 0);
    SOL_CHECK(canHauler > 0);
    // ⚑ And none is universal, which is what keeps a room's news worth hearing:
    // a line every bar in the galaxy can say is wallpaper.
    SOL_CHECK(canFront < rooms);
    SOL_CHECK(canHauler < rooms);
    // A warm galaxy leaves nobody with nothing. At t=0 this number is 23 of 62,
    // which is why stage A's house lines ship underneath all of this.
    SOL_CHECK(mute == 0);
}
