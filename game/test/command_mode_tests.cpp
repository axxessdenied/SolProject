// Ship commands (engine plan Phase 28 stage A): the vocabulary that replaced
// the autopilot bool, and the one decision in the phase a test can actually
// settle.
//
// ⚑⚑ THE ASYMMETRY IS THE POINT. Autopilot TERMINATES — it is going somewhere,
// so a nudge of the stick replaces its plan and cancels it, which is exactly
// what it has done since it existed. Every other mode is STANDING — it is a
// frame you are flying inside, so a nudge is layered on top of it and the order
// resumes when the stick comes back to centre. Those two behaviours share one
// guard and one threshold, and the only thing that separates them is
// isStandingCommand(), which is why it is worth pinning both halves here.

#include "command_menu.hpp"
#include "space_world.hpp"

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using game::CommandMode;
using sol::assets::DefDatabase;

namespace {

// The same minimal def set the save tests use, and for the same reason: the
// commodities are load-bearing rather than decorative (see save_format_tests).
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
base_price = 8.0

[[commodity]]
id = "sol.ore"
name = "Raw Ore"
base_price = 12.0
ore_weight_core = 1.0
ore_weight_frontier = 1.0
ore_weight_fringe = 1.0

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5
)";

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    explicit Fixture(std::uint64_t seed = 1701)
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "test_defs.toml", &error));
        world.spawn(seed);
        world.generateUniverse(defs);
    }
};

// A stick shove past the 0.25 threshold the guard has used since Phase 8.
sol::sim::FlightInput deflected()
{
    sol::sim::FlightInput input;
    input.linear.x = 1.0f;
    return input;
}

constexpr double kDt = 1.0 / 60.0;

} // namespace

SOL_TEST(command_classification_is_one_answer_in_one_place)
{
    // Three guards ask "is this standing?" and two ask "does this need a
    // target?". Both questions have exactly one implementation, so pin them.
    SOL_CHECK(!game::isStandingCommand(CommandMode::None));
    SOL_CHECK(!game::isStandingCommand(CommandMode::Autopilot));
    SOL_CHECK(game::isStandingCommand(CommandMode::Orbit));
    SOL_CHECK(game::isStandingCommand(CommandMode::MatchSpeed));
    SOL_CHECK(game::isStandingCommand(CommandMode::KeepDistance));
    SOL_CHECK(game::isStandingCommand(CommandMode::Hold));
    SOL_CHECK(game::isStandingCommand(CommandMode::Follow));

    // Hold is the one command about a PLACE rather than a thing, so it is the
    // one that survives having no target.
    SOL_CHECK(!game::commandNeedsTarget(CommandMode::None));
    SOL_CHECK(!game::commandNeedsTarget(CommandMode::Hold));
    SOL_CHECK(game::commandNeedsTarget(CommandMode::Autopilot));
    SOL_CHECK(game::commandNeedsTarget(CommandMode::Orbit));
}

SOL_TEST(engaging_a_command_replaces_the_one_before_it)
{
    Fixture fixture;
    SOL_CHECK(fixture.world.commandMode() == CommandMode::None);

    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Orbit));
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Orbit);

    // One slot, as it always was — this used to be a bool.
    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::KeepDistance));
    SOL_CHECK(fixture.world.commandMode() == CommandMode::KeepDistance);

    fixture.world.clearCommand();
    SOL_CHECK(fixture.world.commandMode() == CommandMode::None);
}

SOL_TEST(autopilot_still_answers_to_its_own_two_verbs)
{
    // The HUD, the pause menu and two Lua bindings all ask specifically about
    // autopilot rather than about commands, so those spellings keep working.
    Fixture fixture;
    SOL_REQUIRE(fixture.world.engageAutopilot());
    SOL_CHECK(fixture.world.autopilotActive());
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Autopilot);

    fixture.world.disengageAutopilot();
    SOL_CHECK(!fixture.world.autopilotActive());
    SOL_CHECK(fixture.world.commandMode() == CommandMode::None);

    // ...and it does NOT reach in and cancel somebody else's order.
    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Orbit));
    fixture.world.disengageAutopilot();
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Orbit);
}

