/// The surface-to-200-km trajectory sampling harness required by A2.
///
/// The increment's deliverable is "a numerical surface-to-200-km trajectory sampling harness
/// reporting per-boundary conversion error". This walks a kinematic ascent from the ADR 0008
/// launch anchor to 200 km, and at every sample converts a state through every boundary of the
/// frame chain and back, recording the error each boundary contributes.
///
/// What makes this different from FrameRoundTrip is that everything moves. The epoch advances,
/// so Earth's rotation angle changes and the celestial origins translate; the vehicle's local
/// origin floats along the trajectory, so the LaunchSiteEnu -> VehicleLocal offset grows from
/// zero to 1300 km. A conversion error that is constant on the pad and grows with altitude is
/// a different architectural problem from one that is constant everywhere, and a single-point
/// measurement cannot tell them apart.
///
/// Errors are reported as distributions, not single numbers. A1's evidence established that
/// timing results are distributions and numerical results are exact; this scenario is the case
/// where a *numerical* result is also a distribution, because it varies over the trajectory
/// rather than over the scheduler. Each series is exactly reproducible run to run.

#include "Sol/Proto/Frames/AscentProfile.h"
#include "Sol/Proto/Frames/FlatFrameModel.h"
#include "Sol/Proto/Frames/FrameGraph.h"
#include "Sol/Proto/Frames/HierarchicalFrameModel.h"
#include "Sol/Proto/Frames/ReferenceData.h"

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/MetricSeries.h"
#include "Sol/Proto/Harness/ScenarioReport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sol::proto::CheckContext;
using sol::proto::JsonWriter;
using sol::proto::MetricSeries;
using sol::proto::MetricSummary;
using sol::proto::ScenarioMetadata;
using sol::proto::ScenarioReport;
using namespace sol::proto::frames;

constexpr std::uint64_t kSampleCount = 2001;
constexpr double kAscentDurationSeconds = 500.0;
constexpr double kInsertionAltitudeMetres = 200000.0;
constexpr double kInsertionSpeedMetresPerSecond = 7784.0;
constexpr double kPositionThresholdMetres = 1.0e-3;
constexpr double kVelocityThresholdMetresPerSecond = 1.0e-6;

/// The five boundaries of the surface-to-root chain, plus the whole chain as a sixth series.
constexpr std::size_t kBoundaryCount = kSurfaceToRootChain.size() - 1;

/// One model's error series across the trajectory.
struct ModelSeries {
    std::string name;
    std::vector<MetricSeries> boundaryPositionError;
    std::vector<MetricSeries> boundaryVelocityError;
    MetricSeries fullChainPositionError;
    MetricSeries fullChainVelocityError;

    explicit ModelSeries(std::string modelName)
        : name{std::move(modelName)}
        , fullChainPositionError{name + ".fullChain.positionError", "m"}
        , fullChainVelocityError{name + ".fullChain.velocityError", "m/s"}
    {
        boundaryPositionError.reserve(kBoundaryCount);
        boundaryVelocityError.reserve(kBoundaryCount);
        for (std::size_t i = 0; i < kBoundaryCount; ++i) {
            const std::string label = std::string{frameName(kSurfaceToRootChain[i])} + "_to_"
                                    + std::string{frameName(kSurfaceToRootChain[i + 1])};
            boundaryPositionError.emplace_back(name + "." + label + ".positionError", "m");
            boundaryVelocityError.emplace_back(name + "." + label + ".velocityError", "m/s");
        }
        reserveAll();
    }

    void reserveAll()
    {
        // Reserved up front so no sample loop iteration reallocates. Allocation counts are a
        // mandatory P1a measurement, and a growth-triggered reallocation inside the sampled
        // region would land in the numbers.
        const std::size_t count = static_cast<std::size_t>(kSampleCount);
        fullChainPositionError.reserve(count);
        fullChainVelocityError.reserve(count);
        for (std::size_t i = 0; i < kBoundaryCount; ++i) {
            boundaryPositionError[i].reserve(count);
            boundaryVelocityError[i].reserve(count);
        }
    }
};

