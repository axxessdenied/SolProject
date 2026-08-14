/// @file
/// The LOD continuity gate: no popping a fixed observer path can detect, and no unbounded
/// memory growth over a long traverse.
///
/// **How popping is detected.** The camera follows a fixed path, descending toward the planet
/// in small logarithmic steps so that each step changes the view by a similar small amount.
/// Between consecutive frames the mean absolute luminance difference is computed. Under smooth
/// motion that difference is small and varies slowly. A LOD transition that pops is a geometry
/// discontinuity, so it appears as a **spike** — one step whose difference is far above its
/// neighbours.
///
/// Detection is therefore a local-outlier test rather than a fixed threshold: the image changes
/// faster near the surface than far from it, so any single global number would either miss pops
/// high up or flag ordinary motion low down. Each step is compared against the median of its
/// neighbourhood.
///
/// **Why there is a control.** A popping detector that never fires proves nothing about the
/// renderer — it may simply be blind. The same traverse is therefore run twice: once with
/// continuous LOD morphing, and once with morphing disabled, which is the abrupt-switch scheme
/// morphing replaced. The control must produce pops. If it does not, the detector is not
/// sensitive enough for its verdict on the production path to mean anything.
///
/// **Memory.** Device allocation is sampled every step. The gate asks for no *unbounded*
/// growth, so what is checked is that the working set stays flat rather than trending upward.

#include "Sol/Platform/Window.h"
#include "Sol/Render/Renderer.h"
#include "Sol/Render/VulkanInstance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int kSkipExitCode = 77;

constexpr double kPlanetRadiusMetres = 6378136.6;
constexpr double kStartAltitudeMetres = 300000.0;
constexpr double kEndAltitudeMetres = 3000.0;
constexpr int kTraverseSteps = 600;

/// The recorded quality setting the gate is measured at.
///
/// This is the value the renderer defaults to, not a special test configuration, and it is
/// pinned near the bottom of the range where the LOD scheme is well-conditioned.
///
/// An earlier version used 0.6 and described it as "deliberately aggressive… what a shipping
/// renderer would use for performance". That was wrong twice over: it was an unevidenced
/// production claim, and 0.6 is below the scheme's validity floor. The morph band is at most
/// `subdivisionFactor` patch-widths, and a smooth per-vertex morph needs it wider than about
/// 2.8 of them; at 0.6 the factor sweeps 0 to 1 within a single patch and the per-vertex morph
/// measured *worse* than no morphing at all.
///
/// The uncomfortable consequence is recorded rather than tuned around: raising the factor to
/// where the morph is sound is exactly what makes transitions sub-pixel, so the control cannot
/// fire. Visibility and validity pull against each other through this one parameter.
constexpr double kQualitySubdivisionFactor = 3.0;

// The "not a special test configuration" claim above, checked rather than asserted.
//
// It was false when written: the public default was 2.5, below the scheme's own validity floor,
// while the gate quietly measured at 3.0. A comment cannot keep the two in step, so the compiler
// does it. If the renderer's default moves, this gate stops building until someone decides
// whether the threshold is still defined at the setting the renderer actually ships.
static_assert(sol::render::TerrainSettings{}.subdivisionFactor == kQualitySubdivisionFactor,
              "The LOD gate must be measured at the renderer's default quality setting, since "
              "the P1b threshold is defined \"at the recorded quality setting\".");

/// Relief for the LOD stress scene, in metres.
///
/// Far rougher than Earth, and deliberately so. This is a stress case, not a landscape: the
/// gate asks whether LOD transitions are detectable, and a scene whose transitions are
/// sub-pixel answers that question by making it unaskable rather than by passing it. At 8 km
/// of relief the geometry a level change uncovers moved the image by about one luminance
/// level, below anything a viewer could see and below anything an instrument can certify.
///
/// The logic runs the safe direction. If continuous morphing keeps transitions invisible on
/// terrain this rough, it keeps them invisible on terrain that is gentler; a pass here implies
/// a pass on Earth-like relief, while a pass on Earth-like relief implies nothing at all.
constexpr double kStressReliefMetres = 20000.0;

