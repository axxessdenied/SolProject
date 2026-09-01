// A lead is a mission (engine plan Phase 35 stage D): work heard in the room
// rather than read off a board, posted through `MissionSim` and held to the
// same validation a board contract is.
//
// ⚑⚑⚑⚑ THE ONE THING THIS STAGE MUST NOT DO IS MAKE THE TWO SURFACES THE SAME
// LIST, AND THE SPEC SAID SO IN WORDS BEFORE A LINE WAS WRITTEN. `sim.unit`
// holds that as a property of the data structure - two vectors under two clocks.
// This file holds the half `sim.unit` cannot see: what the SHIPPED galaxy does
// with it, through the real `GameContent`, the real `init.lua`, and 125 real
// docks. Every count below is printed and floored, because a selection rule
// that quietly stops selecting raises no error anywhere - which is the rule
// stage A wrote after finding that deleting a house fact left the suite green.

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
#include <sol/test/test.hpp>

using game::GameContent;
using game::StationScreenState;
using sol::assets::StationScreen;
using sol::ui::InfoRow;
using sol::ui::StationPanel;

namespace {

[[nodiscard]] constexpr std::uint32_t screenBit(StationScreen screen)
{
    return 1u << static_cast<std::uint32_t>(screen);
}

// Runs the galaxy forward the way `GameContent` does, decisions drained and all.
//
// ⚑ THE DRAIN IS NOT OPTIONAL AND THE PHASE'S OWN SPEC RECORDS WHY: a probe
// that ticks the world alone reports zero raids and zero contests forever,
// because `SpaceWorld::tick` runs the faction sim's drift and does not dispatch
// its decisions. The first run of the spec's measurement did exactly that and
// was wrong for two sim hours before anybody noticed.
void warm(game::SpaceWorld& world, GameContent& content, double seconds)
{
    std::vector<sol::sim::FactionDecision> decisions;
    for (double t = 0.0; t < seconds; t += 1.0 / 30.0) {
        world.tick(1.0 / 30.0);
        decisions.clear();
        world.factionSim().takeDueDecisions(decisions);
        for (const sol::sim::FactionDecision& decision : decisions) {
            world.applyDefaultFactionDecision(decision);
        }
        content.tick(1.0 / 30.0);
    }
}

// Parks 100 m off a station and takes the dev shortcut in, then runs one tick so
// `GameContent` composes the board and the room. There is no dockAt(index).
[[nodiscard]] bool walkInto(game::SpaceWorld& world,
                            GameContent& content,
                            std::uint32_t system,
                            std::uint32_t station)
{
    if (!world.enterSystem(system) || !world.warpToStationOffset(station, {100.0, 0.0, 0.0}) ||
        !world.tryDockNearestStation(1000.0)) {
        return false;
    }
    content.tick(1.0 / 30.0);
    return world.dockedStationIndex() == station;
}

} // namespace

// ⚑⚑⚑⚑ THE DEFECT THIS TEST EXISTS FOR WAS REAL, WAS SHIPPED INTO THE WORKING
// TREE, AND WAS FOUND BY A PROBE RATHER THAN BY A TEST. `openRoom` - which
// clears the last dock's lead - lived inside the composer, and the composer only
// runs at a dock that HAS a room. So a lead heard in a bar stayed posted, and
// stayed TAKEABLE, at every room-less dock the player visited afterwards.
// Measured before the fix: 71 docks offering a lead in a galaxy with 62 rooms,
// and 122 of 125 once the galaxy was warm.
//
// ⚑⚑ THE GENERAL FORM IS WORTH MORE THAN THE BUG: state is cleared where it is
// CLEARED, not where it is FILLED, because the filling path is only one of the
// ways out of a function and the other two here were "undocked" and "no room".
// `runMissionBoard` had already learned this - `openBoard` runs ahead of the
// facility gate, and its comment says exactly why.
SOL_TEST(work_is_only_ever_offered_in_a_room_and_never_follows_the_player_out)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));
    warm(world, content, 300.0);

    int docks = 0;
    int rooms = 0;
    int roomsWithWork = 0;
    int leadsWithoutARoom = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (!walkInto(world, content, s, t)) {
                continue;
            }
            ++docks;
            const bool room = (world.dockedStationScreens() & screenBit(StationScreen::Bar)) != 0u;
            const bool offered = !world.missionSim().leads().empty();
            rooms += room ? 1 : 0;
            roomsWithWork += (room && offered) ? 1 : 0;
            leadsWithoutARoom += (!room && offered) ? 1 : 0;
            SOL_REQUIRE(world.undock());
            content.tick(1.0 / 30.0);
            // Undocking unbinds the room, so nothing is takeable in flight.
            SOL_CHECK(world.missionSim().leads().empty());
        }
    }
    std::printf("  %d dock(s), %d room(s), %d offering work, %d leads with no room\n",
                docks,
                rooms,
                roomsWithWork,
                leadsWithoutARoom);
    SOL_REQUIRE(docks > 100);
    SOL_CHECK(rooms == 62);
    // The claim, and it is the one the defect broke.
    SOL_CHECK(leadsWithoutARoom == 0);
    // ... and the floor that stops it passing by never offering anything at
    // all, which is how the assertion above would go vacuous.
    SOL_CHECK(roomsWithWork >= 55);
    SOL_CHECK(roomsWithWork <= rooms);
}

