/// @file
/// Device capability facts, expressed without Vulkan types.
///
/// ADR 0002 confines Vulkan types to the renderer module. This header is the reason that
/// confinement costs nothing: it holds everything the rest of the engine, the diagnostics
/// path, and the tests need to know about a device, in plain types.
///
/// The second reason matters more for P1b. Because a DeviceCapabilities value can be written
/// by hand, the capability check is testable against devices that are not present. Both GPUs
/// available to this project exceed the Vulkan 1.2 candidate floor, so the rejection path has
/// no real device that triggers it; synthetic values are how that gate gets exercised at all.
/// See SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sol::render {

/// A Vulkan API version, unpacked from its encoded form.
struct ApiVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    friend constexpr auto operator<=>(const ApiVersion&, const ApiVersion&) = default;
};

/// Renders as "1.2.198".
std::string toString(const ApiVersion& version);

/// Physical device category, mirroring VkPhysicalDeviceType without naming it.
enum class DeviceKind : std::uint8_t {
    Other,
    IntegratedGpu,
    DiscreteGpu,
    VirtualGpu,
    Cpu,
};

std::string_view toString(DeviceKind kind);

/// Capabilities of one queue family.
struct QueueFamily {
    std::uint32_t index = 0;
    std::uint32_t count = 0;
    bool graphics = false;
    bool compute = false;
    bool transfer = false;
    /// Whether this family can present to a Win32 surface. Queried through the platform
    /// presentation-support entry point, which needs no window to answer.
    bool presentation = false;
};

/// One memory heap and whether it is device-local.
struct MemoryHeap {
    std::uint32_t index = 0;
    /// Heap size in bytes.
    std::uint64_t sizeBytes = 0;
    bool deviceLocal = false;
};

/// Support for one image format under optimal tiling.
///
/// Only the formats the renderer actually asks about are recorded; enumerating every Vulkan
/// format would produce a large report in which the load-bearing entries are invisible.
struct FormatSupport {
    /// Format name as spelled in the Vulkan registry, e.g. "VK_FORMAT_D32_SFLOAT".
    std::string name;
    bool optimalTilingDepthStencilAttachment = false;
    bool optimalTilingColorAttachment = false;
    bool optimalTilingSampledImage = false;
};

/// The subset of device limits the renderer has a demonstrated reason to inspect.
///
/// This deliberately does not mirror VkPhysicalDeviceLimits. A limit earns a place here when
/// an increment shows it constrains a real decision; a wholesale copy would imply the renderer
/// reasons about limits it has never consulted.
struct DeviceLimits {
    std::uint32_t maxImageDimension2D = 0;
    /// Bit mask width matters for reversed-Z: a 24-bit depth buffer and a 32-bit float one
    /// behave differently over a surface-to-orbit depth range.
    float maxSamplerAnisotropy = 0.0F;
    std::uint32_t maxViewports = 0;
    std::uint64_t bufferImageGranularity = 0;
    /// Timestamp period in nanoseconds; zero when the device reports no timestamp support.
    /// GPU-side frame timing in P1b is measured through this, so a zero here changes what
    /// the performance reports can claim rather than failing a gate.
    float timestampPeriodNanoseconds = 0.0F;
};

/// Device features the renderer inspects.
///
/// shaderFloat64 is deliberately absent. Camera-relative rendering exists precisely so that
/// the GPU never needs double precision; requiring it would both contradict the frame model
/// selected in P1a increment A2 and exclude hardware the project intends to support.
struct DeviceFeatures {
    bool samplerAnisotropy = false;
    bool depthClamp = false;
    bool fillModeNonSolid = false;
    bool independentBlend = false;
    /// Core in Vulkan 1.2 but optional as a feature: timeline semaphores.
    bool timelineSemaphore = false;
};

/// Everything the renderer knows about one physical device.
struct DeviceCapabilities {
    std::string deviceName;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    DeviceKind kind = DeviceKind::Other;

    /// Highest API version this device supports. May exceed the instance's version.
    ApiVersion apiVersion;

    /// Vendor-encoded driver version, already decoded per vendor where the encoding is known,
    /// and otherwise printed as a raw integer. Recorded for evidence provenance, never
    /// compared against a requirement: driver version schemes are not ordered across vendors.
    std::string driverVersionText;

    std::vector<std::string> extensions;
    DeviceFeatures features;
    DeviceLimits limits;
    std::vector<QueueFamily> queueFamilies;
    std::vector<MemoryHeap> memoryHeaps;
    std::vector<FormatSupport> formats;

    /// True when @p name appears in @ref extensions.
    [[nodiscard]] bool hasExtension(std::string_view name) const;

    /// The named format's record, or nullptr when the renderer never queried it.
    [[nodiscard]] const FormatSupport* findFormat(std::string_view name) const;

    /// Total size of all device-local heaps, in bytes.
    [[nodiscard]] std::uint64_t deviceLocalMemoryBytes() const;
};

} // namespace sol::render
