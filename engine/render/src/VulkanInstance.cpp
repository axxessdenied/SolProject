#include "Sol/Render/VulkanInstance.h"

#include <volk.h>

#include <algorithm>
#include <array>
#include <format>

namespace sol::render {
namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

/// Formats the image formats the renderer asks about. Kept beside the enumeration code
/// because the pair (registry name, VkFormat) must not drift apart.
struct QueriedFormat {
    const char* name;
    VkFormat format;
};

constexpr std::array kQueriedFormats{
    QueriedFormat{"VK_FORMAT_D32_SFLOAT", VK_FORMAT_D32_SFLOAT},
    QueriedFormat{"VK_FORMAT_D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT},
    QueriedFormat{"VK_FORMAT_D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT},
    QueriedFormat{"VK_FORMAT_B8G8R8A8_SRGB", VK_FORMAT_B8G8R8A8_SRGB},
    QueriedFormat{"VK_FORMAT_R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT},
};

ApiVersion unpackApiVersion(std::uint32_t packed)
{
    return ApiVersion{
        .major = VK_API_VERSION_MAJOR(packed),
        .minor = VK_API_VERSION_MINOR(packed),
        .patch = VK_API_VERSION_PATCH(packed),
    };
}

DeviceKind toDeviceKind(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return DeviceKind::IntegratedGpu;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return DeviceKind::DiscreteGpu;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return DeviceKind::VirtualGpu;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return DeviceKind::Cpu;
    default:
        return DeviceKind::Other;
    }
}

/// Decodes a driver version using the vendor's own packing.
///
/// Vulkan does not define this field's layout, so a raw integer is meaningless and a
/// VK_API_VERSION-style decode is wrong for NVIDIA and for Intel on Windows. Getting this
/// right matters because the driver version is provenance on every P1b report, and a report
/// that misstates the driver cannot be reproduced against.
std::string decodeDriverVersion(std::uint32_t vendorId, std::uint32_t version)
{
    constexpr std::uint32_t kVendorNvidia = 0x10DE;
    constexpr std::uint32_t kVendorIntel = 0x8086;

    if (vendorId == kVendorNvidia) {
        return std::format("{}.{}.{}.{}",
                           (version >> 22U) & 0x3FFU,
                           (version >> 14U) & 0x0FFU,
                           (version >> 6U) & 0x0FFU,
                           version & 0x03FU);
    }

    // Intel packs a two-part version on Windows only. This build targets Windows exclusively
    // (ADR 0001), so the platform condition is a constant here rather than a branch.
    if (vendorId == kVendorIntel) {
        return std::format("{}.{}", version >> 14U, version & 0x3FFFU);
    }

    const ApiVersion decoded = unpackApiVersion(version);
    return toString(decoded);
}

std::vector<std::string> enumerateInstanceLayerNames()
{
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkLayerProperties> properties(count);
    if (vkEnumerateInstanceLayerProperties(&count, properties.data()) != VK_SUCCESS) {
        return {};
    }

    std::vector<std::string> names;
    names.reserve(properties.size());
    for (const VkLayerProperties& layer : properties) {
        names.emplace_back(layer.layerName);
    }
    return names;
}

std::vector<std::string> enumerateInstanceExtensionNames()
{
    std::uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, properties.data()) != VK_SUCCESS) {
        return {};
    }

    std::vector<std::string> names;
    names.reserve(properties.size());
    for (const VkExtensionProperties& extension : properties) {
        names.emplace_back(extension.extensionName);
    }
    return names;
}

std::string describeResult(VkResult result)
{
    switch (result) {
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "the host is out of memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "the device is out of memory";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "initialisation failed";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "a requested layer is not installed";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "a requested extension is not installed";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "the installed driver is incompatible with the requested API version";
    default:
        return std::format("VkResult {}", static_cast<std::int32_t>(result));
    }
}

} // namespace

bool LoaderInfo::hasValidationLayer() const
{
    return std::ranges::find(availableLayers, kValidationLayerName) != availableLayers.end();
}

struct VulkanInstance::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    LoaderInfo loader;
    bool validationEnabled = false;

    ~Impl()
    {
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};

