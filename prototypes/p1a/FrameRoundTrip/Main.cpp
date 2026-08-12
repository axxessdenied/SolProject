/// The A2 frame-conversion and conversion-drift gates.
///
/// P1a's accepted thresholds put two criteria on this scenario:
///
///   Frame conversion  - a round trip through the full frame chain preserves position within
///                       1 mm and velocity within 1 um/s at the surface anchor, and within the
///                       measured floating-point budget at orbital distances.
///   Conversion drift  - 10^6 repeated conversions show bounded, non-accumulating error; any
///                       growth is characterised and attributed to a named frame boundary.
///
/// Two things about how the gate is applied are deliberate.
///
/// First, the thresholds are asserted against *the increment*, not against each candidate. A2
/// exists to select or reject candidate frame models, so a candidate that misses a threshold is
/// a result, not a build failure. This scenario fails only if **no** candidate meets a
/// threshold, and it records each model's number either way. Relabelling a miss as a pass is
/// forbidden by the P1a measurement rules; so is treating a candidate comparison as a
/// regression test.
///
/// Second, drift is measured in two modes, because "repeated conversion" describes two
/// different programs. A system that re-derives each frame's view from one stored authoritative
/// state converts statelessly; a system that stores whatever the last conversion produced
/// converts iteratively. They have completely different error behaviour, and which one the
/// architecture picks is a decision A2 should inform rather than assume.

#include "Sol/Proto/Frames/FlatFrameModel.h"
#include "Sol/Proto/Frames/FrameGraph.h"
#include "Sol/Proto/Frames/HierarchicalFrameModel.h"
#include "Sol/Proto/Frames/ReferenceData.h"

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/ScenarioReport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sol::proto::CheckContext;
using sol::proto::JsonWriter;
using sol::proto::ScenarioMetadata;
using sol::proto::ScenarioReport;
using namespace sol::proto::frames;

/// P1a accepted thresholds, in SI units.
constexpr double kPositionThresholdMetres = 1.0e-3;
constexpr double kVelocityThresholdMetresPerSecond = 1.0e-6;
constexpr std::uint64_t kDriftConversions = 1'000'000;

struct ErrorPair {
    double positionMetres{0.0};
    double velocityMetresPerSecond{0.0};
};

[[nodiscard]] ErrorPair differenceBetween(const StateVector& a, const StateVector& b) noexcept
{
    ErrorPair error;
    error.positionMetres = distance(a.position.metres(), b.position.metres());
    error.velocityMetresPerSecond =
        distance(a.velocity.metresPerSecond(), b.velocity.metresPerSecond());
    return error;
}

/// Round trip through the whole chain, source frame to root and back.
template <typename Model>
[[nodiscard]] ErrorPair fullChainRoundTrip(const Model& model, const StateVector& original)
{
    const StateVector atRoot = model.convert(original, FrameId::SsbIcrf);
    const StateVector returned = model.convert(atRoot, original.frame);
    return differenceBetween(original, returned);
}

/// Round trip to each successive frame up the chain.
///
/// The point is attribution. Converting to the chain's k-th frame and back gives the error
/// accumulated across the first k boundaries; the increase from k-1 to k is what the k-th
/// boundary contributed. A single end-to-end number cannot say which boundary spent the budget,
/// and A2's done criteria require that every failure name the boundary responsible.
template <typename Model>
[[nodiscard]] std::vector<ErrorPair> chainPrefixRoundTrips(const Model& model,
                                                           const StateVector& original)
{
    std::vector<ErrorPair> errors;
    errors.reserve(kSurfaceToRootChain.size() - 1);
    for (std::size_t i = 1; i < kSurfaceToRootChain.size(); ++i) {
        const StateVector converted = model.convert(original, kSurfaceToRootChain[i]);
        const StateVector returned = model.convert(converted, original.frame);
        errors.push_back(differenceBetween(original, returned));
    }
    return errors;
}

struct DriftResult {
    double maxPositionErrorMetres{0.0};
    double finalPositionErrorMetres{0.0};
    double maxVelocityErrorMetresPerSecond{0.0};
    double finalVelocityErrorMetresPerSecond{0.0};
    /// Position error observed at 10^3, 10^4, 10^5, and 10^6 conversions.
    std::array<double, 4> positionErrorAtDecade{};
    /// Iterations after which the state stopped changing, bit for bit. Zero means it never
    /// settled within the run. Only meaningful for the iterated mode.
    std::uint64_t iterationsToFixedPoint{0};
};

