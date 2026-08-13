/// @file
/// The presenting renderer: device, swapchain, depth buffer, and frame loop.
///
/// No Vulkan type crosses this interface (ADR 0002). The window is handed over as the
/// operating system's own handles, so `sol::platform` and `sol::render` share a seam without
/// sharing a library.

#pragma once

#include "Sol/Render/DeviceCapabilities.h"
#include "Sol/Render/WorldVec3.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace sol::render {

class VulkanInstance;

/// The window the renderer presents to, in OS terms.
struct SurfaceTarget {
    /// `HWND` on Windows.
    void* nativeWindow = nullptr;
    /// `HINSTANCE` on Windows.
    void* nativeInstance = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// Camera pose in the authoritative world frame.
///
/// Position is `double` and world-space; the renderer subtracts it before anything reaches the
/// GPU. @ref nearPlaneMetres is the only depth-precision control, because the projection has
/// no far plane — see the reversed-Z rationale in the implementation.
struct CameraState {
    WorldVec3 position;
    WorldVec3 forward{0.0, 0.0, -1.0};
    WorldVec3 up{0.0, 1.0, 0.0};
    double verticalFovRadians = 1.0472; // 60 degrees
    double nearPlaneMetres = 0.1;
};

/// One object in the reference scene.
///
/// A unit cube scaled by @ref radiusMetres and placed at @ref worldPosition. Crude on purpose:
/// B1 gates on depth behaviour and precision, not on content, and authored geometry would add
/// an asset pipeline dependency this increment has no reason to take on.
struct SceneObject {
    WorldVec3 worldPosition;
    double radiusMetres = 1.0;
    float colour[3]{1.0F, 1.0F, 1.0F};
};

/// What one presented frame did, for the performance and jitter reports.
struct FrameStats {
    /// Frames presented since creation.
    std::uint64_t frameIndex = 0;
    /// True when the swapchain was rebuilt this frame, which invalidates timing for it.
    bool swapchainRebuilt = false;
};

/// A device, swapchain, and frame loop presenting to one window.
///
/// Move-only. Not thread-safe; a moved-from renderer may only be destroyed or assigned to.
class Renderer {
public:
    /// Selects a device, creates the swapchain, and prepares the frame loop.
    ///
    /// @param instance must outlive the renderer.
    /// @param target   the window to present to.
    ///
    /// Fails with an actionable diagnostic when no device meets the requirement set, naming
    /// every unmet requirement per device rather than reporting a bare "unsupported".
    [[nodiscard]] static std::expected<Renderer, std::string> create(
        const VulkanInstance& instance,
        const SurfaceTarget& target);

    ~Renderer();

    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// The device actually selected. Recorded in every evidence report, because on a hybrid
    /// laptop which device serves a surface is a driver policy rather than a fact about the
    /// hardware.
    [[nodiscard]] const DeviceCapabilities& selectedDevice() const;

    /// Replaces the reference scene.
    void setScene(std::vector<SceneObject> objects);

    /// Tells the renderer the framebuffer changed size. The swapchain is rebuilt on the next
    /// frame rather than immediately, so a burst of resize events costs one rebuild.
    void notifyResized(std::uint32_t width, std::uint32_t height);

    /// Renders and presents one frame.
    ///
    /// Returns without presenting, and without error, when the framebuffer has zero area — a
    /// minimised window is a normal state, not a failure.
    [[nodiscard]] std::expected<FrameStats, std::string> renderFrame(const CameraState& camera);

    /// Blocks until the device is idle. Call before destroying anything the GPU may still be
    /// reading; the destructor does this too.
    void waitIdle();

private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sol::render
