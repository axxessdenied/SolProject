/// @file
/// The LOD threshold's memory half: no unbounded growth over a 30-minute traverse.
///
/// **Why this is a separate program from the LOD gate.** The gate's memory reading was withdrawn
/// on 2026-08-13 on two independent grounds, and only one of them was about duration. The other
/// was that the figure it read *could not vary*: every device allocation happens once during
/// creation and terrain capacity is fixed, so `max == min` with a zero trend is arithmetic rather
/// than an observation. Running that same reading for thirty minutes instead of thirty seconds
/// would have produced thirty minutes of the same tautology.
///
/// So this measures things that can move:
///
///   - **Process private bytes** (`PagefileUsage`), the commit charge. A host-side leak — a vector
///     that grows per frame, a handle never released — appears here and nowhere else, and VMA
///     cannot see it because VMA does not own it. This is the primary instrument.
///   - **Working set**, reported alongside but not gated on. The OS trims it under pressure, so
///     it falls as well as rises for reasons that have nothing to do with this program.
///   - **Allocator block bytes and live object counts**, which VMA does own. A device-side leak
///     that fits inside existing blocks moves the allocation total; one that exhausts them moves
///     the block total; either moves the counts. Integer counts drift visibly long before byte
///     totals do.
///
/// **The traverse is driven by wall clock, not by step count**, because the criterion is stated in
/// minutes. The camera continuously sweeps altitude between 3 km and 300 km while orbiting, so
/// terrain is reselected and rebuilt every frame against a constantly changing view — which is
/// the condition under which a per-rebuild leak would accumulate. A traverse that revisited the
/// same view would let a leak hide behind a steady state.
///
/// **Warm-up is excluded from the trend and reported separately.** A process's commit charge rises
/// during its first seconds for reasons that are not leaks: driver lazy initialisation, first
/// touch of the swapchain, allocator block growth to its steady size. Fitting a line through that
/// would report a leak that stops.
///
/// **The threshold this grades against was ratified by the user on 2026-08-14**, and the constants
/// block below carries its full derivation. P1b requires a gate to be given its number by a
/// documented planning update the user approves; that document is the LOD memory measurement
/// method in `SolProjectNotes/Milestones/P1b-Renderer-and-Craft.md`.
///
/// **A graded run that breaches the threshold fails.** A run shorter than the graded duration
/// reports every figure and exits 0 regardless, because a 45-second sample cannot certify a
/// clause stated in thirty minutes — and, as the constants below record, cannot even see the leak
/// the sensitivity claim rests on. Short runs keep the instrument under test; only a 30-minute run
/// rules on the clause.
///
/// **`--leak N` is the negative control.** It leaks N bytes per frame deliberately and inverts the
/// verdict: a control that PASSES the threshold is a failure, reported as such and exited
/// non-zero, because a limit a real leak passes certifies nothing. The suite runs a gross 4 KiB
/// per frame at 45 s to verify continuously that the gate can fail; the 8-byte-per-frame run that
/// establishes how *small* a leak it catches needs the full 30 minutes and is performed
/// deliberately. Leak size and duration trade against each other, which is the whole reason the
/// clause's 30 minutes matters — see the constants below.

#include "Sol/Platform/Window.h"
#include "Sol/Render/Renderer.h"
#include "Sol/Render/ShaderBuild.h"
#include "Sol/Render/VulkanInstance.h"
#include "Support/DeviceOption.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kSkipExitCode = 77;

constexpr double kPlanetRadiusMetres = 6378136.6;
constexpr double kLowAltitudeMetres = 3000.0;
constexpr double kHighAltitudeMetres = 300000.0;

/// One full altitude sweep, low to high and back. Short enough that a 30-minute run performs
/// many of them, so a leak that is per-sweep rather than per-frame still accumulates visibly.
constexpr double kSweepPeriodSeconds = 90.0;

/// How far the camera travels around the planet per second, radians. Slow enough that
/// consecutive frames are similar, fast enough that a full run visits fresh terrain rather than
/// orbiting the same patches.
constexpr double kAngularRateRadiansPerSecond = 0.004;

/// Ignored when fitting the trend, and reported on its own. See the file comment.
constexpr double kWarmUpSeconds = 120.0;

