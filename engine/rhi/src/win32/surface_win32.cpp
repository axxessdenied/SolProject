#include "../surface.hpp"
#include "../vk_check.hpp"

// clang-format off
// ⚑ THIS ORDER IS LOAD-BEARING AND THE GUARD IS THE ONLY THING HOLDING IT.
// `vulkan_win32.h` names HINSTANCE and HWND without declaring them, so it does
// not compile unless <windows.h> is already in. `IncludeBlocks: Regroup`
// ignores the blank line that used to separate the two, and sorting puts the
// Vulkan header first - which fails inside the SDK header with a wall of
// "unknown override specifier" that names no file of ours.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vulkan/vulkan_win32.h>
// clang-format on

namespace sol::rhi {

VkSurfaceKHR createNativeSurface(VkInstance instance, const platform::NativeWindowHandle& window)
{
    VkWin32SurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = static_cast<HINSTANCE>(window.instanceHandle);
    createInfo.hwnd = static_cast<HWND>(window.windowHandle);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    SOL_VK_CHECK(vkCreateWin32SurfaceKHR(instance, &createInfo, nullptr, &surface));
    return surface;
}

const char* nativeSurfaceExtensionName()
{
    return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
}

} // namespace sol::rhi