/// True when two states are bit-identical in every position and velocity component.
///
/// Bitwise, not tolerance-based: the question this answers is whether iteration has reached an
/// exact fixed point, and a tolerance would report "settled" for a state still drifting below
/// the tolerance -- which is precisely the failure mode the drift threshold exists to catch.
[[nodiscard]] bool bitIdentical(const StateVector& a, const StateVector& b) noexcept
{
    const auto sameComponent = [](double left, double right) {
        return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
    };
    const Vec3& positionA = a.position.metres();
    const Vec3& positionB = b.position.metres();
    const Vec3& velocityA = a.velocity.metresPerSecond();
    const Vec3& velocityB = b.velocity.metresPerSecond();
    return sameComponent(positionA.x, positionB.x) && sameComponent(positionA.y, positionB.y)
        && sameComponent(positionA.z, positionB.z) && sameComponent(velocityA.x, velocityB.x)
        && sameComponent(velocityA.y, velocityB.y) && sameComponent(velocityA.z, velocityB.z);
}

/// Stateless drift: every iteration converts the *same* original state.
///
/// This is what a system that keeps one authoritative state and derives every other view from
/// it actually does. The error cannot accumulate, because nothing carries forward; the loop
/// runs anyway, because "cannot accumulate" is an argument and the threshold asks for a
/// measurement.
template <typename Model>
[[nodiscard]] DriftResult measureStatelessDrift(const Model& model, const StateVector& original,
                                                std::uint64_t conversions)
{
    DriftResult result;
    std::size_t decadeIndex = 0;
    std::uint64_t nextDecade = 1000;

    for (std::uint64_t i = 0; i < conversions; ++i) {
        const StateVector atRoot = model.convert(original, FrameId::SsbIcrf);
        const StateVector returned = model.convert(atRoot, original.frame);
        const ErrorPair error = differenceBetween(original, returned);

        result.maxPositionErrorMetres = std::max(result.maxPositionErrorMetres, error.positionMetres);
        result.maxVelocityErrorMetresPerSecond =
            std::max(result.maxVelocityErrorMetresPerSecond, error.velocityMetresPerSecond);
        result.finalPositionErrorMetres = error.positionMetres;
        result.finalVelocityErrorMetresPerSecond = error.velocityMetresPerSecond;

        if (i + 1 == nextDecade && decadeIndex < result.positionErrorAtDecade.size()) {
            result.positionErrorAtDecade[decadeIndex++] = error.positionMetres;
            nextDecade *= 10;
        }
    }
    return result;
}

/// Iterated drift: each conversion consumes the previous conversion's output.
///
/// This is what a system that stores the converted state does, and it is the case the drift
/// threshold is really about. Rounding at each step is an independent perturbation, so the
/// error executes a random walk and grows like the square root of the conversion count rather
/// than staying put.
template <typename Model>
[[nodiscard]] DriftResult measureIteratedDrift(const Model& model, const StateVector& original,
                                               std::uint64_t conversions)
{
    DriftResult result;
    StateVector current = original;
    std::size_t decadeIndex = 0;
    std::uint64_t nextDecade = 1000;

    for (std::uint64_t i = 0; i < conversions; ++i) {
        const StateVector previous = current;
        const StateVector atRoot = model.convert(current, FrameId::SsbIcrf);
        current = model.convert(atRoot, original.frame);

        // Record where the iteration stops moving. If the round trip is idempotent, this is the
        // reason the error stays bounded, and stating the mechanism is worth more to the
        // architecture than the bound alone.
        if (result.iterationsToFixedPoint == 0 && i > 0 && bitIdentical(previous, current)) {
            result.iterationsToFixedPoint = i;
        }

        const ErrorPair error = differenceBetween(original, current);

        result.maxPositionErrorMetres = std::max(result.maxPositionErrorMetres, error.positionMetres);
        result.maxVelocityErrorMetresPerSecond =
            std::max(result.maxVelocityErrorMetresPerSecond, error.velocityMetresPerSecond);
        result.finalPositionErrorMetres = error.positionMetres;
        result.finalVelocityErrorMetresPerSecond = error.velocityMetresPerSecond;

        if (i + 1 == nextDecade && decadeIndex < result.positionErrorAtDecade.size()) {
            result.positionErrorAtDecade[decadeIndex++] = error.positionMetres;
            nextDecade *= 10;
        }
    }
    return result;
}