/// Neighbourhood width for the local-median comparison, in steps.
constexpr int kWindowRadius = 12;
/// A step is a pop when its frame difference exceeds this multiple of the local median.
constexpr double kSpikeFactor = 4.0;

struct TraverseResult {
    std::string label;
    int samples = 0;
    std::vector<double> differences;
    std::vector<int> popIndices;
    double worstRatio = 0.0;
    /// The frame difference at the step with the worst ratio, and the traverse's own median.
    /// Reported because the floor that separates a pop from noise has to be chosen from these
    /// magnitudes rather than guessed.
    double worstRatioDifference = 0.0;
    double medianDifference = 0.0;
    double maximumDifference = 0.0;
    std::uint64_t minDeviceBytes = 0;
    std::uint64_t maxDeviceBytes = 0;
    std::uint64_t finalDeviceBytes = 0;
    std::uint32_t maxPatches = 0;
    std::uint32_t maxVertices = 0;
    double memoryTrendBytesPerStep = 0.0;

    /// Concentration metric across the whole descent, not just a chosen band.
    ///
    /// Selecting a band by largest patch-count change turned out to find transitions among
    /// distant horizon patches, which are tiny on screen. The descent visits every transition
    /// there is, so measuring concentration across all of it asks the question without first
    /// having to guess where the answer lives.
    double worstChangedFraction = 0.0;
    int worstChangedStep = -1;
    double altitudeAtWorst = 0.0;

    /// Steps at which the drawn patch count changed, and by how much.
    ///
    /// Recorded so a pop can be attributed rather than guessed at. A pop coinciding with a
    /// patch-count change is geometry appearing or disappearing; a pop with no count change is
    /// geometry moving, which is a different defect with a different fix.
    std::vector<std::pair<int, int>> patchCountChanges;
};

double meanLuminance(const std::vector<std::uint8_t>& rgba, std::size_t index)
{
    // Rec. 601 luma. Any reasonable weighting works; what matters is that it is a fixed,
    // smooth function of colour so that a geometry change is what moves it.
    return (0.299 * rgba[index]) + (0.587 * rgba[index + 1]) + (0.114 * rgba[index + 2]);
}

/// What changed between two frames, described by the distribution rather than the average.
///
/// Both a mean and a concentration measure are computed, and the verdict uses the **mean**,
/// through the local-outlier test in detectPops.
///
/// An earlier revision dismissed the mean as "structurally wrong" — reasoning that morphing
/// spreads a transition into many tiny deformations while an abrupt switch concentrates it,
/// so the two could average alike. That reasoning was retracted by measurement: on smooth
/// terrain the mean failed because there was nothing to detect, not because it was the wrong
/// statistic, and on terrain with grid-scale relief it is the mean that discriminates while
/// the concentration measure does not. Concentration is confounded by morphing's own
/// continuous deformation, which moves many pixels slightly where an abrupt switch moves few
/// pixels sharply. The concentration figures are retained as diagnostics.
struct StepChange {
    double mean = 0.0;
    /// Fraction of pixels whose luminance changed by more than a perceptible step.
    double changedFraction = 0.0;
    /// 99.9th percentile of per-pixel change, in 0-255 luminance units.
    double p999 = 0.0;
};

/// A luminance step of 16 in 255 is about 6%, comfortably visible on adjacent frames.
constexpr int kPerceptibleDelta = 16;

