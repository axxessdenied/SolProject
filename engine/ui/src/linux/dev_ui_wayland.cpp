// Wayland half of the ImGui platform backend (Phase 21) — STAGE A SKELETON,
// filled in by stage C. Sanctioned platform-specific spot outside sol::platform
// (AGENTS.md section 4), same standing as the Win32 twin.
//
// ⚑⚑ THERE IS NO UPSTREAM `imgui_impl_wayland`. `imgui_impl_win32` has no
// counterpart, so the Win32 file's one-line delegations have nothing to
// delegate to and this backend is written by hand. Only the PLATFORM half is
// missing - `ImGui_ImplVulkan_*`, the renderer half, is portable and untouched.
//
// ⚑⚑⚑ AND THE HOOK IS THE PART THAT CANNOT BE COPIED. `devUiPlatformMessageHook`
// takes (windowHandle, message, wParam, lParam) - the Win32 message SHAPE,
// which window.hpp:104 admits to mirroring while avoiding the Win32 types.
// Wayland has no such message. Phase 21 decided AGAINST refactoring that seam
// (it would mean rewriting the working Win32 path on the platform this machine
// tests worst), so here the hook is a permanent no-op and stage C feeds ImGui
// by POLLING what Window already exposes: isKeyDown, mousePosition,
// isMouseButtonDown, wheelDelta and textInput.
//
// The honest cost of polling, recorded so nobody reports it as a bug later: a
// key pressed AND released inside a single frame never reaches the overlay.
// The game's own input is unaffected - it already reads that same polled state.

#include "../dev_ui_platform.hpp"

#include <imgui.h>

namespace sol::ui {

bool devUiPlatformInit(const platform::NativeWindowHandle& handle)
{
    (void)handle;
    return false; // stage C
}

void devUiPlatformShutdown()
{
}

void devUiPlatformNewFrame()
{
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
