#include "sol/rhi/context.hpp"

#include "surface.hpp"
#include "vk_check.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/log.hpp"

#include <atomic>
#include <cstring>
#include <vector>

namespace sol::rhi {

namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

std::atomic<std::uint64_t> g_validationMessageCount{0};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                             void* userData)
{
    (void)userData;

    // Only actual API-misuse findings count toward the failure counter; GENERAL-type
    // loader chatter (e.g. third-party overlay layers with bad names) is logged only.
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0) {
        g_validationMessageCount.fetch_add(1, std::memory_order_relaxed);
    }

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        SOL_LOG_ERROR("[vulkan] %s", callbackData->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        SOL_LOG_WARN("[vulkan] %s", callbackData->pMessage);
    } else {
        SOL_LOG_TRACE("[vulkan] %s", callbackData->pMessage);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo()
{
    VkDebugUtilsMessengerCreateInfoEXT createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = &debugCallback;
    return createInfo;
}

bool isValidationLayerAvailable()
{
    std::uint32_t layerCount = 0;
    SOL_VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));
    std::vector<VkLayerProperties> layers(layerCount);
    SOL_VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()));

    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, kValidationLayerName) == 0) {
            return true;
        }
    }
    return false;
}

struct QueueFamilyPick
{
    bool found = false;
    std::uint32_t index = 0;
};

QueueFamilyPick findGraphicsPresentQueueFamily(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

    for (std::uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        VkBool32 presentSupported = VK_FALSE;
        SOL_VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupported));
        if (presentSupported == VK_TRUE) {
            return {true, i};
        }
    }
    return {};
}

bool hasSwapchainExtension(VkPhysicalDevice physicalDevice)
{
    std::uint32_t extensionCount = 0;
    SOL_VK_CHECK(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr));
    std::vector<VkExtensionProperties> extensions(extensionCount);
    SOL_VK_CHECK(
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data()));

    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            return true;
        }
    }
    return false;
}

bool hasRequiredFeatures(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;

    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    // shaderDemoteToHelperInvocation: glslc targeting vulkan1.3 compiles
    // fragment `discard` to OpDemoteToHelperInvocation.
    return features13.dynamicRendering == VK_TRUE && features13.synchronization2 == VK_TRUE &&
           features13.shaderDemoteToHelperInvocation == VK_TRUE;
}

} // namespace

Context::~Context()
{
    shutdown();
}

