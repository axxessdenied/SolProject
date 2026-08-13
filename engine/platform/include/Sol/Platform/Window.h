/// @file
/// A native window, with no graphics API in its interface.
///
/// `sol::platform` owns the window library; `sol::render` owns Vulkan. Neither appears in the
/// other's headers. The seam between them is @ref NativeWindowHandle: the window hands over
/// the operating system's own handles, and the renderer builds its surface from those. That
/// keeps ADR 0002's rule intact — Vulkan stays inside the renderer — without the window
/// module having to know a surface exists.

#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace sol::platform {

/// Operating-system window handles, as plain pointers.
///
/// On Windows these are `HWND` and `HINSTANCE`. They are `void*` here so that neither
/// `windows.h` nor a window library's headers enter this interface.
struct NativeWindowHandle {
    void* window = nullptr;
    void* instance = nullptr;
};

/// Framebuffer size in pixels.
///
/// Distinct from window size: on a display with scaling they differ, and a swapchain is sized
/// in pixels. Using the wrong one produces a stretched image that looks like a projection bug.
struct FramebufferSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// Keys the flight and camera paths read.
///
/// A deliberately small set, matching the bindings recorded in `docs/project_status.md` that
/// B1's camera traversal actually needs. It is not an input abstraction; input device support
/// is a later milestone's decision.
enum class Key : std::uint8_t {
    W, A, S, D, Q, E, Shift, Control, Space, Escape,
};

/// A window and its event loop.
///
/// Move-only. Not thread-safe: the underlying window library requires that window creation and
/// event polling happen on the thread that initialised it.
class Window {
public:
    /// Creates a window with no graphics-API context attached.
    ///
    /// Returns an actionable diagnostic on failure — a headless session, a missing display, or
    /// a window library that could not initialise are all real end-user conditions.
    [[nodiscard]] static std::expected<Window, std::string> create(
        const std::string& title,
        std::uint32_t width,
        std::uint32_t height);

    ~Window();

    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// @pre This window has not been moved from.
    [[nodiscard]] NativeWindowHandle nativeHandle() const;

    /// Current framebuffer size in pixels. Zero in either axis while the window is minimised,
    /// which callers must treat as "do not render" rather than as an error.
    [[nodiscard]] FramebufferSize framebufferSize() const;

    [[nodiscard]] bool shouldClose() const;

    /// True when the framebuffer changed size since the last call, and clears the flag.
    /// The swapchain must be rebuilt when this returns true.
    [[nodiscard]] bool consumeResizeFlag();

    [[nodiscard]] bool isKeyDown(Key key) const;

    /// Processes pending window-system events. Must be called once per frame.
    void pollEvents();

    /// Requests that the window close, as if the user had closed it. Used by headless-style
    /// runs that render a fixed number of frames and exit.
    void requestClose();

private:
    struct Impl;
    explicit Window(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace sol::platform