SOL_TEST(a_nudge_cancels_autopilot_and_a_standing_order_survives_it)
{
    // ⚑⚑⚑ PHASE 28 DECISION 1, AND THE ONLY PART OF IT A TEST CAN SETTLE. The
    // rest — whether it FEELS right — is the first question its playtest asks.
    Fixture fixture;

    // Terminating: your input replaces the plan.
    SOL_REQUIRE(fixture.world.engageAutopilot());
    fixture.world.setShipInput(deflected());
    fixture.world.tick(kDt);
    SOL_CHECK(fixture.world.commandMode() == CommandMode::None);

    // Standing: your input is layered on top and the order is still there.
    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Orbit));
    fixture.world.setShipInput(deflected());
    fixture.world.tick(kDt);
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Orbit);

    // Still there after a while of being flown manually, not just one tick.
    for (int i = 0; i < 120; ++i) {
        fixture.world.tick(kDt);
    }
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Orbit);

    // And it takes the ship back the moment the stick centres: the applied
    // input stops being the player's.
    fixture.world.setShipInput(sol::sim::FlightInput{});
    fixture.world.tick(kDt);
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Orbit);
    SOL_CHECK(fixture.world.shipInput().assist); // steered, not passed through
}

SOL_TEST(a_standing_order_is_only_overridden_while_the_stick_is_held)
{
    // The override is per-tick and reads the CURRENT input, so nothing latches:
    // a deflection that ends leaves no trace on the mode or on what is flown.
    Fixture fixture;
    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::KeepDistance));

    fixture.world.setShipInput(deflected());
    fixture.world.tick(kDt);
    const sol::sim::FlightInput held = fixture.world.shipInput();
    SOL_CHECK(held.linear.x == 1.0f); // the player's own input, passed through

    fixture.world.setShipInput(sol::sim::FlightInput{});
    fixture.world.tick(kDt);
    SOL_CHECK(fixture.world.commandMode() == CommandMode::KeepDistance);
}

SOL_TEST(hold_engages_and_is_standing)
{
    // ⚑ Named for what it actually checks. It was first written as
    // "hold_needs_no_target_and_the_others_do", which promised a refusal this
    // test never makes: a generated galaxy always has nav targets, so the
    // no-target path is unreachable from this fixture. The classification test
    // above pins commandNeedsTarget's answer; the refusal itself is covered
    // where it is reachable, which is not here.
    Fixture fixture;
    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Hold));
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Hold);

    // Hold is standing, so a nudge does not end it either.
    fixture.world.setShipInput(deflected());
    fixture.world.tick(kDt);
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Hold);
}

// --- The context menu's contents (stage B) ---

SOL_TEST(the_menu_offers_the_same_vocabulary_the_keys_do)
{
    // ⚑ THE PHASE EXIT IS "fly the loop twice, once by menu and once by key,
    // and have both do the same things". That is only possible if the two are
    // built from one list. Every command mode must appear exactly once, plus
    // the cancel entry — and if somebody adds an eighth mode without adding a
    // row, this is what says so.
    const std::span<const game::CommandMenuEntry> entries = game::commandMenuEntries();

    int seen[7] = {};
    for (const game::CommandMenuEntry& entry : entries) {
        seen[static_cast<std::size_t>(entry.mode)] += 1;
    }
    SOL_CHECK(seen[static_cast<std::size_t>(CommandMode::None)] == 1); // the cancel row
    SOL_CHECK(seen[static_cast<std::size_t>(CommandMode::Autopilot)] == 1);
    SOL_CHECK(seen[static_cast<std::size_t>(CommandMode::Orbit)] == 1);
    SOL_CHECK(seen[static_cast<std::size_t>(CommandMode::MatchSpeed)] == 1);
    SOL_CHECK(seen[static_cast<std::size_t>(CommandMode::KeepDistance)] == 1);
    SOL_CHECK(seen[static_cast<std::size_t>(CommandMode::Hold)] == 1);
    SOL_CHECK(seen[static_cast<std::size_t>(CommandMode::Follow)] == 1);
}

SOL_TEST(a_menu_entry_and_its_key_binding_do_the_identical_thing)
{
    // Decision 4's real content: "the bound key and the menu entry do the
    // identical thing". Proven by doing both and comparing the resulting mode.
    const std::span<const game::CommandMenuEntry> entries = game::commandMenuEntries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].mode == CommandMode::None) {
            continue;
        }
        Fixture viaKey;
        Fixture viaMenu;
        SOL_REQUIRE(viaKey.world.engageCommand(entries[i].mode));
        game::applyCommandMenu(viaMenu.world, entries[i]);
        SOL_CHECK(viaKey.world.commandMode() == viaMenu.world.commandMode());
    }
}

