#include "Sol/Platform/Window.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <array>
#include <format>

namespace sol::platform {
namespace {

/// GLFW is a process-global library with a reference-counted init here, so that a second
/// Window does not terminate the first one's event system on destruction.
int g_initCount = 0;
std::string g_lastError;

void onGlfwError(int code, const char* description)
{
    g_lastError = std::format("GLFW error {}: {}", code, description != nullptr ? description : "");
}

int toGlfwKey(Key key)
{
    switch (key) {
    case Key::W: return GLFW_KEY_W;
    case Key::A: return GLFW_KEY_A;
    case Key::S: return GLFW_KEY_S;
    case Key::D: return GLFW_KEY_D;
    case Key::Q: return GLFW_KEY_Q;
    case Key::E: return GLFW_KEY_E;
    case Key::Shift: return GLFW_KEY_LEFT_SHIFT;
    case Key::Control: return GLFW_KEY_LEFT_CONTROL;
    case Key::Space: return GLFW_KEY_SPACE;
    case Key::Escape: return GLFW_KEY_ESCAPE;
    }
    return GLFW_KEY_UNKNOWN;
}

} // namespace

struct Window::Impl {
    GLFWwindow* handle = nullptr;
    bool resized = false;

    ~Impl()
    {
        if (handle != nullptr) {
            glfwDestroyWindow(handle);
        }
        if (--g_initCount == 0) {
            glfwTerminate();
        }
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    static void onFramebufferSize(GLFWwindow* window, int /*width*/, int /*height*/)
    {
        auto* impl = static_cast<Impl*>(glfwGetWindowUserPointer(window));
        if (impl != nullptr) {
            impl->resized = true;
        }
    }
};

std::expected<Window, std::string> Window::create(
    const std::string& title,
    std::uint32_t width,
    std::uint32_t height)
{
    glfwSetErrorCallback(&onGlfwError);
    g_lastError.clear();

    if (g_initCount == 0 && glfwInit() != GLFW_TRUE) {
        return std::unexpected(std::format(
            "The window system could not be initialised.\n"
            "  This usually means no display is available — a headless session, a remote "
            "connection without a desktop, or a service account.\n"
            "  Detail: {}",
            g_lastError.empty() ? "no further detail reported" : g_lastError));
    }
    ++g_initCount;

    auto impl = std::make_unique<Impl>();

    // No OpenGL or OpenGL ES context: the renderer owns the graphics API, and asking GLFW for
    // a context would both waste a driver resource and let a GL call compile by accident.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    impl->handle = glfwCreateWindow(
        static_cast<int>(width), static_cast<int>(height), title.c_str(), nullptr, nullptr);

    if (impl->handle == nullptr) {
        return std::unexpected(std::format(
            "The window could not be created at {}x{}.\n  Detail: {}",
            width,
            height,
            g_lastError.empty() ? "no further detail reported" : g_lastError));
    }

    glfwSetWindowUserPointer(impl->handle, impl.get());
    glfwSetFramebufferSizeCallback(impl->handle, &Impl::onFramebufferSize);

    return Window{std::move(impl)};
}

Window::Window(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

Window::~Window() = default;
Window::Window(Window&&) noexcept = default;
Window& Window::operator=(Window&&) noexcept = default;

NativeWindowHandle Window::nativeHandle() const
{
    return NativeWindowHandle{
        .window = glfwGetWin32Window(m_impl->handle),
        .instance = GetModuleHandleW(nullptr),
    };
}

FramebufferSize Window::framebufferSize() const
{
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_impl->handle, &width, &height);
    return FramebufferSize{
        .width = static_cast<std::uint32_t>(width < 0 ? 0 : width),
        .height = static_cast<std::uint32_t>(height < 0 ? 0 : height),
    };
}

bool Window::shouldClose() const
{
    return glfwWindowShouldClose(m_impl->handle) == GLFW_TRUE;
}

bool Window::consumeResizeFlag()
{
    const bool resized = m_impl->resized;
    m_impl->resized = false;
    return resized;
}

bool Window::isKeyDown(Key key) const
{
    return glfwGetKey(m_impl->handle, toGlfwKey(key)) == GLFW_PRESS;
}

void Window::pollEvents()
{
    glfwPollEvents();
}

void Window::requestClose()
{
    glfwSetWindowShouldClose(m_impl->handle, GLFW_TRUE);
}

} // namespace sol::platform
