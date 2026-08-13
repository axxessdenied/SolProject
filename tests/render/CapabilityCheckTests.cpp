/// @file
/// Exercises the capability requirement declaration and its rejection path.
///
/// This is the negative control the P1b evidence plan requires. Both GPUs available to this
/// project exceed the Vulkan 1.2 candidate floor and satisfy every requirement, so on real
/// hardware the rejection path is dead code that no run would ever enter. A gate that is
/// never exercised is not a gate. Here each requirement is failed deliberately, one at a
/// time, and the diagnostic is asserted to name the thing that was missing.

#include "Sol/Render/CapabilityRequirement.h"
#include "Support/TestCheck.h"
#include "render/BaselineDeviceProfiles.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

using sol::render::ApiVersion;
using sol::render::CapabilityRequirement;
using sol::render::DeviceCapabilities;
using sol::render::DeviceFeatures;
using sol::render::Rejection;
using sol::render::baselineRequirement;
using sol::render::checkCapabilities;

/// A device that satisfies the baseline requirement in every respect. Each negative-control
/// case starts from this and breaks exactly one thing, so a rejection can only be attributed
/// to the property that was broken.
DeviceCapabilities compliantDevice()
{
    return sol::test::gtx1060().capabilities;
}

/// True when some rejection's requirement text contains @p fragment.
bool mentions(const std::vector<Rejection>& rejections, std::string_view fragment)
{
    return std::ranges::any_of(rejections, [fragment](const Rejection& rejection) {
        return rejection.requirement.find(fragment) != std::string::npos;
    });
}

/// Every rejection must carry a non-empty reason. An "unsupported device" message with no
/// explanation is precisely what ADR 0002's actionable-diagnostics requirement rules out.
bool allCarryReasons(const std::vector<Rejection>& rejections)
{
    return std::ranges::all_of(rejections, [](const Rejection& rejection) {
        return !rejection.reason.empty() && !rejection.requirement.empty()
               && !rejection.found.empty();
    });
}

void testCompliantDeviceIsAccepted(sol::test::CheckContext& context)
{
    const auto rejections = checkCapabilities(baselineRequirement(), compliantDevice());
    context.check(rejections.empty(), "a fully compliant device produces no rejections");
    if (!rejections.empty()) {
        std::printf("        unexpected: %s\n",
                    sol::render::formatRejections("compliant", rejections).c_str());
    }
}

void testApiVersionBelowFloorIsRejected(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    device.apiVersion = ApiVersion{1, 1, 0};

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(!rejections.empty(), "a Vulkan 1.1 device is rejected");
    context.check(mentions(rejections, "Vulkan API"), "the rejection names the API version");
    context.check(allCarryReasons(rejections), "the API rejection carries an actionable reason");
}

void testMissingSwapchainExtensionIsRejected(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    std::erase(device.extensions, "VK_KHR_swapchain");

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(mentions(rejections, "VK_KHR_swapchain"),
                  "a device without VK_KHR_swapchain is rejected by name");
    context.check(allCarryReasons(rejections), "the extension rejection carries a reason");
}

void testMissingDepthFormatIsRejected(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    for (auto& format : device.formats) {
        if (format.name == "VK_FORMAT_D32_SFLOAT") {
            format.optimalTilingDepthStencilAttachment = false;
        }
    }

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(mentions(rejections, "VK_FORMAT_D32_SFLOAT"),
                  "a device that cannot use D32_SFLOAT as depth is rejected by name");
    context.check(mentions(rejections, "depth/stencil attachment"),
                  "the rejection names the specific usage that failed, not just the format");
}

void testUnqueriedFormatIsRejectedDistinctly(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    std::erase_if(device.formats, [](const auto& format) {
        return format.name == "VK_FORMAT_D32_SFLOAT";
    });

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    const bool distinct = std::ranges::any_of(rejections, [](const Rejection& rejection) {
        return rejection.found == "not queried";
    });
    context.check(distinct,
                  "a format the renderer never queried is reported as 'not queried' rather "
                  "than silently passing or masquerading as unsupported hardware");
}

