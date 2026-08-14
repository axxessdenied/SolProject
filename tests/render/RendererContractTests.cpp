/// @file
/// Two renderer contracts that no gate exercises, and that were both broken until reviewed.
///
/// The gates measure thresholds — jitter, depth, LOD continuity. Neither defect below is a
/// threshold: each is a behaviour the gates quietly depend on, which is exactly why both
/// survived a passing suite. They are tested here rather than inside a gate so that a gate's
/// verdict never rests on an assumption this file has not checked first.
///
/// Both tests are written to fail loudly on the *old* behaviour rather than merely to pass on
/// the new. A test that cannot distinguish the two would be the same mistake the LOD gate's
/// missing control already taught this project once.

#include "Sol/Platform/Window.h"
#include "Sol/Render/Renderer.h"
#include "Sol/Render/VulkanInstance.h"
#include "Support/TestCheck.h"

#include <cstdio>
#include <optional>
#include <vector>

namespace {

constexpr int kSkipExitCode = 77;

/// Earth's equatorial radius, metres, from the ADR 0008 reference ellipsoid.
constexpr double kPlanetRadiusMetres = 6378136.6;

/// Stress relief, matching the LOD gate. A gentler planet makes the coarse and fine grids
/// nearly identical, which would let the root-morph test pass without testing anything.
constexpr double kStressReliefMetres = 20000.0;

/// Captures a frame, retrying past the empty results a swapchain rebuild legitimately returns.
///
/// A rebuild invalidates the frame it happens on, so the capture path reports nothing rather
/// than half-sized pixels. That is correct and is not what this file is testing.
std::optional<sol::render::CapturedFrame> captureSettled(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    const sol::render::CameraState& camera)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        window.pollEvents();
        auto captured = renderer.renderFrameCaptured(camera);
        if (!captured.has_value()) {
            std::printf("  capture failed: %s\n", captured.error().c_str());
            return std::nullopt;
        }
        if (!captured->empty()) {
            return std::move(*captured);
        }
    }
    return std::nullopt;
}

/// Pixels whose RGB differs at all. Exact, not tolerant: the two frames being compared are
/// rendered from identical state, and this renderer's frames are already relied upon to be
/// bit-identical under identical input by the jitter gate.
std::size_t differingPixels(
    const sol::render::CapturedFrame& a,
    const sol::render::CapturedFrame& b)
{
    if (a.rgba.size() != b.rgba.size()) {
        return a.rgba.size() + b.rgba.size();
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        if (a.rgba[i] != b.rgba[i] || a.rgba[i + 1] != b.rgba[i + 1]
            || a.rgba[i + 2] != b.rgba[i + 2]) {
            ++differing;
        }
    }
    return differing;
}

