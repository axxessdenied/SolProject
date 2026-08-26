// Wayland half of native Vulkan surface creation (Phase 21). Along with the
// ImGui bridge in sol_ui, this is a sanctioned platform-specific spot outside
// sol::platform (engine plan 2.1 / AGENTS.md section 4).
//
// ⚑ This one is REAL, not a stage-A skeleton, because it needs nothing from
// the window but the two pointers `NativeWindowHandle` already carries. That
// struct is declared in terms of HWND and HINSTANCE and is nevertheless the
// piece that ports for free: vkCreateWaylandSurfaceKHR wants exactly two
// opaque pointers too.

#include "../surface.hpp"
#include "../vk_check.hpp"

// clang-format off
// ⚑ Order is NOT load-bearing here, unlike the Win32 twin. <wayland-client.h>
// is self-contained and vulkan_wayland.h declares wl_display/wl_surface as
// incomplete types, so either order compiles. The guard is kept anyway so the
// two files read the same way and nobody "fixes" one by copying the other.
#include <wayland-client.h>

#include <vulkan/vulkan_wayland.h>
// clang-format on

namespace sol::rhi {

VkSurfaceKHR createNativeSurface(VkInstance instance, const platform::NativeWindowHandle& window)
{
    // instanceHandle is the wl_display*, windowHandle the wl_surface* - the
    // same slots HINSTANCE and HWND fill on Windows, in the same order: the
    // connection first, the thing on screen second.
    VkWaylandSurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    createInfo.display = static_cast<wl_display*>(window.instanceHandle);
    createInfo.surface = static_cast<wl_surface*>(window.windowHandle);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateWaylandSurfaceKHR(instance, &createInfo, nullptr, &surface));
    return surface;
}

const char* nativeSurfaceExtensionName()
{
    return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
}

} // namespace sol::rhi