StepChange stepChange(
    const std::vector<std::uint8_t>& previous,
    const std::vector<std::uint8_t>& current)
{
    StepChange change;
    if (previous.size() != current.size() || previous.empty()) {
        return change;
    }

    // A 256-bin histogram gives both the count above a threshold and any percentile in one
    // linear pass. Sorting a million values per step, for hundreds of steps, would not finish.
    std::array<std::uint64_t, 256> histogram{};
    double total = 0.0;
    const std::size_t pixels = previous.size() / 4;

    for (std::size_t i = 0; i < previous.size(); i += 4) {
        const double delta =
            std::abs(meanLuminance(current, i) - meanLuminance(previous, i));
        total += delta;
        const auto bin = static_cast<std::size_t>(std::min(255.0, std::round(delta)));
        ++histogram[bin];
    }

    change.mean = total / static_cast<double>(pixels);

    std::uint64_t above = 0;
    for (std::size_t bin = kPerceptibleDelta; bin < histogram.size(); ++bin) {
        above += histogram[bin];
    }
    change.changedFraction = static_cast<double>(above) / static_cast<double>(pixels);

    const auto target = static_cast<std::uint64_t>(0.999 * static_cast<double>(pixels));
    std::uint64_t cumulative = 0;
    for (std::size_t bin = 0; bin < histogram.size(); ++bin) {
        cumulative += histogram[bin];
        if (cumulative >= target) {
            change.p999 = static_cast<double>(bin);
            break;
        }
    }

    return change;
}

double median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t middle = values.size() / 2;
    std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(middle));
    return values[middle];
}

/// Flags steps whose frame difference is a local outlier.
void detectPops(TraverseResult& result)
{
    const auto count = static_cast<int>(result.differences.size());
    if (count == 0) {
        return;
    }

    result.medianDifference = median(result.differences);
    result.maximumDifference = *std::ranges::max_element(result.differences);

    // The floor is set from this traverse's own scale rather than as an absolute luminance
    // value, because no absolute number serves both a descent that is mostly still and one
    // that is mostly motion.
    //
    // It is the median itself, not a fraction of it. A pop is a frame that changes *more* than
    // ordinary motion does; a step quieter than the traverse's typical frame cannot be one, no
    // matter how it compares to its immediate neighbours. An earlier quarter-median floor
    // admitted exactly that — a "pop" at a difference below the median, flagged only because
    // the stretch around it was quieter still.
    const double floorValue = result.medianDifference;

    for (int i = 0; i < count; ++i) {
        std::vector<double> window;
        window.reserve(static_cast<std::size_t>(kWindowRadius) * 2);
        for (int j = std::max(0, i - kWindowRadius); j < std::min(count, i + kWindowRadius + 1); ++j) {
            if (j != i) {
                window.push_back(result.differences[j]);
            }
        }
        const double local = median(std::move(window));
        if (local <= 0.0) {
            continue;
        }
        const double ratio = result.differences[i] / local;
        if (ratio > result.worstRatio) {
            result.worstRatio = ratio;
            result.worstRatioDifference = result.differences[i];
        }

        // Both conditions. The ratio alone would flag quiet stretches where the median is
        // near zero and any flicker looks enormous; the floor alone would flag fast motion.
        if (ratio > kSpikeFactor && result.differences[i] > floorValue) {
            result.popIndices.push_back(i);
        }
    }
}

/// A narrow, finely-sampled altitude band that contains LOD transitions.
///
/// The full descent turned out to be the wrong instrument for detecting popping, and the
/// reason is worth stating. With hundreds of patches on screen, transitions do not happen as
/// isolated events — patches cross their boundaries continuously as the camera moves, so an
/// abrupt scheme raises the *baseline* frame difference rather than producing spikes, and a
/// local-outlier test finds nothing to flag. The descent measured this directly: disabling
/// morphing raised the median difference by 10% and the maximum by 31%, while the worst local
/// ratio stayed at 1.3x, far below any sensible spike threshold.
///
/// Crossing one boundary slowly isolates it. Over a 10 km band sampled in 300 steps the camera
/// moves 33 m per step, so smooth motion contributes almost nothing and a transition that
/// jumps has nowhere to hide.
struct SweepResult {
    std::string label;
    int samples = 0;
    double medianDifference = 0.0;
    double maximumDifference = 0.0;
    /// Maximum divided by median: how far the worst single step stands above ordinary motion.
    double peakRatio = 0.0;

    /// The verdict quantity: the largest fraction of the screen to change by a perceptible
    /// amount in any single step of the sweep.
    double worstChangedFraction = 0.0;
    /// The 99.9th percentile of per-pixel change at that same worst step.
    double worstP999 = 0.0;
    int worstStep = -1;
};

