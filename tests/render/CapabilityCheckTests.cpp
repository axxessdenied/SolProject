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
#include "render/LunarGBaselineProfiles.h"

#include <algorithm>
#include <cstdint>
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
///
/// Requires at least one rejection. `all_of` over an empty list is vacuously true, so without
/// this a regression that produced no rejections at all would *pass* the "carries an
/// actionable reason" check while failing the one beside it.
bool allCarryReasons(const std::vector<Rejection>& rejections)
{
    return !rejections.empty()
           && std::ranges::all_of(rejections, [](const Rejection& rejection) {
                  return !rejection.reason.empty() && !rejection.requirement.empty()
                         && !rejection.found.empty();
              });
}

/// True when exactly one requirement was rejected and it mentions @p fragment.
///
/// Each negative control breaks one property of an otherwise-compliant device, so anything
/// beyond one rejection means the check is firing on something it should not. Asserting only
/// "at least one mentions X" would let a spurious second rejection pass unnoticed.
bool onlyRejection(const std::vector<Rejection>& rejections, std::string_view fragment)
{
    return rejections.size() == 1
           && rejections.front().requirement.find(fragment) != std::string::npos;
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
    context.check(onlyRejection(rejections, "Vulkan API"),
                  "a Vulkan 1.1 device is rejected for the API version and nothing else");
    context.check(allCarryReasons(rejections), "the API rejection carries an actionable reason");
}

void testMissingSwapchainExtensionIsRejected(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    std::erase(device.extensions, "VK_KHR_swapchain");

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(onlyRejection(rejections, "VK_KHR_swapchain"),
                  "a device without VK_KHR_swapchain is rejected by name and nothing else");
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
    context.check(onlyRejection(rejections, "graphics and presentation"),
                  "a device with no graphics+present queue family is rejected, and only for "
                  "that");
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
    context.check(onlyRejection(rejections, "maxImageDimension2D"),
                  "a device below Vulkan's guaranteed image dimension is rejected, and only "
                  "for that");
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
    context.checkEqual(rejections.size(), std::size_t{3},
                       "a device failing three requirements reports exactly three, so a user "
                       "is not sent through three rounds of trial and error");
    context.check(mentions(rejections, "Vulkan API"), "all three: the API version is named");
    context.check(mentions(rejections, "VK_KHR_swapchain"), "all three: the extension is named");
    context.check(mentions(rejections, "graphics and presentation"),
                  "all three: the queue family is named");

    const std::string text = sol::render::formatRejections(device.deviceName, rejections);
    context.check(text.find(device.deviceName) != std::string::npos,
                  "the formatted diagnostic names the device it is about");
}

/// A conformant device that supports only the other guaranteed depth format is still rejected.
///
/// Vulkan mandates D16_UNORM for depth attachment and then guarantees only that at least one
/// of X8_D24_UNORM_PACK32 and D32_SFLOAT supports it. The requirement set demands D32_SFLOAT
/// specifically, so such a device conforms and is rejected anyway. That is a deliberate
/// narrowing recorded in the requirement's own reason text, and this test exists so the
/// narrowing is visible rather than discovered on someone's hardware.
void testConformantDeviceWithoutD32IsStillRejected(sol::test::CheckContext& context)
{
    DeviceCapabilities device = compliantDevice();
    for (auto& format : device.formats) {
        if (format.name == "VK_FORMAT_D32_SFLOAT") {
            format.optimalTilingDepthStencilAttachment = false;
        }
    }
    device.formats.push_back({.name = "VK_FORMAT_X8_D24_UNORM_PACK32",
                              .optimalTilingDepthStencilAttachment = true,
                              .optimalTilingColorAttachment = false,
                              .optimalTilingSampledImage = true});

    const auto rejections = checkCapabilities(baselineRequirement(), device);
    context.check(!rejections.empty(),
                  "a device offering only X8_D24_UNORM_PACK32 depth is rejected, which is a "
                  "conformant device the requirement set deliberately excludes");
}

/// A requirement declaring no field to read must not be reported as a hardware limitation.
void testMalformedFeatureRequirementIsNotBlamedOnTheDevice(sol::test::CheckContext& context)
{
    CapabilityRequirement requirement = baselineRequirement();
    requirement.features.push_back({
        .name = "someFeature",
        .field = nullptr,
        .reason = "synthetic malformed requirement",
    });

    const auto rejections = checkCapabilities(requirement, compliantDevice());
    const bool blamed = std::ranges::any_of(rejections, [](const Rejection& rejection) {
        return rejection.found == "unsupported";
    });
    context.check(!rejections.empty() && !blamed,
                  "a malformed requirement is reported as our defect, not as unsupported "
                  "hardware");
}

/// Checks the requirement set against every baseline class P1b names.
///
/// This is a design check on our requirements, not a driver test — see the warning in
/// BaselineDeviceProfiles.h. It answers one question: has the requirement declaration quietly
/// excluded a device the project has promised to support?
///
/// @warning It answers that question weakly today, and the weakness is worth naming. All four
/// profiles are built from the same three helper functions and differ, across every field the
/// requirement actually consults, in exactly one value — maxImageDimension2D, at 32768 versus
/// 16384, against a threshold of 4096. So this is one assertion evaluated four times, not four
/// independent checks, and the profiles were authored by the same hand that wrote the
/// requirements. Populating them from real vulkan.gpuinfo.org reports is what would give this
/// test independent power.
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

