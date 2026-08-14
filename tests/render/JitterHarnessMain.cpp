/// @file
/// The screen-space jitter gate: 0.25 pixels, measured by the method the P1b plan fixes.
///
/// The original P1 plan asserted a 0.25-pixel gate without saying how to measure it, which
/// made it unmeasurable as written. The accepted method, from
/// `SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md`, is implemented here verbatim:
///
///   1. Place a small high-contrast reference marker at a fixed position in the authoritative
///      world frame.
///   2. Hold the camera stationary in the same frame. Disable temporal antialiasing, jittered
///      sampling, motion blur, and any other deliberate sub-pixel perturbation.
///   3. Render at least 600 consecutive frames.
///   4. Compute the marker's screen-space centroid per frame to sub-pixel accuracy.
///   5. Jitter is the maximum absolute deviation of the per-frame centroid from the mean
///      centroid, reported per axis in pixels. Report both the maximum and the p99.
///
/// Run at two reference views: the surface anchor, and a 200 km orbital vantage. Both must
/// satisfy the gate.
///
/// **What this actually tests, and what it does not.** Nothing moves between frames — camera
/// and scene are both constants — so an ideal renderer produces byte-identical frames and
/// exactly zero jitter. The gate therefore measures **temporal stability only**.
///
/// It is worth being blunt that the gate is *magnitude-inert*. The marker sits at
/// `camera + offset`, so every component the offset does not touch is bit-identical in both
/// operands and subtracts to exactly zero — in `float` as much as in `double`. Placing the
/// camera at Earth-radius magnitude does not by itself make the frame model observable here;
/// an earlier version of this comment claimed it did, and was wrong.
///
/// The **sub-pixel response control** below is what carries the precision claim, and it only
/// does so because it steps along the axis that carries the large magnitude. Read the two
/// results together: the gate says the renderer is stable, the control says it is stable for
/// the right reason.

#include "Sol/Platform/Window.h"
#include "Sol/Render/Renderer.h"
#include "Sol/Render/ShaderBuild.h"
#include "Sol/Render/VulkanInstance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int kSkipExitCode = 77;

/// The plan requires at least 600. Warm-up frames are discarded on top of this.
constexpr int kMeasuredFrames = 600;
constexpr int kWarmupFrames = 30;

/// The accepted gate, in pixels, per axis.
constexpr double kJitterGatePixels = 0.25;

/// Earth's equatorial radius from the ADR 0008 reference ellipsoid, metres.
constexpr double kEarthRadiusMetres = 6378136.6;

/// Marker signal threshold, 0-255, applied to red minus the brighter of green and blue.
///
/// Deliberately low. The marker has a smooth radial intensity profile, and it is that profile's
/// faint outer skirt that carries the sub-pixel information. A high threshold would clip the
/// skirt and reintroduce a hard, whole-pixel boundary — exactly the quantisation the smooth
/// marker exists to remove. Low enough to keep the skirt, high enough to reject the background,
/// which is near-black and has no red excess at all.
constexpr int kMarkerThreshold = 6;

struct Centroid {
    double x = 0.0;
    double y = 0.0;
};

/// Luminance-weighted centroid of the marker, to sub-pixel accuracy.
///
/// Weighting by intensity rather than counting thresholded pixels is what makes this
/// sub-pixel: an edge pixel half-covered by the marker contributes half as much, so the
/// centroid moves continuously as the marker's projected position moves by less than a pixel.
/// A pure count would quantise to whole pixels and could not resolve a 0.25-pixel gate at all.
std::optional<Centroid> markerCentroid(const sol::render::CapturedFrame& frame)
{
    double weightedX = 0.0;
    double weightedY = 0.0;
    double total = 0.0;

    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * frame.width + x) * 4;
            const int red = frame.rgba[index];
            const int green = frame.rgba[index + 1];
            const int blue = frame.rgba[index + 2];

            // The marker is the only red thing in the scene; keying on red minus the other
            // channels rejects the background without needing it to be pure black.
            const int signal = red - std::max(green, blue);
            if (signal < kMarkerThreshold) {
                continue;
            }

            // Background-subtracted weight. A pixel exactly at the threshold contributes
            // nothing, so pixels crossing the threshold as the marker moves sub-pixel enter
            // and leave with zero weight instead of jumping in at full threshold value. This
            // is what makes the boundary of the measured region continuous; without it the
            // threshold itself becomes the hard edge the smooth profile was meant to avoid.
            const auto weight = static_cast<double>(signal - kMarkerThreshold);
            weightedX += weight * (static_cast<double>(x) + 0.5);
            weightedY += weight * (static_cast<double>(y) + 0.5);
            total += weight;
        }
    }

    if (total <= 0.0) {
        return std::nullopt;
    }
    return Centroid{weightedX / total, weightedY / total};
}

