/// Conversion cost of the two candidate frame models.
///
/// A2's done criteria require that "the selected model's conversion cost is recorded so P1b can
/// budget against it". P1b will convert every visible object's state into a camera-relative
/// frame once per frame, so the number that matters is nanoseconds per conversion, broken down
/// by which frame pair is being crossed.
///
/// Measurement discipline follows A1's, which is the point of A1 having existed:
///   - warm-up runs before any sample is recorded;
///   - the timer is QueryPerformanceCounter with a queried frequency;
///   - results are accumulated into a live value so the optimiser cannot delete the work;
///   - distributions are reported, never a single number;
///   - Debug numbers are recorded and explicitly marked as not evidence.
///
/// The two models are concrete types and the timing loop is a template, so neither pays for
/// virtual dispatch. A virtual call is a few nanoseconds and a conversion here is tens, so
/// dispatch would have been a double-digit percentage of the measurement -- the P1a rules
/// forbid instrumentation that materially distorts a result, and that would have.
///
/// Cost is also reported per transform application, because the two models perform different
/// numbers of applications for the same conversion. Comparing calls alone would credit the flat
/// model for work it does not do and penalise it for work it does.

#include "Sol/Proto/Frames/FlatFrameModel.h"
#include "Sol/Proto/Frames/FrameGraph.h"
#include "Sol/Proto/Frames/HierarchicalFrameModel.h"
#include "Sol/Proto/Frames/ReferenceData.h"

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/MetricSeries.h"
#include "Sol/Proto/Harness/ScenarioReport.h"
#include "Sol/Proto/Harness/SolToolchainFacts.h"

#include <windows.h>

#include <array>
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
using sol::proto::MetricSummary;
using sol::proto::ScenarioMetadata;
using sol::proto::ScenarioReport;
using namespace sol::proto::frames;

constexpr std::uint64_t kSeed = 0x5010'2026'0101'0002ull;
constexpr std::uint64_t kWarmupIterations = 8;
constexpr std::uint64_t kSampleCount = 64;
constexpr std::size_t kConversionsPerSample = 4096;

/// Rebuilds are batched for the same reason conversions are: a single rebuild is faster than
/// the performance counter's tick. Timing one and reporting the result gives 0.0 ns, which is
/// not a fast measurement but an absent one, and it is exactly the kind of number A1's evidence
/// warned against quoting.
constexpr std::size_t kRebuildsPerSample = 512;

class HighResolutionClock {
public:
    HighResolutionClock() noexcept
    {
        LARGE_INTEGER frequency{};
        ::QueryPerformanceFrequency(&frequency);
        m_ticksPerSecond = static_cast<double>(frequency.QuadPart);
    }

