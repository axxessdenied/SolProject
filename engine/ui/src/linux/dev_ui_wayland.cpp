// Wayland half of the ImGui platform backend (Phase 21 stage C). Sanctioned
// platform-specific spot outside sol::platform (AGENTS.md section 4), same
// standing as the Win32 twin.
//
// ⚑⚑ THERE IS NO UPSTREAM `imgui_impl_wayland`. `imgui_impl_win32` has no
// counterpart, so the Win32 file's one-line delegations have nothing to
// delegate to and this backend is written by hand. Only the PLATFORM half is
// missing - `ImGui_ImplVulkan_*`, the renderer half, is portable and untouched.
//
// ⚑⚑⚑ IT POLLS, IT DOES NOT LISTEN, AND THAT IS THE WHOLE DESIGN.
// `devUiPlatformMessageHook` takes (windowHandle, message, wParam, lParam) -
// the Win32 message SHAPE, which window.hpp:104 admits to mirroring while
// avoiding the Win32 types. Wayland has no such message. Phase 21 decision 2
// chose against refactoring that seam (it would mean rewriting the working
// Win32 path on the platform this machine tests worst), so the hook here is a
// permanent no-op and `NewFrame` reads what `Window` already exposes publicly:
// isKeyDown, mousePosition, isMouseButtonDown, wheelDelta and textInput.
//
// The honest cost of polling, recorded so nobody reports it later as a bug: a
// key pressed AND released inside a single frame never reaches the overlay.
// The game's own input is unaffected - it already reads that same polled state.
//
// ⚑⚑ WHAT MAKES THE POLLING SAFE IS THE PUMP ORDER, NOT THIS FILE.
// `pumpEvents()` CLEARS wheelDelta and textInput at its start and refills them,
// so both are per-frame quantities that survive exactly until the next pump.
// This function runs from `ImGuiHost::beginFrame()`, which the frame loop calls
// after the pump and before the next one - so it sees this frame's scroll and
// this frame's typing. Called anywhere else it would read an emptied buffer and
// the overlay would look dead in a way nothing logs.
//
// ⚑ AND TEXT ARRIVES HERE UNCONDITIONALLY, WHICH IS DELIBERATE (contract 2 in
// window_wayland.cpp). On Win32 the message hook empties `textInput()` before
// the game sees it; on Wayland nothing can, so the WINDOW hands everyone the
// same text and the GAME asks `KeyboardRouting::text` before using it. ImGui is
// the consumer that always wants it, so this is the one caller with no gate.

#include "../dev_ui_platform.hpp"
#include "imgui_keys.hpp"

#include "sol/core/math/vec.hpp"
#include "sol/platform/time.hpp"

#include <imgui.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace sol::ui {

