#include "sol/core/assert.hpp"
#include "sol/core/log.hpp"
#include "sol/platform/window.hpp"

#include <string>

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
    if (virtualKey >= VK_F1 && virtualKey <= VK_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(virtualKey - VK_F1));
    }

    switch (virtualKey) {
    case VK_ESCAPE:
        return Key::Escape;
    case VK_SPACE:
        return Key::Space;
    case VK_RETURN:
        return Key::Enter;
    case VK_TAB:
        return Key::Tab;
    case VK_SHIFT:
        return Key::LeftShift;
    case VK_CONTROL:
        return Key::LeftControl;
    case VK_MENU:
        return Key::LeftAlt;
    case VK_UP:
        return Key::Up;
    case VK_DOWN:
        return Key::Down;
    case VK_LEFT:
        return Key::Left;
    case VK_RIGHT:
        return Key::Right;
    case VK_BACK:
        return Key::Backspace;
    case VK_DELETE:
        return Key::Delete;
    case VK_HOME:
        return Key::Home;
    case VK_END:
        return Key::End;
    default:
        return Key::Unknown;
    }
}

// Appends one Unicode code point as UTF-8. WM_CHAR delivers UTF-16 code
// units, so anything outside the BMP arrives as two messages and is
// reassembled by the caller before it gets here.
void appendUtf8(std::string& out, std::uint32_t codePoint)
{
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
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
    bool mouseDown[static_cast<int>(MouseButton::Count)] = {};
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    float wheel = 0.0f;
    std::string textInput;
    // High surrogate held between the two WM_CHARs of an astral code point.
    std::uint16_t pendingSurrogate = 0;
    bool cursorLocked = false;
    bool cursorHidden = false;
    MessageHook messageHook = nullptr;

    void clearInput()
    {
        for (bool& down : keyDown) {
            down = false;
        }
        for (bool& down : mouseDown) {
            down = false;
        }
    }

    void applyCursorClip() const
    {
        if (cursorLocked && hwnd != nullptr) {
            RECT clientRect = {};
            GetClientRect(hwnd, &clientRect);
            POINT topLeft = {clientRect.left, clientRect.top};
            POINT bottomRight = {clientRect.right, clientRect.bottom};
            ClientToScreen(hwnd, &topLeft);
            ClientToScreen(hwnd, &bottomRight);
            const RECT screenRect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
            ClipCursor(&screenRect);
        } else {
            ClipCursor(nullptr);
        }
    }

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

    // Key state is recorded BEFORE the dev-UI hook gets a say, because it is a
    // physical fact: whether a key is down does not depend on who wants the
    // keystroke. The hook consumes keyboard messages while ImGui wants them,
    // and it can take a key UP without having taken the matching DOWN - ImGui
    // owns Tab for its own focus navigation - which used to leave that key
    // latched down forever. A stuck Tab reads as "navigate next" every frame,
    // which is how it was found: it dragged focus out of a text field.
    // Whether the GAME acts on a key is gated separately, by wantsMouseCapture.
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const Key key = translateVirtualKey(wParam);
        if (key != Key::Unknown) {
            impl->keyDown[static_cast<int>(key)] = true;
        }
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const Key key = translateVirtualKey(wParam);
        if (key != Key::Unknown) {
            impl->keyDown[static_cast<int>(key)] = false;
        }
        break;
    }
    default:
        break;
    }

    if (impl->messageHook != nullptr &&
        impl->messageHook(
            hwnd, message, static_cast<std::uint64_t>(wParam), static_cast<std::int64_t>(lParam))) {
        return 0;
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
        impl->applyCursorClip();
        return 0;
    }

    case WM_INPUT: {
        RAWINPUT raw = {};
        UINT size = sizeof(raw);
        if (GetRawInputData(
                reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                static_cast<UINT>(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
            impl->mouseDeltaX += static_cast<float>(raw.data.mouse.lLastX);
            impl->mouseDeltaY += static_cast<float>(raw.data.mouse.lLastY);
        }
        break; // let DefWindowProc do cleanup
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        const MouseButton button = (message == WM_LBUTTONDOWN)   ? MouseButton::Left
                                   : (message == WM_RBUTTONDOWN) ? MouseButton::Right
                                                                 : MouseButton::Middle;
        impl->mouseDown[static_cast<int>(button)] = true;
        SetCapture(hwnd);
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        const MouseButton button = (message == WM_LBUTTONUP)   ? MouseButton::Left
                                   : (message == WM_RBUTTONUP) ? MouseButton::Right
                                                               : MouseButton::Middle;
        impl->mouseDown[static_cast<int>(button)] = false;
        ReleaseCapture();
        return 0;
    }

    case WM_MOUSEWHEEL:
        impl->wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        return 0;

    // State was already recorded above; these cases exist only to decide who
    // else gets the message. WM_KEYDOWN is CONSUMED, which the arrival of text
    // input made load-bearing: pumpEvents already calls TranslateMessage, and
    // letting DefWindowProc see the same keystroke gets it translated a second
    // time, so every typed character arrived TWICE. WM_SYSKEYDOWN still falls
    // through to DefWindowProc, so Alt+F4 and the system menu keep working.
    case WM_KEYDOWN:
        return 0;

    case WM_SYSKEYDOWN:
        break;

    case WM_CHAR: {
        // Only reached when the dev UI's message hook did not want the
        // keystroke - the hook runs above and returns early - so a focused
        // ImGui console and a focused game text field never both see it.
        const auto unit = static_cast<std::uint16_t>(wParam);
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            impl->pendingSurrogate = unit; // high half; the low half follows
            return 0;
        }
        std::uint32_t codePoint = unit;
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (impl->pendingSurrogate == 0) {
                return 0; // orphaned low half
            }
            codePoint = 0x10000u + ((impl->pendingSurrogate - 0xD800u) << 10) + (unit - 0xDC00u);
        }
        impl->pendingSurrogate = 0;
        // Control codes arrive here too (Backspace is 0x08, Enter 0x0D);
        // those are keys, handled as keys, and are not text.
        if (codePoint >= 0x20 && codePoint != 0x7F) {
            appendUtf8(impl->textInput, codePoint);
        }
        return 0;
    }

    case WM_KEYUP:
        return 0;

    case WM_SYSKEYUP:
        break;

    case WM_KILLFOCUS:
        // Avoid stuck input when focus is lost mid-press.
        impl->clearInput();
        ClipCursor(nullptr);
        break;

    case WM_SETFOCUS:
        impl->applyCursorClip();
        break;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