// ⚑⚑⚑⚑ THE NUMBER THE SPEC's RISK SECTION ASKED FOR, AND IT ASKED FOR IT HERE:
// "the number to watch is how many docks in the galaxy offer no work of any kind
// before and after - it is one probe, and it belongs in stage D rather than in
// the record."
//
// ⚑⚑ IT IS A COST AS WELL AS A DIVIDEND, AND BOTH HALVES ARE ASSERTED. Phase 34
// stage C's dividend was that a dock which offers something is worth flying to;
// making the bar a second posting facility spends part of that. Measured on the
// shipped galaxy, warm: 54 docks of 125 offer no work at all today, and 28 do
// after this stage - so a silent dock goes from two in five to two in nine. That
// is the price of the ruling, recorded rather than discovered later.
SOL_TEST(a_bar_is_a_second_posting_facility_and_this_is_what_that_costs)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));
    warm(world, content, 300.0);

    int docks = 0;
    int silentBefore = 0; // no board offers
    int silentAfter = 0;  // no board offers and no lead either
    int boardless = 0;         // no mission board at all
    int boardlessWithRoom = 0; // ... and somewhere to hear about work instead
    int boardlessWithWork = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (!walkInto(world, content, s, t)) {
                continue;
            }
            ++docks;
            const bool board = (world.dockedStationScreens() & screenBit(StationScreen::Missions)) != 0u;
            const bool offers = !world.missionSim().offers().empty();
            const bool lead = !world.missionSim().leads().empty();
            silentBefore += offers ? 0 : 1;
            silentAfter += (offers || lead) ? 0 : 1;
            boardless += board ? 0 : 1;
            // ⚑ OF THE BOARD-LESS DOCKS, ONLY THE ONES WITH A ROOM ARE THIS
            // STAGE'S TO FIX, and the first draft of this test asserted over all
            // 54 of them - which is 62 rooms and 125 docks confused with each
            // other. 54 docks have no board; 26 of those have somewhere to
            // drink. The other 28 are outside this stage's reach by construction
            // and are exactly the docks that stay silent afterwards.
            const bool room = (world.dockedStationScreens() & screenBit(StationScreen::Bar)) != 0u;
            boardlessWithRoom += (!board && room) ? 1 : 0;
            boardlessWithWork += (!board && lead) ? 1 : 0;
            SOL_REQUIRE(world.undock());
            content.tick(1.0 / 30.0);
        }
    }
    std::printf("  %d docks: no work before %d -> after %d | %d boardless (%d with a room), %d hiring\n",
                docks,
                silentBefore,
                silentAfter,
                boardless,
                boardlessWithRoom,
                boardlessWithWork);
    SOL_REQUIRE(docks > 100);
    // The dividend: a dock with no board is where this is worth the most, and
    // essentially all of them now have something.
    SOL_CHECK(boardlessWithRoom >= 20);
    SOL_CHECK(boardlessWithRoom < boardless); // a room is not the common case
    SOL_CHECK(boardlessWithWork >= boardlessWithRoom - 2);
    // The cost: the stage really does roughly halve the silent docks, which is
    // exactly what makes finding one less of an event.
    SOL_CHECK(silentAfter < silentBefore);
    SOL_CHECK(silentAfter <= silentBefore / 2 + 5);
    // And a floor, so this cannot pass by the galaxy having gone quiet.
    SOL_CHECK(silentBefore >= 30);
}

