#include "Sol/Render/DeviceCapabilities.h"

#include <algorithm>
#include <format>

namespace sol::render {

std::string toString(const ApiVersion& version)
{
    return std::format("{}.{}.{}", version.major, version.minor, version.patch);
}

std::string_view toString(DeviceKind kind)
{
    switch (kind) {
    case DeviceKind::Other:
        return "other";
    case DeviceKind::IntegratedGpu:
        return "integrated GPU";
    case DeviceKind::DiscreteGpu:
        return "discrete GPU";
    case DeviceKind::VirtualGpu:
        return "virtual GPU";
    case DeviceKind::Cpu:
        return "CPU";
    }
    return "unrecognised";
}

bool DeviceCapabilities::hasExtension(std::string_view name) const
{
    return std::ranges::find(extensions, name) != extensions.end();
}

const FormatSupport* DeviceCapabilities::findFormat(std::string_view name) const
{
    const auto it = std::ranges::find(formats, name, &FormatSupport::name);
    return it == formats.end() ? nullptr : &*it;
}

std::uint64_t DeviceCapabilities::deviceLocalMemoryBytes() const
{
    std::uint64_t total = 0;
    for (const MemoryHeap& heap : memoryHeaps) {
        if (heap.deviceLocal) {
            total += heap.sizeBytes;
        }
    }
    return total;
}

} // namespace sol::render
