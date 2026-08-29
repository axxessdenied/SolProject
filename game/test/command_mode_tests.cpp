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
        game::applyCommandMenu(viaMenu.world, static_cast<int>(i));
        SOL_CHECK(viaKey.world.commandMode() == viaMenu.world.commandMode());
    }
}

SOL_TEST(the_cancel_row_clears_whatever_is_running)
{
    Fixture fixture;
    const std::span<const game::CommandMenuEntry> entries = game::commandMenuEntries();
    int cancelIndex = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].mode == CommandMode::None) {
            cancelIndex = static_cast<int>(i);
        }
    }
    SOL_REQUIRE(cancelIndex >= 0);

    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Orbit));
    game::applyCommandMenu(fixture.world, cancelIndex);
    SOL_CHECK(fixture.world.commandMode() == CommandMode::None);
}

SOL_TEST(an_out_of_range_menu_index_does_nothing_rather_than_aborting)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Orbit));
    game::applyCommandMenu(fixture.world, -1);
    game::applyCommandMenu(fixture.world, 9999);
    SOL_CHECK(fixture.world.commandMode() == CommandMode::Orbit);
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
        const char* reason = "";
        if (entry.mode == CommandMode::None) {
            SOL_CHECK(!game::commandMenuEntryEnabled(fixture.world, entry, reason));
            SOL_CHECK(std::string_view(reason) == "nothing to cancel");
        }
    }

    // Engaged: the row for the mode you are already in goes off, the cancel
    // row comes on, and every other row stays available.
    SOL_REQUIRE(fixture.world.engageCommand(CommandMode::Orbit));
    for (const game::CommandMenuEntry& entry : entries) {
        const char* reason = "";
        const bool enabled = game::commandMenuEntryEnabled(fixture.world, entry, reason);
        if (entry.mode == CommandMode::Orbit) {
            SOL_CHECK(!enabled);
            SOL_CHECK(std::string_view(reason) == "already engaged");
        } else {
            SOL_CHECK(enabled);
        }
    }
}
