#include "imgui_keys.hpp"

namespace sol::ui {

namespace {

using platform::Key;

// Written out row by row, including the three stretches where both enums are
// contiguous and a loop would do. The tests below check that the contiguous
// stretches ARE monotonic, which is a property a formula would assert about
// itself rather than a property anything checked - and a table that is half
// data and half arithmetic cannot be walked.
//
// ⚑ Note what is NOT here, because the gap is deliberate. `Key` has one entry
// per MODIFIER PAIR (LeftShift covers Shift_R too - Win32's VK_SHIFT does the
// same, and `xkb_keys.cpp` maps both keysyms onto it), so the ImGuiKey_Right*
// rows have no `Key` to sit beside. ImGui is told about the SIDELESS mods
// separately, in dev_ui_wayland.cpp, which is what a shortcut actually reads.
constexpr ImGuiKeyRow kRows[] = {
    {Key::A, ImGuiKey_A},
    {Key::B, ImGuiKey_B},
    {Key::C, ImGuiKey_C},
    {Key::D, ImGuiKey_D},
    {Key::E, ImGuiKey_E},
    {Key::F, ImGuiKey_F},
    {Key::G, ImGuiKey_G},
    {Key::H, ImGuiKey_H},
    {Key::I, ImGuiKey_I},
    {Key::J, ImGuiKey_J},
    {Key::K, ImGuiKey_K},
    {Key::L, ImGuiKey_L},
    {Key::M, ImGuiKey_M},
    {Key::N, ImGuiKey_N},
    {Key::O, ImGuiKey_O},
    {Key::P, ImGuiKey_P},
    {Key::Q, ImGuiKey_Q},
    {Key::R, ImGuiKey_R},
    {Key::S, ImGuiKey_S},
    {Key::T, ImGuiKey_T},
    {Key::U, ImGuiKey_U},
    {Key::V, ImGuiKey_V},
    {Key::W, ImGuiKey_W},
    {Key::X, ImGuiKey_X},
    {Key::Y, ImGuiKey_Y},
    {Key::Z, ImGuiKey_Z},
    {Key::Num0, ImGuiKey_0},
    {Key::Num1, ImGuiKey_1},
    {Key::Num2, ImGuiKey_2},
    {Key::Num3, ImGuiKey_3},
    {Key::Num4, ImGuiKey_4},
    {Key::Num5, ImGuiKey_5},
    {Key::Num6, ImGuiKey_6},
    {Key::Num7, ImGuiKey_7},
    {Key::Num8, ImGuiKey_8},
    {Key::Num9, ImGuiKey_9},
    {Key::Escape, ImGuiKey_Escape},
    {Key::Space, ImGuiKey_Space},
    {Key::Enter, ImGuiKey_Enter},
    {Key::Tab, ImGuiKey_Tab},
    {Key::LeftShift, ImGuiKey_LeftShift},
    {Key::LeftControl, ImGuiKey_LeftCtrl},
    {Key::LeftAlt, ImGuiKey_LeftAlt},
    {Key::Up, ImGuiKey_UpArrow},
    {Key::Down, ImGuiKey_DownArrow},
    {Key::Left, ImGuiKey_LeftArrow},
    {Key::Right, ImGuiKey_RightArrow},
    {Key::Backspace, ImGuiKey_Backspace},
    {Key::Delete, ImGuiKey_Delete},
    {Key::Home, ImGuiKey_Home},
    {Key::End, ImGuiKey_End},
    {Key::F1, ImGuiKey_F1},
    {Key::F2, ImGuiKey_F2},
    {Key::F3, ImGuiKey_F3},
    {Key::F4, ImGuiKey_F4},
    {Key::F5, ImGuiKey_F5},
    {Key::F6, ImGuiKey_F6},
    {Key::F7, ImGuiKey_F7},
    {Key::F8, ImGuiKey_F8},
    {Key::F9, ImGuiKey_F9},
    {Key::F10, ImGuiKey_F10},
    {Key::F11, ImGuiKey_F11},
    {Key::F12, ImGuiKey_F12},
};

} // namespace

const ImGuiKeyRow* imguiKeyRows()
{
    return kRows;
}

std::size_t imguiKeyRowCount()
{
    return sizeof(kRows) / sizeof(kRows[0]);
}

ImGuiKey imguiKeyFor(platform::Key key)
{
    for (const ImGuiKeyRow& row : kRows) {
        if (row.key == key) {
            return row.imguiKey;
        }
    }
    return ImGuiKey_None;
}

} // namespace sol::ui