    [[nodiscard]] std::int64_t now() const noexcept
    {
        LARGE_INTEGER counter{};
        ::QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    [[nodiscard]] double ticksToNanoseconds(std::int64_t ticks) const noexcept
    {
        return (static_cast<double>(ticks) / m_ticksPerSecond) * 1.0e9;
    }

    [[nodiscard]] double ticksPerSecond() const noexcept { return m_ticksPerSecond; }

private:
    double m_ticksPerSecond{1.0};
};

/// The frame pairs worth costing, chosen for what they will actually be asked to do.
struct ConversionCase {
    FrameId from;
    FrameId to;
    const char* label;
    const char* whyItMatters;
};

constexpr std::array<ConversionCase, 4> kCases{{
    {FrameId::VehicleLocal, FrameId::LaunchSiteEnu, "adjacent",
     "the commonest conversion in a local scene: one hop between neighbouring frames"},
    {FrameId::VehicleLocal, FrameId::EarthIcrf, "surfaceToGeocentric",
     "what an orbital map or a trajectory query asks for"},
    {FrameId::VehicleLocal, FrameId::SsbIcrf, "surfaceToRoot",
     "the deepest chain: everything the frame graph has"},
    {FrameId::EarthIcrf, FrameId::MoonIcrf, "acrossBranches",
     "a sibling conversion that must climb to a common ancestor and back down"},
}};

/// Times one model on one conversion case.
///
/// The input states vary so that the loop cannot be hoisted and so that the measurement is not
/// of one cached value's conversion. The accumulator is returned for the same reason A1's
/// timing scenario returns its reduction: without a live result the loop is dead code.
template <typename Model>
double timeConversions(const Model& model, const std::vector<StateVector>& inputs, FrameId target,
                       MetricSeries& series, const HighResolutionClock& clock)
{
    double live = 0.0;

    for (std::uint64_t warmup = 0; warmup < kWarmupIterations; ++warmup) {
        for (const StateVector& input : inputs) {
            live += model.convert(input, target).position.metres().x;
        }
    }

    for (std::uint64_t sample = 0; sample < kSampleCount; ++sample) {
        const std::int64_t start = clock.now();
        for (const StateVector& input : inputs) {
            live += model.convert(input, target).position.metres().x;
        }
        const std::int64_t end = clock.now();
        series.addSample(clock.ticksToNanoseconds(end - start)
                         / static_cast<double>(inputs.size()));
    }

    return live;
}

void writeCase(JsonWriter& writer, const ConversionCase& conversionCase,
               const MetricSummary& flat, int flatApplications, const MetricSummary& hierarchical,
               int hierarchicalApplications)
{
    writer.beginObject();
    writer.write("case", conversionCase.label);
    writer.write("from", frameName(conversionCase.from));
    writer.write("to", frameName(conversionCase.to));
    writer.write("whyItMatters", conversionCase.whyItMatters);

    writer.beginObject("flat");
    writer.write("transformApplications", static_cast<std::int64_t>(flatApplications));
    writer.write("nanosecondsPerConversionMedian", flat.median);
    writer.write("nanosecondsPerConversionP95", flat.p95);
    writer.write("nanosecondsPerConversionMin", flat.min);
    writer.write("nanosecondsPerApplicationMedian",
                 flatApplications > 0 ? flat.median / static_cast<double>(flatApplications) : 0.0);
    writer.endObject();

    writer.beginObject("hierarchical");
    writer.write("transformApplications", static_cast<std::int64_t>(hierarchicalApplications));
    writer.write("nanosecondsPerConversionMedian", hierarchical.median);
    writer.write("nanosecondsPerConversionP95", hierarchical.p95);
    writer.write("nanosecondsPerConversionMin", hierarchical.min);
    writer.write("nanosecondsPerApplicationMedian",
                 hierarchicalApplications > 0
                     ? hierarchical.median / static_cast<double>(hierarchicalApplications)
                     : 0.0);
    writer.endObject();

    writer.write("hierarchicalRelativeToFlat", hierarchical.median / flat.median);
    writer.endObject();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        CheckContext checks;

        const std::filesystem::path fixtureRoot = parseFixtureRoot(argc, argv);
        const ReferenceData data = ReferenceData::loadFromDirectory(fixtureRoot);
        const TdbEpoch epoch = data.fixtureEpoch();

        VehicleState vehicle;
        vehicle.positionInEnu = Vec3{4.0e5, 1.0e4, 8.0e4};
        vehicle.velocityInEnu = Vec3{3200.0, 40.0, 700.0};
        const FrameGraphSnapshot snapshot = buildSnapshot(data, epoch, vehicle);

        const FlatFrameModel flat{snapshot};
        const HierarchicalFrameModel hierarchical{snapshot};

        const HighResolutionClock clock;
        double live = 0.0;

        // Rebuild cost is measured separately from conversion cost. It is paid once per
        // timestep rather than once per object, so folding it into the per-conversion number
        // would misprice both.
        MetricSeries flatRebuild("rebuild.flat", "ns");
        MetricSeries hierarchicalRebuild("rebuild.hierarchical", "ns");
        flatRebuild.reserve(static_cast<std::size_t>(kSampleCount));
        hierarchicalRebuild.reserve(static_cast<std::size_t>(kSampleCount));

        std::vector<MetricSeries> flatSeries;
        std::vector<MetricSeries> hierarchicalSeries;
        flatSeries.reserve(kCases.size());
        hierarchicalSeries.reserve(kCases.size());
        for (const ConversionCase& conversionCase : kCases) {
            flatSeries.emplace_back(std::string{"flat."} + conversionCase.label, "ns");
            hierarchicalSeries.emplace_back(std::string{"hierarchical."} + conversionCase.label,
                                            "ns");
            flatSeries.back().reserve(static_cast<std::size_t>(kSampleCount));
            hierarchicalSeries.back().reserve(static_cast<std::size_t>(kSampleCount));
        }

        // Input states are built once, outside every measured region.
        std::array<std::vector<StateVector>, kCases.size()> inputs;
        for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
            inputs[caseIndex].reserve(kConversionsPerSample);
            for (std::size_t i = 0; i < kConversionsPerSample; ++i) {
                const double offset = static_cast<double>(i % 251) * 0.37;
                StateVector state;
                state.position = PositionMetres::fromMetres(
                    Vec3{1.0e5 + offset, -2.0e5 - offset, 3.0e5 + offset * 2.0});
                state.velocity = VelocityMetresPerSecond::fromMetresPerSecond(
                    Vec3{100.0 + offset, -200.0, 300.0});
                state.frame = kCases[caseIndex].from;
                state.epoch = epoch;
                inputs[caseIndex].push_back(state);
            }
        }

