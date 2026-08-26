#include "input_actions.hpp"

#include <sol/test/test.hpp>

// Phase 20. Who is allowed to act on a keypress, pinned as a truth table.
//
// This rule used to live in main.cpp as a pair of bools, which is the one
// translation unit no suite can link (game/CMakeLists.txt builds sol_game from
// src/main.cpp alone). That is why the defect it carried survived nine
// playtests: not because it was subtle, but because nothing could assert it.
// Same linkage boundary Phase 19 hit with the cockpit resolution, same fix.

namespace {

// The case that was broken, and the reason the whole phase exists. Typing into
// the dev console fired every bound action AND ran the flight mapper, because
// nothing in the game's gates consulted ImGui's keyboard capture at all.
SOL_TEST(imgui_capture_in_flight_stops_gameplay_and_menus_alike)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, false, false, true);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(!routing.menus);
}

SOL_TEST(imgui_capture_outside_flight_stops_menus_too)
{
    // The dev console is drawn over the game, not inside it, so a menu screen
    // underneath must not read the keys either - or arrows typed into the
    // console would walk the pause menu's selection.
    const game::KeyboardRouting routing = game::routeKeyboard(false, false, false, true);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(!routing.menus);
}

// The asymmetry that a single bool could not express. The bookmark prompt is
// built out of menu keys - its Backspace, Home and Enter all arrive through
// the same edge helper the pause menu uses - so it needs the UI fed while the
// throttle stays shut. Collapsing these two answers back together is exactly
// the mutation that reintroduces the phase's defect.
SOL_TEST(the_bookmark_prompt_suppresses_gameplay_but_still_feeds_the_ui)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, false, true, false);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(routing.menus);
}

// ImGui wins over the game's own text field. A console opened on top of a
// half-typed bookmark name must not keep editing it from underneath.
SOL_TEST(imgui_capture_outranks_the_bookmark_prompt)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, false, true, true);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(!routing.menus);
}

SOL_TEST(an_unclaimed_keyboard_in_flight_flies_the_ship_and_nothing_else)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, false, false, false);
    SOL_CHECK(routing.gameplay);
    SOL_CHECK(!routing.menus);
}

SOL_TEST(an_unclaimed_keyboard_outside_flight_drives_the_menus_and_nothing_else)
{
    const game::KeyboardRouting routing = game::routeKeyboard(false, false, false, false);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(routing.menus);
}

// Jumping is a cutscene the player cannot steer out of. It suppresses gameplay
// only - a jump does not hand the keyboard to the menus.
SOL_TEST(a_jump_suppresses_gameplay_without_handing_the_keys_to_the_menus)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, true, false, false);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(!routing.menus);
}

} // namespace