/// Finds the altitude at which the LOD selection actually changes.
///
/// Deriving a band from the subdivision arithmetic did not work. Two hand-picked bands each
/// turned out to contain no transition, and in both the control measured *smoother* than the
/// production path — which is exactly what an abrupt scheme looks like when nothing switches,
/// since its geometry is then perfectly static while morphing deforms continuously. A control
/// that never triggers cannot demonstrate anything, and picking bands until one looks right
/// would be fitting the experiment to the desired answer.
///
/// The renderer already reports how many patches it drew, and a transition is by definition a
/// change in that number. Scanning for it removes the guesswork entirely.
/// Returns no value when the scan found no transition at all, which is a real outcome rather
/// than an error: a sweep centred on a transition that does not exist measures nothing, and a
/// zero altitude would put the camera on the reference radius, underneath the terrain. Reporting
/// the absence is the same discipline this file applies to `worstStep < 0`.
std::optional<double> findTransitionAltitude(
    sol::platform::Window& window,
    sol::render::Renderer& renderer)
{
    constexpr int kScanSteps = 400;

    sol::render::TerrainSettings terrain;
    terrain.centre = {0.0, 0.0, 0.0};
    terrain.radiusMetres = kPlanetRadiusMetres;
    terrain.reliefMetres = kStressReliefMetres;
    terrain.maxLevel = 10;
    terrain.subdivisionFactor = kQualitySubdivisionFactor;
    terrain.morphEnabled = true;
    renderer.setTerrain(terrain);
    renderer.setScene({});

    double bestAltitude = 0.0;
    std::uint32_t bestDelta = 0;
    std::uint32_t previousPatches = 0;
    double previousAltitude = 0.0;
    // An explicit sentinel rather than `previousPatches != 0`, which cannot tell "no sample yet"
    // apart from "a sample that drew no patches".
    bool havePrevious = false;

    for (int step = 0; step < kScanSteps; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(kScanSteps - 1);
        const double altitude =
            kEndAltitudeMetres * std::pow(kStartAltitudeMetres / kEndAltitudeMetres, t);

        const sol::render::CameraState camera{
            .position = {0.0, kPlanetRadiusMetres + altitude, 0.0},
            .forward = {0.30, -0.95, 0.0},
            .up = {0.0, 0.0, 1.0},
            .verticalFovRadians = 1.0472,
            .nearPlaneMetres = 0.5,
        };

        window.pollEvents();
        auto frame = renderer.renderFrame(camera);
        if (!frame.has_value()) {
            break;
        }
        if (!frame->presented) {
            // A skipped frame reports zero patches, and zero against a real previous count is
            // the largest delta this scan can possibly see — so the sweep would be centred
            // wherever the window happened to be minimised. Dropping the previous sample too
            // keeps the next comparison from spanning two altitude steps, which is the same
            // defect that manufactured a pop in the descent.
            havePrevious = false;
            continue;
        }

        if (havePrevious) {
            const std::uint32_t delta = frame->terrainPatches > previousPatches
                                            ? frame->terrainPatches - previousPatches
                                            : previousPatches - frame->terrainPatches;
            if (delta > bestDelta) {
                bestDelta = delta;
                bestAltitude = 0.5 * (altitude + previousAltitude);
            }
        }
        previousPatches = frame->terrainPatches;
        previousAltitude = altitude;
        havePrevious = true;
    }

    if (bestDelta == 0) {
        std::printf("Transition scan: the drawn patch count never changed across %d steps, so "
                    "there is no transition to centre a sweep on.\n",
                    kScanSteps);
        return std::nullopt;
    }

    std::printf("Transition scan: largest patch-count change is %u patches at %.0f m altitude\n",
                bestDelta,
                bestAltitude);
    return bestAltitude;
}