        sol::proto::allocations::reset();

        for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
            live += timeConversions(flat, inputs[caseIndex], kCases[caseIndex].to,
                                    flatSeries[caseIndex], clock);
            live += timeConversions(hierarchical, inputs[caseIndex], kCases[caseIndex].to,
                                    hierarchicalSeries[caseIndex], clock);
        }

        FlatFrameModel rebuiltFlat;
        HierarchicalFrameModel rebuiltHierarchical;
        for (std::uint64_t warmup = 0; warmup < kWarmupIterations; ++warmup) {
            rebuiltFlat.rebuild(snapshot);
            rebuiltHierarchical.rebuild(snapshot);
        }
        for (std::uint64_t sample = 0; sample < kSampleCount; ++sample) {
            const std::int64_t flatStart = clock.now();
            for (std::size_t i = 0; i < kRebuildsPerSample; ++i) {
                rebuiltFlat.rebuild(snapshot);
                // Reading one field back keeps each rebuild live; without it the compiler is
                // free to notice that 512 identical rebuilds have one observable effect.
                live += rebuiltFlat.rootToFrame(FrameId::EarthIcrf).originInParent.x;
            }
            const std::int64_t flatEnd = clock.now();
            flatRebuild.addSample(clock.ticksToNanoseconds(flatEnd - flatStart)
                                  / static_cast<double>(kRebuildsPerSample));

            const std::int64_t hierarchicalStart = clock.now();
            for (std::size_t i = 0; i < kRebuildsPerSample; ++i) {
                rebuiltHierarchical.rebuild(snapshot);
                live += rebuiltHierarchical.parentToFrame(FrameId::EarthIcrf).originInParent.x;
            }
            const std::int64_t hierarchicalEnd = clock.now();
            hierarchicalRebuild.addSample(
                clock.ticksToNanoseconds(hierarchicalEnd - hierarchicalStart)
                / static_cast<double>(kRebuildsPerSample));
        }

        const auto allocationCounts = sol::proto::allocations::snapshot();

        const bool isReleaseBuild =
            std::string_view(sol::proto::facts::kBuildType) == std::string_view("Release");

        checks.check(clock.ticksPerSecond() > 0.0,
                     "QueryPerformanceFrequency returned a usable frequency");
        checks.check(allocationCounts.allocationCount == 0,
                     "no conversion or rebuild allocates");
        checks.check(flatRebuild.summarize().min > 0.0 && hierarchicalRebuild.summarize().min > 0.0,
                     "rebuild cost is above the timer's resolution, so it is a measurement rather "
                     "than a floor");
        for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
            checks.check(flatSeries[caseIndex].summarize().min > 0.0,
                         std::string{"the flat model's "} + kCases[caseIndex].label
                             + " case is resolvable by the timer");
            checks.check(hierarchicalSeries[caseIndex].summarize().min > 0.0,
                         std::string{"the hierarchical model's "} + kCases[caseIndex].label
                             + " case is resolvable by the timer");
        }

        ScenarioMetadata metadata;
        metadata.name = "frames.conversionCost";
        metadata.version = "1";
        metadata.inputDescription =
            "4096 distinct states per case, converted across four frame pairs by each candidate "
            "model, at the ADR 0008 epoch. Rebuild cost is timed separately. Reported per "
            "conversion and per transform application.";
        metadata.seed = kSeed;
        metadata.warmupIterations = kWarmupIterations;
        metadata.sampleCount = kSampleCount;

        ScenarioReport report(metadata);
        report.addMetric(flatRebuild);
        report.addMetric(hierarchicalRebuild);
        for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
            report.addMetric(flatSeries[caseIndex]);
            report.addMetric(hierarchicalSeries[caseIndex]);
        }

        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("evidenceEligibility");
            writer.write("isReleaseBuild", isReleaseBuild);
            writer.write("note",
                         isReleaseBuild
                             ? "Release build: timings are eligible as P1a performance evidence."
                             : "Debug build: timings are recorded for comparison only and are not "
                               "P1a performance evidence.");
            writer.endObject();

            writer.beginArray("cases");
            for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
                writeCase(writer, kCases[caseIndex], flatSeries[caseIndex].summarize(),
                          FlatFrameModel::transformApplications(kCases[caseIndex].from,
                                                                kCases[caseIndex].to),
                          hierarchicalSeries[caseIndex].summarize(),
                          HierarchicalFrameModel::transformApplications(kCases[caseIndex].from,
                                                                        kCases[caseIndex].to));
            }
            writer.endArray();

            writer.beginObject("timer");
            writer.write("ticksPerSecond", clock.ticksPerSecond());
            writer.write("resolutionNanoseconds", 1.0e9 / clock.ticksPerSecond());
            writer.write("note",
                         "Every reported figure is a batch divided by its batch size, so the "
                         "per-operation numbers are far below this resolution by construction. "
                         "A single unbatched operation could not have been measured at all.");
            writer.endObject();

            writer.beginObject("rebuildCost");
            writer.write("rebuildsPerSample", static_cast<std::uint64_t>(kRebuildsPerSample));
            writer.write("flatMedianNanoseconds", flatRebuild.summarize().median);
            writer.write("hierarchicalMedianNanoseconds", hierarchicalRebuild.summarize().median);
            writer.write("note",
                         "Paid once per timestep. The flat model composes every frame down from "
                         "the root; the hierarchical model copies what it was given. The "
                         "difference is the flat model's conversion-time advantage, moved to "
                         "build time.");
            writer.endObject();

            writer.beginObject("p1bBudgetingNote");
            writer.write("guidance",
                         "P1b converts every visible object into a camera-relative frame once per "
                         "rendered frame. Multiply the adjacent-case median by the object count "
                         "and compare against the frame budget; use the per-application figure to "
                         "re-derive a cost if P1b's chain is a different depth from A2's.");
            writer.write("measuredOnSingleMachine",
                         "One machine, as recorded in the environment section. Timing is a "
                         "distribution and must be re-measured on other hardware rather than "
                         "carried across as a constant.");
            writer.endObject();

            writer.beginObject("workload");
            writer.write("conversionsPerSample", static_cast<std::uint64_t>(kConversionsPerSample));
            writer.writeBits("liveResultBits", live);
            writer.endObject();

            writer.beginObject("measuredRegionAllocations");
            writer.write("allocationCount", allocationCounts.allocationCount);
            writer.write("totalAllocatedBytes", allocationCounts.totalAllocatedBytes);
            writer.endObject();

            writer.beginObject("checks");
            writer.write("passed", static_cast<std::uint64_t>(checks.passedCount()));
            writer.write("failed", static_cast<std::uint64_t>(checks.failedCount()));
            writer.endObject();
        });

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "frame-model-cost.json");
        report.setIncludeRawSamples(true);
        report.writeToFile(outputPath);
        std::cout << "FrameModelCost: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("FrameModelCost");
    } catch (const std::exception& error) {
        std::cerr << "FrameModelCost: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