/// Everything measured for one candidate model.
struct ModelResults {
    std::string name;
    ErrorPair surfaceRoundTrip;
    ErrorPair orbitalRoundTrip;
    std::vector<ErrorPair> surfacePrefixErrors;
    int fullChainApplications{0};
    DriftResult statelessDrift;
    DriftResult iteratedDrift;
};

/// Measures one model against the surface case. The orbital case uses a different frame-graph
/// snapshot -- the vehicle's floating origin has moved -- so it is measured by the caller with
/// its own model instance rather than folded in here.
template <typename Model>
[[nodiscard]] ModelResults measureModel(const Model& model, const StateVector& surfaceState)
{
    ModelResults results;
    results.name = std::string{Model::modelName()};
    results.surfaceRoundTrip = fullChainRoundTrip(model, surfaceState);
    results.surfacePrefixErrors = chainPrefixRoundTrips(model, surfaceState);
    results.fullChainApplications =
        2 * Model::transformApplications(FrameId::VehicleLocal, FrameId::SsbIcrf);
    results.statelessDrift = measureStatelessDrift(model, surfaceState, kDriftConversions);
    results.iteratedDrift = measureIteratedDrift(model, surfaceState, kDriftConversions);
    return results;
}

void writeErrorPair(JsonWriter& writer, const char* key, const ErrorPair& error)
{
    writer.beginObject(key);
    writer.write("positionErrorMetres", error.positionMetres);
    writer.writeBits("positionErrorMetresBits", error.positionMetres);
    writer.write("velocityErrorMetresPerSecond", error.velocityMetresPerSecond);
    writer.writeBits("velocityErrorMetresPerSecondBits", error.velocityMetresPerSecond);
    writer.write("meetsPositionThreshold", error.positionMetres <= kPositionThresholdMetres);
    writer.write("meetsVelocityThreshold",
                 error.velocityMetresPerSecond <= kVelocityThresholdMetresPerSecond);
    writer.endObject();
}

void writeDrift(JsonWriter& writer, const char* key, const DriftResult& drift, const char* mode)
{
    writer.beginObject(key);
    writer.write("mode", mode);
    writer.write("conversions", kDriftConversions);
    writer.write("maxPositionErrorMetres", drift.maxPositionErrorMetres);
    writer.write("finalPositionErrorMetres", drift.finalPositionErrorMetres);
    writer.write("maxVelocityErrorMetresPerSecond", drift.maxVelocityErrorMetresPerSecond);
    writer.write("finalVelocityErrorMetresPerSecond", drift.finalVelocityErrorMetresPerSecond);
    writer.beginArray("positionErrorMetresAtDecade");
    for (const double value : drift.positionErrorAtDecade) {
        writer.writeValue(value);
    }
    writer.endArray();
    writer.write("decadeLabels", "errors after 1e3, 1e4, 1e5, and 1e6 conversions");
    writer.write("iterationsToBitwiseFixedPoint", drift.iterationsToFixedPoint);
    writer.write("fixedPointNote",
                 "Measured in the iterated mode only; the stateless mode carries nothing forward, "
                 "so it always reports 0 here and the field means nothing for it. In the iterated "
                 "mode a nonzero value is the conversion count after which the state stopped "
                 "changing bit for bit, which makes the error bound a property of the arithmetic "
                 "rather than of how long this particular run was. Zero there would mean the "
                 "state never settled.");
    writer.endObject();
}

