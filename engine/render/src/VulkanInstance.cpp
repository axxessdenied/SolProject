#include "Sol/Render/VulkanInstance.h"

#include <volk.h>

#include <algorithm>
#include <array>
#include <format>

namespace sol::render {
namespace {

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

constexpr std::uint32_t kVendorNvidia = 0x10DE;
constexpr std::uint32_t kVendorIntel = 0x8086;

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
    QueriedFormat{"VK_FORMAT_X8_D24_UNORM_PACK32", VK_FORMAT_X8_D24_UNORM_PACK32},
    QueriedFormat{"VK_FORMAT_B8G8R8A8_SRGB", VK_FORMAT_B8G8R8A8_SRGB},
    QueriedFormat{"VK_FORMAT_R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT},
};

std::string describeResult(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "success";
    case VK_INCOMPLETE:
        return "the result buffer was too small";
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

/// Runs Vulkan's two-call enumeration idiom correctly.
///
/// Three things this gets right that the obvious spelling does not. VK_INCOMPLETE is a
/// *success* code, returned when the count grew between the two calls, and the documented
/// response is to retry rather than to give up. The driver may write fewer entries than the
/// buffer holds, so the vector is resized to what was actually written — otherwise the tail
/// is value-initialised and, for name arrays, appends empty strings that look like real
/// entries. And a failed query is reported rather than silently yielding an empty list, which
/// would otherwise be indistinguishable from a device that genuinely has none.
template <typename T, typename Query>
VkResult enumerateInto(std::vector<T>& out, Query&& query)
{
    for (;;) {
        std::uint32_t count = 0;
        if (const VkResult result = query(&count, nullptr); result != VK_SUCCESS) {
            return result;
        }

        out.assign(count, T{});
        if (count == 0) {
            return VK_SUCCESS;
        }

        const VkResult result = query(&count, out.data());
        if (result == VK_INCOMPLETE) {
            continue;
        }
        if (result != VK_SUCCESS) {
            return result;
        }

        out.resize(count);
        return VK_SUCCESS;
    }
}

ApiVersion unpackApiVersion(std::uint32_t packed)
{
    return ApiVersion{
        .major = VK_API_VERSION_MAJOR(packed),
        .minor = VK_API_VERSION_MINOR(packed),
        .patch = VK_API_VERSION_PATCH(packed),
    };
}

std::uint32_t packApiVersion(const ApiVersion& version)
{
    return VK_MAKE_API_VERSION(0, version.major, version.minor, version.patch);
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
///
/// AMD's Windows packing is deliberately not special-cased: no AMD device is available to
/// this project, so an implementation could not be checked against anything. The fallback
/// decode is used and is likely wrong for AMD; that is recorded rather than guessed at.
std::string decodeDriverVersion(std::uint32_t vendorId, std::uint32_t version)
{
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

    return toString(unpackApiVersion(version));
}

std::string_view describeSeverity(VkDebugUtilsMessageSeverityFlagBitsEXT severity)
{
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        return "ERROR";
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        return "WARNING";
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0) {
        return "INFO";
    }
    return "VERBOSE";
}

} // namespace

bool LoaderInfo::hasValidationLayer() const
{
    return std::ranges::find(availableLayers, kValidationLayerName) != availableLayers.end();
}

struct VulkanInstance::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    LoaderInfo loader;
    bool validationEnabled = false;
    std::vector<std::string> validationMessages;

    /// Validation callback. Appends to the owning Impl's message list.
    ///
    /// A static member rather than a free function because `Impl` is a private nested type
    /// that nothing outside the class may name.
    ///
    /// The layer invokes this synchronously on whichever thread made the offending Vulkan
    /// call. VulkanInstance documents single-threaded use, so no lock is taken; if that
    /// contract is ever relaxed this is the first thing that needs one.
    static VKAPI_ATTR VkBool32 VKAPI_CALL onValidationMessage(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* userData);

    ~Impl()
    {
        if (messenger != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        }
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

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanInstance::Impl::onValidationMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData)
{
    auto* impl = static_cast<Impl*>(userData);
    if (impl != nullptr && data != nullptr) {
        impl->validationMessages.push_back(std::format(
            "[{}] {}: {}",
            describeSeverity(severity),
            data->pMessageIdName != nullptr ? data->pMessageIdName : "(unnamed)",
            data->pMessage != nullptr ? data->pMessage : "(no text)"));
    }
    // VK_FALSE: report, do not abort the call that triggered it.
    return VK_FALSE;
}

namespace {

VkDebugUtilsMessengerCreateInfoEXT makeMessengerCreateInfo(
    PFN_vkDebugUtilsMessengerCallbackEXT callback,
    void* userData)
{
    return VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                           | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = callback,
        .pUserData = userData,
    };
}

} // namespace

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

    std::vector<VkLayerProperties> layerProperties;
    if (const VkResult result = enumerateInto(
            layerProperties,
            [](std::uint32_t* count, VkLayerProperties* data) {
                return vkEnumerateInstanceLayerProperties(count, data);
            });
        result != VK_SUCCESS) {
        return std::unexpected(std::format(
            "Querying the Vulkan loader's layer list failed: {}.\n"
            "  This usually indicates a corrupt driver or SDK installation.",
            describeResult(result)));
    }
    for (const VkLayerProperties& layer : layerProperties) {
        impl->loader.availableLayers.emplace_back(layer.layerName);
    }

