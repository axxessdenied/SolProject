#include "triangle_renderer.hpp"

#include "sol/core/log.hpp"
#include "sol/core/version.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/platform/platform.hpp"
#include "sol/platform/time.hpp"
#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/swapchain.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

#if defined(NDEBUG)
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

// --frames N: render N frames, then exit (for automated runs).
std::uint64_t parseMaxFrames(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            return std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::uint64_t maxFrames = parseMaxFrames(argc, argv);

    SOL_LOG_INFO("Sol Engine %s on %s", sol::core::engineVersionString(), sol::platform::platformName());

    sol::platform::Window window;
    sol::platform::WindowDesc windowDesc = {};
    windowDesc.title = "Sol";
    if (!window.create(windowDesc)) {
        return EXIT_FAILURE;
    }

    sol::rhi::Context context;
    sol::rhi::ContextDesc contextDesc = {};
    contextDesc.appName = "Sol";
    contextDesc.enableValidation = kEnableValidation;
    if (!context.initialize(contextDesc, window.nativeHandle())) {
        return EXIT_FAILURE;
    }

    sol::rhi::Swapchain swapchain;
    if (!swapchain.create(context, window.width(), window.height())) {
        return EXIT_FAILURE;
    }

    game::TriangleRenderer renderer;
    const std::string shaderDirectory = sol::platform::executableDirectory() + "shaders/";
    if (!renderer.initialize(context, swapchain, shaderDirectory.c_str())) {
        return EXIT_FAILURE;
    }

    SOL_LOG_INFO("Entering frame loop (%ux%u, ESC to quit)", window.width(), window.height());

    std::uint64_t frameCount = 0;
    bool failed = false;
    while (true) {
        window.pumpEvents();
        if (window.shouldClose() || window.isKeyDown(sol::platform::Key::Escape)) {
            break;
        }
        if (window.isMinimized()) {
            sol::platform::sleepMilliseconds(10);
            continue;
        }

        bool needRecreate = window.consumeResize();
        if (!needRecreate) {
            switch (renderer.drawFrame()) {
            case game::TriangleRenderer::DrawResult::Success:
                ++frameCount;
                break;
            case game::TriangleRenderer::DrawResult::NeedSwapchainRecreate:
                needRecreate = true;
                break;
            case game::TriangleRenderer::DrawResult::Failure:
                failed = true;
                break;
            }
        }
        if (failed) {
            break;
        }

        if (needRecreate) {
            context.waitIdle();
            if (swapchain.recreate(window.width(), window.height())) {
                if (!renderer.onSwapchainRecreated()) {
                    failed = true;
                    break;
                }
            }
            // A failed recreate (zero extent) is retried once the window is restored.
        }

        if (maxFrames > 0 && frameCount >= maxFrames) {
            break;
        }
    }

    context.waitIdle();
    renderer.shutdown();
    swapchain.destroy();
    context.shutdown();
    window.destroy();

    const std::uint64_t validationMessages = sol::rhi::Context::validationMessageCount();
    if (failed || validationMessages > 0) {
        SOL_LOG_ERROR("Exited with failure=%d, %llu validation message(s)",
                      failed ? 1 : 0,
                      static_cast<unsigned long long>(validationMessages));
        return EXIT_FAILURE;
    }

    SOL_LOG_INFO("Clean shutdown after %llu frame(s), 0 validation messages",
                 static_cast<unsigned long long>(frameCount));
    return EXIT_SUCCESS;
}
