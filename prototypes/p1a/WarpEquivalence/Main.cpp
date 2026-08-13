/// The A3 time-warp equivalence gate.
///
/// P1a's accepted thresholds put one criterion on this scenario:
///
///   Warp equivalence - accelerated and real-time runs of the same elapsed campaign time agree
///                      within the handoff and reference-orbit tolerances.
///
/// ### What "the same elapsed campaign time" requires before anything else
///
/// Two runs cannot be compared unless they arrive at the same instant, and a clock accumulated
/// in doubles does not: 3600 additions of 0.1 s do not reach 360 s. ADR 0010's integer campaign
/// time is what makes the comparison possible at all, and OrbitSelfCheck asserts it directly.
/// Everything below assumes it.
///
/// ### Three claims, and only one of them is about accuracy
///
///   1. An anchored analytical coast is warp-invariant *exactly* -- bit for bit, at every warp
///      factor -- because increments are never composed. This is a structural property, not a
///      tolerance, and it is asserted as bitwise equality.
///   2. A stepped coast is not, because each increment applies its own rounding. Measured, so
///      the choice between the two rests on a number.
///   3. The local numerical regime is warp-invariant exactly when the warp tick is an integer
///      multiple of the fixed local step, and not otherwise. This is the result that decides
///      the warp rule, and it is the one this scenario got wrong on its first run: it asserted
///      the local regime could never be warp-invariant, and its own measurement said otherwise,
///      because every tick it happened to use divided the step. Both the aligned and unaligned
///      cases are now measured.
///
/// ### ADR 0011's own validation item
///
/// The ADR requires "a scenario confirms that a 200 km circular orbit shows no secular altitude
/// change over an extended warped coast, since decay is not modelled". That is here, over 100
/// days at maximum warp.

#include "Sol/Proto/Frames/ReferenceData.h"

#include "Sol/Proto/Orbit/BodySystem.h"
#include "Sol/Proto/Orbit/CampaignClock.h"
#include "Sol/Proto/Orbit/ConicEphemeris.h"
#include "Sol/Proto/Orbit/HybridPropagator.h"
#include "Sol/Proto/Orbit/KeplerPropagator.h"
#include "Sol/Proto/Orbit/OrbitalElements.h"
#include "Sol/Proto/Orbit/TwoBody.h"

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/ScenarioReport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sol::proto::CheckContext;
using sol::proto::JsonWriter;
using sol::proto::ScenarioMetadata;
using sol::proto::ScenarioReport;
using namespace sol::proto::frames;
using namespace sol::proto::orbit;

/// The tighter of the two P1a tolerances this gate inherits. Warp equivalence is required to
/// hold "within the handoff and reference-orbit tolerances", and the handoff pair is stricter.
constexpr double kPositionThresholdMetres = 1.0;
constexpr double kVelocityThresholdMetresPerSecond = 1.0e-3;

/// Warp granularities, in seconds of campaign time per advance increment.
constexpr std::array<double, 5> kTickSeconds{1.0, 10.0, 100.0, 1000.0, 10000.0};

constexpr int kOrbitsCompared = 10;

/// Days of warped coast for the ADR 0011 no-decay check.
constexpr double kNoDecayDays = 100.0;

struct ModeResult {
    std::string mode;
    double tickSeconds{0.0};
    double positionDifferenceFromBaselineMetres{0.0};
    double velocityDifferenceFromBaselineMetresPerSecond{0.0};
    bool bitIdenticalToBaseline{false};
    std::uint64_t coastEvaluations{0};
    std::uint64_t integratorSteps{0};
    bool meetsThresholds{false};
};

