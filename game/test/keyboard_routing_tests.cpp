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
SOL_TEST(imgui_capture_in_flight_stops_every_consumer)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, false, true);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(!routing.menus);
    SOL_CHECK(!routing.shortcuts);
    SOL_CHECK(!routing.text);
}

SOL_TEST(imgui_capture_outside_flight_stops_every_consumer)
{
    // The dev console is drawn over the game, not inside it, so a menu screen
    // underneath must not read the keys either - or arrows typed into the
    // console would walk the pause menu's selection.
    const game::KeyboardRouting routing = game::routeKeyboard(false, false, true);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(!routing.menus);
    SOL_CHECK(!routing.shortcuts);
}

// The asymmetry that a single bool could not express. The bookmark prompt is
// built out of menu keys - its Backspace, Home and Enter all arrive through
// the same edge helper the pause menu uses - so it needs the UI fed while the
// throttle stays shut. Collapsing these two answers back together is exactly
// the mutation that reintroduces the phase's defect.
SOL_TEST(the_bookmark_prompt_suppresses_gameplay_but_still_feeds_the_ui)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, true, false);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(routing.menus);
    // And "i" in a bookmark name must not open the ship readout, which is the
    // case main.cpp's own comment called out long before this phase existed.
    SOL_CHECK(!routing.shortcuts);
}

// ImGui wins over the game's own text field. A console opened on top of a
// half-typed bookmark name must not keep editing it from underneath.
SOL_TEST(imgui_capture_outranks_the_bookmark_prompt)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, true, true);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(!routing.menus);
    SOL_CHECK(!routing.shortcuts);
}

SOL_TEST(an_unclaimed_keyboard_in_flight_flies_the_ship)
{
    const game::KeyboardRouting routing = game::routeKeyboard(true, false, false);
    SOL_CHECK(routing.gameplay);
    SOL_CHECK(!routing.menus);
    SOL_CHECK(routing.shortcuts);
}

SOL_TEST(an_unclaimed_keyboard_outside_flight_drives_the_menus)
{
    const game::KeyboardRouting routing = game::routeKeyboard(false, false, false);
    SOL_CHECK(!routing.gameplay);
    SOL_CHECK(routing.menus);
    SOL_CHECK(routing.shortcuts);
}

// The three consumers are genuinely three: for every pair there is a reachable
// state where they disagree. Without this a later reader could reasonably fold
// `shortcuts` back into one of the others and only a playtest would find out.
SOL_TEST(the_three_answers_are_not_two_answers_wearing_a_disguise)
{
    // gameplay and menus part company in both directions.
    const game::KeyboardRouting flying = game::routeKeyboard(true, false, false);
    SOL_CHECK(flying.gameplay && !flying.menus);
    const game::KeyboardRouting docked = game::routeKeyboard(false, false, false);
    SOL_CHECK(!docked.gameplay && docked.menus);
    // shortcuts outlives gameplay: the map and ship-readout keys open from a
    // station, where there is no gameplay to speak of.
    SOL_CHECK(docked.shortcuts && !docked.gameplay);
    // and shortcuts dies while menus live on, which is the bookmark prompt and
    // the only row in the table where those two part company.
    const game::KeyboardRouting naming = game::routeKeyboard(true, true, false);
    SOL_CHECK(naming.menus && !naming.shortcuts);
}

// ⚑⚑ Phase 21. Typed characters, which on Windows the message hook takes away
// before the window records them and on Wayland nothing does. Deleting this
// answer is a one-platform regression to exactly the defect Phase 20 fixed:
// the dev console would type into the bookmark prompt underneath it, and no
// Windows test - not one of the six above - would notice.
SOL_TEST(imgui_capture_takes_the_typed_characters_too)
{
    SOL_CHECK(!game::routeKeyboard(true, false, true).text);
    SOL_CHECK(!game::routeKeyboard(false, false, true).text);
    SOL_CHECK(!game::routeKeyboard(true, true, true).text);
}

// And it is not a synonym for `menus`, which is the fold a later reader would
// reach for. The bookmark prompt is the row where they agree; flight with no
// prompt open is the row where they do not, and collapsing them there would
// suppress text for a reason that has nothing to do with who is typing.
SOL_TEST(text_survives_where_menus_do_not)
{
    const game::KeyboardRouting flying = game::routeKeyboard(true, false, false);
    SOL_CHECK(flying.text && !flying.menus);
    const game::KeyboardRouting naming = game::routeKeyboard(true, true, false);
    SOL_CHECK(naming.text && naming.menus);
    const game::KeyboardRouting docked = game::routeKeyboard(false, false, false);
    SOL_CHECK(docked.text && docked.menus);
}

} // namespace
