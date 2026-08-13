/// @file
/// Opens a window, renders the reference scene, and reports what happened.
///
/// This is the first presenting path in the project. It exists to prove the loop end to end —
/// device, surface, swapchain, reversed-Z depth, present — and to be the harness the depth and
/// jitter gates are later measured through.
///
/// Runs headless-ish by default: a fixed frame count, then exit, so it can sit in CTest. Pass
/// `--interactive` to keep the window open until closed. A machine with no Vulkan loader, or
/// no display, reports a skip rather than a failure — both are real conditions and neither is
/// a defect in this code.

#include "Sol/Platform/Window.h"
#include "Sol/Render/Renderer.h"
#include "Sol/Render/VulkanInstance.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kSkipExitCode = 77;
constexpr std::uint64_t kAutomatedFrameCount = 240;

/// Earth's equatorial radius, metres, from the ADR 0008 reference ellipsoid.
constexpr double kEarthRadiusMetres = 6378136.6;

/// The reference scene: objects spanning nine orders of magnitude in distance.
///
/// This is the depth gate's subject. A conventional projection cannot hold a 1 m object at
/// arm's length and a planet-sized one 400 km away in the same depth buffer; reversed-Z with
/// an infinite far plane is the claim under test, and the scene exists to falsify it if the
/// claim is wrong. The objects are crude on purpose — B1 gates on depth behaviour, not content.
std::vector<sol::render::SceneObject> referenceScene()
{
    return {
        // Immediate foreground, at the near plane's scale.
        {.worldPosition = {0.0, 0.0, -3.0}, .radiusMetres = 0.5, .colour = {0.9F, 0.3F, 0.2F}},
        // Ten metres.
        {.worldPosition = {2.0, 0.5, -12.0}, .radiusMetres = 1.0, .colour = {0.3F, 0.8F, 0.4F}},
        // One hundred metres.
        {.worldPosition = {-8.0, 2.0, -110.0}, .radiusMetres = 6.0, .colour = {0.4F, 0.6F, 0.95F}},
        // One kilometre.
        {.worldPosition = {30.0, -10.0, -1200.0}, .radiusMetres = 40.0, .colour = {0.95F, 0.8F, 0.3F}},
        // One hundred kilometres.
        {.worldPosition = {0.0, 0.0, -120000.0}, .radiusMetres = 3000.0, .colour = {0.7F, 0.4F, 0.9F}},
        // A planet-scale body at orbital distance. If depth collapses anywhere, it is here.
        {.worldPosition = {0.0, -kEarthRadiusMetres - 400000.0, -200000.0},
         .radiusMetres = kEarthRadiusMetres,
         .colour = {0.2F, 0.35F, 0.55F}},
    };
}

} // namespace

int main(int argc, char** argv)
{
    bool interactive = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--interactive") == 0) {
            interactive = true;
        }
    }

    auto window = sol::platform::Window::create("Frontiers of Sol — reference view", 1280, 720);
    if (!window.has_value()) {
        std::printf("%s\n\nSKIPPED: no window system available.\n", window.error().c_str());
        return kSkipExitCode;
    }

    const sol::render::InstanceConfig config{
        .applicationName = "SolRenderLoop",
        .validation = sol::render::ValidationMode::Enabled,
        .requestedApiVersion = {1, 2, 0},
    };
    auto instance = sol::render::VulkanInstance::create(config);
    if (!instance.has_value()) {
        std::printf("%s\n\nSKIPPED: no Vulkan loader.\n", instance.error().c_str());
        return kSkipExitCode;
    }

    const auto size = window->framebufferSize();
    const sol::render::SurfaceTarget target{
        .nativeWindow = window->nativeHandle().window,
        .nativeInstance = window->nativeHandle().instance,
        .width = size.width,
        .height = size.height,
    };

    auto renderer = sol::render::Renderer::create(*instance, target);
    if (!renderer.has_value()) {
        std::printf("Renderer creation failed.\n\n%s\n", renderer.error().c_str());
        return 1;
    }

    std::printf("Rendering on: %s\n", renderer->selectedDevice().deviceName.c_str());
    std::printf("Validation:   %s\n", instance->validationEnabled() ? "active" : "not active");
    std::printf("Mode:         %s\n",
                interactive ? "interactive" : "automated, 240 frames");

    renderer->setScene(referenceScene());

    // Positioned at the launch-anchor scale, looking along -Z at the reference objects. The
    // camera's world position is deliberately large — 6378 km from the world origin — because
    // that is the magnitude camera-relative rendering exists to absorb. If the frame model
    // were wrong, this is where it would show.
    sol::render::CameraState camera{
        .position = {0.0, 5.0, 0.0},
        .forward = {0.0, 0.0, -1.0},
        .up = {0.0, 1.0, 0.0},
        .verticalFovRadians = 1.0472,
        .nearPlaneMetres = 0.1,
    };

    std::uint64_t presented = 0;
    std::uint64_t rebuilds = 0;

    while (!window->shouldClose()) {
        window->pollEvents();

        if (window->isKeyDown(sol::platform::Key::Escape)) {
            window->requestClose();
        }

        if (window->consumeResizeFlag()) {
            const auto current = window->framebufferSize();
            renderer->notifyResized(current.width, current.height);
        }

        auto frame = renderer->renderFrame(camera);
        if (!frame.has_value()) {
            std::printf("\nFrame failed.\n\n%s\n", frame.error().c_str());
            renderer->waitIdle();
            return 1;
        }
        if (frame->swapchainRebuilt) {
            ++rebuilds;
        }
        presented = frame->frameIndex;

        if (!interactive && presented >= kAutomatedFrameCount) {
            window->requestClose();
        }
    }

    renderer->waitIdle();

    std::printf("\nFrames presented: %llu (swapchain rebuilds: %llu)\n",
                static_cast<unsigned long long>(presented),
                static_cast<unsigned long long>(rebuilds));

    const auto& messages = instance->validationMessages();
    std::printf("Validation messages: %zu\n", messages.size());
    for (const std::string& message : messages) {
        std::printf("  %s\n", message.c_str());
    }

    if (presented == 0) {
        std::printf("FAILED: no frame was presented.\n");
        return 1;
    }
    return 0;
}
