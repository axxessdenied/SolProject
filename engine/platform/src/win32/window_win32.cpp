#include "sol/platform/window.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/log.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace sol::platform {

namespace {

constexpr wchar_t kWindowClassName[] = L"SolWindowClass";

Key translateVirtualKey(WPARAM virtualKey)
{
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(virtualKey - 'A'));
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return static_cast<Key>(static_cast<int>(Key::Num0) + static_cast<int>(virtualKey - '0'));
    }

    switch (virtualKey) {
    case VK_ESCAPE: return Key::Escape;
    case VK_SPACE: return Key::Space;
    case VK_RETURN: return Key::Enter;
    case VK_TAB: return Key::Tab;
    case VK_SHIFT: return Key::LeftShift;
    case VK_CONTROL: return Key::LeftControl;
    case VK_MENU: return Key::LeftAlt;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    default: return Key::Unknown;
    }
}

} // namespace

struct Window::Impl
{
    HWND hwnd = nullptr;
    HINSTANCE hinstance = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool closeRequested = false;
    bool minimized = false;
    bool resized = false;
    bool keyDown[static_cast<int>(Key::Count)] = {};

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

LRESULT CALLBACK Window::Impl::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    if (impl == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_CLOSE:
        impl->closeRequested = true;
        return 0;

    case WM_SIZE: {
        impl->minimized = (wParam == SIZE_MINIMIZED);
        const std::uint32_t newWidth = LOWORD(lParam);
        const std::uint32_t newHeight = HIWORD(lParam);
        if (!impl->minimized && (newWidth != impl->width || newHeight != impl->height)) {
            impl->width = newWidth;
            impl->height = newHeight;
            impl->resized = true;
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const Key key = translateVirtualKey(wParam);
        if (key != Key::Unknown) {
            impl->keyDown[static_cast<int>(key)] = true;
        }
        break; // fall through to DefWindowProc so Alt+F4 etc. keep working
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const Key key = translateVirtualKey(wParam);
        if (key != Key::Unknown) {
            impl->keyDown[static_cast<int>(key)] = false;
        }
        break;
    }

    case WM_KILLFOCUS:
        // Avoid stuck keys when focus is lost mid-press.
        for (bool& down : impl->keyDown) {
            down = false;
        }
        break;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

Window::Window() : m_impl(std::make_unique<Impl>()) {}

Window::~Window()
{
    destroy();
}

bool Window::create(const WindowDesc& desc)
{
    SOL_ASSERT(m_impl->hwnd == nullptr);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    m_impl->hinstance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Impl::windowProc;
    windowClass.hInstance = m_impl->hinstance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        SOL_LOG_ERROR("RegisterClassExW failed (error %lu)", GetLastError());
        return false;
    }

    wchar_t wideTitle[256] = {};
    MultiByteToWideChar(CP_UTF8, 0, desc.title, -1, wideTitle, 255);

    // desc gives the client area size; adjust to the outer window size.
    RECT rect = {0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);

    m_impl->hwnd = CreateWindowExW(0,
                                   kWindowClassName,
                                   wideTitle,
                                   style,
                                   CW_USEDEFAULT,
                                   CW_USEDEFAULT,
                                   rect.right - rect.left,
                                   rect.bottom - rect.top,
                                   nullptr,
                                   nullptr,
                                   m_impl->hinstance,
                                   m_impl.get());
    if (m_impl->hwnd == nullptr) {
        SOL_LOG_ERROR("CreateWindowExW failed (error %lu)", GetLastError());
        return false;
    }

    RECT clientRect = {};
    GetClientRect(m_impl->hwnd, &clientRect);
    m_impl->width = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
    m_impl->height = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);

    ShowWindow(m_impl->hwnd, SW_SHOW);
    return true;
}

void Window::destroy()
{
    if (m_impl && m_impl->hwnd != nullptr) {
        DestroyWindow(m_impl->hwnd);
        m_impl->hwnd = nullptr;
    }
}

void Window::pumpEvents()
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        if (message.message == WM_QUIT) {
            m_impl->closeRequested = true;
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool Window::shouldClose() const
{
    return m_impl->closeRequested;
}

std::uint32_t Window::width() const
{
    return m_impl->width;
}

std::uint32_t Window::height() const
{
    return m_impl->height;
}

bool Window::isMinimized() const
{
    return m_impl->minimized;
}

bool Window::consumeResize()
{
    const bool resized = m_impl->resized;
    m_impl->resized = false;
    return resized;
}

bool Window::isKeyDown(Key key) const
{
    SOL_ASSERT(key < Key::Count);
    return m_impl->keyDown[static_cast<int>(key)];
}

NativeWindowHandle Window::nativeHandle() const
{
    return NativeWindowHandle{m_impl->hwnd, m_impl->hinstance};
}

} // namespace sol::platform