// ⚑⚑⚑⚑ WHAT TELLS THE TWO SURFACES APART (ruled by the user, 2026-09-01): the
// RELATIONSHIP. A board gates its best work on standing with a faction; a room
// gates its scarcest work on `regard` - whether the person doing the talking
// knows you - which is the field stage C wrote, deliberately left unread, and
// said in its own header that stage D would spend.
//
// ⚑⚑ AND THE GATE IS ON *WHICH* WORK, NEVER ON *WHETHER THERE IS ANY*. That
// distinction is one this project has paid for three separate times: Phase 8u
// rep-gated the only war contract on a board and hid the feature from every new
// pilot, Phase 8x required cargo on the only escort and posted nothing at the
// player's own start station, and stage A's ruling was that the room is never
// mute. So the test asserts BOTH halves - the stranger is offered something, and
// it is not the war.
SOL_TEST(the_scarcest_work_in_a_room_goes_to_somebody_the_room_knows)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));
    warm(world, content, 300.0);

    // Every room, as a stranger: nobody is offered a war, and almost everybody
    // is offered something.
    int rooms = 0;
    int strangerOffered = 0;
    int strangerWars = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> roomDocks;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if ((world.stationScreens(s, t) & screenBit(StationScreen::Bar)) == 0u) {
                continue;
            }
            if (!walkInto(world, content, s, t)) {
                continue;
            }
            ++rooms;
            roomDocks.emplace_back(s, t);
            const std::vector<sol::sim::Mission>& leads = world.missionSim().leads();
            strangerOffered += leads.empty() ? 0 : 1;
            if (!leads.empty() && leads[0].objectives[0].kind == sol::sim::ObjectiveKind::Hold) {
                ++strangerWars;
            }
            SOL_REQUIRE(world.undock());
            content.tick(1.0 / 30.0);
        }
    }

    // Now the same rooms, as a regular. Nothing else about the galaxy moves.
    int regularOffered = 0;
    int regularWars = 0;
    int warsTheBoardCouldNotPost = 0;
    std::vector<sol::sim::ContestCandidate> gated;
    for (const auto& [s, t] : roomDocks) {
        world.adjustCastRegard(world.castKeyAt(s, t), GameContent::kRegardForFront);
        if (!walkInto(world, content, s, t)) {
            continue;
        }
        const std::vector<sol::sim::Mission>& leads = world.missionSim().leads();
        regularOffered += leads.empty() ? 0 : 1;
        if (!leads.empty() && leads[0].objectives[0].kind == sol::sim::ObjectiveKind::Hold) {
            ++regularWars;
            // ⚑ And the half that makes a war worth gating on the relationship:
            // it is the one piece of work the board on this very dock is
            // structurally forbidden to sell, because `contestCandidates` gates
            // on the station owner being a party to the fight.
            world.missionSim().contestCandidates(
                world.galaxy(), world.factionSim(), s, world.systemOwnerFaction(s), gated);
            const std::uint32_t fought = leads[0].objectives[0].system;
            warsTheBoardCouldNotPost +=
                std::any_of(gated.begin(),
                            gated.end(),
                            [fought](const sol::sim::ContestCandidate& c) { return c.system == fought; })
                    ? 0
                    : 1;
        }
        SOL_REQUIRE(world.undock());
        content.tick(1.0 / 30.0);
    }

    std::printf("  %d room(s): stranger offered %d (wars %d) | regular offered %d (wars %d, %d "
                "unpostable next door)\n",
                rooms,
                strangerOffered,
                strangerWars,
                regularOffered,
                regularWars,
                warsTheBoardCouldNotPost);
    SOL_REQUIRE(rooms == 62);
    // The gate holds: a stranger is never pointed at a war.
    SOL_CHECK(strangerWars == 0);
    // It is a tier and not a lockout: a stranger is still offered work almost
    // everywhere, which is the whole difference from Phase 8u's mistake.
    SOL_CHECK(strangerOffered >= rooms - 3);
    // Being a regular buys something real, and it is not nothing.
    SOL_CHECK(regularWars >= 20);
    SOL_CHECK(regularOffered >= strangerOffered);
    // ... and a good share of it could not have been bought off the board
    // standing three metres away.
    SOL_CHECK(warsTheBoardCouldNotPost >= 8);
}