[[nodiscard]] bool bitIdentical(const TwoBodyState& a, const TwoBodyState& b) noexcept
{
    const auto same = [](double left, double right) {
        return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
    };
    return same(a.position.x, b.position.x) && same(a.position.y, b.position.y)
        && same(a.position.z, b.position.z) && same(a.velocity.x, b.velocity.x)
        && same(a.velocity.y, b.velocity.y) && same(a.velocity.z, b.velocity.z);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        CheckContext checks;

        const std::filesystem::path fixtureRoot = parseFixtureRoot(argc, argv);
        const ReferenceData data = ReferenceData::loadFromDirectory(fixtureRoot);
        const BodySystem system = BodySystem::fromReferenceData(data);
        const ConicEphemeris ephemeris = ConicEphemeris::fromReferenceData(data, system);

        const GravitationalBody& earth = system.body(kNaifEarth);
        const double mu = earth.gravitationalParameter;
        const double orbitRadius = earth.surfaceRadiusMetres + 200.0e3;

        HybridPropagator::Settings baseSettings;
        baseSettings.campaignEpochTdb =
            data.timeScales().utcToTdb(UtcDateTime{2026, 1, 1, 0, 0, 0.0});
        baseSettings.integrator = IntegratorKind::RungeKutta4;
        baseSettings.localStep = CampaignDuration::fromSecondsRounded(1.0);

        CraftState initial;
        initial.state.position = Vec3{orbitRadius, 0.0, 0.0};
        initial.state.velocity = Vec3{0.0, circularSpeed(orbitRadius, mu), 0.0};
        initial.centralBodyNaifId = kNaifEarth;
        initial.regime = PropagationRegime::LocalNumerical;

        const OrbitalElements initialElements = elementsFromState(initial.state, mu);
        const CampaignDuration comparedSpan =
            CampaignDuration::fromSecondsRounded(initialElements.periodSeconds * kOrbitsCompared);
        const CampaignInstant target = initial.instant + comparedSpan;

        sol::proto::allocations::reset();

        // ------------------------------------------------------------------------------
        // Three modes across five warp factors.
        // ------------------------------------------------------------------------------
        std::vector<ModeResult> results;

        struct ModeSpec {
            const char* name;
            CoastEvaluation evaluation;
            bool coasting;
        };
        constexpr std::array<ModeSpec, 3> kModes{{
            {"analyticalCoast.anchoredFromEpoch", CoastEvaluation::AnchoredFromEpoch, true},
            {"analyticalCoast.steppedFromPrevious", CoastEvaluation::SteppedFromPrevious, true},
            {"localNumerical.fixedStep", CoastEvaluation::AnchoredFromEpoch, false},
        }};

        for (const ModeSpec& mode : kModes) {
            HybridPropagator::Settings settings = baseSettings;
            settings.coastEvaluation = mode.evaluation;
            const HybridPropagator propagator{system, ephemeris, settings};

            const CraftState start =
                mode.coasting ? propagator.beginCoast(initial).value() : initial;

            TwoBodyState baseline;
            bool haveBaseline = false;

            for (const double tickSeconds : kTickSeconds) {
                const AdvanceResult advanced = propagator.advanceTo(
                    start, target, CampaignDuration::fromSecondsRounded(tickSeconds));

                if (!haveBaseline) {
                    baseline = advanced.craft.state;
                    haveBaseline = true;
                }

                ModeResult result;
                result.mode = mode.name;
                result.tickSeconds = tickSeconds;
                result.positionDifferenceFromBaselineMetres =
                    distance(advanced.craft.state.position, baseline.position);
                result.velocityDifferenceFromBaselineMetresPerSecond =
                    distance(advanced.craft.state.velocity, baseline.velocity);
                result.bitIdenticalToBaseline = bitIdentical(advanced.craft.state, baseline);
                result.coastEvaluations = advanced.coastEvaluations;
                result.integratorSteps = advanced.integratorSteps;
                result.meetsThresholds =
                    result.positionDifferenceFromBaselineMetres <= kPositionThresholdMetres
                    && result.velocityDifferenceFromBaselineMetresPerSecond
                           <= kVelocityThresholdMetresPerSecond;
                results.push_back(std::move(result));
            }
        }

        // ------------------------------------------------------------------------------
        // Local-regime warp: whether the tick divides the fixed step.
        //
        // The measurement above found the local numerical regime bit-identical at every warp
        // factor, which contradicted the expectation that a different tick must produce a
        // different result. The expectation was wrong for a specific and useful reason: every
        // tick in that set -- 1, 10, 100, 1000, 10000 s -- is an exact multiple of the 1 s local
        // step, so no increment ever needs a truncated remainder and the sequence of full steps
        // is identical.
        //
        // That turns a vague warning into a precise constraint, so it is measured directly here
        // with ticks that do not divide the step. This is the number the warp rule for powered
        // flight has to be built on.
        // ------------------------------------------------------------------------------
        struct AlignmentResult {
            double tickSeconds{0.0};
            bool tickIsMultipleOfLocalStep{false};
            double positionDifferenceFromBaselineMetres{0.0};
            bool bitIdenticalToBaseline{false};
            std::uint64_t integratorSteps{0};
        };

        constexpr std::array<double, 5> kAlignmentTicks{1.0, 2.0, 2.5, 3.7, 10.0};
        std::vector<AlignmentResult> alignment;

        {
            const HybridPropagator propagator{system, ephemeris, baseSettings};
            TwoBodyState alignmentBaseline;
            bool haveAlignmentBaseline = false;

            for (const double tickSeconds : kAlignmentTicks) {
                const AdvanceResult advanced = propagator.advanceTo(
                    initial, target, CampaignDuration::fromSecondsRounded(tickSeconds));
                if (!haveAlignmentBaseline) {
                    alignmentBaseline = advanced.craft.state;
                    haveAlignmentBaseline = true;
                }

                AlignmentResult result;
                result.tickSeconds = tickSeconds;
                // The local step is 1 s here, so a tick divides it exactly when it is a whole
                // number of seconds. Computed rather than tabulated so the two cannot drift
                // apart if the step changes.
                const std::int64_t tickNanoseconds =
                    CampaignDuration::fromSecondsRounded(tickSeconds).nanoseconds();
                result.tickIsMultipleOfLocalStep =
                    tickNanoseconds % baseSettings.localStep.nanoseconds() == 0;
                result.positionDifferenceFromBaselineMetres =
                    distance(advanced.craft.state.position, alignmentBaseline.position);
                result.bitIdenticalToBaseline =
                    bitIdentical(advanced.craft.state, alignmentBaseline);
                result.integratorSteps = advanced.integratorSteps;
                alignment.push_back(result);
            }
        }

        // ------------------------------------------------------------------------------
        // ADR 0011's no-decay validation item.
        // ------------------------------------------------------------------------------
        const HybridPropagator noDecayPropagator{system, ephemeris, baseSettings};
        const CraftState coasting = noDecayPropagator.beginCoast(initial).value();
        const CampaignInstant hundredDays =
            coasting.instant + CampaignDuration::fromSecondsRounded(kNoDecayDays * 86400.0);
        const AdvanceResult longCoast = noDecayPropagator.advanceTo(
            coasting, hundredDays, CampaignDuration::fromSecondsRounded(10000.0));
        const OrbitalElements longCoastElements =
            elementsFromState(longCoast.craft.state, mu);

        const double semiMajorAxisChange =
            longCoastElements.semiMajorAxis - initialElements.semiMajorAxis;
        const double periapsisChange =
            longCoastElements.periapsisRadius - initialElements.periapsisRadius;
        const double apoapsisChange =
            longCoastElements.apoapsisRadius - initialElements.apoapsisRadius;

        // ------------------------------------------------------------------------------
        // Determinism: identical inputs, identical output, bit for bit.
        // ------------------------------------------------------------------------------
        const AdvanceResult repeated = noDecayPropagator.advanceTo(
            coasting, hundredDays, CampaignDuration::fromSecondsRounded(10000.0));
        const bool repeatIsBitIdentical =
            bitIdentical(repeated.craft.state, longCoast.craft.state);

        const auto allocationCounts = sol::proto::allocations::snapshot();

        // ------------------------------------------------------------------------------
        // Gates
        // ------------------------------------------------------------------------------
        for (const ModeResult& result : results) {
            const std::string label =
                result.mode + " at " + std::to_string(static_cast<int>(result.tickSeconds))
                + " s tick";

            if (result.mode == "analyticalCoast.anchoredFromEpoch") {
                // Bitwise, not tolerance-based. An anchored coast never composes increments, so
                // agreement is structural and a tolerance would hide a regression that broke it.
                checks.check(result.bitIdenticalToBaseline,
                             label + ": bit-identical to the 1 s run, because an anchored coast "
                                     "never composes increments");
            }
            checks.check(result.meetsThresholds,
                         label + ": agrees with the 1 s run within the 1 m and 1 mm/s handoff "
                                 "tolerances");
        }

        for (const AlignmentResult& result : alignment) {
            const std::string label =
                "localNumerical at " + std::to_string(result.tickSeconds) + " s tick";
            if (result.tickIsMultipleOfLocalStep) {
                checks.check(result.bitIdenticalToBaseline,
                             label + ": a tick that divides the fixed local step reproduces the "
                                     "baseline bit for bit");
            } else {
                checks.check(!result.bitIdenticalToBaseline,
                             label + ": a tick that does not divide the fixed local step changes "
                                     "the step sequence and so does not reproduce the baseline, "
                                     "which is the constraint the warp rule has to respect");
            }
        }

        // ADR 0011 says orbits do not decay. The gate is that the orbit is unchanged to within
        // the position tolerance, not merely that it has not decayed much.
        checks.check(std::abs(semiMajorAxisChange) <= kPositionThresholdMetres,
                     "a 200 km circular orbit shows no secular semi-major axis change over 100 "
                     "days of warped coast, as ADR 0011 requires");
        checks.check(std::abs(periapsisChange) <= kPositionThresholdMetres
                         && std::abs(apoapsisChange) <= kPositionThresholdMetres,
                     "periapsis and apoapsis are unchanged over 100 days of warped coast");
        checks.check(longCoast.events.empty(),
                     "100 days of coasting crosses no sphere-of-influence boundary");

        checks.check(repeatIsBitIdentical,
                     "repeating the 100-day warped coast with identical inputs reproduces a "
                     "bit-identical state, per ADR 0010");

        // ------------------------------------------------------------------------------
        // Report
        // ------------------------------------------------------------------------------
        ScenarioMetadata metadata;
        metadata.name = "orbit.warpEquivalence";
        metadata.version = "1";
        metadata.inputDescription =
            "A 200 km circular Earth orbit from the pinned ADR 0008 gravitational parameter and "
            "ellipsoid, advanced over exactly 10 orbital periods of campaign time at five warp "
            "granularities from 1 s to 10000 s per increment, in three modes: an anchored "
            "analytical coast, a stepped analytical coast, and fixed-step local numerical "
            "integration. A separate 100-day warped coast covers ADR 0011's own no-decay "
            "validation item.";
        metadata.seed = 0;
        metadata.warmupIterations = 0;
        metadata.sampleCount = kModes.size() * kTickSeconds.size();

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("thresholds");
            writer.write("positionMetres", kPositionThresholdMetres);
            writer.write("velocityMetresPerSecond", kVelocityThresholdMetresPerSecond);
            writer.write("comparedAgainst", "the 1 s tick run of the same mode");
            writer.write("elapsedCampaignTimeSeconds", comparedSpan.seconds());
            writer.write("elapsedCampaignTimeNanoseconds", comparedSpan.nanoseconds());
            writer.write("whyIntegerCampaignTimeIsAPrerequisite",
                         "Two runs cannot be compared unless they arrive at the same instant, "
                         "and a double-accumulated clock does not: 3600 additions of 0.1 s do "
                         "not reach 360 s. ADR 0010's integer campaign time is what makes this "
                         "comparison possible, and OrbitSelfCheck asserts it directly.");
            writer.endObject();

            writer.beginArray("modes");
            for (const ModeResult& result : results) {
                writer.beginObject();
                writer.write("mode", result.mode);
                writer.write("tickSeconds", result.tickSeconds);
                writer.write("positionDifferenceFromBaselineMetres",
                             result.positionDifferenceFromBaselineMetres);
                writer.writeBits("positionDifferenceFromBaselineMetresBits",
                                 result.positionDifferenceFromBaselineMetres);
                writer.write("velocityDifferenceFromBaselineMetresPerSecond",
                             result.velocityDifferenceFromBaselineMetresPerSecond);
                writer.write("bitIdenticalToBaseline", result.bitIdenticalToBaseline);
                writer.write("coastEvaluations", result.coastEvaluations);
                writer.write("integratorSteps", result.integratorSteps);
                writer.write("meetsThresholds", result.meetsThresholds);
                writer.endObject();
            }
            writer.endArray();

            writer.beginObject("localStepAlignment");
            writer.write("localStepSeconds", baseSettings.localStep.seconds());
            writer.write("finding",
                         "The local numerical regime is warp-invariant exactly when the warp "
                         "tick is an integer multiple of the fixed local step, and not "
                         "otherwise. The first version of this scenario asserted that the local "
                         "regime could not be warp-invariant at all; its own measurement "
                         "contradicted that, because every tick it used happened to divide the "
                         "step. The corrected statement is a usable constraint rather than a "
                         "warning.");
            writer.beginArray("ticks");
            for (const AlignmentResult& result : alignment) {
                writer.beginObject();
                writer.write("tickSeconds", result.tickSeconds);
                writer.write("tickIsMultipleOfLocalStep", result.tickIsMultipleOfLocalStep);
                writer.write("positionDifferenceFromBaselineMetres",
                             result.positionDifferenceFromBaselineMetres);
                writer.write("bitIdenticalToBaseline", result.bitIdenticalToBaseline);
                writer.write("integratorSteps", result.integratorSteps);
                writer.endObject();
            }
            writer.endArray();
            writer.endObject();

            writer.beginObject("noDecay");
            writer.write("requiredBy", "ADR 0011 validation");
            writer.write("days", kNoDecayDays);
            writer.write("tickSeconds", 10000.0);
            writer.write("semiMajorAxisChangeMetres", semiMajorAxisChange);
            writer.write("periapsisChangeMetres", periapsisChange);
            writer.write("apoapsisChangeMetres", apoapsisChange);
            writer.write("finalEccentricity", longCoastElements.eccentricity);
            writer.write("transitions",
                         static_cast<std::uint64_t>(longCoast.events.size()));
            writer.endObject();

            writer.beginObject("determinism");
            writer.write("repeatIsBitIdentical", repeatIsBitIdentical);
            writer.write("scope",
                         "same build and machine, per ADR 0010; cross-machine agreement is "
                         "tolerance-based and no second machine has been measured");
            writer.endObject();

            writer.beginObject("whatThisDecides");
            writer.write("warpRule",
                         "Anchor the analytical coast rather than stepping it, and require warp "
                         "ticks to be integer multiples of the fixed local step. The anchored "
                         "coast is then bit-identical at every warp factor because it never "
                         "composes increments, and the local regime is bit-identical too "
                         "because the step sequence is unchanged. The stepped coast is neither, "
                         "and gains nothing in exchange.");
            writer.write("consequenceForGameplay",
                         "Warp does not have to be forbidden under thrust or inside the "
                         "atmosphere on determinism grounds, which is what this scenario "
                         "originally assumed. It has to be quantised: the set of offered warp "
                         "factors must divide evenly into the local step. Whether powered warp "
                         "is desirable for other reasons -- player control authority, and "
                         "whether an analytic thrust arc is worth having -- is a P2/M5 question "
                         "this increment does not answer.");
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

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "warp-equivalence.json");
        report.writeToFile(outputPath);
        std::cout << "WarpEquivalence: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("WarpEquivalence");
    } catch (const std::exception& error) {
        std::cerr << "WarpEquivalence: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