void writeModel(JsonWriter& writer, const ModelResults& results)
{
    writer.beginObject();
    writer.write("model", results.name);
    writer.write("fullChainTransformApplications",
                 static_cast<std::int64_t>(results.fullChainApplications));
    writeErrorPair(writer, "surfaceAnchorRoundTrip", results.surfaceRoundTrip);
    writeErrorPair(writer, "orbitalRoundTrip", results.orbitalRoundTrip);

    writer.beginArray("chainPrefixRoundTrips");
    double previousPosition = 0.0;
    for (std::size_t i = 0; i < results.surfacePrefixErrors.size(); ++i) {
        const ErrorPair& error = results.surfacePrefixErrors[i];
        writer.beginObject();
        writer.write("throughFrame", frameName(kSurfaceToRootChain[i + 1]));
        writer.write("boundaryCrossed",
                     std::string{frameName(kSurfaceToRootChain[i])} + " -> "
                         + std::string{frameName(kSurfaceToRootChain[i + 1])});
        writer.write("positionErrorMetres", error.positionMetres);
        writer.write("velocityErrorMetresPerSecond", error.velocityMetresPerSecond);
        writer.write("positionErrorAddedByThisBoundaryMetres",
                     error.positionMetres - previousPosition);
        writer.endObject();
        previousPosition = error.positionMetres;
    }
    writer.endArray();

    writeDrift(writer, "statelessDrift", results.statelessDrift,
               "every conversion starts from the same stored original state");
    writeDrift(writer, "iteratedDrift", results.iteratedDrift,
               "every conversion consumes the previous conversion's output");
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

        // Case 1: on the pad. The vehicle's local origin sits at the launch site, and the
        // state under test is a point 12 m out on the structure -- the scale at which a
        // millimetre actually matters to a player looking at a docking port or a landing leg.
        VehicleState onPad;
        onPad.positionInEnu = Vec3{0.0, 0.0, 0.0};
        onPad.velocityInEnu = Vec3{0.0, 0.0, 0.0};

        StateVector surfaceState;
        surfaceState.position = PositionMetres::fromMetres(Vec3{12.0, -3.5, 8.25});
        surfaceState.velocity = VelocityMetresPerSecond::fromMetresPerSecond(Vec3{0.0, 0.0, 0.0});
        surfaceState.frame = FrameId::VehicleLocal;
        surfaceState.epoch = epoch;

        // Case 2: at 200 km with orbital velocity, the P1a reference altitude. The local
        // origin has floated 1300 km downrange and 200 km up, and the state carries 7.8 km/s.
        VehicleState inOrbit;
        inOrbit.positionInEnu = Vec3{1.297e6, 0.0, 2.0e5};
        inOrbit.velocityInEnu = Vec3{7784.0, 0.0, 0.0};

        StateVector orbitalState = surfaceState;
        orbitalState.velocity = VelocityMetresPerSecond::fromMetresPerSecond(Vec3{2.0, -1.0, 0.5});

        const FrameGraphSnapshot padSnapshot = buildSnapshot(data, epoch, onPad);
        const FrameGraphSnapshot orbitSnapshot = buildSnapshot(data, epoch, inOrbit);

        // Each model is measured twice, once per snapshot, so the surface and orbital cases
        // use frame graphs whose vehicle origin actually differs. Reusing one snapshot would
        // silently measure the pad case twice.
        const FlatFrameModel flatOnPad{padSnapshot};
        const FlatFrameModel flatInOrbit{orbitSnapshot};
        const HierarchicalFrameModel hierarchicalOnPad{padSnapshot};
        const HierarchicalFrameModel hierarchicalInOrbit{orbitSnapshot};

        sol::proto::allocations::reset();

        ModelResults flatResults = measureModel(flatOnPad, surfaceState);
        flatResults.orbitalRoundTrip = fullChainRoundTrip(flatInOrbit, orbitalState);

        ModelResults hierarchicalResults = measureModel(hierarchicalOnPad, surfaceState);
        hierarchicalResults.orbitalRoundTrip = fullChainRoundTrip(hierarchicalInOrbit, orbitalState);

        const auto allocationCounts = sol::proto::allocations::snapshot();

        const std::array<const ModelResults*, 2> allResults{&flatResults, &hierarchicalResults};

        const bool anyMeetsSurfacePosition =
            std::any_of(allResults.begin(), allResults.end(), [](const ModelResults* results) {
                return results->surfaceRoundTrip.positionMetres <= kPositionThresholdMetres;
            });
        const bool anyMeetsSurfaceVelocity =
            std::any_of(allResults.begin(), allResults.end(), [](const ModelResults* results) {
                return results->surfaceRoundTrip.velocityMetresPerSecond
                       <= kVelocityThresholdMetresPerSecond;
            });
        const bool anyMeetsOrbitalPosition =
            std::any_of(allResults.begin(), allResults.end(), [](const ModelResults* results) {
                return results->orbitalRoundTrip.positionMetres <= kPositionThresholdMetres;
            });
        // Bounded means the millionth conversion is no worse than the largest seen, and that
        // the stateless mode -- which carries nothing forward -- genuinely does not grow.
        const bool anyStatelessIsFlat =
            std::any_of(allResults.begin(), allResults.end(), [](const ModelResults* results) {
                return results->statelessDrift.maxPositionErrorMetres
                       <= kPositionThresholdMetres;
            });

        checks.check(anyMeetsSurfacePosition,
                     "at least one candidate frame model round-trips the surface anchor within "
                     "1 mm");
        checks.check(anyMeetsSurfaceVelocity,
                     "at least one candidate frame model round-trips surface velocity within "
                     "1 um/s");
        checks.check(anyMeetsOrbitalPosition,
                     "at least one candidate frame model round-trips the orbital case within "
                     "1 mm");
        checks.check(anyStatelessIsFlat,
                     "at least one candidate holds bounded error across 10^6 stateless "
                     "conversions");
        for (const ModelResults* results : allResults) {
            checks.check(results->statelessDrift.maxPositionErrorMetres
                             == results->statelessDrift.finalPositionErrorMetres,
                         results->name
                             + ": stateless conversion error is constant, not accumulating");
            // The interesting mode. If the iterated state reaches a bitwise fixed point, the
            // bound is a property of the arithmetic rather than of how long this run happened
            // to be, and the architecture is free to store converted states.
            checks.check(results->iteratedDrift.iterationsToFixedPoint > 0,
                         results->name
                             + ": iterated conversion reaches a bitwise fixed point, so repeated "
                               "conversion cannot random-walk");
            checks.check(results->iteratedDrift.maxPositionErrorMetres <= kPositionThresholdMetres,
                         results->name
                             + ": iterated error over 10^6 conversions stays inside 1 mm");
        }

        ScenarioMetadata metadata;
        metadata.name = "frames.roundTripAndDrift";
        metadata.version = "1";
        metadata.inputDescription =
            "Two states in the VehicleLocal frame at the ADR 0008 epoch: a 12 m offset on the "
            "pad at rest, and the same offset with the local origin floated to 1297 km downrange "
            "and 200 km altitude carrying 7784 m/s. Each is round-tripped through the full "
            "VehicleLocal-to-SsbIcrf chain and through every chain prefix, then subjected to "
            "1e6 repeated conversions in stateless and iterated modes.";
        metadata.seed = 0;
        metadata.warmupIterations = 0;
        metadata.sampleCount = kDriftConversions;

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("thresholds");
            writer.write("positionMetres", kPositionThresholdMetres);
            writer.write("velocityMetresPerSecond", kVelocityThresholdMetresPerSecond);
            writer.write("driftConversions", kDriftConversions);
            writer.write("appliedTo",
                         "the increment, not each candidate: A2 selects or rejects models, so a "
                         "candidate that misses a threshold is recorded as a result and only a "
                         "universal miss fails the scenario");
            writer.endObject();

            writer.beginObject("frameChain");
            writer.beginArray("frames");
            for (const FrameId frame : kSurfaceToRootChain) {
                writer.writeValue(frameName(frame));
            }
            writer.endArray();
            writer.write("originMotionModel", OriginMotionModel::kDescription);
            writer.endObject();

            writer.beginArray("models");
            writeModel(writer, flatResults);
            writeModel(writer, hierarchicalResults);
            writer.endArray();

            writer.beginObject("verdict");
            writer.write("anyCandidateMeetsSurfacePosition", anyMeetsSurfacePosition);
            writer.write("anyCandidateMeetsSurfaceVelocity", anyMeetsSurfaceVelocity);
            writer.write("anyCandidateMeetsOrbitalPosition", anyMeetsOrbitalPosition);
            writer.write("anyCandidateHoldsBoundedStatelessDrift", anyStatelessIsFlat);
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

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "frame-round-trip.json");
        report.writeToFile(outputPath);
        std::cout << "FrameRoundTrip: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("FrameRoundTrip");
    } catch (const std::exception& error) {
        std::cerr << "FrameRoundTrip: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
