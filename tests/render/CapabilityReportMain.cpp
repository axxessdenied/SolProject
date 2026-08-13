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

/// Reported to CTest as a skip rather than a pass. A machine with no Vulkan loader must not
/// contribute a green result to a suite total that is quoted as evidence.
constexpr int kSkipExitCode = 77;

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
    std::printf("  driver            %s (vendor encoding; not comparable across vendors)\n",
                device.driverVersionText.c_str());

    // ADR 0002 requires the startup report to enumerate features, extensions, limits, and
    // memory — not merely to have queried them. Printing counts and aggregates instead was
    // hiding most of what the enumeration collects.
    std::printf("  features\n");
    std::printf("    samplerAnisotropy   %s\n", device.features.samplerAnisotropy ? "yes" : "no");
    std::printf("    depthClamp          %s\n", device.features.depthClamp ? "yes" : "no");
    std::printf("    fillModeNonSolid    %s\n", device.features.fillModeNonSolid ? "yes" : "no");
    std::printf("    independentBlend    %s\n", device.features.independentBlend ? "yes" : "no");
    std::printf("    timelineSemaphore   %s\n", device.features.timelineSemaphore ? "yes" : "no");

    std::printf("  limits\n");
    std::printf("    maxImageDimension2D     %u\n", device.limits.maxImageDimension2D);
    std::printf("    maxSamplerAnisotropy    %.1f\n",
                static_cast<double>(device.limits.maxSamplerAnisotropy));
    std::printf("    maxViewports            %u\n", device.limits.maxViewports);
    std::printf("    bufferImageGranularity  %llu\n",
                static_cast<unsigned long long>(device.limits.bufferImageGranularity));
    std::printf("    timestampPeriod         %.4f ns\n",
                static_cast<double>(device.limits.timestampPeriodNanoseconds));

    std::printf("  memory heaps\n");
    for (const sol::render::MemoryHeap& heap : device.memoryHeaps) {
        std::printf("    [%u] %8.2f GiB%s\n",
                    heap.index,
                    static_cast<double>(heap.sizeBytes) / kBytesPerGibibyte,
                    heap.deviceLocal ? "  device-local" : "");
    }
    std::printf("    total device-local      %.2f GiB\n",
                static_cast<double>(device.deviceLocalMemoryBytes()) / kBytesPerGibibyte);

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

    // Names, not just a count. An extension list is provenance: a report saying "252
    // extensions" cannot be reconciled against another machine's, and reconciliation is the
    // whole purpose of recording it.
    std::printf("  extensions (%zu)\n", device.extensions.size());
    for (const std::string& extension : device.extensions) {
        std::printf("    %s\n", extension.c_str());
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
        return kSkipExitCode;
    }

    printLoader(instance->loader(), instance->validationEnabled());

    const auto devices = instance->enumerateDevices();
    if (!devices.has_value()) {
        // Distinct from "no devices". Conflating the two would report a driver failure as an
        // absent GPU and send the reader looking for the wrong problem.
        std::printf("Device enumeration failed.\n\n%s\n", devices.error().c_str());
        return 1;
    }
    if (devices->empty()) {
        std::printf("The loader reported no physical devices.\n");
        return 1;
    }

    const sol::render::CapabilityRequirement requirement = sol::render::baselineRequirement();

    int acceptable = 0;
    for (const auto& device : *devices) {
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

    std::printf("%d of %zu device(s) meet the requirement set.\n", acceptable, devices->size());

    // The validation gate is about what the layer actually said, so the messages are printed
    // whether or not there are any. "0 messages" is the evidence; silence is not.
    const auto& messages = instance->validationMessages();
    if (instance->validationEnabled()) {
        std::printf("\nValidation messages: %zu\n", messages.size());
        for (const std::string& message : messages) {
            std::printf("  %s\n", message.c_str());
        }
    } else {
        std::printf("\nValidation was not active; no validation evidence from this run.\n");
    }

    if (acceptable == 0) {
        std::printf("FAILED: a Vulkan loader is present but no device can run the renderer.\n");
        return 1;
    }
    return 0;
}
