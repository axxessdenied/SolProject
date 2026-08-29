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

#include "space_world.hpp"

#include <cstring>
#include <string>

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