std::expected<VulkanInstance, std::string> VulkanInstance::create(const InstanceConfig& config)
{
    // volkInitialize is where an absent loader is discovered. Because nothing links
    // vulkan-1.lib, this is a value we can report rather than a process that failed to start.
    if (const VkResult result = volkInitialize(); result != VK_SUCCESS) {
        return std::unexpected(
            "No Vulkan loader was found on this system.\n"
            "  The game needs a Vulkan-capable graphics driver. Vulkan support ships with the "
            "graphics driver rather than with Windows.\n"
            "  Install or update the driver from your GPU vendor (NVIDIA, AMD, or Intel) and "
            "run the game again.\n"
            "  Detail: " + describeResult(result));
    }

    auto impl = std::make_unique<Impl>();

    impl->loader.instanceVersion = unpackApiVersion(volkGetInstanceVersion());
    impl->loader.availableLayers = enumerateInstanceLayerNames();
    impl->loader.availableExtensions = enumerateInstanceExtensionNames();

    const ApiVersion& requested = config.requestedApiVersion;
    if (impl->loader.instanceVersion < requested) {
        return std::unexpected(std::format(
            "The installed Vulkan loader supports API {} but this renderer requires {}.\n"
            "  Update your graphics driver; the loader version follows it.",
            toString(impl->loader.instanceVersion),
            toString(requested)));
    }

    // Surface extensions are instance-level and are needed even to ask whether a queue family
    // can present, which this enumeration does without ever creating a window.
    std::vector<const char*> extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    for (const char* required : extensions) {
        const bool present = std::ranges::find(impl->loader.availableExtensions, required)
                             != impl->loader.availableExtensions.end();
        if (!present) {
            return std::unexpected(std::format(
                "The Vulkan loader does not offer the required instance extension {}.\n"
                "  This indicates an incomplete or unusually old driver installation.",
                required));
        }
    }

    std::vector<const char*> layers;
    if (config.validation == ValidationMode::Enabled) {
        if (impl->loader.hasValidationLayer()) {
            layers.push_back(kValidationLayerName);
            impl->validationEnabled = true;
        }
        // A missing validation layer is not an error. It is absent on end-user machines by
        // design, and the caller learns what actually happened from validationEnabled().
    }

    const VkApplicationInfo applicationInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = config.applicationName.c_str(),
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 0, 0),
        .pEngineName = "SolEngine",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 0, 0),
        .apiVersion = VK_MAKE_API_VERSION(0, requested.major, requested.minor, requested.patch),
    };

    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &applicationInfo,
        .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.empty() ? nullptr : layers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    if (const VkResult result = vkCreateInstance(&createInfo, nullptr, &impl->instance);
        result != VK_SUCCESS) {
        return std::unexpected(std::format(
            "Creating the Vulkan instance failed: {}.\n"
            "  Requested API {} with {} layer(s) and {} extension(s).",
            describeResult(result),
            toString(requested),
            layers.size(),
            extensions.size()));
    }

    volkLoadInstance(impl->instance);

    return VulkanInstance{std::move(impl)};
}

