#pragma once

#include <cstdint>
#include <memory>

namespace sol::platform {

struct NativeWindowHandle
{
    void* windowHandle = nullptr;   // HWND on Windows
    void* instanceHandle = nullptr; // HINSTANCE on Windows
};

enum class Key : std::uint8_t
{
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Escape, Space, Enter, Tab,
    LeftShift, LeftControl, LeftAlt,
    Up, Down, Left, Right,
    Count
};

struct WindowDesc
{
    const char* title = "Sol";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
};

class Window
{
public:
    Window();
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] bool create(const WindowDesc& desc);
    void destroy();

    // Processes pending OS events; call once per frame.
    void pumpEvents();

    [[nodiscard]] bool shouldClose() const;
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;
    [[nodiscard]] bool isMinimized() const;

    // True if the client size changed since the last call; consumes the flag.
    [[nodiscard]] bool consumeResize();

    [[nodiscard]] bool isKeyDown(Key key) const;

    [[nodiscard]] NativeWindowHandle nativeHandle() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sol::platform
