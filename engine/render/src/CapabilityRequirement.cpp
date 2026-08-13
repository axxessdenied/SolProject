#include "Sol/Render/CapabilityRequirement.h"

#include <algorithm>
#include <format>

namespace sol::render {
namespace {

/// Vulkan's own guaranteed minimum for maxImageDimension2D. Requiring exactly this cannot
/// reject a conformant implementation, which is the point: it catches a non-conformant or
/// software device that under-reports, without narrowing supported hardware by one device.
constexpr std::uint32_t kVulkanGuaranteedMaxImageDimension2D = 4096;

} // namespace

CapabilityRequirement baselineRequirement()
{
    CapabilityRequirement requirement;

    // ADR 0002 fixes the candidate floor. Devices above it are still driven at the floor; a
    // later capability path may opt into more, but nothing may become an undeclared baseline.
    requirement.minimumApiVersion = ApiVersion{1, 2, 0};

    requirement.extensions = {
        {.name = "VK_KHR_swapchain",
         .reason = "Core Vulkan has no presentation. Without a swapchain the renderer can "
                   "produce images but cannot put one on screen, which is every P1b gate."},
    };

    // Empty by design. ADR 0002 forbids making a capability a baseline requirement before an
    // increment demonstrates the need, and B1 has not yet built a path that needs one. The
    // RequiredFeature machinery is still exercised: the capability-rejection negative control
    // constructs a synthetic feature requirement to prove the rejection path reports it.
    requirement.features = {};

    requirement.formats = {
        {.name = "VK_FORMAT_D32_SFLOAT",
         .depthStencilAttachment = true,
         .colorAttachment = false,
         .sampledImage = false,
         .reason = "The depth gate spans a planetary surface to orbital altitude. A 24-bit "
                   "normalised depth buffer cannot hold that range without collapse; 32-bit "
                   "float depth with a reversed-Z projection is what the gate is measured "
                   "against. This is the one requirement here that can reject a conformant "
                   "device: Vulkan mandates D16_UNORM and then guarantees only that at least "
                   "one of X8_D24_UNORM_PACK32 and D32_SFLOAT supports depth attachment, so "
                   "a device supporting only the former conforms and is still rejected. That "
                   "is a deliberate narrowing of supported hardware, not an oversight."},
    };

    requirement.requiresGraphicsAndPresentQueue = true;

    // Left disabled deliberately. Integrated devices expose host-visible system memory as
    // device-local, so a byte threshold would reject the integrated investigation tier for a
    // reason unrelated to whether it can render. Memory sufficiency is measured in P1b's
    // reports and gated at M2 against representative assets, not asserted here.
    requirement.minimumDeviceLocalMemoryBytes = 0;

    requirement.minimumMaxImageDimension2D = kVulkanGuaranteedMaxImageDimension2D;

    return requirement;
}

std::string Rejection::message() const
{
    return std::format("requires {} but device reports {} — {}", requirement, found, reason);
}

std::vector<Rejection> checkCapabilities(
    const CapabilityRequirement& requirement,
    const DeviceCapabilities& device)
{
    std::vector<Rejection> rejections;

    if (device.apiVersion < requirement.minimumApiVersion) {
        rejections.push_back({
            .requirement = std::format("Vulkan API {}", toString(requirement.minimumApiVersion)),
            .found = std::format("Vulkan API {}", toString(device.apiVersion)),
            .reason = "ADR 0002 sets Vulkan 1.2 as the candidate minimum for P1. A driver "
                      "update may raise this device; otherwise it is out of scope.",
        });
    }

    for (const RequiredExtension& extension : requirement.extensions) {
        if (!device.hasExtension(extension.name)) {
            rejections.push_back({
                .requirement = std::format("extension {}", extension.name),
                .found = "not present",
                .reason = extension.reason,
            });
        }
    }

    for (const RequiredFeature& feature : requirement.features) {
        // A null field is a malformed requirement declaration, not a hardware limitation.
        // Reporting it as "unsupported" would blame the device for our own bug, which is the
        // one thing Rejection exists to prevent.
        if (feature.field == nullptr) {
            rejections.push_back({
                .requirement = std::format("feature {}", feature.name),
                .found = "not checked — the requirement declares no field to read",
                .reason = "This is a defect in the requirement declaration, not a property of "
                          "the device. The device was not evaluated against this feature.",
            });
            continue;
        }

        if (!(device.features.*(feature.field))) {
            rejections.push_back({
                .requirement = std::format("feature {}", feature.name),
                .found = "unsupported",
                .reason = feature.reason,
            });
        }
    }

    for (const RequiredFormat& format : requirement.formats) {
        const FormatSupport* support = device.findFormat(format.name);
        if (support == nullptr) {
            rejections.push_back({
                .requirement = std::format("format {}", format.name),
                .found = "not queried",
                .reason = format.reason,
            });
            continue;
        }

        // Each usage is reported separately. "Format unsupported" would hide which of the
        // three usages actually failed, and they fail for different hardware reasons.
        const auto checkUsage = [&](bool required, bool present, std::string_view usage) {
            if (required && !present) {
                rejections.push_back({
                    .requirement = std::format("format {} usable as {}", format.name, usage),
                    .found = "unsupported under optimal tiling",
                    .reason = format.reason,
                });
            }
        };
        checkUsage(format.depthStencilAttachment,
                   support->optimalTilingDepthStencilAttachment,
                   "a depth/stencil attachment");
        checkUsage(format.colorAttachment,
                   support->optimalTilingColorAttachment,
                   "a colour attachment");
        checkUsage(format.sampledImage,
                   support->optimalTilingSampledImage,
                   "a sampled image");
    }

    if (requirement.requiresGraphicsAndPresentQueue) {
        const bool found = std::ranges::any_of(device.queueFamilies, [](const QueueFamily& family) {
            return family.graphics && family.presentation && family.count > 0;
        });
        if (!found) {
            rejections.push_back({
                .requirement = "a queue family supporting both graphics and presentation",
                .found = "none",
                .reason = "The renderer submits drawing and presentation on one queue. A "
                          "device that separates them is supportable but needs ownership "
                          "transfers this increment has no reason to implement.",
            });
        }
    }

    if (requirement.minimumDeviceLocalMemoryBytes > 0) {
        const std::uint64_t available = device.deviceLocalMemoryBytes();
        if (available < requirement.minimumDeviceLocalMemoryBytes) {
            rejections.push_back({
                .requirement = std::format("{} bytes of device-local memory",
                                           requirement.minimumDeviceLocalMemoryBytes),
                .found = std::format("{} bytes", available),
                .reason = "Insufficient device-local memory for the renderer's resources.",
            });
        }
    }

    if (device.limits.maxImageDimension2D < requirement.minimumMaxImageDimension2D) {
        rejections.push_back({
            .requirement = std::format("maxImageDimension2D of at least {}",
                                       requirement.minimumMaxImageDimension2D),
            .found = std::format("{}", device.limits.maxImageDimension2D),
            .reason = "Vulkan guarantees 4096 for any conformant implementation. A device "
                      "below it is reporting a non-conformant or software implementation.",
        });
    }

    return rejections;
}

std::string formatRejections(
    std::string_view deviceName,
    const std::vector<Rejection>& rejections)
{
    if (rejections.empty()) {
        return std::format("{}: meets every renderer requirement.", deviceName);
    }

    std::string text = std::format(
        "{} cannot run this renderer. {} unmet requirement{}:\n",
        deviceName,
        rejections.size(),
        rejections.size() == 1 ? "" : "s");

    for (const Rejection& rejection : rejections) {
        text += std::format("  - {}\n", rejection.message());
    }

    return text;
}

} // namespace sol::render