VulkanInstance::VulkanInstance(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

VulkanInstance::~VulkanInstance() = default;
VulkanInstance::VulkanInstance(VulkanInstance&&) noexcept = default;
VulkanInstance& VulkanInstance::operator=(VulkanInstance&&) noexcept = default;

const LoaderInfo& VulkanInstance::loader() const
{
    return m_impl->loader;
}

bool VulkanInstance::validationEnabled() const
{
    return m_impl->validationEnabled;
}

std::vector<DeviceCapabilities> VulkanInstance::enumerateDevices() const
{
    std::uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(m_impl->instance, &deviceCount, nullptr) != VK_SUCCESS) {
        return {};
    }

    std::vector<VkPhysicalDevice> handles(deviceCount);
    if (vkEnumeratePhysicalDevices(m_impl->instance, &deviceCount, handles.data()) != VK_SUCCESS) {
        return {};
    }

    std::vector<DeviceCapabilities> devices;
    devices.reserve(handles.size());

    for (VkPhysicalDevice handle : handles) {
        DeviceCapabilities capabilities;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(handle, &properties);

        capabilities.deviceName = properties.deviceName;
        capabilities.vendorId = properties.vendorID;
        capabilities.deviceId = properties.deviceID;
        capabilities.kind = toDeviceKind(properties.deviceType);
        capabilities.apiVersion = unpackApiVersion(properties.apiVersion);
        capabilities.driverVersionText =
            decodeDriverVersion(properties.vendorID, properties.driverVersion);

        capabilities.limits.maxImageDimension2D = properties.limits.maxImageDimension2D;
        capabilities.limits.maxSamplerAnisotropy = properties.limits.maxSamplerAnisotropy;
        capabilities.limits.maxViewports = properties.limits.maxViewports;
        capabilities.limits.bufferImageGranularity = properties.limits.bufferImageGranularity;
        capabilities.limits.timestampPeriodNanoseconds = properties.limits.timestampPeriod;

        VkPhysicalDeviceVulkan12Features vulkan12Features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        };
        VkPhysicalDeviceFeatures2 features2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &vulkan12Features,
        };
        vkGetPhysicalDeviceFeatures2(handle, &features2);

        capabilities.features.samplerAnisotropy = features2.features.samplerAnisotropy == VK_TRUE;
        capabilities.features.depthClamp = features2.features.depthClamp == VK_TRUE;
        capabilities.features.fillModeNonSolid = features2.features.fillModeNonSolid == VK_TRUE;
        capabilities.features.independentBlend = features2.features.independentBlend == VK_TRUE;
        capabilities.features.timelineSemaphore = vulkan12Features.timelineSemaphore == VK_TRUE;

        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(handle, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensionProperties(extensionCount);
        vkEnumerateDeviceExtensionProperties(
            handle, nullptr, &extensionCount, extensionProperties.data());
        capabilities.extensions.reserve(extensionProperties.size());
        for (const VkExtensionProperties& extension : extensionProperties) {
            capabilities.extensions.emplace_back(extension.extensionName);
        }
        // Sorted so that two reports from the same device compare cleanly. The loader's own
        // order is not guaranteed stable across driver updates.
        std::ranges::sort(capabilities.extensions);

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(handle, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> familyProperties(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(handle, &familyCount, familyProperties.data());

        capabilities.queueFamilies.reserve(familyCount);
        for (std::uint32_t index = 0; index < familyCount; ++index) {
            const VkQueueFamilyProperties& family = familyProperties[index];
            capabilities.queueFamilies.push_back({
                .index = index,
                .count = family.queueCount,
                .graphics = (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0,
                .compute = (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0,
                .transfer = (family.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0,
                // Asks the driver whether this family could present to *any* Win32 surface.
                // This needs no window, which is what lets the capability report run headless
                // and be usable as a diagnostic before the renderer starts.
                .presentation =
                    vkGetPhysicalDeviceWin32PresentationSupportKHR(handle, index) == VK_TRUE,
            });
        }

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(handle, &memoryProperties);
        capabilities.memoryHeaps.reserve(memoryProperties.memoryHeapCount);
        for (std::uint32_t index = 0; index < memoryProperties.memoryHeapCount; ++index) {
            const VkMemoryHeap& heap = memoryProperties.memoryHeaps[index];
            capabilities.memoryHeaps.push_back({
                .index = index,
                .sizeBytes = heap.size,
                .deviceLocal = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0,
            });
        }

        capabilities.formats.reserve(kQueriedFormats.size());
        for (const QueriedFormat& queried : kQueriedFormats) {
            VkFormatProperties formatProperties{};
            vkGetPhysicalDeviceFormatProperties(handle, queried.format, &formatProperties);
            const VkFormatFeatureFlags optimal = formatProperties.optimalTilingFeatures;
            capabilities.formats.push_back({
                .name = queried.name,
                .optimalTilingDepthStencilAttachment =
                    (optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0,
                .optimalTilingColorAttachment =
                    (optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0,
                .optimalTilingSampledImage =
                    (optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
            });
        }

        devices.push_back(std::move(capabilities));
    }

    return devices;
}

} // namespace sol::render
