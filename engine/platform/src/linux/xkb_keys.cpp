#include "xkb_keys.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace sol::platform {

namespace {

// Written out rather than generated from arithmetic even where the keysyms are
// contiguous ASCII. The Win32 twin uses arithmetic for A-Z, 0-9 and F1-F12
// because a VK code IS the character; a keysym only happens to be, and a table
// that is half data and half formula cannot be walked by a test.
constexpr KeysymRow kRows[] = {
    {XKB_KEY_a, Key::A},
    {XKB_KEY_b, Key::B},
    {XKB_KEY_c, Key::C},
    {XKB_KEY_d, Key::D},
    {XKB_KEY_e, Key::E},
    {XKB_KEY_f, Key::F},
    {XKB_KEY_g, Key::G},
    {XKB_KEY_h, Key::H},
    {XKB_KEY_i, Key::I},
    {XKB_KEY_j, Key::J},
    {XKB_KEY_k, Key::K},
    {XKB_KEY_l, Key::L},
    {XKB_KEY_m, Key::M},
    {XKB_KEY_n, Key::N},
    {XKB_KEY_o, Key::O},
    {XKB_KEY_p, Key::P},
    {XKB_KEY_q, Key::Q},
    {XKB_KEY_r, Key::R},
    {XKB_KEY_s, Key::S},
    {XKB_KEY_t, Key::T},
    {XKB_KEY_u, Key::U},
    {XKB_KEY_v, Key::V},
    {XKB_KEY_w, Key::W},
    {XKB_KEY_x, Key::X},
    {XKB_KEY_y, Key::Y},
    {XKB_KEY_z, Key::Z},
    {XKB_KEY_0, Key::Num0},
    {XKB_KEY_1, Key::Num1},
    {XKB_KEY_2, Key::Num2},
    {XKB_KEY_3, Key::Num3},
    {XKB_KEY_4, Key::Num4},
    {XKB_KEY_5, Key::Num5},
    {XKB_KEY_6, Key::Num6},
    {XKB_KEY_7, Key::Num7},
    {XKB_KEY_8, Key::Num8},
    {XKB_KEY_9, Key::Num9},
    {XKB_KEY_Escape, Key::Escape},
    {XKB_KEY_space, Key::Space},
    {XKB_KEY_Return, Key::Enter},
    {XKB_KEY_KP_Enter, Key::Enter},
    {XKB_KEY_Tab, Key::Tab},
    {XKB_KEY_Shift_L, Key::LeftShift},
    {XKB_KEY_Shift_R, Key::LeftShift},
    {XKB_KEY_Control_L, Key::LeftControl},
    {XKB_KEY_Control_R, Key::LeftControl},
    {XKB_KEY_Alt_L, Key::LeftAlt},
    {XKB_KEY_Alt_R, Key::LeftAlt},
    {XKB_KEY_Up, Key::Up},
    {XKB_KEY_Down, Key::Down},
    {XKB_KEY_Left, Key::Left},
    {XKB_KEY_Right, Key::Right},
    {XKB_KEY_BackSpace, Key::Backspace},
    {XKB_KEY_Delete, Key::Delete},
    {XKB_KEY_Home, Key::Home},
    {XKB_KEY_End, Key::End},
    {XKB_KEY_F1, Key::F1},
    {XKB_KEY_F2, Key::F2},
    {XKB_KEY_F3, Key::F3},
    {XKB_KEY_F4, Key::F4},
    {XKB_KEY_F5, Key::F5},
    {XKB_KEY_F6, Key::F6},
    {XKB_KEY_F7, Key::F7},
    {XKB_KEY_F8, Key::F8},
    {XKB_KEY_F9, Key::F9},
    {XKB_KEY_F10, Key::F10},
    {XKB_KEY_F11, Key::F11},
    {XKB_KEY_F12, Key::F12},
};

} // namespace

const KeysymRow* keysymRows()
{
    return kRows;
}

std::size_t keysymRowCount()
{
    return sizeof(kRows) / sizeof(kRows[0]);
}

Key keyFromKeysym(std::uint32_t keysym)
{
    if (keysym >= XKB_KEY_A && keysym <= XKB_KEY_Z) {
        keysym += XKB_KEY_a - XKB_KEY_A;
    }
    for (const KeysymRow& row : kRows) {
        if (row.keysym == keysym) {
            return row.key;
        }
    }
    return Key::Unknown;
}

std::size_t expectedKeysymRowCount(Key key)
{
    switch (key) {
    case Key::Enter:       // Return and KP_Enter
    case Key::LeftShift:   // Shift_L and Shift_R
    case Key::LeftControl: // Control_L and Control_R
    case Key::LeftAlt:     // Alt_L and Alt_R
        return 2;
    case Key::Unknown:
    case Key::Count:
        return 0;
    default:
        return 1;
    }
}

} // namespace sol::platform
