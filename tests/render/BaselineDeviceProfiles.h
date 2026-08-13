/// @file
/// Synthetic capability profiles for the baseline device classes.
///
/// **These are not measurements and must never be reported as device testing.**
///
/// P1b names four baseline device classes — GTX 1060 6 GB, RX 580 8 GB, Intel UHD 630, and
/// AMD Vega 8 — and none of them is available to this project. The accepted evidence plan
/// (SolProjectNotes/Milestones/P1b-Reference-Hardware-Evidence-Plan.md) permits constructing
/// synthetic profiles from published device data so that the *required-set declaration* can be
/// checked against those classes, while stating plainly that a profile check is not a driver
/// test. This header is that construction, and the distinction is the whole point of it:
///
///   - It can answer "does our required set exclude a baseline device on paper?"
///   - It cannot answer "does that device's driver actually work?"
///
/// @warning PROVENANCE IS UNVERIFIED. The values below are drawn from published hardware
/// specifications and general driver knowledge, not from a capability dump of the real part.
/// Before the capability-reporting gate may be claimed closed, each profile must be
/// reconciled against a real report — the Vulkan hardware database at vulkan.gpuinfo.org is
/// the intended source — and this warning replaced with the report identifiers used. Until
/// then a profile result is a design check on our requirement set, not evidence about the
/// device. This is tracked as an open item in increment B1's handoff.

#pragma once

#include "Sol/Render/DeviceCapabilities.h"

#include <string>
#include <vector>

