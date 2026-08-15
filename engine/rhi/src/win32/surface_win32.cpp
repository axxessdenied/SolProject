#include "../surface.hpp"
#include "../vk_check.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vulkan/vulkan_win32.h>

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
