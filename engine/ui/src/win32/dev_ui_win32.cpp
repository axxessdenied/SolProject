// Win32 half of the ImGui platform backend integration. Along with the
// Vulkan surface in sol_rhi, this is a sanctioned platform-specific spot
// outside sol::platform (AGENTS.md section 4).

#include "../dev_ui_platform.hpp"

// clang-format off
// ⚑ THIS ORDER IS LOAD-BEARING AND THE GUARD IS THE ONLY THING HOLDING IT.
// `imgui_impl_win32.h` declares its WndProc handler in terms of HWND, UINT and
// LPARAM, so it needs <windows.h> ahead of it. `IncludeBlocks: Regroup` ignores
// the blank line that used to separate them and sorts `imgui` above `windows`.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
// clang-format on

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace sol::ui {

bool devUiPlatformInit(const platform::NativeWindowHandle& handle)
{
    return ImGui_ImplWin32_Init(handle.windowHandle);
}

void devUiPlatformShutdown()
{
    ImGui_ImplWin32_Shutdown();
}

void devUiPlatformNewFrame()
{
    ImGui_ImplWin32_NewFrame();
}

bool devUiPlatformMessageHook(void* windowHandle,
                              std::uint32_t message,
                              std::uint64_t wParam,
                              std::int64_t lParam)
{
    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    if (ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(windowHandle),
                                       message,
                                       static_cast<WPARAM>(wParam),
                                       static_cast<LPARAM>(lParam)) != 0) {
        return true;
    }
    // Swallow keyboard/mouse for the game only when ImGui wants them.
    const ImGuiIO& io = ImGui::GetIO();
    const bool isMouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    const bool isKey = message >= WM_KEYFIRST && message <= WM_KEYLAST;
    return (isMouse && io.WantCaptureMouse) || (isKey && io.WantCaptureKeyboard);
}

} // namespace sol::ui
