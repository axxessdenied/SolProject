#pragma once

#include "sol/platform/window.hpp"

#include <cstdint>

namespace sol::ui {

// Implemented per OS under src/<os>/.
[[nodiscard]] bool devUiPlatformInit(const platform::NativeWindowHandle& handle);
void devUiPlatformShutdown();
void devUiPlatformNewFrame();
[[nodiscard]] bool devUiPlatformMessageHook(void* windowHandle,
                                            std::uint32_t message,
                                            std::uint64_t wParam,
                                            std::int64_t lParam);

} // namespace sol::ui