struct AxisResult {
    double maximumDeviation = 0.0;
    double p99Deviation = 0.0;
};

AxisResult summarise(std::vector<double> deviations)
{
    AxisResult result;
    if (deviations.empty()) {
        return result;
    }
    std::ranges::sort(deviations);
    result.maximumDeviation = deviations.back();

    // Nearest-rank p99, which for 600 samples is the 594th. Reported alongside the maximum
    // because the plan asks for both: the maximum is what gates, the p99 says whether a
    // failure is a distribution or a single outlier.
    const auto rank = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(deviations.size())));
    const std::size_t index = std::min(deviations.size(), std::max<std::size_t>(rank, 1)) - 1;
    result.p99Deviation = deviations[index];
    return result;
}

struct ViewResult {
    std::string name;
    int samples = 0;
    int droppedFrames = 0;
    AxisResult x;
    AxisResult y;
    bool identicalFrames = false;
    bool passed = false;
};

/// One reference view: where the camera is, and where the marker sits relative to it.
struct ReferenceView {
    std::string name;
    sol::render::WorldVec3 cameraPosition;
    /// Marker offset from the camera, metres. The world *position* is what carries the large
    /// magnitude under test; this offset only sets how big the marker appears.
    sol::render::WorldVec3 markerOffset;
    /// Sized so the marker spans roughly fifty pixels.
    ///
    /// Centroid accuracy improves with the number of pixels sampling the intensity profile.
    /// At about eleven pixels across, the profile is quantised coarsely enough by the 8-bit
    /// framebuffer to leave ~0.15 px of centroid noise — which is most of a 0.25 px gate's
    /// budget spent on the instrument rather than on what it is measuring.
    double markerRadiusMetres = 0.0;
};

ViewResult measureView(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    const ReferenceView& view)
{
    ViewResult result;
    result.name = view.name;

    const sol::render::WorldVec3 markerWorld{
        view.cameraPosition.x + view.markerOffset.x,
        view.cameraPosition.y + view.markerOffset.y,
        view.cameraPosition.z + view.markerOffset.z,
    };

    // The marker only. A second object could occlude it or bleed red into the centroid, and
    // the gate is about one marker's position.
    renderer.setScene({sol::render::SceneObject{
        .worldPosition = markerWorld,
        .radiusMetres = view.markerRadiusMetres,
        .colour = {1.0F, 0.0F, 0.0F},
        .smoothMarker = true,
    }});

    const sol::render::CameraState camera{
        .position = view.cameraPosition,
        .forward = {0.0, 0.0, -1.0},
        .up = {0.0, 1.0, 0.0},
        .verticalFovRadians = 1.0472,
        // No temporal antialiasing, no jittered sampling, no motion blur exist in this
        // renderer, so there is nothing to disable — step 2 of the method is satisfied by
        // construction rather than by a switch.
        .nearPlaneMetres = 0.1,
    };

    std::vector<Centroid> centroids;
    centroids.reserve(kMeasuredFrames);
    std::vector<std::uint8_t> firstFrameBytes;
    bool allIdentical = true;

    for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
        window.pollEvents();

        auto captured = renderer.renderFrameCaptured(camera);
        if (!captured.has_value()) {
            std::printf("  frame %d failed: %s\n", frame, captured.error().c_str());
            break;
        }
        if (captured->empty()) {
            ++result.droppedFrames;
            continue;
        }
        if (frame < kWarmupFrames) {
            continue;
        }

        if (firstFrameBytes.empty()) {
            firstFrameBytes = captured->rgba;
        } else if (allIdentical && firstFrameBytes != captured->rgba) {
            allIdentical = false;
        }

        const auto centroid = markerCentroid(*captured);
        if (!centroid.has_value()) {
            ++result.droppedFrames;
            continue;
        }
        centroids.push_back(*centroid);
    }

    result.samples = static_cast<int>(centroids.size());
    result.identicalFrames = allIdentical && !centroids.empty();

    if (centroids.empty()) {
        return result;
    }

    double meanX = 0.0;
    double meanY = 0.0;
    for (const Centroid& centroid : centroids) {
        meanX += centroid.x;
        meanY += centroid.y;
    }
    meanX /= static_cast<double>(centroids.size());
    meanY /= static_cast<double>(centroids.size());

    std::vector<double> deviationsX;
    std::vector<double> deviationsY;
    deviationsX.reserve(centroids.size());
    deviationsY.reserve(centroids.size());
    for (const Centroid& centroid : centroids) {
        deviationsX.push_back(std::abs(centroid.x - meanX));
        deviationsY.push_back(std::abs(centroid.y - meanY));
    }

    result.x = summarise(std::move(deviationsX));
    result.y = summarise(std::move(deviationsY));
    result.passed = result.x.maximumDeviation <= kJitterGatePixels
                    && result.y.maximumDeviation <= kJitterGatePixels;
    return result;
}