    std::vector<VkExtensionProperties> extensionProperties;
    if (const VkResult result = enumerateInto(
            extensionProperties,
            [](std::uint32_t* count, VkExtensionProperties* data) {
                return vkEnumerateInstanceExtensionProperties(nullptr, count, data);
            });
        result != VK_SUCCESS) {
        return std::unexpected(std::format(
            "Querying the Vulkan loader's instance extension list failed: {}.",
            describeResult(result)));
    }
    for (const VkExtensionProperties& extension : extensionProperties) {
        impl->loader.availableExtensions.emplace_back(extension.extensionName);
    }

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
    const bool debugUtilsAvailable =
        std::ranges::find(impl->loader.availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
        != impl->loader.availableExtensions.end();

    if (config.validation == ValidationMode::Enabled) {
        // Both halves are required. The layer produces the messages; debug_utils is the only
        // way to receive them programmatically. Reporting validation as active without a way
        // to capture its output would make "clean validation output" unfalsifiable, and P1b
        // gates on exactly that.
        if (impl->loader.hasValidationLayer() && debugUtilsAvailable) {
            layers.push_back(kValidationLayerName);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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
        .apiVersion = packApiVersion(requested),
    };

    // Chaining the messenger create-info into instance creation is what captures messages
    // emitted *during* vkCreateInstance itself, which a messenger created afterwards misses.
    const VkDebugUtilsMessengerCreateInfoEXT messengerInfo =
        makeMessengerCreateInfo(&Impl::onValidationMessage, impl.get());

    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = impl->validationEnabled ? &messengerInfo : nullptr,
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

    if (impl->validationEnabled) {
        if (const VkResult result = vkCreateDebugUtilsMessengerEXT(
                impl->instance, &messengerInfo, nullptr, &impl->messenger);
            result != VK_SUCCESS) {
            return std::unexpected(std::format(
                "The validation layer loaded but its message callback could not be created: "
                "{}.\n"
                "  Continuing would report validation as active while discarding every "
                "message it produced.",
                describeResult(result)));
        }
    }

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

const std::vector<std::string>& VulkanInstance::validationMessages() const
{
    return m_impl->validationMessages;
}

std::expected<std::vector<DeviceCapabilities>, std::string>
VulkanInstance::enumerateDevices() const
{
    std::vector<VkPhysicalDevice> handles;
    if (const VkResult result = enumerateInto(
            handles,
            [this](std::uint32_t* count, VkPhysicalDevice* data) {
                return vkEnumeratePhysicalDevices(m_impl->instance, count, data);
            });
        result != VK_SUCCESS) {
        return std::unexpected(std::format(
            "Enumerating Vulkan physical devices failed: {}.\n"
            "  A loader is installed and an instance was created, so this is a driver-level "
            "failure rather than an absent GPU.",
            describeResult(result)));
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

        // The 1.2 feature struct may only be chained at a device that supports 1.2. Chaining
        // it unconditionally is invalid usage the validation layer reports, and a driver that
        // ignores the unrecognised sType returns a value-initialised struct — making
        // "unsupported" indistinguishable from "never answered". This matters specifically
        // because the evidence plan requires exercising sub-floor devices through the
        // profiles layer, which is exactly when the guard fires.
        const bool supportsVulkan12 =
            properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0);

        VkPhysicalDeviceVulkan12Features vulkan12Features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        };
        VkPhysicalDeviceFeatures2 features2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = supportsVulkan12 ? &vulkan12Features : nullptr,
        };

        // vkGetPhysicalDeviceFeatures2 is core in 1.1. Below that, fall back to the 1.0 call.
        if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
            vkGetPhysicalDeviceFeatures2(handle, &features2);
        } else {
            vkGetPhysicalDeviceFeatures(handle, &features2.features);
        }

        capabilities.features.samplerAnisotropy = features2.features.samplerAnisotropy == VK_TRUE;
        capabilities.features.depthClamp = features2.features.depthClamp == VK_TRUE;
        capabilities.features.fillModeNonSolid = features2.features.fillModeNonSolid == VK_TRUE;
        capabilities.features.independentBlend = features2.features.independentBlend == VK_TRUE;
        capabilities.features.timelineSemaphore =
            supportsVulkan12 && vulkan12Features.timelineSemaphore == VK_TRUE;

        std::vector<VkExtensionProperties> extensionProperties;
        if (const VkResult result = enumerateInto(
                extensionProperties,
                [handle](std::uint32_t* count, VkExtensionProperties* data) {
                    return vkEnumerateDeviceExtensionProperties(handle, nullptr, count, data);
                });
            result != VK_SUCCESS) {
            return std::unexpected(std::format(
                "Enumerating device extensions for '{}' failed: {}.\n"
                "  The device cannot be evaluated, and reporting it as lacking an extension "
                "would blame the hardware for a failed query.",
                capabilities.deviceName,
                describeResult(result)));
        }
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
        familyProperties.resize(familyCount);

        const auto familyTotal = static_cast<std::uint32_t>(familyProperties.size());
        capabilities.queueFamilies.reserve(familyTotal);
        for (std::uint32_t index = 0; index < familyTotal; ++index) {
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