SOL_TEST(the_cancel_row_clears_whatever_is_running)
{
    Fixture fixture;
    const game::CommandMenuEntry* cancel = nullptr;
    for (const game::CommandMenuEntry& entry : game::commandMenuEntries()) {
        if (entry.action == game::CommandMenuAction::Command && entry.mode == CommandMode::None) {
            cancel = &entry;
        }
    }
    SOL_REQUIRE(cancel != nullptr);

    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Orbit));
    game::applyCommandMenu(fixture.world, *cancel);
    SOL_CHECK(fixture.world.commandMode() == CommandMode::None);
}

// ⚑ "an_out_of_range_menu_index_does_nothing_rather_than_aborting" USED TO BE
// HERE AND WAS DELETED IN STAGE C, WHICH IS WORTH SAYING RATHER THAN DOING
// SILENTLY. It guarded a real hazard: a row number computed against one list and
// applied against another. Stage C removed the hazard instead of testing it —
// buildCommandMenu answers with the ENTRY, and the only index in the phase never
// leaves the function that drew the rows it indexes. The test below covers the
// ground that is left, which is that every row the menu will actually offer can
// be applied.

SOL_TEST(every_row_the_menu_offers_can_be_applied)
{
    Fixture fixture;
    std::vector<game::CommandMenuRow> rows;
    game::fillCommandMenu(fixture.world, rows);
    SOL_REQUIRE(!rows.empty());
    for (const game::CommandMenuRow& row : rows) {
        Fixture fresh;
        game::applyCommandMenu(fresh.world, row.entry);
    }
}

SOL_TEST(a_menu_row_says_why_it_cannot_be_chosen)
{
    // Decision 3 as a predicate. The reason matters as much as the disabling:
    // "Request Docking - 412 km" greyed out teaches the range rule, while a row
    // that silently vanishes teaches nothing.
    Fixture fixture;
    const std::span<const game::CommandMenuEntry> entries = game::commandMenuEntries();

    // Nothing engaged: the cancel row is off, and it says so.
    for (const game::CommandMenuEntry& entry : entries) {
        std::string reason;
        if (entry.mode == CommandMode::None) {
            SOL_CHECK(!game::commandMenuEntryEnabled(fixture.world, entry, reason));
            SOL_CHECK(reason == "nothing to cancel");
        }
    }

    // Engaged: the row for the mode you are already in goes off, the cancel
    // row comes on, and every other row stays available.
    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Orbit));
    for (const game::CommandMenuEntry& entry : entries) {
        std::string reason;
        const bool enabled = game::commandMenuEntryEnabled(fixture.world, entry, reason);
        if (entry.mode == CommandMode::Orbit) {
            SOL_CHECK(!enabled);
            SOL_CHECK(reason == "already engaged");
        } else {
            SOL_CHECK(enabled);
        }
    }
}

// --- What the world was asked (stage C) ---
//
// ⚑⚑ THE MENU IS NO LONGER A FIXED LIST, AND THESE PIN THE TWO RULES THAT
// REPLACED IT. A verb is OFFERED when it applies to the KIND of thing selected
// (docking to a station, hailing to a pilot); an offered verb is LIVE or greyed
// out by what is true RIGHT NOW. Both are predicates rather than colours, so
// every one of these runs with no font, no window and no UI context.