/// Does the renderer resolve camera motion far below what a `float` world coordinate could?
///
/// The jitter gate above is necessary but not sufficient, and it is worth being explicit about
/// why. It measures *temporal stability* with everything held constant. A renderer that
/// narrowed world coordinates to `float` before subtracting the camera — the exact mistake
/// `cameraRelative()` exists to prevent — would still render bit-identical frames and still
/// score zero jitter. It would simply be stably wrong. Passing the gate therefore says nothing
/// about precision on its own.
///
/// This does. The camera is stepped laterally in increments far smaller than one ULP of a
/// `float` at the camera's world magnitude, and the marker's centroid is required to respond
/// smoothly. With `double` world coordinates the response is continuous and matches the
/// geometric prediction. With `float` world coordinates every step would fall inside the same
/// representable value, and the centroid would either not move at all or jump by the full
/// quantisation step — which at Earth-radius magnitude is 0.5 m, or about 8.5 pixels
/// at this marker distance. The two outcomes are not subtle, which is what makes this a
/// discriminating control rather than a second stability check.
struct ResponseResult {
    /// Taken from the captured frame rather than from the window, because the swapchain is
    /// sized by the surface and the two can disagree. The geometric prediction depends on it.
    std::uint32_t capturedWidth = 0;
    std::uint32_t capturedHeight = 0;
    int steps = 0;
    double stepMetres = 0.0;
    double floatUlpMetres = 0.0;
    double predictedPixelsPerStep = 0.0;
    double observedPixelsPerStep = 0.0;
    double worstStepError = 0.0;
    bool monotonic = false;
    bool passed = false;
};