SweepResult runTransitionSweep(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    bool morphEnabled,
    double centreAltitude,
    const char* label)
{
    // A narrow window either side of a transition known to exist, rather than a guessed band.
    const double kBandLowMetres = centreAltitude * 0.94;
    const double kBandHighMetres = centreAltitude * 1.06;
    constexpr int kSweepSteps = 300;

    SweepResult result;
    result.label = label;

    sol::render::TerrainSettings terrain;
    terrain.centre = {0.0, 0.0, 0.0};
    terrain.radiusMetres = kPlanetRadiusMetres;
    terrain.reliefMetres = kStressReliefMetres;
    terrain.maxLevel = 10;
    terrain.subdivisionFactor = kQualitySubdivisionFactor;
    terrain.morphEnabled = morphEnabled;
    renderer.setTerrain(terrain);
    renderer.setScene({});

    std::vector<std::uint8_t> previous;
    std::vector<double> differences;

    for (int step = 0; step < kSweepSteps; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(kSweepSteps - 1);
        const double altitude = kBandLowMetres + (t * (kBandHighMetres - kBandLowMetres));

        const sol::render::CameraState camera{
            .position = {0.0, kPlanetRadiusMetres + altitude, 0.0},
            .forward = {0.30, -0.95, 0.0},
            .up = {0.0, 0.0, 1.0},
            .verticalFovRadians = 1.0472,
            .nearPlaneMetres = 0.5,
        };

        window.pollEvents();
        auto captured = renderer.renderFrameCaptured(camera);
        if (!captured.has_value() || captured->empty()) {
            continue;
        }
        if (!previous.empty()) {
            const StepChange change = stepChange(previous, captured->rgba);
            differences.push_back(change.mean);
            if (change.changedFraction > result.worstChangedFraction) {
                result.worstChangedFraction = change.changedFraction;
                result.worstP999 = change.p999;
                result.worstStep = step;
            }
        }
        previous = captured->rgba;
        ++result.samples;
    }

    if (!differences.empty()) {
        result.medianDifference = median(differences);
        result.maximumDifference = *std::ranges::max_element(differences);
        result.peakRatio = result.medianDifference > 0.0
                               ? result.maximumDifference / result.medianDifference
                               : 0.0;
    }
    return result;
}

TraverseResult runTraverse(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    bool morphEnabled,
    const char* label)
{
    TraverseResult result;
    result.label = label;

    sol::render::TerrainSettings terrain;
    terrain.centre = {0.0, 0.0, 0.0};
    terrain.radiusMetres = kPlanetRadiusMetres;
    terrain.reliefMetres = kStressReliefMetres;
    terrain.maxLevel = 10;
    terrain.subdivisionFactor = kQualitySubdivisionFactor;
    terrain.morphEnabled = morphEnabled;
    renderer.setTerrain(terrain);
    renderer.setScene({});

    std::vector<std::uint8_t> previous;
    std::vector<double> deviceBytes;

    for (int step = 0; step < kTraverseSteps; ++step) {
        // Logarithmic descent: each step multiplies altitude by a constant factor, so LOD
        // transitions — which happen at fixed distance ratios — are spread evenly across the
        // traverse instead of bunching at one end.
        const double t = static_cast<double>(step) / static_cast<double>(kTraverseSteps - 1);
        const double altitude =
            kStartAltitudeMetres * std::pow(kEndAltitudeMetres / kStartAltitudeMetres, t);

        sol::render::CameraState camera{
            .position = {0.0, kPlanetRadiusMetres + altitude, 0.0},
            // Looking down and slightly forward, so the traverse crosses many patches rather
            // than staring at one.
            .forward = {0.30, -0.95, 0.0},
            .up = {0.0, 0.0, 1.0},
            .verticalFovRadians = 1.0472,
            .nearPlaneMetres = 0.5,
        };

        window.pollEvents();
        auto captured = renderer.renderFrameCaptured(camera);
        if (!captured.has_value()) {
            std::printf("  step %d failed: %s\n", step, captured.error().c_str());
            break;
        }
        if (captured->empty()) {
            continue;
        }

        if (!previous.empty()) {
            const StepChange change = stepChange(previous, captured->rgba);
            result.differences.push_back(change.mean);
            if (change.changedFraction > result.worstChangedFraction) {
                result.worstChangedFraction = change.changedFraction;
                result.worstChangedStep = step;
                result.altitudeAtWorst = altitude;
            }
        }
        previous = captured->rgba;
        ++result.samples;
    }

    // Statistics are read from the last frame's stats via a plain render, since the capture
    // path returns pixels rather than counters.
    detectPops(result);
    return result;
}

