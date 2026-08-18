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

    // Pool for short-lived one-shot command buffers (uploads).
    [[nodiscard]] VkCommandPool transientCommandPool() const { return m_transientCommandPool; }

    // GPU timestamp capability (Phase 8o). Nanoseconds per tick of the
    // timestamp counter, and how many of its low bits are meaningful on the
    // graphics queue. `timestampValidBits() == 0` means this queue cannot
    // timestamp at all - a real case, not a theoretical one - and callers
    // must degrade rather than divide by a period they never got.
    [[nodiscard]] float timestampPeriod() const { return m_timestampPeriod; }
    [[nodiscard]] std::uint32_t timestampValidBits() const { return m_timestampValidBits; }
    [[nodiscard]] bool supportsTimestamps() const { return m_timestampValidBits > 0; }

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
    VkCommandPool m_transientCommandPool = VK_NULL_HANDLE;
    float m_timestampPeriod = 0.0f;
    std::uint32_t m_timestampValidBits = 0;
};

} // namespace sol::rhi