namespace sol::test {

/// A named synthetic profile plus the provenance of its numbers.
struct BaselineProfile {
    std::string label;
    /// Where the values came from, carried into any output so a reader cannot mistake a
    /// profile for a measurement.
    std::string provenance;
    render::DeviceCapabilities capabilities;
};

namespace detail {

/// Extensions every one of these classes exposes on a current driver. Kept to the set the
/// requirement declaration actually consults; a full extension list would imply a fidelity
/// these hand-built profiles do not have.
inline std::vector<std::string> commonExtensions()
{
    return {
        "VK_KHR_swapchain",
        "VK_KHR_maintenance1",
        "VK_KHR_maintenance2",
        "VK_KHR_maintenance3",
    };
}

/// The depth and colour formats the requirement declaration consults.
///
/// @warning The `D32_SFLOAT` value below is the least trustworthy number in this file, and it
/// is also the only one that can reject a real conformant device. An earlier version of this
/// comment claimed D32_SFLOAT is a Vulkan-mandatory depth/stencil attachment format. **It is
/// not.** The specification's required-format table mandates `D16_UNORM` for that usage, and
/// otherwise guarantees only that *at least one of* `X8_D24_UNORM_PACK32` and `D32_SFLOAT`
/// supports it — a device may conform while supporting the other one.
///
/// In practice every device class here is believed to support D32_SFLOAT, and both GPUs this
/// project can measure do. But "believed" is the operative word: setting this true is an
/// assumption about four absent devices, not a spec guarantee, and it is exactly what the
/// reconciliation against real reports has to confirm.
inline std::vector<render::FormatSupport> commonFormats()
{
    return {
        {.name = "VK_FORMAT_D32_SFLOAT",
         .optimalTilingDepthStencilAttachment = true,
         .optimalTilingColorAttachment = false,
         .optimalTilingSampledImage = true},
        {.name = "VK_FORMAT_B8G8R8A8_SRGB",
         .optimalTilingDepthStencilAttachment = false,
         .optimalTilingColorAttachment = true,
         .optimalTilingSampledImage = true},
    };
}

inline std::vector<render::QueueFamily> graphicsAndPresentQueues()
{
    return {
        {.index = 0, .count = 1, .graphics = true, .compute = true, .transfer = true, .presentation = true},
    };
}

} // namespace detail

/// NVIDIA GeForce GTX 1060 6 GB — Pascal, discrete baseline class.
inline BaselineProfile gtx1060()
{
    render::DeviceCapabilities device;
    device.deviceName = "NVIDIA GeForce GTX 1060 6GB";
    device.vendorId = 0x10DE;
    device.deviceId = 0x1C03;
    device.kind = render::DeviceKind::DiscreteGpu;
    device.apiVersion = {1, 3, 0};
    device.driverVersionText = "(synthetic profile — no driver)";
    device.extensions = detail::commonExtensions();
    device.formats = detail::commonFormats();
    device.queueFamilies = detail::graphicsAndPresentQueues();
    device.memoryHeaps = {{.index = 0, .sizeBytes = 6ULL * 1024 * 1024 * 1024, .deviceLocal = true}};
    device.limits = {.maxImageDimension2D = 32768,
                     .maxSamplerAnisotropy = 16.0F,
                     .maxViewports = 16,
                     .bufferImageGranularity = 1024,
                     .timestampPeriodNanoseconds = 1.0F};
    device.features = {.samplerAnisotropy = true,
                       .depthClamp = true,
                       .fillModeNonSolid = true,
                       .independentBlend = true,
                       .timelineSemaphore = true};
    return {.label = "GTX 1060 6 GB (discrete baseline)",
            .provenance = "published specification; unverified",
            .capabilities = device};
}

/// AMD Radeon RX 580 8 GB — Polaris, discrete baseline class.
inline BaselineProfile rx580()
{
    render::DeviceCapabilities device;
    device.deviceName = "AMD Radeon RX 580";
    device.vendorId = 0x1002;
    device.deviceId = 0x67DF;
    device.kind = render::DeviceKind::DiscreteGpu;
    device.apiVersion = {1, 3, 0};
    device.driverVersionText = "(synthetic profile — no driver)";
    device.extensions = detail::commonExtensions();
    device.formats = detail::commonFormats();
    device.queueFamilies = detail::graphicsAndPresentQueues();
    device.memoryHeaps = {{.index = 0, .sizeBytes = 8ULL * 1024 * 1024 * 1024, .deviceLocal = true}};
    device.limits = {.maxImageDimension2D = 16384,
                     .maxSamplerAnisotropy = 16.0F,
                     .maxViewports = 16,
                     .bufferImageGranularity = 1,
                     .timestampPeriodNanoseconds = 40.0F};
    device.features = {.samplerAnisotropy = true,
                       .depthClamp = true,
                       .fillModeNonSolid = true,
                       .independentBlend = true,
                       .timelineSemaphore = true};
    return {.label = "RX 580 8 GB (discrete baseline)",
            .provenance = "published specification; unverified",
            .capabilities = device};
}

/// Intel UHD Graphics 630 — Gen9.5, integrated investigation class.
inline BaselineProfile uhd630()
{
    render::DeviceCapabilities device;
    device.deviceName = "Intel(R) UHD Graphics 630";
    device.vendorId = 0x8086;
    device.deviceId = 0x3E92;
    device.kind = render::DeviceKind::IntegratedGpu;
    device.apiVersion = {1, 3, 0};
    device.driverVersionText = "(synthetic profile — no driver)";
    device.extensions = detail::commonExtensions();
    device.formats = detail::commonFormats();
    device.queueFamilies = detail::graphicsAndPresentQueues();
    // Integrated parts expose shared system memory as device-local. This is exactly the shape
    // that made a device-local byte threshold the wrong requirement to impose.
    device.memoryHeaps = {{.index = 0, .sizeBytes = 6ULL * 1024 * 1024 * 1024, .deviceLocal = true}};
    device.limits = {.maxImageDimension2D = 16384,
                     .maxSamplerAnisotropy = 16.0F,
                     .maxViewports = 16,
                     .bufferImageGranularity = 1,
                     .timestampPeriodNanoseconds = 83.333F};
    device.features = {.samplerAnisotropy = true,
                       .depthClamp = true,
                       .fillModeNonSolid = true,
                       .independentBlend = true,
                       .timelineSemaphore = true};
    return {.label = "Intel UHD 630 (integrated investigation)",
            .provenance = "published specification; unverified",
            .capabilities = device};
}

/// AMD Radeon Vega 8 — Raven Ridge integrated, investigation class.
inline BaselineProfile vega8()
{
    render::DeviceCapabilities device;
    device.deviceName = "AMD Radeon Vega 8 Graphics";
    device.vendorId = 0x1002;
    device.deviceId = 0x15D8;
    device.kind = render::DeviceKind::IntegratedGpu;
    device.apiVersion = {1, 3, 0};
    device.driverVersionText = "(synthetic profile — no driver)";
    device.extensions = detail::commonExtensions();
    device.formats = detail::commonFormats();
    device.queueFamilies = detail::graphicsAndPresentQueues();
    device.memoryHeaps = {{.index = 0, .sizeBytes = 2ULL * 1024 * 1024 * 1024, .deviceLocal = true}};
    device.limits = {.maxImageDimension2D = 16384,
                     .maxSamplerAnisotropy = 16.0F,
                     .maxViewports = 16,
                     .bufferImageGranularity = 1,
                     .timestampPeriodNanoseconds = 40.0F};
    device.features = {.samplerAnisotropy = true,
                       .depthClamp = true,
                       .fillModeNonSolid = true,
                       .independentBlend = true,
                       .timelineSemaphore = true};
    return {.label = "AMD Vega 8 (integrated investigation)",
            .provenance = "published specification; unverified",
            .capabilities = device};
}

/// Every baseline class P1b names, in plan order.
inline std::vector<BaselineProfile> allBaselineProfiles()
{
    return {gtx1060(), rx580(), uhd630(), vega8()};
}

} // namespace sol::test
