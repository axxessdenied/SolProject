/// @file
/// The instance pimpl, shared between renderer-module translation units only.
///
/// This header is **internal to `sol::render`** and lives under `src/`, so it is not on any
/// consumer's include path. It exists because the renderer needs the `VkInstance` and the
/// physical-device handles that the public interface deliberately hides — and the alternative,
/// exposing an opaque `void*` publicly, would put a Vulkan handle in a public header wearing a
/// disguise. ADR 0002's boundary is about what leaves the module, not about what the module's
/// own files may share.

#pragma once

#include "Sol/Render/DeviceCapabilities.h"
#include "Sol/Render/VulkanInstance.h"

#include <volk.h>

#include <string>
#include <vector>

namespace sol::render {

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

namespace detail {

/// Builds the plain-value description of one physical device.
///
/// Shared so that `VulkanInstance::enumerateDevices` and the renderer's device selection
/// cannot drift apart: the renderer needs the `VkPhysicalDevice` handle as well as the
/// description, and re-implementing the query would let the reported capabilities differ from
/// the ones actually selected against.
[[nodiscard]] std::expected<DeviceCapabilities, std::string> describeDevice(
    VkPhysicalDevice handle);

/// Human-readable form of a VkResult, for diagnostics.
[[nodiscard]] std::string describeResult(VkResult result);

/// Vulkan's two-call enumeration idiom, retrying on VK_INCOMPLETE and resizing to what was
/// actually written. See the commentary in VulkanInstance.cpp.
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

} // namespace detail
} // namespace sol::render