// ⚑⚑⚑⚑ THE PHASE'S EXIT CRITERION, WHOLE: "a lead taken in a bar completes as a
// mission on a station with no mission board". All four beats are here - the
// dock has a room and no board, the work is offered, taking it puts it in the
// journal, and the journal is what puts the Missions tab on the strip at a dock
// whose station never had one.
//
// ⚑⚑ THE LAST BEAT IS PHASE 34 STAGE C's RULE PAYING OFF WITHOUT BEING TOUCHED:
// "a tab is on the strip when the station is equipped for it, OR when the
// player's own half of it has something in it". A board is a facility and a
// journal is yours, and that sentence is the only reason a lead taken at a
// board-less dock is visible anywhere afterwards.
SOL_TEST(work_taken_in_a_room_is_in_the_journal_at_a_dock_with_no_board)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));
    warm(world, content, 300.0);

    bool found = false;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size() && !found; ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size() && !found; ++t) {
            const std::uint32_t screens = world.stationScreens(s, t);
            if ((screens & screenBit(StationScreen::Bar)) == 0u ||
                (screens & screenBit(StationScreen::Missions)) != 0u) {
                continue; // wanted: a room, and no board
            }
            if (!walkInto(world, content, s, t) || world.missionSim().leads().empty()) {
                continue;
            }
            found = true;

            // The board really is silent here, which is the premise.
            SOL_CHECK(world.missionSim().offers().empty());
            const std::string title = world.missionSim().leads()[0].title;
            const std::uint64_t who = world.castKeyAt(s, t);
            const sol::sim::MissionSim& missions = world.missionSim();
            const std::size_t before = missions.active().size();

            // Take it.
            SOL_REQUIRE(world.acceptLead(0));
            SOL_CHECK(missions.leads().empty());
            SOL_REQUIRE(missions.active().size() == before + 1);
            SOL_CHECK(missions.active().back().title == title);

            // ⚑ And the room remembers being asked. This is where `regard`
            // moves, and it moves HERE rather than on completion for a reason
            // that is a save-format fact: a `MissionEvent` carries a `Mission`
            // snapshot, `m_active` is saved, and for the journal to know whose
            // lead it was carrying, `Mission` would need a field - the second
            // `kSaveVersion` bump this phase was ruled not to pay.
            const game::SpaceWorld::CastMemory* memory = world.castMemory(who);
            SOL_REQUIRE(memory != nullptr);
            SOL_CHECK(memory->regard >= 1);

            // The tab appears, on a station that offers no board at all.
            std::deque<std::string> text;
            std::vector<sol::ui::MissionRow> offerRows;
            std::vector<sol::ui::MissionRow> journalRows;
            StationPanel panel;
            panel.screens = screens;
            game::fillStationMissions(world, text, panel, offerRows, journalRows);
            SOL_CHECK(panel.missionOffers.empty());
            SOL_REQUIRE(!panel.missionJournal.empty());
            SOL_CHECK(game::stationTabOnStrip(panel, StationScreenState::Missions));
        }
    }
    SOL_CHECK(found);
}

