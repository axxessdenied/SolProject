/// @file
/// Loader initialisation, instance creation, and physical-device enumeration.
///
/// This is the only interface through which the rest of the project reaches Vulkan, and no
/// Vulkan type crosses it. The implementation is hidden behind a pointer rather than a header
/// include, so ADR 0002's boundary is enforced by the compiler: a consumer that wanted a
/// VkDevice could not name one.

#pragma once

#include "Sol/Render/DeviceCapabilities.h"

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace sol::render {

/// Facts about the installed loader, independent of any device.
///
/// Recorded in every P1b report. The loader is a separately installed component on Windows
/// and its version is not implied by either the SDK or the driver.
struct LoaderInfo {
    /// Highest instance-level API version the loader supports.
    ApiVersion instanceVersion;
    std::vector<std::string> availableLayers;
    std::vector<std::string> availableExtensions;

    /// True when the Khronos validation layer is installed and can be enabled.
    [[nodiscard]] bool hasValidationLayer() const;
};

/// Whether to enable the Khronos validation layer.
///
/// P1b gates on clean validation output, so this is a first-class configuration rather than a
/// debug-build accident. Validation runs are reported separately from performance runs under
/// the milestone's shared measurement rules.
enum class ValidationMode : std::uint8_t {
    Disabled,
    Enabled,
};

/// Instance creation inputs.
struct InstanceConfig {
    std::string applicationName = "Frontiers of Sol";
    ValidationMode validation = ValidationMode::Disabled;

    /// Requesting the ADR 0002 floor rather than the loader's maximum is deliberate. It keeps
    /// a 1.3-or-later capability from being used by accident on this machine's hardware and
    /// silently becoming an undeclared baseline requirement, which ADR 0002 forbids.
    ApiVersion requestedApiVersion{1, 2, 0};
};

/// An initialised Vulkan loader and instance.
///
/// Move-only; destroying it destroys the instance. Devices enumerated from it hold no
/// resources and remain valid after it is gone, because they are plain values.
///
/// @par Moved-from state
/// A moved-from `VulkanInstance` may only be destroyed or assigned to. Calling @ref loader,
/// @ref validationEnabled, or @ref enumerateDevices on one is undefined. This matters more
/// than the usual moved-from caveat because @ref create returns `std::expected`, so
/// `std::move(*result)` leaves a moved-from instance sitting inside a `result` that still
/// tests as having a value.
///
/// @par Threading and instance count
/// Not thread-safe, and instances are **not independent**: the underlying loader keeps one
/// process-global dispatch table, so creating a second `VulkanInstance` rebinds the entry
/// points the first one dispatches through. Create one, on one thread, and keep it. This is a
/// property of the loader shim rather than of Vulkan itself, and it is a documented contract
/// rather than an enforced one because B1 has no use for a second instance.
class VulkanInstance {
public:
    /// Initialises the loader and creates an instance.
    ///
    /// Returns an actionable diagnostic string on failure rather than throwing or aborting.
    /// The most common failure — no Vulkan loader installed — is a user-fixable condition on
    /// an end user's machine, and ADR 0002 requires the shipped game to report it clearly.
    [[nodiscard]] static std::expected<VulkanInstance, std::string> create(
        const InstanceConfig& config);

    ~VulkanInstance();

    VulkanInstance(VulkanInstance&&) noexcept;
    VulkanInstance& operator=(VulkanInstance&&) noexcept;
    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    /// @pre This instance has not been moved from.
    [[nodiscard]] const LoaderInfo& loader() const;

    /// True when the validation layer was actually enabled, which may differ from what was
    /// requested if the layer is not installed.
    ///
    /// @pre This instance has not been moved from.
    [[nodiscard]] bool validationEnabled() const;

    /// Validation messages collected since instance creation, oldest first.
    ///
    /// Empty when validation is disabled. P1b gates on clean validation output, so the
    /// messages have to be a value the project holds rather than text that went to a debugger
    /// console nobody captured.
    ///
    /// @pre This instance has not been moved from.
    [[nodiscard]] const std::vector<std::string>& validationMessages() const;

    /// Every physical device the loader reports, in the loader's own order.
    ///
    /// The order is preserved rather than sorted: on a hybrid laptop it reflects the driver's
    /// own preference, and P1b's evidence has to record which device was actually selected
    /// rather than which one a sort put first.
    ///
    /// Returns a diagnostic rather than an empty vector when enumeration fails, so that "the
    /// query failed" and "this machine genuinely has no devices" stay distinguishable. ADR
    /// 0002 gates on actionable diagnostics, and a report that says "no devices" when the
    /// truth is "the driver returned an error" is worse than no report.
    ///
    /// @pre This instance has not been moved from.
    [[nodiscard]] std::expected<std::vector<DeviceCapabilities>, std::string>
    enumerateDevices() const;

private:
    struct Impl;
    explicit VulkanInstance(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sol::render