/// Checks the requirement set against LunarG's desktop-baseline intersection profiles.
///
/// This is what the hand-authored device-class profiles above could not be. Each profile is the
/// intersection of a collection of real vulkan.gpuinfo.org reports, shipped in the pinned SDK
/// at a recorded digest, so a pass here says every device in that collection clears our
/// requirement — not that four values someone typed clear requirements the same person wrote.
///
/// The four profiles span Vulkan 1.1 to 1.4, so **both signs are exercised with real-derived
/// data**: the 2022 intersection must be rejected for being below ADR 0002's floor, and the
/// rest must be accepted. The expectation travels with each profile rather than being derived
/// from what the code currently does, so a change in either the requirement set or the SDK has
/// to be looked at rather than silently re-baselined.
void testLunarGIntersectionProfiles(sol::test::CheckContext& context)
{
    for (const auto& profile : sol::test::allLunarGProfiles()) {
        const auto rejections = checkCapabilities(sol::test::profileAnswerableRequirement(),
                                                  profile.capabilities);
        const bool acceptable = rejections.empty();

        context.checkEqual(acceptable,
                           profile.expectedAcceptable,
                           profile.label + " — " + profile.expectation);

        // A rejection has to be for the reason claimed. "Rejected" alone would also be
        // satisfied by a profile failing on a transcription slip in some unrelated field,
        // which would look like the floor doing its job while proving nothing about it.
        if (!profile.expectedAcceptable) {
            context.check(mentions(rejections, "Vulkan API"),
                          profile.label + " — rejected specifically on the API version");
        }

        if (!rejections.empty() && profile.expectedAcceptable) {
            std::printf("        %s\n",
                        sol::render::formatRejections(profile.label, rejections).c_str());
        }
    }
}

/// Pins which requirements a profile check drops, and why that set is exactly these two.
///
/// `profileAnswerableRequirement()` removes the graphics-and-present queue clause and the
/// device-local memory floor, because the Vulkan profile schema describes neither. That is a
/// legitimate reduction, and it is also the kind of reduction that quietly grows: a requirement
/// added later that profiles cannot answer would either be dropped without anyone noticing, or
/// would start failing every profile for a schema reason reported as a hardware one.
///
/// So the reduction is asserted rather than trusted. If this test fails, the right response is
/// to decide what the new requirement means for profile-based evidence — not to widen the
/// reduction until it passes.
void testProfileReductionIsExactlyWhatWeThink(sol::test::CheckContext& context)
{
    const CapabilityRequirement full = baselineRequirement();
    const CapabilityRequirement reduced = sol::test::profileAnswerableRequirement();

    context.check(full.requiresGraphicsAndPresentQueue && !reduced.requiresGraphicsAndPresentQueue,
                  "the queue-family clause is the one a profile cannot answer, and is dropped");

    // Everything a profile *can* answer must survive the reduction untouched. Compared by size
    // and content rather than by trusting that the copy left them alone.
    context.checkEqual(reduced.minimumApiVersion,
                       full.minimumApiVersion,
                       "profile reduction preserves the API-version floor");
    context.checkEqual(reduced.extensions.size(),
                       full.extensions.size(),
                       "profile reduction preserves every required extension");
    context.checkEqual(reduced.formats.size(),
                       full.formats.size(),
                       "profile reduction preserves every required format");
    context.checkEqual(reduced.features.size(),
                       full.features.size(),
                       "profile reduction preserves every required feature");
    context.checkEqual(reduced.minimumMaxImageDimension2D,
                       full.minimumMaxImageDimension2D,
                       "profile reduction preserves the image-dimension floor");

    // The memory floor is dropped, and is currently zero anyway. Asserting it is zero in the
    // full requirement keeps this test honest: if a later increment sets a real floor, the
    // reduction starts hiding something and this line is where that becomes visible.
    context.checkEqual(full.minimumDeviceLocalMemoryBytes,
                       std::uint64_t{0},
                       "no device-local memory floor is set, so dropping it hides nothing");
}

} // namespace

int main()
{
    std::printf("Vulkan capability requirement checks\n");
    std::printf("  Two profile families are checked here and they are NOT equivalent:\n"
                "    device-class profiles (BaselineDeviceProfiles.h) are hand-authored from\n"
                "      published specifications, still unverified per device, and near-identical\n"
                "      across every field the requirement consults -- one assertion, not four.\n"
                "    LunarG intersection profiles (LunarGBaselineProfiles.h) are transcribed from\n"
                "      the pinned SDK's VP_LUNARG_desktop_baseline.json at a recorded digest, each\n"
                "      the intersection of a collection of real gpuinfo.org reports. A pass there\n"
                "      holds for every device in the collection.\n"
                "  Neither is a driver test. No baseline-class device is present on this machine.\n");

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
    testConformantDeviceWithoutD32IsStillRejected(context);
    testMalformedFeatureRequirementIsNotBlamedOnTheDevice(context);
    testBaselineClassesAreNotExcluded(context);
    testProfileReductionIsExactlyWhatWeThink(context);
    testLunarGIntersectionProfiles(context);

    return context.finish();
}
