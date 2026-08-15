/// @file
/// The depth gate: no depth collapse, z-fighting, or near/far discontinuity across the full
/// surface-to-orbit path.
///
/// The threshold is stated qualitatively, which makes it tempting to satisfy by rendering a
/// scene and looking at it. That proves nothing — the same trap the jitter gate's first zero
/// reading fell into. This measures instead.
///
/// **What is measured.** Under the reversed-Z infinite projection the stored depth value is an
/// analytic function of distance:
///
///     depth = nearPlaneMetres / distance
///
/// so a depth value converts back to a distance exactly. Three things follow, and together
/// they are the whole gate:
///
///   1. **Accuracy.** The depth read back at a known distance must match `near / distance`.
///      A mismatch means the projection is wrong somewhere along the path.
///   2. **No collapse.** Two distinct distances must produce two distinct depth values. The
///      smallest separation that still does is the depth resolution at that range, and it is
///      measured directly by bisection rather than asserted.
///   3. **No discontinuity.** Because relative precision is constant in a float and depth goes
///      as 1/distance, the resolvable separation must scale *linearly* with distance across
///      the whole range. A break in that line is a near/far discontinuity.
///
/// z-fighting is the same phenomenon as collapse: two surfaces fight when their depths are not
/// distinguishable. Measuring the resolution measures the z-fighting threshold.
///
/// **Why there is a negative control.** A passing depth measurement means little unless the
/// measurement could have failed. The conventional finite-far projection — what reversed-Z
/// replaced — is run through the identical harness. It should collapse badly at long range. If
/// it does not, the harness is not sensitive enough to trust on the production path either.

#include "Sol/Platform/Window.h"
#include "Sol/Render/Renderer.h"
#include "Sol/Render/ShaderBuild.h"
#include "Sol/Render/VulkanInstance.h"
#include "Support/DeviceOption.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kSkipExitCode = 77;
constexpr double kNearPlaneMetres = 0.1;

/// Earth's equatorial radius from the ADR 0008 reference ellipsoid, metres.
constexpr double kEarthRadiusMetres = 6378136.6;

/// The surface-to-orbit path, sampled logarithmically from arm's length to beyond orbit.
const std::vector<double> kProbeDistances{
    1.0, 10.0, 100.0, 1.0e3, 1.0e4, 1.0e5, 1.0e6, kEarthRadiusMetres, 1.0e7,
};

/// Renders one flat marker at @p distance ahead of the camera and returns the depth at the
/// centre pixel, or a negative value when nothing was drawn there.
double probeDepth(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    double distance,
    sol::render::ProjectionMode mode,
    double cameraAltitude)
{
    // The camera sits at Earth-radius magnitude throughout, so this exercises the same large
    // world coordinates the rest of the renderer must survive rather than a convenient origin.
    const sol::render::WorldVec3 cameraPosition{0.0, cameraAltitude, 0.0};

    // Radius scales with distance so the marker always covers the centre pixel; a fixed size
    // would vanish below a pixel long before the far end of the sweep.
    const double radius = distance * 0.05;

    renderer.setScene({sol::render::SceneObject{
        .worldPosition = {cameraPosition.x, cameraPosition.y, cameraPosition.z - distance},
        .radiusMetres = radius,
        .colour = {0.9F, 0.9F, 0.9F},
    }});

    sol::render::CameraState camera{
        .position = cameraPosition,
        .forward = {0.0, 0.0, -1.0},
        .up = {0.0, 1.0, 0.0},
        .verticalFovRadians = 1.0472,
        .nearPlaneMetres = kNearPlaneMetres,
    };
    camera.projection = mode;
    camera.farPlaneMetres = 1.0e7;

    window.pollEvents();
    auto captured = renderer.renderFrameCaptured(camera);
    if (!captured.has_value() || captured->empty() || captured->depth.empty()) {
        return -1.0;
    }

    return static_cast<double>(captured->depthAt(captured->width / 2, captured->height / 2));
}

/// The distance the marker's front face actually sits at.
///
/// The marker is a cube of half-extent `distance * 0.05`, and the surface the depth buffer
/// records is its near face — one radius closer than its centre. Predicting from the centre
/// would misstate the expected depth by 5%, which is the same mistake the jitter control's
/// first run made and is worth not repeating.
double frontFaceDistance(double centreDistance)
{
    return centreDistance * 0.95;
}

struct RangeResult {
    double distance = 0.0;
    double measuredDepth = 0.0;
    double predictedDepth = 0.0;
    double relativeError = 0.0;
    /// Smallest separation, in metres, that produces a different stored depth value.
    double resolvedSeparation = 0.0;
    /// resolvedSeparation / distance. Constant across the range for a float reversed-Z buffer.
    double relativeResolution = 0.0;
    bool depthValid = false;
};

