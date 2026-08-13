/// @file
/// Enumerates this machine's Vulkan loader and devices and reports them.
///
/// Satisfies the startup half of ADR 0002's capability-reporting requirement: loader, device
/// version, features, extensions, formats, queues, limits, and memory, with a clear verdict
/// per device. Runs headless — no window is created — so it is usable as a first diagnostic
/// when a machine will not render.
///
/// It reports what is present. It does not decide which device to use; that selection belongs
/// to the renderer and is recorded per evidence run, because on a hybrid laptop the answer is
/// a driver policy rather than a property of the hardware.

#include "Sol/Render/CapabilityRequirement.h"
#include "Sol/Render/VulkanInstance.h"

#include <cstdio>
#include <string>

namespace {

constexpr double kBytesPerGibibyte = 1024.0 * 1024.0 * 1024.0;

void printLoader(const sol::render::LoaderInfo& loader, bool validationEnabled)
{
    std::printf("Loader\n");
    std::printf("  instance API      %s\n", sol::render::toString(loader.instanceVersion).c_str());
    std::printf("  layers available  %zu\n", loader.availableLayers.size());
    std::printf("  validation layer  %s\n",
                loader.hasValidationLayer() ? "installed" : "NOT INSTALLED");
    std::printf("  validation active %s\n", validationEnabled ? "yes" : "no");
    std::printf("\n");
}

void printDevice(const sol::render::DeviceCapabilities& device)
{
    std::printf("Device: %s\n", device.deviceName.c_str());
    std::printf("  kind              %s\n", std::string(sol::render::toString(device.kind)).c_str());
    std::printf("  vendor/device     0x%04X / 0x%04X\n", device.vendorId, device.deviceId);
    std::printf("  device API        %s\n", sol::render::toString(device.apiVersion).c_str());
    std::printf("  driver            %s\n", device.driverVersionText.c_str());
    std::printf("  extensions        %zu\n", device.extensions.size());
    std::printf("  device-local mem  %.2f GiB\n",
                static_cast<double>(device.deviceLocalMemoryBytes()) / kBytesPerGibibyte);
    std::printf("  maxImageDim2D     %u\n", device.limits.maxImageDimension2D);
    std::printf("  timestamp period  %.4f ns\n",
                static_cast<double>(device.limits.timestampPeriodNanoseconds));

    std::printf("  queue families\n");
    for (const sol::render::QueueFamily& family : device.queueFamilies) {
        // A family with none of the recorded capabilities is real — video decode and encode
        // families are common — and must not print as a blank line that reads like a bug.
        const bool anyRecorded =
            family.graphics || family.compute || family.transfer || family.presentation;
        std::printf("    [%u] count=%-3u%s%s%s%s%s\n",
                    family.index,
                    family.count,
                    family.graphics ? " graphics" : "",
                    family.compute ? " compute" : "",
                    family.transfer ? " transfer" : "",
                    family.presentation ? " present" : "",
                    anyRecorded ? "" : " (none of graphics/compute/transfer/present)");
    }

    std::printf("  queried formats\n");
    for (const sol::render::FormatSupport& format : device.formats) {
        std::printf("    %-32s%s%s%s\n",
                    format.name.c_str(),
                    format.optimalTilingDepthStencilAttachment ? " depth-stencil" : "",
                    format.optimalTilingColorAttachment ? " colour" : "",
                    format.optimalTilingSampledImage ? " sampled" : "");
    }
}

} // namespace

int main()
{
    const sol::render::InstanceConfig config{
        .applicationName = "SolRenderCapabilityReport",
        .validation = sol::render::ValidationMode::Enabled,
        .requestedApiVersion = {1, 2, 0},
    };

    auto instance = sol::render::VulkanInstance::create(config);
    if (!instance.has_value()) {
        // Not a test failure. An absent loader is a real end-user condition, and this tool's
        // job in that case is to produce the message the user needs rather than a stack trace.
        std::printf("No usable Vulkan instance on this machine.\n\n%s\n",
                    instance.error().c_str());
        std::printf("\nSKIPPED: nothing to report without a loader.\n");
        return 0;
    }

    printLoader(instance->loader(), instance->validationEnabled());

    const auto devices = instance->enumerateDevices();
    if (devices.empty()) {
        std::printf("The loader reported no physical devices.\n");
        return 1;
    }

    const sol::render::CapabilityRequirement requirement = sol::render::baselineRequirement();

    int acceptable = 0;
    for (const auto& device : devices) {
        printDevice(device);

        const auto rejections = sol::render::checkCapabilities(requirement, device);
        if (rejections.empty()) {
            ++acceptable;
            std::printf("  VERDICT           meets every renderer requirement\n\n");
        } else {
            std::printf("  VERDICT           rejected\n%s\n",
                        sol::render::formatRejections(device.deviceName, rejections).c_str());
        }
    }

    std::printf("%d of %zu device(s) meet the requirement set.\n", acceptable, devices.size());

    if (acceptable == 0) {
        std::printf("FAILED: a Vulkan loader is present but no device can run the renderer.\n");
        return 1;
    }
    return 0;
}
