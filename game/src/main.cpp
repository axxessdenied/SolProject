#include "fly_camera.hpp"
#include "scene_renderer.hpp"
#include "shader_watcher.hpp"
#include "sim_world.hpp"

#include "sol/core/jobs.hpp"
#include "sol/core/log.hpp"
#include "sol/core/version.hpp"
#include "sol/sim/fixed_loop.hpp"
#include "sol/platform/file_io.hpp"
#include "sol/platform/platform.hpp"
#include "sol/platform/time.hpp"
#include "sol/platform/window.hpp"
#include "sol/rhi/context.hpp"
#include "sol/rhi/swapchain.hpp"
#include "sol/ui/dev_ui.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

    const std::string executableDir = sol::platform::executableDirectory();
    const std::string shaderDirectory = executableDir + "shaders/";
    const std::string cookedDirectory = executableDir + "cooked/";
    const std::string savePath = executableDir + "world.sav";

    game::SceneRenderer renderer;
    if (!renderer.initialize(context, swapchain, shaderDirectory.c_str(), cookedDirectory.c_str())) {
        return EXIT_FAILURE;
    }

    sol::ui::DevUi devUi;
    if (!devUi.initialize(window, context, swapchain.imageFormat(), VK_FORMAT_D32_SFLOAT,
                          swapchain.imageCount())) {
        return EXIT_FAILURE;
    }
    renderer.setDevUi(&devUi);

#if !defined(SOL_SHADER_SOURCE_DIR)
    #define SOL_SHADER_SOURCE_DIR ""
#endif
#if !defined(SOL_GLSLC_PATH)
    #define SOL_GLSLC_PATH ""
#endif
    game::ShaderWatcher shaderWatcher(SOL_SHADER_SOURCE_DIR, SOL_GLSLC_PATH, shaderDirectory);

    game::FlyCamera camera;
    float smoothedFps = 0.0f;
    bool previousF5 = false;
    bool previousF9 = false;
    bool previousF10 = false;

    // Phase 3: 10k+ entities on a 60 Hz fixed timestep, rendered with
    // interpolation at uncapped framerate.
    sol::core::JobSystem jobs;
    sol::sim::FixedLoop simLoop(60.0);
    game::SimWorld world;
    world.spawn(10'000, 1337);
    std::vector<game::RenderInstance> renderInstances;
    SOL_LOG_INFO("Sim world: %u entities, %u job workers", world.entityCount(), jobs.workerCount());

    SOL_LOG_INFO("Entering frame loop (%ux%u). RMB+mouse look, WASD move, ESC quits.", window.width(),
                 window.height());

    double lastFrameTime = sol::platform::timeSeconds();
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

        const double now = sol::platform::timeSeconds();
        const float deltaSeconds = sol::core::clamp(static_cast<float>(now - lastFrameTime), 0.0f, 0.1f);
        lastFrameTime = now;

        camera.update(window, deltaSeconds);

        simLoop.beginFrame(deltaSeconds);
        while (simLoop.shouldTick()) {
            world.tick(jobs, simLoop.tickDelta());
        }
        const float simAlpha = simLoop.alpha();
        const double interpolatedSimTime =
            simLoop.simTimeSeconds() + static_cast<double>(simAlpha) * simLoop.tickDelta();
        world.buildRenderInstances(jobs, simAlpha, interpolatedSimTime, renderInstances);

        // World save/load round trip: F9 saves, F10 loads (edge-triggered).
        const bool f9Down = window.isKeyDown(sol::platform::Key::F9);
        if (f9Down && !previousF9) {
            SOL_LOG_INFO(world.saveTo(savePath.c_str()) ? "world saved to %s"
                                                        : "world save FAILED (%s)",
                         savePath.c_str());
        }
        previousF9 = f9Down;
        const bool f10Down = window.isKeyDown(sol::platform::Key::F10);
        if (f10Down && !previousF10) {
            SOL_LOG_INFO(world.loadFrom(savePath.c_str()) ? "world loaded from %s"
                                                          : "world load FAILED (%s)",
                         savePath.c_str());
        }
        previousF10 = f10Down;

        // Shader hot-reload: automatic on file change, F5 to force.
        const bool f5Down = window.isKeyDown(sol::platform::Key::F5);
        const bool forceReload = f5Down && !previousF5;
        previousF5 = f5Down;
        if (shaderWatcher.poll(now, forceReload)) {
            context.waitIdle();
            if (!renderer.reloadShaders()) {
                SOL_LOG_ERROR("pipeline reload failed; keeping previous shaders");
            }
        }

        if (deltaSeconds > 0.0f) {
            const float instantFps = 1.0f / deltaSeconds;
            smoothedFps = smoothedFps == 0.0f ? instantFps
                                              : sol::core::lerp(smoothedFps, instantFps, 0.05f);
        }
        sol::ui::OverlayStats stats;
        stats.fps = smoothedFps;
        stats.frameMilliseconds = deltaSeconds * 1000.0f;
        stats.cameraPosition = camera.position();
        stats.cameraSpeed = camera.speed();
        stats.drawCalls = renderer.drawCallCount();
        stats.simTicks = simLoop.tickCount();
        stats.simEntities = world.entityCount();
        stats.simAlpha = simAlpha;
        devUi.beginFrame(stats);

        bool needRecreate = window.consumeResize();
        if (needRecreate) {
            devUi.discardFrame();
        }
        if (!needRecreate) {
            switch (renderer.drawFrame(camera, renderInstances)) {
            case game::SceneRenderer::DrawResult::Success:
                ++frameCount;
                break;
            case game::SceneRenderer::DrawResult::NeedSwapchainRecreate:
                needRecreate = true;
                devUi.discardFrame();
                break;
            case game::SceneRenderer::DrawResult::Failure:
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
    devUi.shutdown();
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