ResponseResult measureSubPixelResponse(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    const ReferenceView& view,
    // Height only: with square pixels the focal length in pixels is set by the vertical field
    // of view and the vertical resolution, and the width does not enter the prediction.
    std::uint32_t framebufferHeight)
{
    constexpr int kSteps = 12;
    constexpr double kStepMetres = 0.010; // 10 mm

    ResponseResult result;
    result.steps = kSteps;
    result.stepMetres = kStepMetres;

    // One ULP of a float at the camera's world magnitude: the coarsest position a float could
    // represent there, and the floor this test is designed to sit far beneath.
    const auto magnitude = static_cast<float>(view.cameraPosition.y);
    result.floatUlpMetres =
        static_cast<double>(std::nextafter(magnitude, std::numeric_limits<float>::infinity())
                            - magnitude);

    // The face that carries the intensity profile, not the marker's centre.
    //
    // The marker is a cube of half-extent `markerRadiusMetres`, and the visible profile is
    // rendered on the face nearest the camera — one radius closer than the centre. Predicting
    // from the centre distance instead overstates the distance and understates the expected
    // shift by the ratio of the two, which presented as a clean ~9% scale error and looked
    // convincingly like a renderer bug.
    const double markerDistance = std::abs(view.markerOffset.z) - view.markerRadiusMetres;
    const double focalPixels =
        (static_cast<double>(framebufferHeight) * 0.5) / std::tan(1.0472 * 0.5);
    result.predictedPixelsPerStep = kStepMetres / markerDistance * focalPixels;

    const sol::render::WorldVec3 markerWorld{
        view.cameraPosition.x + view.markerOffset.x,
        view.cameraPosition.y + view.markerOffset.y,
        view.cameraPosition.z + view.markerOffset.z,
    };
    renderer.setScene({sol::render::SceneObject{
        .worldPosition = markerWorld,
        .radiusMetres = view.markerRadiusMetres,
        .colour = {1.0F, 0.0F, 0.0F},
        .smoothMarker = true,
    }});

    // The step index is stored with each sample. A dropped frame would otherwise leave two
    // camera steps between consecutive stored centroids while the arithmetic still divided by
    // one, inflating the measured response by a factor that looks like a real scale error.
    struct Sample {
        int step = 0;
        /// Tracks the screen axis the camera now moves along.
        double centroid = 0.0;
    };
    std::vector<Sample> samples;
    samples.reserve(kSteps);

    for (int step = 0; step < kSteps; ++step) {
        sol::render::CameraState camera{
            .position = view.cameraPosition,
            .forward = {0.0, 0.0, -1.0},
            .up = {0.0, 1.0, 0.0},
            .verticalFovRadians = 1.0472,
            .nearPlaneMetres = 0.1,
        };
        // Step along Y — the axis carrying the large world magnitude — and not along X.
        //
        // This is the whole discriminating power of the control, and an earlier version had it
        // backwards. The marker sits at `camera + offset`, so every component the offset does
        // not touch is *bit-identical* in both operands and subtracts to exactly zero — in
        // `float` just as in `double`. Stepping along X moved a component whose magnitude is
        // at most 0.11 m, where a float ULP is ~1e-8 m; a fully-float world pipeline tracked
        // the geometric prediction to within 4e-9 px and passed the control identically.
        //
        // Along Y the camera sits at 6 378 141.6 m, where a float ULP is 0.5 m. A 10 mm step
        // is 1/50 of that, so a float pipeline rounds camera and marker to the same value and
        // produces *exactly zero* screen motion for fifty consecutive steps. Only a pipeline
        // that subtracts in double before narrowing responds at all.
        camera.position.y += static_cast<double>(step) * kStepMetres;

        window.pollEvents();
        auto captured = renderer.renderFrameCaptured(camera);
        if (!captured.has_value() || captured->empty()) {
            continue;
        }
        result.capturedWidth = captured->width;
        result.capturedHeight = captured->height;

        const auto centroid = markerCentroid(*captured);
        if (centroid.has_value()) {
            samples.push_back({step, centroid->y});
        }
    }

    // Recompute the prediction from the framebuffer that actually rendered, now that it is
    // known. Predicting from the window's reported size instead would silently be wrong
    // wherever the surface disagrees with the window — which is exactly the kind of error that
    // looks like a renderer scale bug.
    if (result.capturedHeight > 0) {
        const double focalFromCapture =
            (static_cast<double>(result.capturedHeight) * 0.5) / std::tan(1.0472 * 0.5);
        result.predictedPixelsPerStep = kStepMetres / markerDistance * focalFromCapture;
    }

    if (samples.size() < 3) {
        return result;
    }

    // The sign is taken from the data rather than asserted, because which way a +Y camera step
    // moves the marker on screen depends on the projection's Y flip. What matters is that the
    // motion is consistent and matches the predicted magnitude — a float pipeline would give
    // exactly zero here, not a sign error.
    const double firstDelta = samples[1].centroid - samples[0].centroid;
    const double direction = firstDelta >= 0.0 ? 1.0 : -1.0;

    result.monotonic = true;
    double totalShift = 0.0;
    int totalSteps = 0;
    double worstError = 0.0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const int stepSpan = samples[i].step - samples[i - 1].step;
        const double delta = (samples[i].centroid - samples[i - 1].centroid) * direction;
        if (delta <= 0.0) {
            result.monotonic = false;
        }
        totalShift += delta;
        totalSteps += stepSpan;

        const double perStep = delta / static_cast<double>(stepSpan);
        worstError = std::max(worstError, std::abs(perStep - result.predictedPixelsPerStep));
    }

    result.observedPixelsPerStep = totalShift / static_cast<double>(totalSteps);
    result.worstStepError = worstError;
    result.steps = static_cast<int>(samples.size());

    // Agreement with the geometric prediction to within a twentieth of a pixel per step, plus
    // a mean within 3%. The tolerance is on the instrument's noise, not on the effect: the
    // effect being demonstrated is a fifty-fold gap between the step size and the float
    // quantisation floor, which is not a close call.
    const double meanError =
        std::abs(result.observedPixelsPerStep - result.predictedPixelsPerStep)
        / result.predictedPixelsPerStep;
    result.passed = result.monotonic && worstError < 0.05 && meanError < 0.03;
    return result;
}

void reportResponse(const ResponseResult& result)
{
    std::printf("\nSub-pixel response control (does the renderer resolve below a float's ULP?)\n");
    if (result.steps == 0 || result.observedPixelsPerStep == 0.0) {
        std::printf("  NO DATA\n");
        return;
    }
    std::printf("  captured frame     %ux%u px\n", result.capturedWidth, result.capturedHeight);
    std::printf("  camera step        %.1f mm\n", result.stepMetres * 1000.0);
    std::printf("  float ULP here     %.4f m  (%.0fx larger than the step)\n",
                result.floatUlpMetres,
                result.floatUlpMetres / result.stepMetres);
    std::printf("  predicted shift    %.6f px/step\n", result.predictedPixelsPerStep);
    std::printf("  observed shift     %.6f px/step\n", result.observedPixelsPerStep);
    std::printf("  worst step error   %.6f px\n", result.worstStepError);
    std::printf("  monotonic          %s\n", result.monotonic ? "yes" : "no");
    std::printf("  verdict            %s\n",
                result.passed ? "PASS — sub-ULP camera motion is resolved smoothly"
                              : "FAIL — response is quantised or does not match geometry");
}