/// Sampling interval. Frequent enough for a meaningful fit over the shortest useful run.
constexpr double kSampleIntervalSeconds = 5.0;

// ---------------------------------------------------------------------------------------------
// The threshold.
//
// **Ratified by the user on 2026-08-14**, under P1b's rule that a gate may be given a number only
// by a documented planning update the user approves — the same route the popping half took. The
// definition and its derivation are recorded in the milestone plan; this block is the derivation
// in the place it is enforced.
//
// "No unbounded memory growth" cannot be proven over any finite window — thirty minutes of flat
// line does not establish an asymptote. What a finite run *can* do is bound the rate where the
// curve has settled, and show the rate is not increasing. Two gated conditions:
//
//   1. The fitted trend over the **second half** of the post-warm-up window stays under the
//      limit. This is the primary statistic, and deliberately not the whole-window trend: see
//      below.
//   2. Total growth after warm-up stays under a cap. Guards a large single step that a slope fit
//      would average away, and bounds the run even if the rate itself is small.
//
// The whole-window trend is **reported and not gated**, because measurement showed it is
// contaminated. On clean 30-minute runs the first half fits well above the second — the process is
// still settling past the 120 s warm-up cut — and the headline figure varied 17.9, 40.7, 56.8 and
// 25.7 KiB/minute across four runs of the same build for that reason, against a second-half
// statistic that sat at 12.2, 2.1 and 15.9 over the same runs. On one of them the whole-window fit
// exceeded *both* halves it spans, 56.8 against 15.3 and 2.1, which is what a discrete settling
// step does to a line fitted across it rather than an arithmetic error. Gating
// on a statistic with that spread would fail clean runs. The second half is both the steadier
// measurement and the one that speaks to where the curve is heading, which is what "unbounded"
// asks about. The warm-up cut stays at 120 s rather than being extended to make the whole-window
// figure behave: moving a cut until a number looks better is a threshold change wearing a
// tuning detail's clothes.
//
// **Where the limit comes from — both bounds, because either alone lands somewhere arbitrary.**
//
// *Sensitivity (the binding one).* The limit must be low enough that a real leak fails, and this
// is demonstrated rather than derived: a deliberate 8-byte-per-frame leak — the smallest thing
// worth calling a leak, one small object per frame — fits 271.7 KiB/minute on the second half
// over a graded run, 4.2x above the limit. Note that the same control at a 3-minute duration
// fitted -60.2 KiB/minute, pure noise, which is why the clause's 30 minutes is not negotiable and
// why a short run reports without grading.
//
// *Session tolerance (the loose one).* 64 KiB/minute extrapolates to about 23 MiB over a
// six-hour session, negligible against the 16 GB baseline machine. Anchoring on this alone would
// have allowed roughly 360 KiB/minute, which the demonstrated leak would pass — so the tighter
// bound governs and this one is recorded as the sanity check it is.
//
// The limit therefore sits inside a measured separation band: 15.9 KiB/min on the worst clean run
// against 271.7 KiB/min leaked, with the limit 4.0x above the former and 4.2x below the latter. It
// is not drawn around the number that happened to be observed.
constexpr double kMaxTrendKiBPerMinute = 64.0;
constexpr double kMaxGrowthAfterWarmUpMiB = 2.0;

/// The duration the milestone clause names. A shorter run still reports every figure, but is not
/// graded: the suite's 45-second registration exists to keep the instrument working, not to
/// certify the clause.
constexpr double kGradedDurationSeconds = 30.0 * 60.0;

/// The negative control's default. At this run's frame rate a 64-byte-per-frame leak is roughly
/// 541 KiB/minute, comfortably over the limit — so if the control does not fail, the gate cannot
/// fail, and this program reports that as an error rather than as a pass.
constexpr std::size_t kDefaultLeakBytesPerFrame = 64;

struct Sample {
    double seconds = 0.0;
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSetBytes = 0;
    std::uint64_t deviceAllocatedBytes = 0;
    std::uint64_t deviceBlockBytes = 0;
    std::uint32_t deviceAllocationCount = 0;
    std::uint32_t deviceBlockCount = 0;
    std::uint32_t terrainPatches = 0;
};

struct ProcessMemory {
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSetBytes = 0;
    bool valid = false;
};