bool Context::initialize(const ContextDesc& desc, const platform::NativeWindowHandle& window)
{
    SOL_ASSERT(m_instance == VK_NULL_HANDLE);

    std::uint32_t instanceVersion = 0;
    SOL_VK_CHECK(vkEnumerateInstanceVersion(&instanceVersion));
    if (instanceVersion < VK_API_VERSION_1_3) {
        SOL_LOG_ERROR("Vulkan 1.3 required; loader reports %u.%u",
                      VK_API_VERSION_MAJOR(instanceVersion),
                      VK_API_VERSION_MINOR(instanceVersion));
        return false;
    }

    bool validation = desc.enableValidation;
    if (validation && !isValidationLayerAvailable()) {
        SOL_LOG_WARN("Validation requested but %s is not available; continuing without it",
                     kValidationLayerName);
        validation = false;
    }

    // Instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = desc.appName;
    appInfo.pEngineName = "Sol Engine";
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, nativeSurfaceExtensionName()};
    std::vector<const char*> layers;
    if (validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back(kValidationLayerName);
    }

    const VkDebugUtilsMessengerCreateInfoEXT messengerCreateInfo = makeDebugMessengerCreateInfo();

    VkInstanceCreateInfo instanceCreateInfo = {};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pApplicationInfo = &appInfo;
    instanceCreateInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instanceCreateInfo.ppEnabledExtensionNames = extensions.data();
    instanceCreateInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instanceCreateInfo.ppEnabledLayerNames = layers.data();
    if (validation) {
        instanceCreateInfo.pNext = &messengerCreateInfo; // covers create/destroy of the instance itself
    }

    SOL_VK_CHECK(vkCreateInstance(&instanceCreateInfo, nullptr, &m_instance));

    if (validation) {
        auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        SOL_VERIFY(createMessenger != nullptr);
        SOL_VK_CHECK(createMessenger(m_instance, &messengerCreateInfo, nullptr, &m_debugMessenger));
    }

    // Surface
    m_surface = createNativeSurface(m_instance, window);

    // Physical device
    std::uint32_t deviceCount = 0;
    SOL_VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr));
    if (deviceCount == 0) {
        SOL_LOG_ERROR("No Vulkan physical devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    SOL_VK_CHECK(vkEnumeratePhysicalDevices(m_instance, &deviceCount, physicalDevices.data()));

    int bestScore = -1;
    for (VkPhysicalDevice candidate : physicalDevices) {
        VkPhysicalDeviceProperties properties = {};
        vkGetPhysicalDeviceProperties(candidate, &properties);

        if (properties.apiVersion < VK_API_VERSION_1_3 || !hasSwapchainExtension(candidate) ||
            !hasRequiredFeatures(candidate)) {
            continue;
        }
        const QueueFamilyPick queueFamily = findGraphicsPresentQueueFamily(candidate, m_surface);
        if (!queueFamily.found) {
            continue;
        }

        int score = 1;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score += 100;
        }

        if (score > bestScore) {
            bestScore = score;
            m_physicalDevice = candidate;
            m_graphicsQueueFamily = queueFamily.index;
        }
    }

    if (m_physicalDevice == VK_NULL_HANDLE) {
        SOL_LOG_ERROR("No suitable GPU (need Vulkan 1.3 + dynamicRendering + synchronization2 + present)");
        return false;
    }

    VkPhysicalDeviceProperties pickedProperties = {};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &pickedProperties);
    SOL_LOG_INFO("GPU: %s (Vulkan %u.%u, queue family %u)",
                 pickedProperties.deviceName,
                 VK_API_VERSION_MAJOR(pickedProperties.apiVersion),
                 VK_API_VERSION_MINOR(pickedProperties.apiVersion),
                 m_graphicsQueueFamily);

    // Timestamp capability (Phase 8o). The period comes from the limits this
    // function has fetched twice and thrown away since Phase 1; the valid-bit
    // count is per queue FAMILY, not per device, so it has to be read from the
    // family that was just chosen. Logged because a capability the log does
    // not mention is one nobody checks before disbelieving a number.
    m_timestampPeriod = pickedProperties.limits.timestampPeriod;
    {
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, families.data());
        if (m_graphicsQueueFamily < familyCount) {
            m_timestampValidBits = families[m_graphicsQueueFamily].timestampValidBits;
        }
    }
    if (m_timestampValidBits > 0) {
        SOL_LOG_INFO("GPU timestamps: %u valid bits, %.3f ns/tick",
                     m_timestampValidBits,
                     static_cast<double>(m_timestampPeriod));
    } else {
        SOL_LOG_INFO("GPU timestamps: unsupported on queue family %u", m_graphicsQueueFamily);
    }

    // Logical device
    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan13Features enabledFeatures13 = {};
    enabledFeatures13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabledFeatures13.dynamicRendering = VK_TRUE;
    enabledFeatures13.synchronization2 = VK_TRUE;
    enabledFeatures13.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceFeatures2 enabledFeatures2 = {};
    enabledFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabledFeatures2.pNext = &enabledFeatures13;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &enabledFeatures2;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 1;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;

    SOL_VK_CHECK(vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);

    VkCommandPoolCreateInfo poolCreateInfo = {};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCreateInfo.queueFamilyIndex = m_graphicsQueueFamily;
    SOL_VK_CHECK(vkCreateCommandPool(m_device, &poolCreateInfo, nullptr, &m_transientCommandPool));

    return true;
}

void Context::shutdown()
{
    if (m_transientCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_transientCommandPool, nullptr);
        m_transientCommandPool = VK_NULL_HANDLE;
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
        m_physicalDevice = VK_NULL_HANDLE;
    }
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger != nullptr) {
            destroyMessenger(m_instance, m_debugMessenger, nullptr);
        }
        m_debugMessenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

void Context::waitIdle() const
{
    if (m_device != VK_NULL_HANDLE) {
        SOL_VK_CHECK(vkDeviceWaitIdle(m_device));
    }
}

std::uint64_t Context::validationMessageCount()
{
    return g_validationMessageCount.load(std::memory_order_relaxed);
}

} // namespace sol::rhi