/// A root patch has no parent, so morphing must do nothing at all there.
///
/// This is the invariant, and it is stronger than "the picture looks right": with `maxLevel = 0`
/// every emitted patch is a root, so enabling and disabling the morph must produce *the same
/// image*, byte for byte. There is no coarser level to blend toward.
///
/// It failed before 2026-08-13. The CPU signals "no parent" by emitting a zero-width morph band,
/// and the shader widened that degenerate band to a `1e-6` epsilon before dividing — which
/// drives the factor to 1 for any distance above a micrometre. Morph enabled therefore rendered
/// the *coarse* grid and morph disabled rendered the fine one, so the two images differed across
/// most of the planet. No gate could see it: they all run 3-300 km at `maxLevel 10`, where level
/// zero always subdivides and is never emitted.
void checkRootPatchMorphIsANoOp(
    sol::test::CheckContext& checks,
    sol::platform::Window& window,
    sol::render::Renderer& renderer)
{
    // Far enough out that the planet is comfortably inside the frame. Level zero is emitted
    // beyond `2R * subdivisionFactor` in normal use; `maxLevel = 0` forbids subdivision outright,
    // which reaches the same state from any distance and keeps the test quick.
    const sol::render::CameraState camera{
        .position = {0.0, kPlanetRadiusMetres * 3.0, 0.0},
        .forward = {0.0, -1.0, 0.0},
        .up = {0.0, 0.0, 1.0},
        .verticalFovRadians = 1.0472,
        .nearPlaneMetres = 1000.0,
    };

    sol::render::TerrainSettings terrain;
    terrain.centre = {0.0, 0.0, 0.0};
    terrain.radiusMetres = kPlanetRadiusMetres;
    terrain.reliefMetres = kStressReliefMetres;
    terrain.maxLevel = 0;

    renderer.setScene({});

    // The vacuity guard. Two identical *blank* frames would satisfy the invariant while proving
    // nothing, so the terrain is first shown to be on screen at all.
    renderer.setTerrain(std::nullopt);
    const auto blank = captureSettled(window, renderer, camera);

    terrain.morphEnabled = true;
    renderer.setTerrain(terrain);
    const auto morphOn = captureSettled(window, renderer, camera);

    terrain.morphEnabled = false;
    renderer.setTerrain(terrain);
    const auto morphOff = captureSettled(window, renderer, camera);

    if (!blank.has_value() || !morphOn.has_value() || !morphOff.has_value()) {
        checks.check(false, "root-morph test could not capture three comparable frames");
        return;
    }

    const std::size_t pixels = static_cast<std::size_t>(morphOn->width) * morphOn->height;
    const std::size_t terrainPixels = differingPixels(*blank, *morphOn);
    const std::size_t morphPixels = differingPixels(*morphOn, *morphOff);

    std::printf("  root patches: terrain covers %zu of %zu pixels; morphing moves %zu\n",
                terrainPixels,
                pixels,
                morphPixels);

    checks.check(terrainPixels > pixels / 20,
                 "terrain must actually be on screen, or the morph comparison is vacuous");
    checks.checkEqual(morphPixels,
                      std::size_t{0},
                      "a root patch has no parent, so enabling the morph must change no pixel");
}

/// A frame that drew nothing must say so, and must not hand back the last frame's pixels.
///
/// The readback buffers are persistently mapped and keep their contents, so a skipped frame that
/// is indistinguishable from a rendered one gives the caller a complete, well-formed, *stale*
/// image. A minimised window was exactly that until 2026-08-13: `submitFrame` returned early
/// without rebuilding anything, so the existing `swapchainRebuilt` flag stayed false and the
/// capture path waited on an already-signalled fence and returned the previous frame.
///
/// Silent, and it corrupts measurement rather than rendering: in the jitter gate a duplicate
/// contributes zero centroid deviation and strengthens the bit-identical reading; in the LOD gate
/// it contributes a zero frame difference and pulls down the median that the outlier floor and
/// every ratio are measured against.
///
/// `notifyResized(0, 0)` reaches the same zero-area path a minimised window does, without needing
/// a window manager to cooperate with a test.
void checkSkippedFrameYieldsNoPixels(
    sol::test::CheckContext& checks,
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    const sol::render::CameraState& camera)
{
    renderer.setTerrain(std::nullopt);

    const auto before = captureSettled(window, renderer, camera);
    checks.check(before.has_value() && !before->empty(),
                 "a normal capture returns pixels, or the rest of this test means nothing");

    renderer.notifyResized(0, 0);

    window.pollEvents();
    const auto skippedStats = renderer.renderFrame(camera);
    checks.check(skippedStats.has_value(),
                 "a zero-area frame is a normal state, not an error");
    checks.check(skippedStats.has_value() && !skippedStats->presented,
                 "a zero-area frame must report presented == false");

    const auto skipped = renderer.renderFrameCaptured(camera);
    checks.check(skipped.has_value(), "capturing a zero-area frame is not an error");
    checks.check(skipped.has_value() && skipped->empty(),
                 "a zero-area capture must return no pixels, not the previous frame's");

    if (skipped.has_value() && !skipped->empty() && before.has_value()) {
        // Naming the failure mode rather than only the failure: a non-empty result here is
        // almost certainly the previous frame handed back unchanged.
        checks.check(differingPixels(*before, *skipped) != 0,
                     "the non-empty zero-area capture is byte-identical to the previous frame, "
                     "which is the stale-readback defect exactly");
    }

    const auto restored = window.framebufferSize();
    renderer.notifyResized(restored.width, restored.height);
    const auto after = captureSettled(window, renderer, camera);
    checks.check(after.has_value() && !after->empty(),
                 "capture recovers once the framebuffer has area again");
}