/// Current, not peak. P1a's harness reads the peak counters, which cannot fall and therefore
/// cannot show a trend; a leak test needs the live values.
ProcessMemory readProcessMemory()
{
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return ProcessMemory{};
    }
    return ProcessMemory{
        .privateBytes = static_cast<std::uint64_t>(counters.PagefileUsage),
        .workingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize),
        .valid = true,
    };
}

double toMiB(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

/// Least-squares slope of private bytes against time over [fromSeconds, toSeconds), in bytes per
/// minute. The window is a half-open range so the two halves of a split cannot share a sample.
double trendBytesPerMinute(const std::vector<Sample>& samples,
                           double fromSeconds,
                           double toSeconds = std::numeric_limits<double>::infinity())
{
    double n = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;
    for (const Sample& sample : samples) {
        if (sample.seconds < fromSeconds || sample.seconds >= toSeconds) {
            continue;
        }
        const double x = sample.seconds / 60.0;
        const double y = static_cast<double>(sample.privateBytes);
        n += 1.0;
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }
    if (n < 3.0) {
        return 0.0;
    }
    const double denominator = (n * sumXX) - (sumX * sumX);
    if (denominator == 0.0) {
        return 0.0;
    }
    return ((n * sumXY) - (sumX * sumY)) / denominator;
}

} // namespace

