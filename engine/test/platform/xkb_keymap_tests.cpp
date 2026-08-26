// The xkb keysym -> Key table (Phase 21 stage B). Linux-only, and headless by
// construction: the table is plain data, so none of this needs a compositor, a
// keyboard, or even a window.
//
// ⚑⚑ WHY THIS SUITE EXISTS AT ALL. Every other part of the Wayland backend
// announces its own failure - no display, no compositor, no surface, and the
// process says so and stops. A wrong row in this table does none of that. It
// compiles, links, runs, and waits for somebody to press that one key. The
// tests below are therefore about the SHAPE of the table rather than about any
// row's correctness, because shape is the part a reading reliably misses.

#include "../../platform/src/linux/xkb_keys.hpp"

#include <cstddef>

#include <sol/platform/window.hpp>
#include <sol/test/test.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

using sol::platform::expectedKeysymRowCount;
using sol::platform::Key;
using sol::platform::keyFromKeysym;
using sol::platform::KeysymRow;
using sol::platform::keysymRowCount;
using sol::platform::keysymRows;

namespace {

std::size_t rowsFor(Key key)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < keysymRowCount(); ++i) {
        if (keysymRows()[i].key == key) {
            ++count;
        }
    }
    return count;
}

} // namespace

// The mutation this catches is a Key with no keysym at all - a key that is
// bound in the settings file, drawn on the Controls screen, and simply never
// goes down. It is the failure with no symptom, so it gets the first test.
SOL_TEST(xkb_every_key_in_the_enum_has_a_keysym)
{
    for (int i = static_cast<int>(Key::Unknown) + 1; i < static_cast<int>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        SOL_CHECK(rowsFor(key) >= 1);
    }
}

// The other half of the same mutation: a keysym that appears twice means one
// of the two rows is unreachable, and which one depends on the scan order
// rather than on anything a reader would notice.
SOL_TEST(xkb_no_keysym_appears_in_two_rows)
{
    for (std::size_t i = 0; i < keysymRowCount(); ++i) {
        for (std::size_t j = i + 1; j < keysymRowCount(); ++j) {
            SOL_CHECK(keysymRows()[i].keysym != keysymRows()[j].keysym);
        }
    }
}

// ⚑ The test the spec asked for by name: two keysyms mapped to one Key. It
// needs the declared alias counts to have anything to fail against, because a
// spurious extra row passes both tests above - the table still covers every
// Key, and the stray keysym is still unique.
SOL_TEST(xkb_only_the_declared_aliases_share_a_key)
{
    for (int i = static_cast<int>(Key::Unknown) + 1; i < static_cast<int>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        SOL_CHECK(rowsFor(key) == expectedKeysymRowCount(key));
    }
    // And the aliases are not a synonym for "anything goes": exactly four Keys
    // are allowed a second row, and Win32 treats each of those four as one
    // input too (VK_SHIFT, VK_CONTROL, VK_MENU, VK_RETURN).
    SOL_CHECK(expectedKeysymRowCount(Key::Enter) == 2);
    SOL_CHECK(expectedKeysymRowCount(Key::LeftShift) == 2);
    SOL_CHECK(expectedKeysymRowCount(Key::LeftControl) == 2);
    SOL_CHECK(expectedKeysymRowCount(Key::LeftAlt) == 2);
    SOL_CHECK(expectedKeysymRowCount(Key::W) == 1);
    SOL_CHECK(expectedKeysymRowCount(Key::Unknown) == 0);
}

// The lookup and the table have to agree, or the tests above are asserting
// over data the backend does not actually consult.
SOL_TEST(xkb_lookup_returns_what_the_table_says)
{
    for (std::size_t i = 0; i < keysymRowCount(); ++i) {
        SOL_CHECK(keyFromKeysym(keysymRows()[i].keysym) == keysymRows()[i].key);
    }
}

// Level 0 is normally the unshifted spelling, so the capitals should never
// arrive - but a layout may put one there, and W is the throttle.
SOL_TEST(xkb_lookup_folds_capitals_onto_the_same_key)
{
    SOL_CHECK(keyFromKeysym(XKB_KEY_W) == Key::W);
    SOL_CHECK(keyFromKeysym(XKB_KEY_A) == Key::A);
    SOL_CHECK(keyFromKeysym(XKB_KEY_Z) == Key::Z);
}

// Unknown is a legal answer and the backend leans on it: an unmapped key must
// leave keyDown[] alone rather than index it with a garbage value.
SOL_TEST(xkb_unmapped_keysyms_are_unknown)
{
    SOL_CHECK(keyFromKeysym(XKB_KEY_F13) == Key::Unknown);
    SOL_CHECK(keyFromKeysym(XKB_KEY_Menu) == Key::Unknown);
    SOL_CHECK(keyFromKeysym(XKB_KEY_Caps_Lock) == Key::Unknown);
    // The keypad digits, deliberately: Win32 does not map VK_NUMPAD0-9 either,
    // so mapping them here would be a Linux-only binding nobody designed.
    SOL_CHECK(keyFromKeysym(XKB_KEY_KP_7) == Key::Unknown);
    SOL_CHECK(keyFromKeysym(XKB_KEY_NoSymbol) == Key::Unknown);
}

// The two spellings of Enter, which is the one alias a player will actually
// notice: the bookmark prompt submits on it.
SOL_TEST(xkb_both_enters_are_enter)
{
    SOL_CHECK(keyFromKeysym(XKB_KEY_Return) == Key::Enter);
    SOL_CHECK(keyFromKeysym(XKB_KEY_KP_Enter) == Key::Enter);
}

// Both shifts, both controls and both alts, for the same reason: LeftShift is
// boost, and a right-hand Shift that does not boost is a bug report.
SOL_TEST(xkb_left_and_right_modifiers_are_one_key)
{
    SOL_CHECK(keyFromKeysym(XKB_KEY_Shift_L) == Key::LeftShift);
    SOL_CHECK(keyFromKeysym(XKB_KEY_Shift_R) == Key::LeftShift);
    SOL_CHECK(keyFromKeysym(XKB_KEY_Control_L) == Key::LeftControl);
    SOL_CHECK(keyFromKeysym(XKB_KEY_Control_R) == Key::LeftControl);
    SOL_CHECK(keyFromKeysym(XKB_KEY_Alt_L) == Key::LeftAlt);
    SOL_CHECK(keyFromKeysym(XKB_KEY_Alt_R) == Key::LeftAlt);
}