/// Samples one model at one point on the trajectory.
///
/// @p state is expressed in VehicleLocal. Each boundary is exercised by converting the state
/// into the boundary's parent frame and back, which isolates that boundary's contribution
/// instead of reporting an accumulated total that no single boundary owns.
template <typename Model>
void sampleModel(const Model& model, const StateVector& state, ModelSeries& series)
{
    for (std::size_t i = 0; i < kBoundaryCount; ++i) {
        // Express the state in the lower frame of this boundary, then cross the boundary and
        // come back. The first conversion is not part of the measurement; the round trip is.
        const StateVector atLower = model.convert(state, kSurfaceToRootChain[i]);
        const StateVector atUpper = model.convert(atLower, kSurfaceToRootChain[i + 1]);
        const StateVector returned = model.convert(atUpper, kSurfaceToRootChain[i]);

        series.boundaryPositionError[i].addSample(
            distance(returned.position.metres(), atLower.position.metres()));
        series.boundaryVelocityError[i].addSample(
            distance(returned.velocity.metresPerSecond(), atLower.velocity.metresPerSecond()));
    }

    const StateVector atRoot = model.convert(state, FrameId::SsbIcrf);
    const StateVector returned = model.convert(atRoot, state.frame);
    series.fullChainPositionError.addSample(
        distance(returned.position.metres(), state.position.metres()));
    series.fullChainVelocityError.addSample(
        distance(returned.velocity.metresPerSecond(), state.velocity.metresPerSecond()));
}

void writeBoundaryTable(JsonWriter& writer, const ModelSeries& series)
{
    writer.beginObject();
    writer.write("model", series.name);

    const MetricSummary fullChain = series.fullChainPositionError.summarize();
    const MetricSummary fullChainVelocity = series.fullChainVelocityError.summarize();
    writer.beginObject("fullChain");
    writer.write("positionErrorMedianMetres", fullChain.median);
    writer.write("positionErrorMaxMetres", fullChain.max);
    writer.write("velocityErrorMedianMetresPerSecond", fullChainVelocity.median);
    writer.write("velocityErrorMaxMetresPerSecond", fullChainVelocity.max);
    writer.write("meetsPositionThresholdEverywhere", fullChain.max <= kPositionThresholdMetres);
    writer.write("meetsVelocityThresholdEverywhere",
                 fullChainVelocity.max <= kVelocityThresholdMetresPerSecond);
    writer.endObject();

    writer.beginArray("boundaries");
    for (std::size_t i = 0; i < kBoundaryCount; ++i) {
        const MetricSummary position = series.boundaryPositionError[i].summarize();
        const MetricSummary velocity = series.boundaryVelocityError[i].summarize();
        writer.beginObject();
        writer.write("boundary", std::string{frameName(kSurfaceToRootChain[i])} + " -> "
                                     + std::string{frameName(kSurfaceToRootChain[i + 1])});
        writer.write("positionErrorMedianMetres", position.median);
        writer.write("positionErrorMaxMetres", position.max);
        writer.write("velocityErrorMedianMetresPerSecond", velocity.median);
        writer.write("velocityErrorMaxMetresPerSecond", velocity.max);
        writer.endObject();
    }
    writer.endArray();
    writer.endObject();
}

/// Identifies the boundary with the largest median position error.
[[nodiscard]] std::string dominantBoundary(const ModelSeries& series)
{
    std::size_t worst = 0;
    double worstMedian = -1.0;
    for (std::size_t i = 0; i < kBoundaryCount; ++i) {
        const double median = series.boundaryPositionError[i].summarize().median;
        if (median > worstMedian) {
            worstMedian = median;
            worst = i;
        }
    }
    return std::string{frameName(kSurfaceToRootChain[worst])} + " -> "
         + std::string{frameName(kSurfaceToRootChain[worst + 1])};
}

} // namespace