int main(int argc, char** argv)
{
    const auto selection = sol::testing::parseDeviceOption(argc, argv);

    double durationSeconds = kGradedDurationSeconds;
    std::size_t leakBytesPerFrame = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--minutes") == 0 && i + 1 < argc) {
            durationSeconds = std::atof(argv[++i]) * 60.0;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            durationSeconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--leak") == 0) {
            leakBytesPerFrame = (i + 1 < argc && argv[i + 1][0] != '-')
                                    ? static_cast<std::size_t>(std::atoll(argv[++i]))
                                    : kDefaultLeakBytesPerFrame;
        }
    }

    if (durationSeconds <= 0.0) {
        std::printf("FAILED: a run of %.1f seconds measures nothing.\n", durationSeconds);
        return 1;
    }

    // The negative control. A host-side leak of the shape the file comment names — a container
    // that grows every frame — held rather than orphaned, so the run ends without an actual leak
    // while still committing the pages. Touched, because untouched pages need not be committed
    // and an instrument reading commit charge would not see them.
    const bool leakControl = leakBytesPerFrame > 0;
    std::vector<std::unique_ptr<char[]>> leaked;

    auto window = sol::platform::Window::create("Memory traverse", 1280, 720);
    if (!window.has_value()) {
        std::printf("%s\n\nSKIPPED: no window system.\n", window.error().c_str());
        return kSkipExitCode;
    }

    const sol::render::InstanceConfig config{
        .applicationName = "SolMemoryTraverse",
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

    if (!readProcessMemory().valid) {
        std::printf("FAILED: the process memory counters could not be read, so this run would "
                    "report zeros as if they were measurements.\n");
        return 1;
    }

    sol::render::TerrainSettings terrain;
    terrain.centre = {0.0, 0.0, 0.0};
    terrain.radiusMetres = kPlanetRadiusMetres;
    terrain.reliefMetres = 20000.0;
    terrain.maxLevel = 10;
    renderer->setScene({});
    renderer->setTerrain(terrain);

    std::printf("LOD memory traverse — unbounded-growth check\n");
    std::printf("%s\n", sol::testing::describeDeviceRequest(selection).c_str());
    std::printf("Device:     %s\n", renderer->selectedDevice().deviceName.c_str());
    std::printf("Shaders:    %s\n",
                std::string(sol::render::shaderBuildDescription()).c_str());
    std::printf("Duration:   %.1f minutes, sampled every %.0f s, first %.0f s excluded from the "
                "trend\n",
                durationSeconds / 60.0,
                kSampleIntervalSeconds,
                kWarmUpSeconds);
    std::printf("Path:       %.0f km to %.0f km altitude every %.0f s, orbiting at %.4f rad/s\n",
                kLowAltitudeMetres / 1000.0,
                kHighAltitudeMetres / 1000.0,
                kSweepPeriodSeconds,
                kAngularRateRadiansPerSecond);
    std::printf("Threshold:  second-half trend <= %.0f KiB/min, growth <= %.1f MiB  (ratified "
                "2026-08-14)\n",
                kMaxTrendKiBPerMinute,
                kMaxGrowthAfterWarmUpMiB);
    if (leakControl) {
        std::printf("Mode:       NEGATIVE CONTROL — leaking %zu bytes/frame deliberately. This "
                    "run MUST fail the threshold.\n",
                    leakBytesPerFrame);
    }
    std::printf("\n");

    const auto started = std::chrono::steady_clock::now();
    std::vector<Sample> samples;
    double nextSampleAt = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t skipped = 0;

    for (;;) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (elapsed >= durationSeconds) {
            break;
        }

        // Altitude sweeps low-high-low in log space, so time is spent evenly across LOD levels
        // rather than bunched at the top where a level spans a wide altitude band.
        const double phase = 0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846
                                                   * (elapsed / kSweepPeriodSeconds)));
        const double altitude =
            kLowAltitudeMetres * std::pow(kHighAltitudeMetres / kLowAltitudeMetres, phase);

        const double theta = elapsed * kAngularRateRadiansPerSecond;
        const double radius = kPlanetRadiusMetres + altitude;
        const double cosTheta = std::cos(theta);
        const double sinTheta = std::sin(theta);

        // Down and slightly along track. `up` is +Z and the camera stays in the XY plane, so the
        // basis is never degenerate however far the traverse runs.
        const sol::render::CameraState camera{
            .position = {radius * cosTheta, radius * sinTheta, 0.0},
            .forward = {(-0.95 * cosTheta) - (0.30 * sinTheta),
                        (-0.95 * sinTheta) + (0.30 * cosTheta),
                        0.0},
            .up = {0.0, 0.0, 1.0},
            .verticalFovRadians = 1.0472,
            .nearPlaneMetres = 0.5,
        };

        window->pollEvents();
        const auto frame = renderer->renderFrame(camera);
        if (!frame.has_value()) {
            std::printf("\nFrame failed at %.1f s: %s\n", elapsed, frame.error().c_str());
            renderer->waitIdle();
            return 1;
        }
        if (!frame->presented) {
            ++skipped;
            continue;
        }
        ++frames;

        if (leakControl) {
            auto block = std::make_unique<char[]>(leakBytesPerFrame);
            std::memset(block.get(), 1, leakBytesPerFrame);
            leaked.push_back(std::move(block));
        }

        if (elapsed >= nextSampleAt) {
            nextSampleAt += kSampleIntervalSeconds;
            const ProcessMemory memory = readProcessMemory();
            samples.push_back(Sample{
                .seconds = elapsed,
                .privateBytes = memory.privateBytes,
                .workingSetBytes = memory.workingSetBytes,
                .deviceAllocatedBytes = frame->deviceAllocatedBytes,
                .deviceBlockBytes = frame->deviceBlockBytes,
                .deviceAllocationCount = frame->deviceAllocationCount,
                .deviceBlockCount = frame->deviceBlockCount,
                .terrainPatches = frame->terrainPatches,
            });
        }
    }

    renderer->waitIdle();

    // The run's own length, not the last sample's timestamp. Sampling every 5 s leaves the final
    // sample up to one interval short of the target, so grading on `samples.back().seconds` made
    // a 30-minute run report 29.9 and fail a `>= 30` check that it had in fact satisfied — a gate
    // that could never certify the clause it exists for.
    const double runSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if (samples.size() < 3) {
        std::printf("FAILED: %zu samples is too few to say anything about a trend.\n",
                    samples.size());
        return 1;
    }

    const auto privateMinMax = std::ranges::minmax_element(
        samples, {}, [](const Sample& s) { return s.privateBytes; });
    const auto patchMinMax = std::ranges::minmax_element(
        samples, {}, [](const Sample& s) { return s.terrainPatches; });

    const double warmUpEnd = std::min(kWarmUpSeconds, samples.back().seconds * 0.5);
    const double slope = trendBytesPerMinute(samples, warmUpEnd);

    // The shape test. Splitting the post-warm-up window in half and fitting each separately is
    // what distinguishes a curve that is flattening from a line that is not — the thing the first
    // run of this program could not do, and the reason it produced a number nobody could grade.
    const double windowMidpoint = warmUpEnd + ((samples.back().seconds - warmUpEnd) * 0.5);
    const double firstHalfSlope = trendBytesPerMinute(samples, warmUpEnd, windowMidpoint);
    const double secondHalfSlope = trendBytesPerMinute(samples, windowMidpoint);

    const Sample* firstAfterWarmUp = nullptr;
    for (const Sample& sample : samples) {
        if (sample.seconds >= warmUpEnd) {
            firstAfterWarmUp = &sample;
            break;
        }
    }
    const std::uint64_t growthAfterWarmUp =
        (firstAfterWarmUp != nullptr && samples.back().privateBytes > firstAfterWarmUp->privateBytes)
            ? samples.back().privateBytes - firstAfterWarmUp->privateBytes
            : 0;

    std::printf("Run\n");
    std::printf("  frames presented       %llu over %.1f minutes (%llu skipped)\n",
                static_cast<unsigned long long>(frames),
                samples.back().seconds / 60.0,
                static_cast<unsigned long long>(skipped));
    std::printf("  samples                %zu\n", samples.size());
    std::printf("  terrain patches        %u min, %u max — terrain was reselected throughout\n",
                patchMinMax.min->terrainPatches,
                patchMinMax.max->terrainPatches);

    std::printf("\nProcess private bytes (commit charge) — the instrument that can see a "
                "host-side leak\n");
    std::printf("  at start               %.2f MiB\n", toMiB(samples.front().privateBytes));
    std::printf("  at end                 %.2f MiB\n", toMiB(samples.back().privateBytes));
    std::printf("  min / max              %.2f / %.2f MiB\n",
                toMiB(privateMinMax.min->privateBytes),
                toMiB(privateMinMax.max->privateBytes));
    std::printf("  growth after warm-up   %.2f MiB (from %.0f s onward)\n",
                toMiB(growthAfterWarmUp),
                warmUpEnd);
    std::printf("  trend after warm-up    %.1f KiB/minute\n", slope / 1024.0);
    std::printf("  trend, first half      %.1f KiB/minute (%.0f–%.0f s)\n",
                firstHalfSlope / 1024.0,
                warmUpEnd,
                windowMidpoint);
    std::printf("  trend, second half     %.1f KiB/minute (%.0f s onward)\n",
                secondHalfSlope / 1024.0,
                windowMidpoint);
    if (std::abs(firstHalfSlope) > 1.0) {
        const double ratio = secondHalfSlope / firstHalfSlope;
        std::printf("  shape                  second half is %.2fx the first — %s\n",
                    ratio,
                    ratio < 0.5    ? "flattening"
                        : ratio < 1.5 ? "close to linear"
                                      : "ACCELERATING");
    }
    std::printf("  working set at end     %.2f MiB (reported, not judged — the OS trims it)\n",
                toMiB(samples.back().workingSetBytes));

    std::printf("\nDevice allocator — VMA's own view\n");
    std::printf("  allocated bytes        %.2f MiB throughout\n",
                toMiB(samples.back().deviceAllocatedBytes));
    std::printf("  block bytes            %.2f MiB throughout\n",
                toMiB(samples.back().deviceBlockBytes));
    std::printf("  live allocations       %u\n", samples.back().deviceAllocationCount);
    std::printf("  live blocks            %u\n", samples.back().deviceBlockCount);

    bool deviceSideConstant = true;
    for (const Sample& sample : samples) {
        if (sample.deviceAllocatedBytes != samples.front().deviceAllocatedBytes
            || sample.deviceBlockBytes != samples.front().deviceBlockBytes
            || sample.deviceAllocationCount != samples.front().deviceAllocationCount
            || sample.deviceBlockCount != samples.front().deviceBlockCount) {
            deviceSideConstant = false;
            break;
        }
    }
    std::printf("  varied during the run  %s\n", deviceSideConstant ? "no" : "YES");
    if (deviceSideConstant) {
        std::printf("    Expected, and weak evidence on its own: every device allocation happens\n"
                    "    once at creation and capacity is fixed, so these four are constant by\n"
                    "    construction. They are here to falsify that, not to confirm it. The\n"
                    "    load-bearing measurement is the private-bytes trend above.\n");
    }

    const auto& messages = instance->validationMessages();
    std::printf("\nValidation messages: %zu\n", messages.size());
    for (const std::string& message : messages) {
        std::printf("  %s\n", message.c_str());
    }

    // The series itself, so the shape is recoverable from the record rather than only the two
    // fitted slopes above. Every sample, because a reader checking whether the curve flattens
    // needs the points and not a summary of them.
    std::printf("\nSample series (s, private MiB, working set MiB, patches)\n");
    for (const Sample& sample : samples) {
        std::printf("  %7.1f  %9.2f  %9.2f  %5u%s\n",
                    sample.seconds,
                    toMiB(sample.privateBytes),
                    toMiB(sample.workingSetBytes),
                    sample.terrainPatches,
                    sample.seconds < warmUpEnd ? "  (warm-up)" : "");
    }

    const double trendKiB = slope / 1024.0;
    const double secondHalfKiB = secondHalfSlope / 1024.0;
    const double growthMiB = toMiB(growthAfterWarmUp);

    const bool secondHalfHolds = secondHalfKiB <= kMaxTrendKiBPerMinute;
    const bool growthHolds = growthMiB <= kMaxGrowthAfterWarmUpMiB;
    const bool graded = runSeconds >= kGradedDurationSeconds;
    const bool holds = secondHalfHolds && growthHolds;

    std::printf("\nAgainst the ratified threshold\n");
    std::printf("  trend over second half %8.1f <= %.0f KiB/min   %s\n",
                secondHalfKiB,
                kMaxTrendKiBPerMinute,
                secondHalfHolds ? "PASS" : "FAIL");
    std::printf("  growth after warm-up   %8.2f <= %.1f MiB       %s\n",
                growthMiB,
                kMaxGrowthAfterWarmUpMiB,
                growthHolds ? "PASS" : "FAIL");
    std::printf("  trend over window      %8.1f                   reported, NOT gated — the "
                "first half is still settling\n",
                trendKiB);
    std::printf("  graded duration        %8.1f >= %.0f minutes    %s\n",
                runSeconds / 60.0,
                kGradedDurationSeconds / 60.0,
                graded ? "yes" : "NO — reported only");

    if (leakControl) {
        // A control that passes is the failure. If a deliberate leak of this size does not move
        // the verdict, the threshold cannot detect a real one and every pass it has awarded is
        // worthless — which is the same argument that kept render.lod-gate disabled.
        std::printf("\nNEGATIVE CONTROL: %s\n",
                    holds ? "*** BROKEN — the deliberate leak PASSED ***"
                          : "the deliberate leak FAILED the threshold, as required");
        if (holds) {
            std::printf("  Leaking %zu bytes/frame did not breach a %.0f KiB/min limit. The gate\n"
                        "  cannot fail, so it cannot certify anything either.\n",
                        leakBytesPerFrame,
                        kMaxTrendKiBPerMinute);
            return 1;
        }
        std::printf("  The threshold responds to a leak of %zu bytes/frame, so a pass on the\n"
                    "  production path is a measurement rather than an instrument that is deaf.\n",
                    leakBytesPerFrame);
        return 0;
    }

    if (!graded) {
        std::printf("\n  %s, but NOT GRADED — this run keeps the instrument under test; the\n"
                    "  milestone clause is ruled on only by a %.0f-minute run, which is also the\n"
                    "  shortest duration at which the sensitivity control can be seen at all.\n",
                    holds ? "Holds" : "Does not hold",
                    kGradedDurationSeconds / 60.0);
        return 0;
    }

    std::printf("\n  %s\n",
                holds ? "PASS. No unbounded memory growth over the 30-minute traverse, under the "
                        "method ratified 2026-08-14."
                      : "FAIL. The 30-minute traverse breaches the ratified memory threshold.");
    std::printf("\n  What a pass here does and does not mean: it bounds the settled growth rate\n"
                "  and shows the rate is not increasing. It does NOT prove an asymptote — no\n"
                "  finite window can — and it cannot see growth below this instrument's noise\n"
                "  floor at %.0f minutes. Quote it with those limits attached.\n",
                kGradedDurationSeconds / 60.0);
    return holds ? 0 : 1;
}