/// Finds the smallest separation at @p distance that the depth buffer still distinguishes.
///
/// Bisection on the separation, comparing stored depth values for exact inequality. "Exact"
/// is the right comparison: two distances that store the same float have collapsed, and that
/// is precisely what the gate forbids.
double findResolvedSeparation(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    double distance,
    sol::render::ProjectionMode mode,
    double cameraAltitude,
    double baseDepth)
{
    double low = 0.0;
    double high = distance * 1.0e-3;

    // Confirm the upper bound actually separates before bisecting into it; otherwise the
    // search would converge on a meaningless value.
    if (probeDepth(window, renderer, distance + high, mode, cameraAltitude) == baseDepth) {
        return -1.0; // Collapsed even at a thousandth of the distance.
    }

    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (low + high);
        const double depth = probeDepth(window, renderer, distance + mid, mode, cameraAltitude);
        if (depth < 0.0) {
            return -1.0;
        }
        if (depth == baseDepth) {
            low = mid;
        } else {
            high = mid;
        }
        if ((high - low) <= distance * 1.0e-12) {
            break;
        }
    }
    return high;
}

std::vector<RangeResult> sweep(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    sol::render::ProjectionMode mode,
    double cameraAltitude)
{
    std::vector<RangeResult> results;

    for (double distance : kProbeDistances) {
        RangeResult result;
        result.distance = distance;

        const double depth = probeDepth(window, renderer, distance, mode, cameraAltitude);
        if (depth < 0.0) {
            results.push_back(result);
            continue;
        }
        result.depthValid = true;
        result.measuredDepth = depth;

        if (mode == sol::render::ProjectionMode::ReversedZInfinite) {
            result.predictedDepth = kNearPlaneMetres / frontFaceDistance(distance);
            result.relativeError = result.predictedDepth > 0.0
                                       ? std::abs(depth - result.predictedDepth)
                                             / result.predictedDepth
                                       : 0.0;
        }

        result.resolvedSeparation =
            findResolvedSeparation(window, renderer, distance, mode, cameraAltitude, depth);
        result.relativeResolution =
            result.resolvedSeparation > 0.0 ? result.resolvedSeparation / distance : -1.0;

        results.push_back(result);
    }

    return results;
}