void report(const ViewResult& result)
{
    std::printf("\n%s\n", result.name.c_str());
    std::printf("  samples            %d (dropped %d)\n", result.samples, result.droppedFrames);
    if (result.samples == 0) {
        std::printf("  NO DATA — the marker was never visible.\n");
        return;
    }
    std::printf("  jitter X  max      %.6f px   p99 %.6f px\n",
                result.x.maximumDeviation,
                result.x.p99Deviation);
    std::printf("  jitter Y  max      %.6f px   p99 %.6f px\n",
                result.y.maximumDeviation,
                result.y.p99Deviation);
    std::printf("  frames identical   %s\n",
                result.identicalFrames ? "yes (bit-for-bit)" : "no");
    std::printf("  gate (%.2f px)      %s\n",
                kJitterGatePixels,
                result.passed ? "PASS" : "FAIL");
}

} // namespace

int main()
{
    auto window = sol::platform::Window::create("Jitter harness", 1280, 720);
    if (!window.has_value()) {
        std::printf("%s\n\nSKIPPED: no window system.\n", window.error().c_str());
        return kSkipExitCode;
    }

    const sol::render::InstanceConfig config{
        .applicationName = "SolJitterHarness",
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

    std::printf("Screen-space jitter gate — method fixed in the P1b milestone plan\n");
    std::printf("Device:      %s\n", renderer->selectedDevice().deviceName.c_str());
    // Which of two shader binaries produced the numbers below. Debug and Release compile
    // different SPIR-V, and nothing constrains what spirv-opt does to float association.
    std::printf("Shaders:     %s\n",
                std::string(sol::render::shaderBuildDescription()).c_str());
    std::printf("Resolution:  %ux%u\n", size.width, size.height);
    std::printf("Frames:      %d measured after %d warm-up, per view\n",
                kMeasuredFrames,
                kWarmupFrames);

    // Both reference views the plan requires. The camera's world position is what matters:
    // at these magnitudes a float would have metres of quantisation, so the frame model is
    // genuinely under test rather than nominally so.
    const std::vector<ReferenceView> views{
        {.name = "Surface anchor (camera at Earth-radius magnitude, 6378 km from origin)",
         .cameraPosition = {0.0, kEarthRadiusMetres + 5.0, 0.0},
         .markerOffset = {0.0, 0.0, -40.0},
         .markerRadiusMetres = 3.2},
        {.name = "Orbital vantage (200 km altitude, 6578 km from origin)",
         .cameraPosition = {0.0, kEarthRadiusMetres + 200000.0, 0.0},
         .markerOffset = {0.0, 0.0, -40.0},
         .markerRadiusMetres = 3.2},
    };

    std::vector<ViewResult> results;
    for (const ReferenceView& view : views) {
        results.push_back(measureView(*window, *renderer, view));
        report(results.back());
    }

    // Run the discriminating control at the surface anchor, where the world magnitude is
    // largest relative to the marker distance and a float frame model would fail hardest.
    const ResponseResult response =
        measureSubPixelResponse(*window, *renderer, views.front(), size.height);
    reportResponse(response);

    renderer->waitIdle();

    const auto& messages = instance->validationMessages();
    std::printf("\nValidation messages: %zu\n", messages.size());
    for (const std::string& message : messages) {
        std::printf("  %s\n", message.c_str());
    }

    const bool gatePassed = std::ranges::all_of(
        results, [](const ViewResult& result) { return result.passed && result.samples > 0; });

    std::printf("\n%s\n", gatePassed ? "GATE PASSED at both reference views."
                                     : "GATE FAILED — see the per-view results above.");
    std::printf("%s\n",
                response.passed
                    ? "CONTROL PASSED — the gate result reflects precision, not just stability."
                    : "CONTROL FAILED — the gate result above cannot be read as evidence of "
                      "precision.");

    // Both must hold. A passing gate with a failing control would mean the renderer is stable
    // and wrong, which the gate alone cannot distinguish from stable and right.
    return (gatePassed && response.passed) ? 0 : 1;
}