// ⚑⚑⚑ THE ROW IS THE OFFER, AND THAT IS THE OTHER HALF OF "not the same list".
// A board contract is a row in a table under a heading that says Board; a lead
// is a thing somebody in the room SAID, with a button on the end of the
// sentence. `InfoRow` has carried the button and the opaque action string since
// Phase 31 stage C3, and stage A's note on `StationPanel::barTalk` predicted
// this stage would want them - so nothing in the UI moved to make this work.
//
// ⚑ EXACTLY ONE ROW, checked by name rather than by count: `barTalk` is five or
// six sentences and a total is not a checksum, which is the instrument stage A
// built after deleting a house fact left the whole suite green.
SOL_TEST(the_offer_is_a_row_in_the_conversation_and_only_one_row_carries_it)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));
    warm(world, content, 300.0);

    std::deque<std::string> text;
    std::vector<InfoRow> rows;
    StationPanel panel;
    int roomsDrawn = 0;
    int roomsWithAButton = 0;
    int roomsWithMoreThanOne = 0;
    int workTopicWithoutAButton = 0;
    int buttonWithoutAWorkTopic = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if ((world.stationScreens(s, t) & screenBit(StationScreen::Bar)) == 0u) {
                continue;
            }
            if (!walkInto(world, content, s, t)) {
                continue;
            }
            ++roomsDrawn;
            text.clear();
            game::fillStationBar(content.barTalk(), content.barRoom(), content.barKeeper(), text, panel, rows);
            int buttons = 0;
            for (const InfoRow& row : panel.barTalk) {
                const bool takeable = row.action[0] != '\0';
                buttons += takeable ? 1 : 0;
                const bool work = std::strcmp(row.label, "Work") == 0;
                workTopicWithoutAButton += (work && !takeable) ? 1 : 0;
                buttonWithoutAWorkTopic += (takeable && !work) ? 1 : 0;
                if (takeable) {
                    // The button says what it does, and the action is the index
                    // the room's own list will be asked for.
                    SOL_CHECK(std::strcmp(row.button, "Take it") == 0);
                    SOL_CHECK(std::atoi(row.action) == content.barLead());
                    SOL_CHECK(row.value[0] != '\0'); // the hook's own title
                } else {
                    SOL_CHECK(row.button[0] == '\0');
                }
            }
            roomsWithAButton += buttons > 0 ? 1 : 0;
            roomsWithMoreThanOne += buttons > 1 ? 1 : 0;
            // A room with work has a lead behind the row, and one without has
            // no row - the two sides of the same fact.
            SOL_CHECK((buttons > 0) == !world.missionSim().leads().empty());
            SOL_REQUIRE(world.undock());
            content.tick(1.0 / 30.0);
        }
    }
    std::printf("  %d room(s) drawn, %d with a Take it, %d with more than one\n",
                roomsDrawn,
                roomsWithAButton,
                roomsWithMoreThanOne);
    SOL_REQUIRE(roomsDrawn == 62);
    SOL_CHECK(roomsWithAButton >= 55);
    SOL_CHECK(roomsWithMoreThanOne == 0);
    // The topic and the button are one fact, in both directions.
    SOL_CHECK(workTopicWithoutAButton == 0);
    SOL_CHECK(buttonWithoutAWorkTopic == 0);
}

// ⚑⚑⚑ A QUIET NIGHT IS A REAL ANSWER (ruled by the user, 2026-09-01), AND DAY
// ONE IS WHEN IT HAPPENS. Measured at t=0: three of the four live sources are
// empty galaxy-wide - every market opens at half capacity so nothing is short,
// and nobody has raided anybody - so the only work in the entire galaxy is a
// hauler leaving, and 24 of the 62 rooms have none of that either.
//
// ⚑⚑ THE ROOM IS STILL NOT MUTE, WHICH IS THE DISTINCTION THE RULING TURNS ON:
// stage A's house lines are true the instant the galaxy exists, so a room with
// no work still has a conversation. Nothing is invented to fill the gap -
// `postLead` exists precisely to refuse work the sim did not generate, and a
// fallback that made some up would defeat the guarantee this stage is built on.
SOL_TEST(a_room_with_nothing_to_offer_on_day_one_still_has_something_to_say)
{
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    int rooms = 0;
    int offering = 0;
    int quiet = 0;
    int quietAndMute = 0;
    int notAnEscort = 0;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if ((world.stationScreens(s, t) & screenBit(StationScreen::Bar)) == 0u) {
                continue;
            }
            if (!walkInto(world, content, s, t)) {
                continue;
            }
            ++rooms;
            const std::vector<sol::sim::Mission>& leads = world.missionSim().leads();
            if (leads.empty()) {
                ++quiet;
                // Quiet about WORK is not silent: the house still talks.
                quietAndMute += content.barTalk().empty() ? 1 : 0;
            } else {
                ++offering;
                notAnEscort +=
                    leads[0].objectives[0].kind == sol::sim::ObjectiveKind::Escort ? 0 : 1;
            }
            SOL_REQUIRE(world.undock());
            content.tick(1.0 / 30.0);
        }
    }
    std::printf("  day one: %d room(s), %d offering work, %d quiet, %d of the offers not an escort\n",
                rooms,
                offering,
                quiet,
                notAnEscort);
    SOL_REQUIRE(rooms == 62);
    // A real share of the galaxy has no work on the day it opens, and that is
    // the answer rather than a gap to be filled.
    SOL_CHECK(quiet >= 15);
    // Not one of them is silent.
    SOL_CHECK(quietAndMute == 0);
    // ⚑ And the day-one shape, which is the reason the escort lead carries no
    // danger floor: at t=0 a departing hauler is the ONLY work the sim has
    // generated anywhere in the galaxy. `mission_board` refuses runs under 0.05
    // danger; a room that copied that rule would open on nothing at all.
    SOL_CHECK(offering >= 20);
    SOL_CHECK(notAnEscort == 0);
}