/// Terrain outside the view frustum is not generated.
///
/// Turning the camera away from the planet must collapse the patch count. Before the frustum
/// cull only the horizon test applied, which removes the far side of the planet and nothing
/// else — so every near-side patch was still selected, subdivided to full depth, uploaded and
/// drawn while sitting behind the camera.
///
/// The image was already blank in that case, which is exactly why this went unnoticed: the
/// defect was pure cost, invisible in every pixel measurement. The count is therefore what is
/// asserted, and the blank image is asserted alongside it so that a cull which started removing
/// *visible* geometry would show up here rather than as a mystery elsewhere.
void checkTerrainOutsideTheFrustumIsNotBuilt(
    sol::test::CheckContext& checks,
    sol::platform::Window& window,
    sol::render::Renderer& renderer)
{
    sol::render::TerrainSettings terrain;
    terrain.centre = {0.0, 0.0, 0.0};
    terrain.radiusMetres = kPlanetRadiusMetres;
    terrain.reliefMetres = kStressReliefMetres;
    terrain.maxLevel = 6;

    renderer.setScene({});
    renderer.setTerrain(terrain);

    const sol::render::WorldVec3 position{0.0, kPlanetRadiusMetres * 1.5, 0.0};
    sol::render::CameraState toward{
        .position = position,
        .forward = {0.0, -1.0, 0.0},
        .up = {0.0, 0.0, 1.0},
        .verticalFovRadians = 1.0472,
        .nearPlaneMetres = 100.0,
    };
    // Straight down would be parallel to that up axis, so tilt slightly — the same shape of
    // camera the gates use, and a live check that the view-basis guard tolerates it.
    toward.forward = {0.30, -0.95, 0.0};

    sol::render::CameraState away = toward;
    away.forward = {-0.30, 0.95, 0.0};

    window.pollEvents();
    const auto towardFrame = renderer.renderFrame(toward);
    window.pollEvents();
    const auto awayFrame = renderer.renderFrame(away);

    if (!towardFrame.has_value() || !awayFrame.has_value()) {
        checks.check(false, "frustum-cull test could not render both directions");
        return;
    }

    std::printf("  frustum cull: %u patches looking at the planet, %u looking away\n",
                towardFrame->terrainPatches,
                awayFrame->terrainPatches);

    checks.check(towardFrame->terrainPatches > 0,
                 "looking at the planet must select terrain, or the comparison is vacuous");
    checks.check(awayFrame->terrainPatches * 5 < towardFrame->terrainPatches,
                 "looking away from the planet must collapse the selected patch count");

    // And the frame that draws nothing must genuinely draw nothing, so a cull that began
    // removing visible geometry would not hide inside a passing count.
    const auto awayCapture = captureSettled(window, renderer, away);
    renderer.setTerrain(std::nullopt);
    const auto blank = captureSettled(window, renderer, away);
    if (awayCapture.has_value() && blank.has_value()) {
        checks.checkEqual(differingPixels(*awayCapture, *blank),
                          std::size_t{0},
                          "a planet entirely behind the camera contributes no pixel");
    } else {
        checks.check(false, "frustum-cull test could not capture the away view");
    }
}

