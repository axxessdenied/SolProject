#pragma once

// The xkb keysym -> Key table (Phase 21 stage B), split out of the Wayland
// backend so it can be tested without a compositor.
//
// ⚑⚑ THIS IS THE SINGLE LARGEST SOURCE OF SILENT WRONGNESS IN A KEYBOARD
// BACKEND, WHICH IS WHY IT IS A FILE RATHER THAN A SWITCH INSIDE A LISTENER.
// A mis-mapped key compiles, links, runs, and stays invisible until somebody
// presses it - there is nothing to see in a screenshot and nothing to see in a
// log. The table is therefore data a test can walk, not control flow.
//
// It is the Wayland counterpart of `translateVirtualKey` in the Win32 backend,
// and it is fed the LEVEL-0 keysym - the one the physical key produces with no
// modifiers - so that Shift+W still reports Key::W held, exactly as a Win32
// virtual key does.

#include "sol/platform/window.hpp"

#include <cstddef>
#include <cstdint>

namespace sol::platform {

// One row: the keysym a physical key produces at level 0, and what it means.
struct KeysymRow
{
    std::uint32_t keysym = 0;
    Key key = Key::Unknown;
};

// The table itself, exposed so the tests can assert over it rather than
// probing `keyFromKeysym` one guessed keysym at a time - a probe can only find
// the rows somebody thought to ask about, which is the same blind spot the
// table has.
[[nodiscard]] const KeysymRow* keysymRows();
[[nodiscard]] std::size_t keysymRowCount();

// Table lookup. Folds an upper-case ASCII keysym down first: level 0 is
// normally the unshifted spelling, but a layout is free to put the capital
// there and a dead `W` is not worth the two lines this saves.
[[nodiscard]] Key keyFromKeysym(std::uint32_t keysym);

// How many rows a Key is ALLOWED to have. One, except for the four inputs
// where two physical keys genuinely mean the same thing and Win32 already
// treats them as one: VK_SHIFT/VK_CONTROL/VK_MENU do not distinguish left from
// right, and VK_RETURN covers the keypad's Enter as well as the main one.
//
// ⚑ It exists so the "two keysyms mapped to one Key" mutation has something to
// fail against. Without it a stray extra row - the classic copy-paste typo -
// passes every other check in the suite: the table still covers every Key and
// still has no duplicate keysym.
[[nodiscard]] std::size_t expectedKeysymRowCount(Key key);

} // namespace sol::platform
