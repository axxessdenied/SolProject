/// One headless timing scenario, proving the measurement pipeline end to end.
///
/// Increment A1 requires "one headless timing scenario [that] emits complete metadata and
/// machine-readable measurements", with peak process memory and allocation counts as
/// mandatory measurements.
///
/// The workload is deliberately uninteresting: a fixed-count double-precision transform
/// loop. A1 is validating the *harness*, not the workload, and a workload with its own
/// numerical questions would confuse a harness failure with a physics failure. Increment A2
/// replaces it with real frame-conversion work.
///
/// Measurement discipline this scenario demonstrates, and that later scenarios inherit:
///   - warm-up iterations run before any sample is recorded;
///   - the timer is QueryPerformanceCounter, whose frequency is queried rather than assumed;
///   - the workload's result is accumulated and reported so the optimizer cannot delete it;
///   - the reported value distribution, not just a mean, is emitted;
///   - Debug timings are recorded but are not evidence. The P1a measurement rules restrict
///     performance claims to Release builds.

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/MetricSeries.h"
#include "Sol/Proto/Harness/ScenarioReport.h"
#include "Sol/Proto/Harness/SolToolchainFacts.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sol::proto::CheckContext;
using sol::proto::JsonWriter;
using sol::proto::MetricSeries;
using sol::proto::ScenarioMetadata;
using sol::proto::ScenarioReport;

constexpr std::uint64_t kSeed = 0x5010'2026'0101'0001ull;
constexpr std::uint64_t kWarmupIterations = 16;
constexpr std::uint64_t kSampleCount = 128;
constexpr std::size_t kElementCount = 1u << 14;

/// Ticks-to-nanoseconds using the queried performance-counter frequency.
class HighResolutionClock {
public:
    HighResolutionClock() noexcept {
        LARGE_INTEGER frequency{};
        ::QueryPerformanceFrequency(&frequency);
        m_ticksPerSecond = static_cast<double>(frequency.QuadPart);
    }

    [[nodiscard]] std::int64_t now() const noexcept {
        LARGE_INTEGER counter{};
        ::QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    [[nodiscard]] double ticksToNanoseconds(std::int64_t ticks) const noexcept {
        return (static_cast<double>(ticks) / m_ticksPerSecond) * 1.0e9;
    }

    [[nodiscard]] double ticksPerSecond() const noexcept { return m_ticksPerSecond; }

private:
    double m_ticksPerSecond{1.0};
};

/// The measured workload: an affine transform plus a reduction over a fixed buffer.
///
/// Returns the reduction so the caller can keep it live; without that the whole loop is
/// dead code and the timing measures nothing.
double transformAndReduce(std::vector<double>& values, double scale, double offset) {
    double total = 0.0;
    for (double& value : values) {
        value = value * scale + offset;
        total += value;
    }
    return total;
}

} // namespace

int main(int argc, char** argv) {
    try {
        CheckContext checks;

        std::vector<double> values(kElementCount);
        for (std::size_t index = 0; index < kElementCount; ++index) {
            values[index] = 1.0 + static_cast<double>(index % 97) * 0.01;
        }

        const HighResolutionClock clock;
        double liveResult = 0.0;

        // Everything the sample loop needs is allocated up front: the series' storage is
        // reserved here so addSample() cannot reallocate mid-measurement, which would put an
        // allocation inside a timed interval and distort the sample that contained it.
        MetricSeries iterationTime("iterationTime", "ns");
        iterationTime.reserve(static_cast<std::size_t>(kSampleCount));

        for (std::uint64_t iteration = 0; iteration < kWarmupIterations; ++iteration) {
            liveResult += transformAndReduce(values, 1.0000001, 1.0e-7);
        }

        // Reset immediately before the timed loop and snapshot immediately after it, so the
        // counts describe the measured region alone rather than scenario or harness setup.
        sol::proto::allocations::reset();

        for (std::uint64_t sample = 0; sample < kSampleCount; ++sample) {
            const std::int64_t start = clock.now();
            liveResult += transformAndReduce(values, 1.0000001, 1.0e-7);
            const std::int64_t end = clock.now();
            iterationTime.addSample(clock.ticksToNanoseconds(end - start));
        }

        const auto allocationCounts = sol::proto::allocations::snapshot();

        MetricSeries perElementTime("perElementTime", "ns");
        perElementTime.reserve(static_cast<std::size_t>(kSampleCount));
        for (const double sample : iterationTime.samples()) {
            perElementTime.addSample(sample / static_cast<double>(kElementCount));
        }

        checks.check(clock.ticksPerSecond() > 0.0,
                     "QueryPerformanceFrequency returned a usable frequency");
        checks.check(iterationTime.samples().size() == kSampleCount,
                     "every requested sample was recorded");
        checks.check(iterationTime.summarize().min > 0.0,
                     "timing resolution is fine enough to measure the workload");
        checks.check(allocationCounts.allocationCount == 0,
                     "the measured region performs no heap allocation");

        const bool isReleaseBuild =
            std::string_view(sol::proto::facts::kBuildType) == std::string_view("Release");

        ScenarioMetadata metadata;
        metadata.name = "timing.transformAndReduce";
        metadata.version = "1";
        metadata.inputDescription =
            "16384 doubles initialised to 1.0 + (index % 97) * 0.01, transformed in place by "
            "value * 1.0000001 + 1e-7 and reduced, once per iteration.";
        metadata.seed = kSeed;
        metadata.warmupIterations = kWarmupIterations;
        metadata.sampleCount = kSampleCount;

        ScenarioReport report(metadata);
        report.addMetric(iterationTime);
        report.addMetric(perElementTime);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("workload");
            writer.write("elementCount", static_cast<std::uint64_t>(kElementCount));
            // Reported so the optimizer must keep the workload, and so a changed result is
            // visible rather than silent.
            writer.writeBits("liveResultBits", liveResult);
            writer.endObject();

            writer.beginObject("measuredRegionAllocations");
            writer.write("allocationCount", allocationCounts.allocationCount);
            writer.write("deallocationCount", allocationCounts.deallocationCount);
            writer.write("totalAllocatedBytes", allocationCounts.totalAllocatedBytes);
            writer.endObject();

            writer.beginObject("evidenceEligibility");
            writer.write("isReleaseBuild", isReleaseBuild);
            writer.write("note",
                         isReleaseBuild
                             ? "Release build: timings are eligible as P1a performance evidence."
                             : "Debug build: timings are recorded for comparison only and are "
                               "not P1a performance evidence.");
            writer.endObject();

            writer.beginObject("checks");
            writer.write("passed", static_cast<std::uint64_t>(checks.passedCount()));
            writer.write("failed", static_cast<std::uint64_t>(checks.failedCount()));
            writer.endObject();
        });

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "timing-scenario.json");

        // Raw sample arrays belong with the raw evidence, which is gitignored.
        report.setIncludeRawSamples(true);
        report.writeToFile(outputPath);
        std::cout << "TimingScenario: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("TimingScenario");
    } catch (const std::exception& error) {
        std::cerr << "TimingScenario: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