int main(int argc, char** argv)
{
    try {
        CheckContext checks;

        const std::filesystem::path fixtureRoot = parseFixtureRoot(argc, argv);
        const ReferenceData data = ReferenceData::loadFromDirectory(fixtureRoot);
        const TdbEpoch liftoffEpoch = data.fixtureEpoch();

        const AscentProfile profile{kAscentDurationSeconds, kInsertionAltitudeMetres,
                                    kInsertionSpeedMetresPerSecond, Radians::fromDegrees(90.0)};

        ModelSeries flatSeries{std::string{FlatFrameModel::modelName()}};
        ModelSeries hierarchicalSeries{std::string{HierarchicalFrameModel::modelName()}};

        // Geodetic altitude along the trajectory, so the report can show that the topocentric
        // "up" coordinate and the true altitude above the ellipsoid are not the same thing.
        MetricSeries geodeticAltitude("trajectory.geodeticAltitude", "m");
        MetricSeries topocentricAltitude("trajectory.topocentricUpCoordinate", "m");
        MetricSeries barycentricDistance("trajectory.distanceFromBarycentre", "m");
        geodeticAltitude.reserve(static_cast<std::size_t>(kSampleCount));
        topocentricAltitude.reserve(static_cast<std::size_t>(kSampleCount));
        barycentricDistance.reserve(static_cast<std::size_t>(kSampleCount));

        // The state under test is a fixed 12 m offset on the vehicle structure, expressed in
        // the floating local frame. It stays put in that frame for the whole ascent, which is
        // exactly what makes it a clean probe: any variation in the measured error comes from
        // the frame graph, not from the state.
        const Vec3 structureOffset{12.0, -3.5, 8.25};
        const Vec3 structureVelocity{0.0, 0.0, 0.0};

        sol::proto::allocations::reset();

        for (std::uint64_t sample = 0; sample < kSampleCount; ++sample) {
            const double t = kAscentDurationSeconds * static_cast<double>(sample)
                           / static_cast<double>(kSampleCount - 1);

            // Campaign time accumulates as exact integer nanoseconds and converts to the
            // ephemeris scale once, here. ADR 0010 requires the integer accumulation; the
            // conversion boundary is the part A2 is demonstrating.
            const CampaignTime elapsed = CampaignTime::fromSecondsRounded(t);
            const TdbEpoch epoch =
                liftoffEpoch.advancedBy(Seconds::fromSeconds(elapsed.seconds()));

            const VehicleState vehicle = profile.sample(t);
            const FrameGraphSnapshot snapshot = buildSnapshot(data, epoch, vehicle);

            const FlatFrameModel flat{snapshot};
            const HierarchicalFrameModel hierarchical{snapshot};

            StateVector state;
            state.position = PositionMetres::fromMetres(structureOffset);
            state.velocity = VelocityMetresPerSecond::fromMetresPerSecond(structureVelocity);
            state.frame = FrameId::VehicleLocal;
            state.epoch = epoch;

            sampleModel(flat, state, flatSeries);
            sampleModel(hierarchical, state, hierarchicalSeries);

            const StateVector bodyFixed = hierarchical.convert(state, FrameId::EarthBodyFixed);
            const Geodetic geodetic = data.earthEllipsoid().fromBodyFixed(bodyFixed.position);
            geodeticAltitude.addSample(geodetic.heightMetres);
            topocentricAltitude.addSample(vehicle.positionInEnu.z);

            const StateVector barycentric = hierarchical.convert(state, FrameId::SsbIcrf);
            barycentricDistance.addSample(length(barycentric.position.metres()));
        }

        const auto allocationCounts = sol::proto::allocations::snapshot();

        const MetricSummary flatFullChain = flatSeries.fullChainPositionError.summarize();
        const MetricSummary hierarchicalFullChain =
            hierarchicalSeries.fullChainPositionError.summarize();
        const MetricSummary flatFullChainVelocity = flatSeries.fullChainVelocityError.summarize();
        const MetricSummary hierarchicalFullChainVelocity =
            hierarchicalSeries.fullChainVelocityError.summarize();

        const MetricSummary geodeticSummary = geodeticAltitude.summarize();
        const MetricSummary topocentricSummary = topocentricAltitude.summarize();

        checks.check(hierarchicalFullChain.max <= kPositionThresholdMetres
                         || flatFullChain.max <= kPositionThresholdMetres,
                     "at least one candidate holds position error under 1 mm across the whole "
                     "ascent");
        checks.check(hierarchicalFullChainVelocity.max <= kVelocityThresholdMetresPerSecond
                         || flatFullChainVelocity.max <= kVelocityThresholdMetresPerSecond,
                     "at least one candidate holds velocity error under 1 um/s across the whole "
                     "ascent");
        checks.check(geodeticSummary.max > kInsertionAltitudeMetres * 0.9,
                     "the trajectory actually reaches roughly the intended altitude");
        // The whole sampled region -- 2001 frame-graph rebuilds, two model constructions each,
        // and twelve conversions per model per sample -- performs no heap allocation. That is a
        // property worth asserting rather than merely reporting: it is what makes the per-sample
        // numbers comparable, and it is the property a future refactor is most likely to break
        // without noticing.
        checks.check(allocationCounts.allocationCount == 0,
                     "the sampled region performs no heap allocation");

        ScenarioMetadata metadata;
        metadata.name = "frames.ascentSampling";
        metadata.version = "1";
        metadata.inputDescription =
            "A 500 s kinematic ascent from the ADR 0008 launch anchor to 200 km and 7784 m/s on "
            "a 90 degree azimuth, sampled at 2001 points. At each point the frame graph is "
            "rebuilt at that epoch with the vehicle's floating origin on the trajectory, and a "
            "fixed 12 m structural offset is round-tripped across every chain boundary and "
            "across the whole chain, in both candidate models.";
        metadata.seed = 0;
        metadata.warmupIterations = 0;
        metadata.sampleCount = kSampleCount;

        ScenarioReport report(metadata);
        report.addMetric(geodeticAltitude);
        report.addMetric(topocentricAltitude);
        report.addMetric(barycentricDistance);
        report.addMetric(flatSeries.fullChainPositionError);
        report.addMetric(flatSeries.fullChainVelocityError);
        report.addMetric(hierarchicalSeries.fullChainPositionError);
        report.addMetric(hierarchicalSeries.fullChainVelocityError);
        for (std::size_t i = 0; i < kBoundaryCount; ++i) {
            report.addMetric(flatSeries.boundaryPositionError[i]);
            report.addMetric(flatSeries.boundaryVelocityError[i]);
            report.addMetric(hierarchicalSeries.boundaryPositionError[i]);
            report.addMetric(hierarchicalSeries.boundaryVelocityError[i]);
        }

        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("trajectory");
            writer.write("durationSeconds", kAscentDurationSeconds);
            writer.write("insertionAltitudeMetres", kInsertionAltitudeMetres);
            writer.write("insertionSpeedMetresPerSecond", kInsertionSpeedMetresPerSecond);
            writer.write("azimuthDegreesFromNorth", 90.0);
            writer.write("kind", "closed-form kinematic profile with exact analytic velocity");
            writer.write("notADynamicsSolution",
                         "No force is integrated and no propellant is consumed. Trajectory "
                         "integration belongs to increment A3 under ADR 0011.");
            writer.write("geodeticAltitudeMaxMetres", geodeticSummary.max);
            writer.write("topocentricUpMaxMetres", topocentricSummary.max);
            writer.write("altitudeDiscrepancyNote",
                         "The topocentric up coordinate exceeds true geodetic altitude because "
                         "the launch-site frame is a plane tangent at the anchor and the vehicle "
                         "flies 1300 km along a curved surface. Reporting one as the other is a "
                         "mistake that hides inside a plausible number.");
            writer.write("campaignTimeRepresentation",
                         "int64 nanoseconds past liftoff, converted to TDB once per sample");
            writer.endObject();

            writer.beginArray("models");
            writeBoundaryTable(writer, flatSeries);
            writeBoundaryTable(writer, hierarchicalSeries);
            writer.endArray();

            writer.beginObject("attribution");
            writer.write("flatDominantBoundary", dominantBoundary(flatSeries));
            writer.write("hierarchicalDominantBoundary", dominantBoundary(hierarchicalSeries));
            writer.write("note",
                         "The dominant boundary is the one with the largest median position "
                         "error across the trajectory. A2's done criteria require every failure "
                         "to name the boundary responsible, so the attribution is computed "
                         "whether or not anything failed.");
            writer.endObject();

            writer.beginObject("sampledRegionAllocations");
            writer.write("allocationCount", allocationCounts.allocationCount);
            writer.write("totalAllocatedBytes", allocationCounts.totalAllocatedBytes);
            writer.write("note",
                         "The sampled region rebuilds the frame graph and appends to metric "
                         "series 2001 times without allocating. Snapshots are value types over "
                         "std::array and series storage is reserved up front, so nothing in the "
                         "loop reaches the heap.");
            writer.endObject();

            writer.beginObject("checks");
            writer.write("passed", static_cast<std::uint64_t>(checks.passedCount()));
            writer.write("failed", static_cast<std::uint64_t>(checks.failedCount()));
            writer.endObject();
        });

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "ascent-sampling.json");
        report.setIncludeRawSamples(true);
        report.writeToFile(outputPath);
        std::cout << "AscentSampling: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("AscentSampling");
    } catch (const std::exception& error) {
        std::cerr << "AscentSampling: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