/// The ratio column, or COLLAPSED. `std::to_string` renders these values as "0.000000",
/// which silently hides the single most informative number in the table.
std::string formatRelative(const RangeResult& result)
{
    if (result.resolvedSeparation <= 0.0) {
        return "COLLAPSED";
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.3e", result.relativeResolution);
    return buffer;
}

void reportSweep(const char* title, const std::vector<RangeResult>& results, bool showPrediction)
{
    std::printf("\n%s\n", title);
    if (showPrediction) {
        std::printf("  %-12s %-14s %-14s %-10s %-14s %s\n",
                    "distance", "depth", "predicted", "rel.err", "resolved sep", "sep/dist");
    } else {
        std::printf("  %-12s %-14s %-14s %s\n", "distance", "depth", "resolved sep", "sep/dist");
    }

    for (const RangeResult& result : results) {
        if (!result.depthValid) {
            std::printf("  %-12.3g  (no geometry rendered at centre pixel)\n", result.distance);
            continue;
        }
        if (showPrediction) {
            std::printf("  %-12.3g %-14.7g %-14.7g %-10.2e %-14.6g %s\n",
                        result.distance,
                        result.measuredDepth,
                        result.predictedDepth,
                        result.relativeError,
                        result.resolvedSeparation,
                        formatRelative(result).c_str());
        } else {
            std::printf("  %-12.3g %-14.7g %-14.6g %s\n",
                        result.distance,
                        result.measuredDepth,
                        result.resolvedSeparation,
                        formatRelative(result).c_str());
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const auto selection = sol::testing::parseDeviceOption(argc, argv);

    auto window = sol::platform::Window::create("Depth gate", 1280, 720);
    if (!window.has_value()) {
        std::printf("%s\n\nSKIPPED: no window system.\n", window.error().c_str());
        return kSkipExitCode;
    }

    const sol::render::InstanceConfig config{
        .applicationName = "SolDepthGate",
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
        },
        selection);
    if (!renderer.has_value()) {
        std::printf("Renderer creation failed.\n\n%s\n", renderer.error().c_str());
        return 1;
    }

    std::printf("Depth gate — surface-to-orbit depth behaviour\n");
    std::printf("%s\n", sol::testing::describeDeviceRequest(selection).c_str());
    std::printf("Device:      %s\n", renderer->selectedDevice().deviceName.c_str());
    // Which of two shader binaries produced the numbers below. Debug and Release compile
    // different SPIR-V, and nothing constrains what spirv-opt does to float association.
    std::printf("Shaders:     %s\n",
                std::string(sol::render::shaderBuildDescription()).c_str());
    std::printf("Format:      VK_FORMAT_D32_SFLOAT, reversed-Z, infinite far plane\n");
    std::printf("Near plane:  %.3f m\n", kNearPlaneMetres);
    std::printf("Camera at:   %.1f km from world origin\n", kEarthRadiusMetres / 1000.0);

    const double altitude = kEarthRadiusMetres + 5.0;

    const auto production =
        sweep(*window, *renderer, sol::render::ProjectionMode::ReversedZInfinite, altitude);
    reportSweep("Production path: reversed-Z, infinite far plane", production, true);

    const auto control =
        sweep(*window, *renderer, sol::render::ProjectionMode::ConventionalFinite, altitude);
    reportSweep("NEGATIVE CONTROL: conventional projection, far plane 1e7 m", control, false);

    renderer->waitIdle();

    // --- Verdicts ----------------------------------------------------------------------
    bool accuracyHolds = true;
    bool noCollapse = true;
    double minRelative = 1.0;
    double maxRelative = 0.0;

    for (const RangeResult& result : production) {
        if (!result.depthValid) {
            noCollapse = false;
            continue;
        }
        if (result.relativeError > 1.0e-4) {
            accuracyHolds = false;
        }
        if (result.resolvedSeparation <= 0.0) {
            noCollapse = false;
            continue;
        }
        minRelative = std::min(minRelative, result.relativeResolution);
        maxRelative = std::max(maxRelative, result.relativeResolution);
    }

    // Constant relative resolution is what "no near/far discontinuity" means for a float
    // reversed-Z buffer: the resolvable separation must track distance linearly, so the ratio
    // must stay within a band across nine orders of magnitude. A jump in this ratio is a
    // discontinuity even if no individual value looks alarming.
    //
    // The band is one order of magnitude, and the reason is worth stating rather than tuning
    // to whatever the first run produced. Two effects put irreducible scatter in this ratio:
    // a float's ULP doubles across each binade, so where a depth value happens to land inside
    // its exponent range moves the resolvable separation by up to 2x; and the bisection
    // converges to a bracket rather than to an exact boundary. Together those account for a
    // few-fold spread, and measurement shows about 4x. A genuine discontinuity does not look
    // like that — it looks like the negative control, which does not widen its ratio but
    // collapses outright. A tighter bound would be a threshold tuned to one run's luck.
    constexpr double kRelativeResolutionBand = 10.0;
    const double spread = minRelative > 0.0 ? maxRelative / minRelative : 0.0;
    const bool noDiscontinuity =
        noCollapse && spread > 0.0 && spread <= kRelativeResolutionBand;

    int controlCollapses = 0;
    for (const RangeResult& result : control) {
        if (result.depthValid && result.resolvedSeparation <= 0.0) {
            ++controlCollapses;
        }
    }
    const bool controlDiscriminates = controlCollapses > 0;

    std::printf("\nVerdicts\n");
    std::printf("  depth matches near/distance      %s\n", accuracyHolds ? "PASS" : "FAIL");
    std::printf("  no collapse at any range         %s\n", noCollapse ? "PASS" : "FAIL");
    std::printf("  relative resolution band         %.6g to %.6g (spread %.2fx)\n",
                minRelative,
                maxRelative,
                spread);
    std::printf("  no near/far discontinuity        %s\n", noDiscontinuity ? "PASS" : "FAIL");
    std::printf("  control collapses (must be > 0)  %d — %s\n",
                controlCollapses,
                controlDiscriminates ? "harness discriminates"
                                     : "HARNESS NOT SENSITIVE; results above are untrustworthy");

    const auto& messages = instance->validationMessages();
    std::printf("\nValidation messages: %zu\n", messages.size());
    for (const std::string& message : messages) {
        std::printf("  %s\n", message.c_str());
    }

    const bool passed = accuracyHolds && noCollapse && noDiscontinuity && controlDiscriminates;
    std::printf("\n%s\n",
                passed ? "DEPTH GATE PASSED across the full surface-to-orbit path."
                       : "DEPTH GATE FAILED — see the verdicts above.");
    return passed ? 0 : 1;
}