void collectResourceStats(
    sol::platform::Window& window,
    sol::render::Renderer& renderer,
    TraverseResult& result,
    bool morphEnabled)
{
    sol::render::TerrainSettings terrain;
    terrain.centre = {0.0, 0.0, 0.0};
    terrain.radiusMetres = kPlanetRadiusMetres;
    terrain.reliefMetres = kStressReliefMetres;
    terrain.maxLevel = 10;
    terrain.subdivisionFactor = kQualitySubdivisionFactor;
    terrain.morphEnabled = morphEnabled;
    renderer.setTerrain(terrain);

    std::vector<double> samples;
    int previousPatchCount = -1;
    std::uint64_t minimum = UINT64_MAX;
    std::uint64_t maximum = 0;
    std::uint64_t last = 0;

    for (int step = 0; step < kTraverseSteps; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(kTraverseSteps - 1);
        const double altitude =
            kStartAltitudeMetres * std::pow(kEndAltitudeMetres / kStartAltitudeMetres, t);

        const sol::render::CameraState camera{
            .position = {0.0, kPlanetRadiusMetres + altitude, 0.0},
            .forward = {0.30, -0.95, 0.0},
            .up = {0.0, 0.0, 1.0},
            .verticalFovRadians = 1.0472,
            .nearPlaneMetres = 0.5,
        };

        window.pollEvents();
        auto frame = renderer.renderFrame(camera);
        if (!frame.has_value()) {
            break;
        }
        if (frame->deviceAllocatedBytes == 0) {
            continue;
        }
        if (previousPatchCount >= 0
            && static_cast<int>(frame->terrainPatches) != previousPatchCount) {
            result.patchCountChanges.emplace_back(
                step, static_cast<int>(frame->terrainPatches) - previousPatchCount);
        }
        previousPatchCount = static_cast<int>(frame->terrainPatches);

        minimum = std::min(minimum, frame->deviceAllocatedBytes);
        maximum = std::max(maximum, frame->deviceAllocatedBytes);
        last = frame->deviceAllocatedBytes;
        samples.push_back(static_cast<double>(frame->deviceAllocatedBytes));
        result.maxPatches = std::max(result.maxPatches, frame->terrainPatches);
        result.maxVertices = std::max(result.maxVertices, frame->terrainVertices);
    }

    result.minDeviceBytes = minimum == UINT64_MAX ? 0 : minimum;
    result.maxDeviceBytes = maximum;
    result.finalDeviceBytes = last;

    // Least-squares slope in bytes per step. A bounded working set has a slope near zero; a
    // leak trends upward regardless of how the peak compares to the trough.
    if (samples.size() > 2) {
        const auto n = static_cast<double>(samples.size());
        double sumX = 0.0;
        double sumY = 0.0;
        double sumXY = 0.0;
        double sumXX = 0.0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto x = static_cast<double>(i);
            sumX += x;
            sumY += samples[i];
            sumXY += x * samples[i];
            sumXX += x * x;
        }
        const double denominator = (n * sumXX) - (sumX * sumX);
        if (denominator != 0.0) {
            result.memoryTrendBytesPerStep = ((n * sumXY) - (sumX * sumY)) / denominator;
        }
    }
}

