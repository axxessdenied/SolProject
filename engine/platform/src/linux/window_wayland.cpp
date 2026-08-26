// Wayland window and input (Phase 21) — STAGE A SKELETON, filled in by stage B.
//
// Everything here compiles, links and reports "no window". That is what lets
// stage A prove the interesting thing: that the ten headless suites build and
// pass on Linux, so everything OUTSIDE this seam is portable as measured
// rather than as asserted. `create` returning false is a real state the caller
// already handles (it is how a failed swapchain surfaces on Windows).
//
// ⚑⚑ TWO CONTRACTS STAGE B MUST HONOUR THAT THIS FILE CANNOT INHERIT, both
// recorded here because they are invisible from the header's signatures:
//
// 1. `textInput()`'s documentation in window.hpp describes the WIN32
//    MECHANISM - "the message hook runs before this is recorded and swallows
//    keyboard messages while ImGui wants them". The CONTRACT is portable ("text
//    the dev UI consumed never appears here"); the mechanism is not. Wayland
//    has no message hook, so stage B honours it by appending text only when
//    ImGui does not want the keyboard.
//
// 2. ⚑⚑⚑ AND THE OPPOSITE RULE APPLIES TO KEY STATE, WHICH IS THE ONE PHASE 20
//    TURNS ON. `window_win32.cpp:152` records keyDown[] BEFORE the dev-UI hook
//    on purpose - a key is a physical fact, and a key ImGui takes the "up" for
//    must not latch down forever. So on Wayland `wl_keyboard.key` ALWAYS writes
//    keyDown[], regardless of what ImGui wants. Protect the text, never the key
//    state. Getting these two backwards reintroduces Phase 20's defect on Linux
//    only, where no test in this repo would catch it.

#include "sol/platform/window.hpp"

#include <string>

namespace sol::platform {

struct Window::Impl
{
    MessageHook messageHook = nullptr;
    bool cursorLocked = false;
    std::string textInput;
};

Window::Window() : m_impl(std::make_unique<Impl>())
{
}

Window::~Window() = default;

bool Window::create(const WindowDesc& desc)
{
    (void)desc;
    return false; // stage B
}

void Window::destroy()
{
}

void Window::pumpEvents()
{
}

bool Window::shouldClose() const
{
    // True, not false: a caller that ignores create()'s failure and spins the
    // frame loop anyway gets one iteration and exits, rather than a hot loop
    // on a window that does not exist.
    return true;
}

std::uint32_t Window::width() const
{
    return 0;
}

std::uint32_t Window::height() const
{
    return 0;
}

bool Window::isMinimized() const
{
    return false;
}

bool Window::consumeResize()
{
    return false;
}

bool Window::isKeyDown(Key key) const
{
    (void)key;
    return false;
}

bool Window::isMouseButtonDown(MouseButton button) const
{
    (void)button;
    return false;
}

core::Vec2 Window::mouseDelta() const
{
    return {};
}

core::Vec2 Window::mousePosition() const
{
    return {};
}

float Window::wheelDelta() const
{
    return 0.0f;
}

std::string_view Window::textInput() const
{
    return m_impl->textInput;
}

void Window::setCursorLocked(bool locked)
{
    m_impl->cursorLocked = locked;
}

bool Window::isCursorLocked() const
{
    return m_impl->cursorLocked;
}

void Window::setMessageHook(MessageHook hook)
{
    // ⚑ Kept, not discarded, even though Wayland will never call it. The hook
    // is the one Win32-SHAPED thing in this interface (window.hpp:104 mirrors
    // HWND/UINT/WPARAM/LPARAM), and Phase 21 decided against refactoring it
    // rather than against supporting it. Storing it keeps setMessageHook /
    // nativeHandle honest as accessors and leaves the door open if a later
    // phase does make the hook semantic.
    m_impl->messageHook = hook;
}

NativeWindowHandle Window::nativeHandle() const
{
    // ⚑ On Wayland these two become wl_surface* and wl_display*, which is
    // exactly the pair vkCreateWaylandSurfaceKHR wants - the struct that looks
    // most Windows-specific is the one that ports for free.
    return {};
}

} // namespace sol::platform
