#pragma once

#include "sol/platform/window.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace sol::rhi {

struct ContextDesc
{
    const char* appName = "Sol";
    bool enableValidation = false;
};

// Owns instance, debug messenger, surface, physical/logical device, and queues.
// Requires a Vulkan 1.3 device with dynamicRendering and synchronization2.
class Context
{
public:
    Context() = default;
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    [[nodiscard]] bool initialize(const ContextDesc& desc, const platform::NativeWindowHandle& window);
    void shutdown();

    [[nodiscard]] VkInstance instance() const { return m_instance; }
    [[nodiscard]] VkSurfaceKHR surface() const { return m_surface; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    [[nodiscard]] VkDevice device() const { return m_device; }
    [[nodiscard]] std::uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }
    [[nodiscard]] VkQueue graphicsQueue() const { return m_graphicsQueue; }

    void waitIdle() const;

    // Validation warnings + errors observed by the debug messenger since startup.
    [[nodiscard]] static std::uint64_t validationMessageCount();

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    std::uint32_t m_graphicsQueueFamily = 0;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
};

} // namespace sol::rhi
