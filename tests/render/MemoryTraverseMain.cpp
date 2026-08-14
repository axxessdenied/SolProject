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

#include "Sol/Platform/Window.h"
#include "Sol/Render/Renderer.h"
#include "Sol/Render/ShaderBuild.h"
#include "Sol/Render/VulkanInstance.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/// Least-squares slope of private bytes against time, in bytes per minute.
double trendBytesPerMinute(const std::vector<Sample>& samples, double fromSeconds)
{
    double n = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;
    for (const Sample& sample : samples) {
        if (sample.seconds < fromSeconds) {
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
    double durationSeconds = 30.0 * 60.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--minutes") == 0 && i + 1 < argc) {
            durationSeconds = std::atof(argv[++i]) * 60.0;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            durationSeconds = std::atof(argv[++i]);
        }
    }

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
        });
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
    std::printf("Device:     %s\n", renderer->selectedDevice().deviceName.c_str());
    std::printf("Shaders:    %s\n",
                std::string(sol::render::shaderBuildDescription()).c_str());
    std::printf("Duration:   %.1f minutes, sampled every %.0f s, first %.0f s excluded from the "
                "trend\n",
                durationSeconds / 60.0,
                kSampleIntervalSeconds,
                kWarmUpSeconds);
    std::printf("Path:       %.0f km to %.0f km altitude every %.0f s, orbiting at %.4f rad/s\n\n",
                kLowAltitudeMetres / 1000.0,
                kHighAltitudeMetres / 1000.0,
                kSweepPeriodSeconds,
                kAngularRateRadiansPerSecond);

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

    std::printf("\nMeasured result\n");
    std::printf("  private-bytes trend after warm-up: %.1f KiB/minute over %.1f minutes\n",
                slope / 1024.0,
                (samples.back().seconds - warmUpEnd) / 60.0);
    std::printf("  total growth after warm-up:        %.2f MiB\n", toMiB(growthAfterWarmUp));
    std::printf("\n  This program reports; it does not rule. \"No unbounded memory growth\" names\n"
                "  no statistic and no limit, exactly as the popping half named none before its\n"
                "  method was defined and ratified. Whether these figures satisfy the P1b clause\n"
                "  is a user decision, and turning them into a pass or fail needs the same kind\n"
                "  of documented planning update.\n");
    return 0;
}