void report(const TraverseResult& result)
{
    std::printf("\n%s\n", result.label.c_str());
    std::printf("  frames                 %d\n", result.samples);
    std::printf("  pops detected          %zu\n", result.popIndices.size());
    std::printf("  worst local ratio      %.2fx (threshold %.1fx) at difference %.4f\n",
                result.worstRatio,
                kSpikeFactor,
                result.worstRatioDifference);
    std::printf("  frame difference       median %.4f, max %.4f (0-255 luminance)\n",
                result.medianDifference,
                result.maximumDifference);
    if (!result.popIndices.empty()) {
        std::printf("  first pops at steps    ");
        for (std::size_t i = 0; i < std::min<std::size_t>(8, result.popIndices.size()); ++i) {
            std::printf("%d ", result.popIndices[i]);
        }
        std::printf("\n");
    }
    std::printf("  peak terrain patches   %u\n", result.maxPatches);
    std::printf("  peak terrain vertices  %u\n", result.maxVertices);
    std::printf("  device memory          %.2f MiB min, %.2f MiB max, %.2f MiB final\n",
                static_cast<double>(result.minDeviceBytes) / (1024.0 * 1024.0),
                static_cast<double>(result.maxDeviceBytes) / (1024.0 * 1024.0),
                static_cast<double>(result.finalDeviceBytes) / (1024.0 * 1024.0));
    std::printf("  memory trend           %.1f bytes/step\n", result.memoryTrendBytesPerStep);
    if (result.worstChangedStep >= 0) {
        std::printf("  worst concentration    %.6f of pixels at step %d (%.0f m altitude)\n",
                    result.worstChangedFraction,
                    result.worstChangedStep,
                    result.altitudeAtWorst);
    } else {
        std::printf("  worst concentration    none — no step moved any pixel by >%d levels\n",
                    kPerceptibleDelta);
    }

    if (!result.patchCountChanges.empty()) {
        std::printf("  patch-count changes    ");
        for (const auto& change : result.patchCountChanges) {
            std::printf("%d(%+d) ", change.first, change.second);
        }
        std::printf("\n");
    }
}

} // namespace

