#pragma once

#include "sol/platform/window.hpp"

#include <cstdint>

namespace sol::ui {

// Implemented per OS under src/<os>/.
//
// ⚑ init takes the WINDOW, not just its native handle, and Phase 21 stage C
// widened it for a reason worth keeping. Win32 needs only the HWND, because
// `imgui_impl_win32` reads the rest of a frame's input out of the message
// stream itself. Wayland has no message stream to read - decision 2 keeps
// `MessageHook` Win32-shaped and makes the Wayland backend POLL instead - so
// its `NewFrame` needs `isKeyDown`, `mousePosition`, `wheelDelta` and
// `textInput`, none of which a `NativeWindowHandle` can answer.
[[nodiscard]] bool devUiPlatformInit(platform::Window& window);
void devUiPlatformShutdown();
void devUiPlatformNewFrame();
[[nodiscard]] bool devUiPlatformMessageHook(void* windowHandle,
                                            std::uint32_t message,
                                            std::uint64_t wParam,
                                            std::int64_t lParam);

} // namespace sol::ui
