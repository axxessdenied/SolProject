/// @file
/// What the renderer requires of a device, declared as data, and the pure function that
/// decides whether a device meets it.
///
/// The separation is deliberate and is required by the P1b evidence plan. Requirements are a
/// value; checking is a pure function of (requirement, capabilities); querying a real device
/// happens elsewhere. Only the last of those three needs a GPU, so the first two can be
/// exercised against devices this project does not own — including every device class the
/// baseline names and none of which is available.
///
/// ADR 0002 also constrains what may appear here: "Do not select mandatory non-core
/// extensions, shader language/compiler, allocator, rendering framework, or pinned SDK until
/// their owning prototype increment demonstrates the need." Every entry in the default
/// requirement below therefore carries the reason it is load-bearing. A requirement without
/// one is a support claim nobody has justified.

#pragma once

#include "Sol/Render/DeviceCapabilities.h"

#include <string>
#include <vector>

namespace sol::render {

/// One required image format and the usages that must be supported under optimal tiling.
struct RequiredFormat {
    std::string name;
    bool depthStencilAttachment = false;
    bool colorAttachment = false;
    bool sampledImage = false;
    /// Why the renderer needs it. Reported verbatim when the requirement is unmet.
    std::string reason;
};

/// One required boolean device feature.
struct RequiredFeature {
    /// Human-readable feature name, matching the field in @ref DeviceFeatures.
    std::string name;
    /// Selects the field from a capabilities value.
    bool DeviceFeatures::* field = nullptr;
    std::string reason;
};

/// A named extension the renderer cannot run without.
struct RequiredExtension {
    std::string name;
    std::string reason;
};

/// The complete set of demands the renderer places on a physical device.
struct CapabilityRequirement {
    /// ADR 0002's candidate floor. Devices below it are rejected; devices above it are used
    /// at the floor unless an optional path queries for more.
    ApiVersion minimumApiVersion{1, 2, 0};

    std::vector<RequiredExtension> extensions;
    std::vector<RequiredFeature> features;
    std::vector<RequiredFormat> formats;

    /// The renderer needs one queue family that can both draw and present. Splitting them is
    /// supportable but adds ownership transfers this increment has no reason to take on.
    bool requiresGraphicsAndPresentQueue = true;

    /// Minimum total device-local memory, in bytes. Zero disables the check.
    std::uint64_t minimumDeviceLocalMemoryBytes = 0;

    std::uint32_t minimumMaxImageDimension2D = 0;
};

/// The requirement set increment B1 actually imposes.
///
/// Kept small on purpose. Each addition is a narrowing of the hardware the game runs on, and
/// ADR 0002 requires a demonstrated need before any is added.
[[nodiscard]] CapabilityRequirement baselineRequirement();

/// One reason a device was rejected.
///
/// ADR 0002 requires "graceful rejection of unsupported drivers and devices with actionable
/// diagnostics". A rejection that says only "unsupported device" fails that. Every field here
/// exists so the message can name what was wanted, what was found, and what the user can do.
struct Rejection {
    /// What was required, e.g. "Vulkan API 1.2.0" or "extension VK_KHR_swapchain".
    std::string requirement;
    /// What the device actually reports.
    std::string found;
    /// Why the renderer needs it, carried through from the requirement declaration.
    std::string reason;

    /// A single-line rendering suitable for a log or an error dialog.
    [[nodiscard]] std::string message() const;
};

/// Evaluates @p device against @p requirement.
///
/// Pure: no Vulkan calls, no global state, no ordering dependence on when it is called.
///
/// Returns every unmet requirement rather than the first. A device that is three versions old
/// and missing two extensions should say so once, not across three runs of trial and error.
/// An empty result means the device is acceptable.
[[nodiscard]] std::vector<Rejection> checkCapabilities(
    const CapabilityRequirement& requirement,
    const DeviceCapabilities& device);

/// Formats a rejection list as an actionable multi-line diagnostic naming @p deviceName.
[[nodiscard]] std::string formatRejections(
    std::string_view deviceName,
    const std::vector<Rejection>& rejections);

} // namespace sol::render
