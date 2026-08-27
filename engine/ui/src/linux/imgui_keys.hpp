#pragma once

// The Key -> ImGuiKey table (Phase 21 stage C), split out of the Wayland dev-UI
// backend so it can be tested without a compositor, a window or a GPU.
//
// ⚑⚑ SAME CLASS OF SILENT WRONGNESS AS THE xkb TABLE NEXT DOOR, ONE LAYER UP.
// `xkb_keys.cpp` answers "which Key is this physical key"; this answers "which
// key does ImGui think that is". A wrong row here compiles, links, runs, and
// shows nothing: the console simply will not backspace, or Home jumps to the
// end of the line, and no log says a word. So it is data a test can walk rather
// than a switch buried inside a NewFrame.
//
// ⚑ It exists at all only because there is no upstream `imgui_impl_wayland`.
// On Windows `imgui_impl_win32` owns the equivalent table and this file has no
// counterpart - which is why the suite that checks it is `if(LINUX)`.

#include "sol/platform/window.hpp"

#include <imgui.h>

#include <cstddef>

namespace sol::ui {

// One row: a key this engine knows about, and ImGui's name for the same key.
struct ImGuiKeyRow
{
    platform::Key key = platform::Key::Unknown;
    ImGuiKey imguiKey = ImGuiKey_None;
};

// The table itself, exposed so a test can assert over its SHAPE - that every
// Key is covered exactly once and no ImGuiKey is claimed twice. Probing
// `imguiKeyFor` one guessed key at a time can only find the rows somebody
// thought to ask about, which is the same blind spot the table has.
[[nodiscard]] const ImGuiKeyRow* imguiKeyRows();
[[nodiscard]] std::size_t imguiKeyRowCount();

// Table lookup. ImGuiKey_None for anything unmapped, which is what
// `io.AddKeyEvent` already treats as "ignore this".
[[nodiscard]] ImGuiKey imguiKeyFor(platform::Key key);

} // namespace sol::ui