void testMissingPresentQueueIsRejected(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    for (auto& family : device.queueFamilies) {
        family.presentation = false;
    }

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(mentions(rejections, "graphics and presentation"),
                  "a device with no graphics+present queue family is rejected");
}

void testZeroCountQueueFamilyDoesNotSatisfyTheRequirement(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    for (auto& family : device.queueFamilies) {
        family.count = 0;
    }

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(mentions(rejections, "graphics and presentation"),
                  "a queue family advertising the right flags but zero queues is rejected");
}

void testUnsupportedFeatureIsRejected(sol::test::CheckContext& context)
{
    // The baseline requirement declares no features, by ADR 0002's rule against undemonstrated
    // requirements. The rejection path for features still has to work for the increment that
    // first needs one, so it is exercised through a synthetic requirement here.
    CapabilityRequirement requirement = baselineRequirement();
    requirement.features.push_back({
        .name = "samplerAnisotropy",
        .field = &DeviceFeatures::samplerAnisotropy,
        .reason = "synthetic requirement exercising the feature rejection path",
    });

    DeviceCapabilities device = compliantDevice();
    device.features.samplerAnisotropy = false;

    const auto rejections = checkCapabilities(requirement, device);
    context.check(mentions(rejections, "samplerAnisotropy"),
                  "an unsupported required feature is rejected by name");
}

void testNonConformantImageDimensionIsRejected(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    device.limits.maxImageDimension2D = 2048;

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(mentions(rejections, "maxImageDimension2D"),
                  "a device below Vulkan's guaranteed image dimension is rejected");
}

void testEveryUnmetRequirementIsReportedAtOnce(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    device.apiVersion = ApiVersion{1, 0, 0};
    std::erase(device.extensions, "VK_KHR_swapchain");
    for (auto& family : device.queueFamilies) {
        family.presentation = false;
    }

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(rejections.size() >= 3,
                  "a device failing three requirements reports all three in one pass, so a "
                  "user is not sent through three rounds of trial and error");

    const std::string text = sol::render::formatRejections(device.deviceName, rejections);
    context.check(text.find(device.deviceName) != std::string::npos,
                  "the formatted diagnostic names the device it is about");
}

/// Checks the requirement set against every baseline class P1b names.
///
/// This is a design check on our requirements, not a driver test — see the warning in
/// BaselineDeviceProfiles.h. It answers one question: has the requirement declaration quietly
/// excluded a device the project has promised to support?
void testBaselineClassesAreNotExcluded(sol::test::CheckContext& context)
{
    for (const auto& profile : sol::test::allBaselineProfiles()) {
        const auto rejections = checkCapabilities(baselineRequirement(), profile.capabilities);
        context.check(rejections.empty(),
                      "baseline class is not excluded by the requirement set: " + profile.label);
        if (!rejections.empty()) {
            std::printf("        %s\n",
                        sol::render::formatRejections(profile.label, rejections).c_str());
        }
    }
}

} // namespace

int main()
{
    std::printf("Vulkan capability requirement checks\n");
    std::printf("  NOTE: baseline-class profiles are synthetic and unverified. They check the\n"
                "        requirement declaration, not any driver. See BaselineDeviceProfiles.h.\n");

    sol::test::CheckContext context("render.capability-check");

    testCompliantDeviceIsAccepted(context);
    testApiVersionBelowFloorIsRejected(context);
    testMissingSwapchainExtensionIsRejected(context);
    testMissingDepthFormatIsRejected(context);
    testUnqueriedFormatIsRejectedDistinctly(context);
    testMissingPresentQueueIsRejected(context);
    testZeroCountQueueFamilyDoesNotSatisfyTheRequirement(context);
    testUnsupportedFeatureIsRejected(context);
    testNonConformantImageDimensionIsRejected(context);
    testEveryUnmetRequirementIsReportedAtOnce(context);
    testBaselineClassesAreNotExcluded(context);

    return context.finish();
}