/// A camera whose forward and up are parallel is refused, not guessed at.
///
/// The side axis is the cross product of the two, so when they are parallel it is zero and the
/// camera's roll about its own view direction is undefined — there is no correct answer to pick.
/// Before 2026-08-13 `normalise` returned a zero vector by design, the basis collapsed, and the
/// frame rendered as a bare clear with no error returned.
///
/// The failure mode this guards is not a black screen, which someone would notice. It is the
/// tempting fix: substituting a fallback up axis renders a plausible image at an arbitrary roll,
/// and a centroid measured in a rotated frame is still a number.
void checkDegenerateViewBasisIsRefused(
    sol::test::CheckContext& checks,
    sol::platform::Window& window,
    sol::render::Renderer& renderer)
{
    window.pollEvents();

    // Forward parallel to up, which is what a straight-down camera with a default up axis is.
    const sol::render::CameraState parallel{
        .position = {0.0, 5.0, 0.0},
        .forward = {0.0, -1.0, 0.0},
        .up = {0.0, 1.0, 0.0},
        .verticalFovRadians = 1.0472,
        .nearPlaneMetres = 0.1,
    };
    const auto parallelFrame = renderer.renderFrame(parallel);
    checks.check(!parallelFrame.has_value(),
                 "a camera with forward parallel to up is refused rather than rendered");

    const sol::render::CameraState zeroUp{
        .position = {0.0, 5.0, 0.0},
        .forward = {0.0, 0.0, -1.0},
        .up = {0.0, 0.0, 0.0},
        .verticalFovRadians = 1.0472,
        .nearPlaneMetres = 0.1,
    };
    checks.check(!renderer.renderFrame(zeroUp).has_value(),
                 "a camera with a zero up vector is refused rather than rendered");

    // And the guard must not be so eager that it rejects ordinary cameras: the harnesses look
    // steeply down at (0.30, -0.95, 0) against an up of (0, 0, 1), which is nowhere near
    // parallel but is steep enough to be worth pinning.
    const sol::render::CameraState steep{
        .position = {0.0, 5.0, 0.0},
        .forward = {0.30, -0.95, 0.0},
        .up = {0.0, 0.0, 1.0},
        .verticalFovRadians = 1.0472,
        .nearPlaneMetres = 0.1,
    };
    checks.check(renderer.renderFrame(steep).has_value(),
                 "a steeply-down camera with a valid up axis still renders");
}

} // namespace

int main()
{
    auto window = sol::platform::Window::create("Renderer contract", 640, 480);
    if (!window.has_value()) {
        std::printf("%s\n\nSKIPPED: no window system.\n", window.error().c_str());
        return kSkipExitCode;
    }

    const sol::render::InstanceConfig config{
        .applicationName = "SolRendererContract",
        .validation = sol::render::ValidationMode::Enabled,
        .requestedApiVersion = {1, 2, 0},
    };
    auto instance = sol::render::VulkanInstance::create(config);
    if (!instance.has_value()) {
        std::printf("%s\n\nSKIPPED: no Vulkan loader.\n", instance.error().c_str());
        return kSkipExitCode;
    }

    const auto size = window->framebufferSize();
    auto renderer = sol::render::Renderer::create(
        *instance,
        sol::render::SurfaceTarget{
            .nativeWindow = window->nativeHandle().window,
            .nativeInstance = window->nativeHandle().instance,
            .width = size.width,
            .height = size.height,
        });
    if (!renderer.has_value()) {
        std::printf("Renderer creation failed.\n\n%s\n", renderer.error().c_str());
        return 1;
    }

    std::printf("Renderer contract tests\n");
    std::printf("Device: %s\n\n", renderer->selectedDevice().deviceName.c_str());

    const sol::render::CameraState nearCamera{
        .position = {0.0, 5.0, 0.0},
        .forward = {0.0, 0.0, -1.0},
        .up = {0.0, 1.0, 0.0},
        .verticalFovRadians = 1.0472,
        .nearPlaneMetres = 0.1,
    };

    // Warm up so the swapchain exists before anything is compared; the first frame rebuilds it
    // and legitimately captures nothing.
    for (int i = 0; i < 3; ++i) {
        window->pollEvents();
        if (const auto frame = renderer->renderFrame(nearCamera); !frame.has_value()) {
            std::printf("Warm-up frame failed.\n\n%s\n", frame.error().c_str());
            return 1;
        }
    }

    sol::test::CheckContext checks("render.renderer-contract");
    checkRootPatchMorphIsANoOp(checks, *window, *renderer);
    checkSkippedFrameYieldsNoPixels(checks, *window, *renderer, nearCamera);
    checkDegenerateViewBasisIsRefused(checks, *window, *renderer);
    checkTerrainOutsideTheFrustumIsNotBuilt(checks, *window, *renderer);

    renderer->waitIdle();

    const auto& messages = instance->validationMessages();
    std::printf("\nValidation messages: %zu\n", messages.size());
    for (const std::string& message : messages) {
        std::printf("  %s\n", message.c_str());
    }

    return checks.finish();
}
