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

    [[nodiscard]] const LoaderInfo& loader() const;

    /// True when the validation layer was actually enabled, which may differ from what was
    /// requested if the layer is not installed.
    [[nodiscard]] bool validationEnabled() const;

    /// Every physical device the loader reports, in the loader's own order.
    ///
    /// The order is preserved rather than sorted: on a hybrid laptop it reflects the driver's
    /// own preference, and P1b's evidence has to record which device was actually selected
    /// rather than which one a sort put first.
    [[nodiscard]] std::vector<DeviceCapabilities> enumerateDevices() const;

private:
    struct Impl;
    explicit VulkanInstance(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sol::render