Window::Window() : m_impl(std::make_unique<Impl>())
{
}

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

    // Raw mouse input for game-quality look deltas.
    RAWINPUTDEVICE rawMouse = {};
    rawMouse.usUsagePage = 0x01; // generic desktop
    rawMouse.usUsage = 0x02;     // mouse
    rawMouse.hwndTarget = m_impl->hwnd;
    if (RegisterRawInputDevices(&rawMouse, 1, sizeof(rawMouse)) == FALSE) {
        SOL_LOG_WARN("RegisterRawInputDevices failed (error %lu); mouse look unavailable", GetLastError());
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
        setCursorLocked(false);
        DestroyWindow(m_impl->hwnd);
        m_impl->hwnd = nullptr;
    }
}

void Window::pumpEvents()
{
    m_impl->mouseDeltaX = 0.0f;
    m_impl->mouseDeltaY = 0.0f;
    m_impl->wheel = 0.0f;
    m_impl->textInput.clear();

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

bool Window::isMouseButtonDown(MouseButton button) const
{
    SOL_ASSERT(button < MouseButton::Count);
    return m_impl->mouseDown[static_cast<int>(button)];
}

core::Vec2 Window::mouseDelta() const
{
    return {m_impl->mouseDeltaX, m_impl->mouseDeltaY};
}

core::Vec2 Window::mousePosition() const
{
    // Queried rather than tracked through WM_MOUSEMOVE: raw input drives the
    // deltas, and those messages do not carry a client position.
    POINT point = {};
    if (GetCursorPos(&point) == 0 || ScreenToClient(m_impl->hwnd, &point) == 0) {
        return {};
    }
    return {static_cast<float>(point.x), static_cast<float>(point.y)};
}

float Window::wheelDelta() const
{
    return m_impl->wheel;
}

std::string_view Window::textInput() const
{
    return m_impl->textInput;
}

void Window::setCursorLocked(bool locked)
{
    if (m_impl->cursorLocked == locked) {
        return;
    }
    m_impl->cursorLocked = locked;
    m_impl->applyCursorClip();

    // ShowCursor is counted; toggle exactly once per state change.
    if (locked && !m_impl->cursorHidden) {
        ShowCursor(FALSE);
        m_impl->cursorHidden = true;
    } else if (!locked && m_impl->cursorHidden) {
        ShowCursor(TRUE);
        m_impl->cursorHidden = false;
    }
}

bool Window::isCursorLocked() const
{
    return m_impl->cursorLocked;
}

void Window::setMessageHook(MessageHook hook)
{
    m_impl->messageHook = hook;
}

NativeWindowHandle Window::nativeHandle() const
{
    return NativeWindowHandle{m_impl->hwnd, m_impl->hinstance};
}

} // namespace sol::platform