namespace {

// ImGui's own backends keep their state in globals beside its process-global
// context, and the four entry points below are free functions with nothing to
// hang an instance off. Following the same shape as `imgui_host.cpp`'s
// `g_hookedWindow` rather than inventing a third convention.
platform::Window* g_window = nullptr;
double g_lastTime = 0.0;
std::string g_textScratch;

// ⚑ A floor rather than a clamp, and ImGui is the reason: NewFrame asserts
// `DeltaTime > 0.0f` on every frame after the first. Two beginFrame calls
// inside one tick of the monotonic clock is not a hypothetical under lavapipe,
// where a frame the compositor never asks for can finish faster than the clock
// resolves.
constexpr float kMinDeltaSeconds = 1.0f / 10000.0f;

void submitMouse(ImGuiIO& io, const platform::Window& window)
{
    // Fed straight through even while the cursor is LOCKED for mouse-look,
    // which matches the Win32 twin: there the cursor is clipped to the client
    // rect and ImGui keeps seeing it move inside that rect. Diverging here
    // would make the overlay behave differently on the two platforms for a case
    // neither of them has a complaint about.
    const core::Vec2 position = window.mousePosition();
    io.AddMousePosEvent(position.x, position.y);
    io.AddMouseButtonEvent(0, window.isMouseButtonDown(platform::MouseButton::Left));
    io.AddMouseButtonEvent(1, window.isMouseButtonDown(platform::MouseButton::Right));
    io.AddMouseButtonEvent(2, window.isMouseButtonDown(platform::MouseButton::Middle));

    const float wheel = window.wheelDelta();
    if (wheel != 0.0f) {
        // Vertical only. `wheelDelta()` is one number by design (window.hpp),
        // and a horizontal wheel has never reached this engine's interface.
        io.AddMouseWheelEvent(0.0f, wheel);
    }
}

void submitKeys(ImGuiIO& io, const platform::Window& window)
{
    for (std::size_t i = 0; i < imguiKeyRowCount(); ++i) {
        const ImGuiKeyRow& row = imguiKeyRows()[i];
        io.AddKeyEvent(row.imguiKey, window.isKeyDown(row.key));
    }

    // ⚑ The sideless modifiers are a SEPARATE event, not a synonym for the
    // Left* rows above, and ImGui will not derive one from the other. Ctrl+V in
    // a text field reads `ImGuiMod_Ctrl`; without these three, every shortcut in
    // every ImGui widget is quietly dead while the individual keys look perfect
    // in a debugger. `Key` has no Super at all, so ImGui is TOLD it is up rather
    // than left to assume - an unsent mod is indistinguishable from a stuck one.
    io.AddKeyEvent(ImGuiMod_Ctrl, window.isKeyDown(platform::Key::LeftControl));
    io.AddKeyEvent(ImGuiMod_Shift, window.isKeyDown(platform::Key::LeftShift));
    io.AddKeyEvent(ImGuiMod_Alt, window.isKeyDown(platform::Key::LeftAlt));
    io.AddKeyEvent(ImGuiMod_Super, false);
}

} // namespace

bool devUiPlatformInit(platform::Window& window)
{
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "sol_wayland";
    // Deliberately no ImGuiBackendFlags_HasMouseCursors: changing the pointer
    // image is a wl_pointer.set_cursor request against a serial only the window
    // has, and decision 2 forbids opening a platform interface for the dev UI to
    // reach it. The overlay gets the arrow the window already set.
    g_window = &window;
    g_lastTime = platform::timeSeconds();
    g_textScratch.clear();
    return true;
}

void devUiPlatformShutdown()
{
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::GetIO().BackendPlatformName = nullptr;
    }
    g_window = nullptr;
    g_textScratch.clear();
}

void devUiPlatformNewFrame()
{
    if (g_window == nullptr) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();

    io.DisplaySize = ImVec2(static_cast<float>(g_window->width()), static_cast<float>(g_window->height()));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    const double now = platform::timeSeconds();
    const auto elapsed = static_cast<float>(now - g_lastTime);
    io.DeltaTime = elapsed > kMinDeltaSeconds ? elapsed : kMinDeltaSeconds;
    g_lastTime = now;

    submitMouse(io, *g_window);
    // Keys before text, so a frame's queue reads press-then-character in the
    // order a message-driven backend would have produced it.
    submitKeys(io, *g_window);

    const std::string_view text = g_window->textInput();
    if (!text.empty()) {
        // ⚑ Copied because AddInputCharactersUTF8 wants a NUL-terminated
        // pointer and a string_view promises no terminator. It happens to have
        // one today (the window stores a std::string), which is exactly the kind
        // of accident that survives until the day it does not.
        g_textScratch.assign(text);
        io.AddInputCharactersUTF8(g_textScratch.c_str());
    }

    // No AddFocusEvent, and the reason is upstream rather than missing: the
    // Wayland backend's keyboard `leave` handler already clears every key, so
    // the next poll reports them all up - which is the state ImGui would have
    // rebuilt from a focus-lost event anyway.
}

bool devUiPlatformMessageHook(void* windowHandle,
                              std::uint32_t message,
                              std::uint64_t wParam,
                              std::int64_t lParam)
{
    (void)windowHandle;
    (void)message;
    (void)wParam;
    (void)lParam;
    // ⚑ Permanently false, and that is a decision rather than a gap. The
    // Wayland window never invokes the hook, so returning true here could only
    // ever swallow input that nothing sent. Whether ImGui wants the keyboard is
    // still answered - by ImGuiHost::wantsKeyboardCapture(), which reads
    // io.WantCaptureKeyboard directly and is what Phase 20 actually gated on.
    return false;
}

} // namespace sol::ui
