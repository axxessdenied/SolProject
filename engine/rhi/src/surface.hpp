#pragma once

#include "sol/platform/window.hpp"

#include <vulkan/vulkan.h>

namespace sol::rhi {

// Implemented per OS (src/win32/...). The one sanctioned platform-specific
// spot outside sol::platform - see docs/engine-plan.md section 2.1.
[[nodiscard]] VkSurfaceKHR createNativeSurface(VkInstance instance,
                                               const platform::NativeWindowHandle& window);

// Instance extension the native surface implementation requires.
[[nodiscard]] const char* nativeSurfaceExtensionName();

} // namespace sol::rhi