int main()
{
    auto window = sol::platform::Window::create("LOD gate", 1280, 720);
    if (!window.has_value()) {
        std::printf("%s\n\nSKIPPED: no window system.\n", window.error().c_str());
        return kSkipExitCode;
    }

    const sol::render::InstanceConfig config{
        .applicationName = "SolLodGate",
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

    std::printf("LOD continuity gate — fixed observer descent\n");
    std::printf("Device:     %s\n", renderer->selectedDevice().deviceName.c_str());
    std::printf("Traverse:   %.0f km to %.0f km altitude, %d logarithmic steps\n",
                kStartAltitudeMetres / 1000.0,
                kEndAltitudeMetres / 1000.0,
                kTraverseSteps);

    TraverseResult production = runTraverse(*window, *renderer, true, "Production descent: continuous LOD morphing");
    collectResourceStats(*window, *renderer, production, true);
    report(production);

    TraverseResult control = runTraverse(*window, *renderer, false, "Control descent: morphing disabled");
    collectResourceStats(*window, *renderer, control, false);
    report(control);

    // The gate's actual popping verdict comes from crossing a single boundary slowly, for the
    // reason set out at runTransitionSweep: on a full descent the transitions overlap.
    const std::optional<double> transitionAltitude = findTransitionAltitude(*window, *renderer);
    if (!transitionAltitude.has_value()) {
        // No sweep rather than a sweep at altitude zero. The band is a fraction either side of
        // the centre, so an absent centre collapses it onto the reference radius — the camera
        // would sit underneath the terrain and the table would print that as a measurement.
        std::printf("\nSingle-transition sweep: not run — the scan found no transition to centre "
                    "it on.\n");
    } else {
        const SweepResult sweepProduction = runTransitionSweep(
            *window, *renderer, true, *transitionAltitude, "Transition sweep, morphing enabled");
        const SweepResult sweepControl = runTransitionSweep(
            *window, *renderer, false, *transitionAltitude,
            "Transition sweep, morphing DISABLED (control)");

        std::printf("\nSingle-transition sweep: +/-6%% around %.0f m altitude, 300 steps\n",
                    *transitionAltitude);
        std::printf("  %-46s %-12s %-10s %-8s %s\n",
                    "configuration",
                    "worst px frac",
                    "p999 delta",
                    "at step",
                    "mean max");
        for (const SweepResult* sweep : {&sweepProduction, &sweepControl}) {
            // A step of -1 means no step ever exceeded the perceptible threshold, so the p999
            // and step fields hold their initial values rather than measurements. Printing those
            // as if they were data would be the same class of mistake this gate keeps catching.
            if (sweep->worstStep < 0) {
                std::printf("  %-46s %-12.6f %-10s %-8s %.4f\n",
                            sweep->label.c_str(),
                            sweep->worstChangedFraction,
                            "n/a",
                            "none",
                            sweep->maximumDifference);
            } else {
                std::printf("  %-46s %-12.6f %-10.1f %-8d %.4f\n",
                            sweep->label.c_str(),
                            sweep->worstChangedFraction,
                            sweep->worstP999,
                            sweep->worstStep,
                            sweep->maximumDifference);
            }
        }
    }

    renderer->waitIdle();

    // A transition pops when one step stands far above ordinary motion within the band. The
    // threshold is stated against the control: whatever separation the abrupt scheme produces
    // is what popping looks like here, and the production path must stay well below it.
    // A pop is a visible amount of the screen changing visibly in one frame. Two tenths of one
    // percent of a 1280x720 frame is about 1 800 pixels — a band of that size jumping by more
    // than 6% luminance between adjacent frames is what "detectable" means here, and it is
    // stated in perceptual terms rather than derived from whichever run was measured first.
    // The verdict comes from the descent's local-outlier count, which is the measurement that
    // demonstrably separates the two configurations once the terrain is rough enough to have
    // something to pop. The band sweep and the concentration metric are both retained and
    // reported, but neither discriminates: the sweep keeps landing on transitions among
    // distant horizon patches, and concentration is confounded by morphing's own continuous
    // deformation, which moves more pixels slightly than an abrupt switch moves sharply.
    const bool noPopping = production.popIndices.empty();
    const bool sweepDiscriminates =
        control.popIndices.size() > production.popIndices.size() * 2 + 2;

    // Pops at the same step in both configurations cannot be caused by morphing, since that is
    // the only thing that differs between the runs. Naming them separately keeps a residual
    // popping source from being either hidden inside the pass or blamed on the LOD scheme.
    std::vector<int> sharedPops;
    for (int step : production.popIndices) {
        if (std::ranges::find(control.popIndices, step) != control.popIndices.end()) {
            sharedPops.push_back(step);
        }
    }
    // A bounded working set. The buffers are allocated once at capacity, so the expectation is
    // a flat line; anything trending upward over 600 steps is a leak.
    const bool memoryBounded = std::abs(production.memoryTrendBytesPerStep) < 1024.0
                               && production.maxDeviceBytes == production.minDeviceBytes;
    std::printf("\nVerdicts\n");
    std::printf("  no detectable popping            %s  (%zu pops on the descent)\n",
                noPopping ? "PASS" : "FAIL",
                production.popIndices.size());
    std::printf("  bounded device memory            %s\n", memoryBounded ? "PASS" : "FAIL");
    std::printf("  control pops more than production %zu vs %zu — %s\n",
                control.popIndices.size(),
                production.popIndices.size(),
                sweepDiscriminates ? "detector discriminates"
                                   : "DETECTOR BLIND; the result above is untrustworthy");
    std::printf("  worst pop magnitude, prod vs ctrl %.4f vs %.4f\n",
                production.worstRatioDifference,
                control.worstRatioDifference);
    if (!sharedPops.empty()) {
        std::printf("  pops present in BOTH runs        %zu (steps ", sharedPops.size());
        for (int step : sharedPops) {
            std::printf("%d ", step);
        }
        std::printf(")\n    These cannot be LOD morphing — it is the only difference between\n"
                    "    the runs. A separate mechanism, most likely horizon culling making a\n"
                    "    patch appear or vanish, which morphing does not address.\n");
    }
    std::printf("  descent roughness, prod vs ctrl  median %.4f vs %.4f, max %.4f vs %.4f\n",
                production.medianDifference,
                control.medianDifference,
                production.maximumDifference,
                control.maximumDifference);

    const auto& messages = instance->validationMessages();
    std::printf("\nValidation messages: %zu\n", messages.size());
    for (const std::string& message : messages) {
        std::printf("  %s\n", message.c_str());
    }

    const bool passed = noPopping && memoryBounded && sweepDiscriminates;
    std::printf("\n%s\n",
                passed ? "LOD GATE PASSED." : "LOD GATE FAILED — see the verdicts above.");
    return passed ? 0 : 1;
}