namespace {

// The first station slot in the target list, or kNoTarget. Stations lead the
// static head, so this is slot 0 in every generated system that has one — but
// asked through navTargetKind rather than assumed, because a fringe system can
// generate with no stations at all.
[[nodiscard]] std::size_t firstStationSlot(const game::SpaceWorld& world)
{
    for (std::size_t i = 0; i < world.navTargets().size(); ++i) {
        if (world.navTargetKind(i) == game::SpaceWorld::NavKind::Station) {
            return i;
        }
    }
    return game::SpaceWorld::kNoTarget;
}

[[nodiscard]] const game::CommandMenuRow* findRow(const std::vector<game::CommandMenuRow>& rows,
                                                  game::CommandMenuAction action)
{
    for (const game::CommandMenuRow& row : rows) {
        if (row.entry.action == action) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

SOL_TEST(the_menu_offers_docking_on_a_station_and_hailing_on_a_ship)
{
    // ⚑ Decision 3 governs whether a row is LIVE, not whether it is there at
    // all. "Hail - not a ship", greyed out on every station for ever, teaches
    // nothing — and a menu that is identical everywhere is not a context menu.
    Fixture fixture;
    const std::size_t station = firstStationSlot(fixture.world);
    SOL_REQUIRE(station != game::SpaceWorld::kNoTarget);
    SOL_REQUIRE(fixture.world.selectTarget(station));

    std::vector<game::CommandMenuRow> rows;
    game::fillCommandMenu(fixture.world, rows);
    SOL_CHECK(findRow(rows, game::CommandMenuAction::RequestDocking) != nullptr);
    SOL_CHECK(findRow(rows, game::CommandMenuAction::Hail) == nullptr);

    // A ship spawns 150-250 m ahead of the player, which is what makes the
    // in-range half of this reachable without moving anything.
    SOL_REQUIRE(!fixture.defs.ships().empty());
    (void)fixture.world.spawnPilotFromDef(fixture.defs.ships()[0], fixture.defs, game::PilotRole::Trader);
    SOL_REQUIRE(fixture.world.contactCount() > 0);
    SOL_REQUIRE(fixture.world.selectTarget(fixture.world.navTargets().size()));
    SOL_REQUIRE(fixture.world.currentTargetInfo().isShip);

    game::fillCommandMenu(fixture.world, rows);
    SOL_CHECK(findRow(rows, game::CommandMenuAction::Hail) != nullptr);
    SOL_CHECK(findRow(rows, game::CommandMenuAction::RequestDocking) == nullptr);
}

SOL_TEST(the_command_block_and_the_scan_pulse_are_in_every_menu)
{
    // The other half of the same rule: a manoeuvre is something you tell your
    // OWN ship, so there is nothing it fails to apply to — including a
    // right-click that hit nothing, which is what keeps Hold and Cancel
    // reachable from empty space.
    Fixture fixture;
    std::vector<game::CommandMenuRow> rows;
    const auto commandRowsPresent = [&] {
        game::fillCommandMenu(fixture.world, rows);
        int found = 0;
        for (const game::CommandMenuEntry& entry : game::commandMenuEntries()) {
            for (const game::CommandMenuRow& row : rows) {
                if (row.entry.action == game::CommandMenuAction::Command && row.entry.mode == entry.mode) {
                    ++found;
                }
            }
        }
        return found;
    };
    const int total = static_cast<int>(game::commandMenuEntries().size());

    SOL_CHECK(commandRowsPresent() == total);
    SOL_CHECK(findRow(rows, game::CommandMenuAction::ScanPulse) != nullptr);

    const std::size_t station = firstStationSlot(fixture.world);
    SOL_REQUIRE(station != game::SpaceWorld::kNoTarget);
    SOL_REQUIRE(fixture.world.selectTarget(station));
    SOL_CHECK(commandRowsPresent() == total);
    SOL_CHECK(findRow(rows, game::CommandMenuAction::ScanPulse) != nullptr);
}

SOL_TEST(the_docking_row_carries_its_range_and_comes_live_as_you_close)
{
    // ⚑⚑ THE STAGE'S OWN EXIT CRITERION, as far as a test can carry it: read
    // "Request Docking - 412.4 km" greyed out from too far away, then watch it
    // enable as you close. The label carries the FACT and the reason carries
    // the RULE, which is why the number moves and the words do not.
    Fixture fixture;
    const std::size_t station = firstStationSlot(fixture.world);
    SOL_REQUIRE(station != game::SpaceWorld::kNoTarget);
    const std::uint32_t stationIndex = fixture.world.navTargetStation(station);
    // Stations lead the static head, so a station slot IS its station index.
    SOL_REQUIRE(stationIndex == static_cast<std::uint32_t>(station));

    // Far out: greyed, and the row still says how far and why.
    SOL_REQUIRE(fixture.world.warpToStationOffset(stationIndex, {2.0e6, 0.0, 0.0}));
    SOL_REQUIRE(fixture.world.selectTarget(station));
    std::vector<game::CommandMenuRow> rows;
    game::fillCommandMenu(fixture.world, rows);
    const game::CommandMenuRow* docking = findRow(rows, game::CommandMenuAction::RequestDocking);
    SOL_REQUIRE(docking != nullptr);
    SOL_CHECK(!docking->enabled);
    SOL_CHECK(docking->reason == "within 20 km");
    SOL_CHECK(docking->label == "Request Docking - 2000.0 km");

    // Closed to 5 km: live, and still saying how far.
    SOL_REQUIRE(fixture.world.warpToStationOffset(stationIndex, {5000.0, 0.0, 0.0}));
    SOL_REQUIRE(fixture.world.selectTarget(station));
    game::fillCommandMenu(fixture.world, rows);
    docking = findRow(rows, game::CommandMenuAction::RequestDocking);
    SOL_REQUIRE(docking != nullptr);
    SOL_CHECK(docking->enabled);
    SOL_CHECK(docking->reason.empty());
    SOL_CHECK(docking->label == "Request Docking - 5000 m");
}

SOL_TEST(the_docking_row_hails_the_station_that_was_picked)
{
    // ⚑⚑⚑ THE ASSUMPTION THIS ROW RESTS ON, PINNED WHERE A COMMENT WOULD NOT
    // HOLD IT. requestDocking() hails the NEAREST station within 20 km and takes
    // no argument, while the row reports the range to the station the player
    // clicked. Those are the same station only because stations are sited
    // 100,000-400,000 km from their hub in random directions, so two of them
    // inside one 20 km bubble is a galaxy this generator does not make. If a
    // later phase ever places two stations close together, this is what says so
    // — and the fix is then to give requestDocking a station, not to reword the
    // label.
    Fixture fixture;
    const std::size_t station = firstStationSlot(fixture.world);
    SOL_REQUIRE(station != game::SpaceWorld::kNoTarget);
    const std::uint32_t stationIndex = fixture.world.navTargetStation(station);
    // Stations lead the static head, so a station slot IS its station index.
    SOL_REQUIRE(stationIndex == static_cast<std::uint32_t>(station));
    SOL_REQUIRE(fixture.world.warpToStationOffset(stationIndex, {5000.0, 0.0, 0.0}));
    SOL_REQUIRE(fixture.world.selectTarget(station));

    std::vector<game::CommandMenuRow> rows;
    game::fillCommandMenu(fixture.world, rows);
    const game::CommandMenuRow* docking = findRow(rows, game::CommandMenuAction::RequestDocking);
    SOL_REQUIRE(docking != nullptr);
    SOL_REQUIRE(docking->enabled);

    game::applyCommandMenu(fixture.world, docking->entry);
    std::uint32_t hailed = 0;
    double roll = 0.0;
    SOL_REQUIRE(fixture.world.takeDockRequest(hailed, roll));
    SOL_CHECK(hailed == stationIndex);

    // And once a clearance stands the row says so rather than asking twice.
    SOL_REQUIRE(fixture.world.grantDocking(stationIndex, 0, "Berth one is yours."));
    SOL_REQUIRE(fixture.world.selectTarget(station));
    game::fillCommandMenu(fixture.world, rows);
    docking = findRow(rows, game::CommandMenuAction::RequestDocking);
    SOL_REQUIRE(docking != nullptr);
    SOL_CHECK(!docking->enabled);
    SOL_CHECK(docking->reason == "already cleared for berth 1");
}

SOL_TEST(the_scan_pulse_row_greys_out_while_the_scanner_charges)
{
    // The one row whose fact is not a range. It is also the one verb in the
    // menu that is about the space around the ship rather than about the
    // selection, which is why it is offered even when nothing was hit.
    Fixture fixture;
    std::vector<game::CommandMenuRow> rows;
    game::fillCommandMenu(fixture.world, rows);
    const game::CommandMenuRow* pulse = findRow(rows, game::CommandMenuAction::ScanPulse);
    SOL_REQUIRE(pulse != nullptr);
    SOL_CHECK(pulse->enabled);

    game::applyCommandMenu(fixture.world, pulse->entry);
    game::fillCommandMenu(fixture.world, rows);
    pulse = findRow(rows, game::CommandMenuAction::ScanPulse);
    SOL_REQUIRE(pulse != nullptr);
    SOL_CHECK(!pulse->enabled);
    SOL_CHECK(pulse->reason == "charging (0%)");
}

SOL_TEST(a_docked_ship_is_offered_nothing_it_could_act_on)
{
    // Docked is the flat refusal, and it covers the two comms verbs as well as
    // the manoeuvres: a ship on a pad may not be given a flying order at all,
    // and "request docking" from inside the station is a question already
    // answered.
    Fixture fixture;
    const std::size_t station = firstStationSlot(fixture.world);
    SOL_REQUIRE(station != game::SpaceWorld::kNoTarget);
    const std::uint32_t stationIndex = fixture.world.navTargetStation(station);
    // Stations lead the static head, so a station slot IS its station index.
    SOL_REQUIRE(stationIndex == static_cast<std::uint32_t>(station));
    SOL_REQUIRE(fixture.world.warpToStationOffset(stationIndex, {100.0, 0.0, 0.0}));
    SOL_REQUIRE(fixture.world.tryDockNearestStation(1000.0));
    SOL_REQUIRE(fixture.world.isDocked());

    std::vector<game::CommandMenuRow> rows;
    game::fillCommandMenu(fixture.world, rows);
    SOL_REQUIRE(!rows.empty());
    for (const game::CommandMenuRow& row : rows) {
        SOL_CHECK(!row.enabled);
        SOL_CHECK(row.reason == "docked");
    }
}
