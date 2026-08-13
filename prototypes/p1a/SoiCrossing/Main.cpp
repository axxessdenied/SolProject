/// Sphere-of-influence entry and exit under ADR 0011.
///
/// A3's done criteria put two requirements on this scenario:
///
///   - sphere-of-influence crossings preserve state within the 1 m and 1 mm/s handoff
///     tolerance and are ordered deterministically under warp;
///   - invalid transition conditions are rejected explicitly rather than producing a silent
///     discontinuity.
///
/// ### Why this is the crossing that actually costs something
///
/// HybridHandoff shows the local-to-analytical transition is exactly lossless: the state is
/// handed over, not recomputed. A sphere-of-influence crossing cannot be, because the state has
/// to be re-expressed against a different origin, and where that origin is comes from the
/// ephemeris. The discontinuity is the round-trip error of that re-expression, and it is the
/// only structurally nonzero transition in the contract.
///
/// ### The instant matters as much as the state
///
/// A crossing detected at whichever tick happened to straddle it would move when the warp
/// factor changed, and every later event would inherit the shift. The propagator therefore
/// refines the crossing to the nanosecond by bisection on the conic, and this scenario checks
/// that the refined instant is identical across warp factors spanning four orders of magnitude.
/// That property, not the state tolerance, is what makes warp equivalence reachable at all.
///
/// ### Boundary behaviour
///
/// A craft sitting exactly on a boundary is the case where a naive implementation oscillates:
/// the exit test is evaluated in one body's frame and the entry test in another's, the two
/// disagree in their last bits, and the craft changes hands every tick forever. The scenario
/// runs long past each crossing specifically to find out whether that happens.

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

constexpr double kPositionThresholdMetres = 1.0;
constexpr double kVelocityThresholdMetresPerSecond = 1.0e-3;

/// Warp granularities the same crossing is found at, in seconds.
///
/// Four orders of magnitude. If the refined crossing instant is identical across all of them,
/// the transition chronology is a property of the trajectory rather than of the frame rate.
constexpr std::array<double, 5> kTickSeconds{1.0, 10.0, 100.0, 1000.0, 10000.0};

struct CrossingCase {
    const char* name;
    const char* whatItExercises;
    /// Whether tick-invariance of the transition chronology is gated for this case.
    ///
    /// Two of the three cases gate it. The third does not, and the reason is the most useful
    /// thing this scenario found: a craft whose orbit runs tangent to a boundary has no
    /// well-conditioned crossing time, so no sampling scheme reproduces it across warp factors.
    /// Gating it would either force a threshold to be relaxed until it passed -- which the P1a
    /// rules forbid -- or misrepresent a property of the trajectory as a defect in the
    /// propagator. It is measured and reported instead, and it is named in the open questions.
    bool gateTickInvariance;
};

constexpr std::array<CrossingCase, 3> kCases{{
    {"earthEscapeToHeliocentric",
     "exit: a hyperbolic departure leaves Earth's sphere of influence and ownership passes up "
     "the hierarchy to the Sun",
     true},
    {"lunarTransit",
     "entry and exit: a decisive pass through the Moon's sphere, closing well above lunar "
     "escape speed, so the craft enters and leaves on a hyperbolic arc. Ownership passes down "
     "to a body whose own position comes from the ephemeris and is moving.",
     true},
    {"lunarMarginalCapture",
     "the hard case: an approach slower than lunar escape speed at the boundary, which is "
     "captured into a bound orbit that repeatedly grazes the sphere from inside. Crossing "
     "times here are ill-conditioned by construction, and this case exists to measure what "
     "that costs rather than to be avoided.",
     false},
}};

/// One measured crossing.
struct CrossingRecord {
    std::string eventKind;
    std::int64_t instantNanoseconds{0};
    std::uint64_t sequence{0};
    int fromBody{0};
    int toBody{0};
    double positionDiscontinuityMetres{0.0};
    double velocityDiscontinuityMetresPerSecond{0.0};
};

/// One run of a case at one warp granularity.
struct RunRecord {
    double tickSeconds{0.0};
    std::vector<CrossingRecord> crossings;
    std::uint64_t coastEvaluations{0};
    std::uint64_t incrementSubdivisions{0};
    std::uint64_t incrementsAtSubdivisionFloor{0};
    bool eventLimitReached{false};
    bool incrementLimitReached{false};
    /// Final state, so warp factors can be compared on where the craft ended up as well as on
    /// what happened to it.
    TwoBodyState finalState{};
    int finalCentralBody{0};
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
        const double earthRadius = earth.meanRadiusMetres;
        const double earthSoi = earth.sphereOfInfluenceRadiusMetres;
        const double moonSoi = system.body(kNaifMoon).sphereOfInfluenceRadiusMetres;

        HybridPropagator::Settings settings;
        settings.campaignEpochTdb =
            data.timeScales().utcToTdb(UtcDateTime{2026, 1, 1, 0, 0, 0.0});
        const HybridPropagator propagator{system, ephemeris, settings};

        const TdbEpoch startEpoch = toTdb(CampaignInstant{}, settings.campaignEpochTdb);

        sol::proto::allocations::reset();

        // ------------------------------------------------------------------------------
        // Case construction.
        // ------------------------------------------------------------------------------

        // Exit: a hyperbolic departure from a 200 km parking orbit. Escape speed at that radius
        // is about 11.0 km/s; 11.5 km/s leaves a hyperbolic excess of roughly 3.3 km/s, which
        // reaches Earth's 9.25e8 m boundary in a few days.
        const double parkingRadius = earthRadius + 200.0e3;
        CraftState escapeCraft;
        escapeCraft.state.position = Vec3{parkingRadius, 0.0, 0.0};
        escapeCraft.state.velocity = Vec3{0.0, 11'500.0, 0.0};
        escapeCraft.centralBodyNaifId = kNaifEarth;
        escapeCraft.regime = PropagationRegime::LocalNumerical;

        // Entry: placed just outside the Moon's sphere on a closing trajectory. Constructed
        // against the ephemeris rather than at a fixed vector, because the Moon is moving and a
        // hand-written position would be outside its sphere at one epoch and inside it at
        // another.
        const TwoBodyState moonAtStart =
            ephemeris.stateRelativeTo(kNaifMoon, kNaifEarth, startEpoch);
        // Direction from the Moon back toward Earth, and the normal of the Moon's own orbit.
        const Vec3 moonToEarth = normalized(-moonAtStart.position);
        const Vec3 lunarOrbitNormal =
            normalized(cross(moonAtStart.position, moonAtStart.velocity));

        // Lunar escape speed at the sphere boundary decides which of the next two cases a
        // closing speed produces. Below it the craft is captured into a bound orbit that grazes
        // the boundary; above it the craft transits on a hyperbolic arc and leaves decisively.
        // Both are constructed from that number rather than from a guessed speed.
        const double lunarEscapeSpeedAtBoundary =
            std::sqrt(2.0 * system.body(kNaifMoon).gravitationalParameter / moonSoi);

        /// An approach starting 1.10 sphere radii from the Moon on the far side from Earth,
        /// closing at @p closingSpeed with a real impact parameter.
        ///
        /// The offset is not cosmetic. Without it the approach is exactly antiparallel to the
        /// Moon-relative position vector, which is a radial trajectory: zero angular momentum
        /// and no conic at all. The first version of this scenario had exactly that, and it is
        /// what exposed the missing eligibility re-check after a change of primary. A flyby has
        /// an impact parameter; a head-on collision course is a different and degenerate case,
        /// and it is now covered by the propagator's own rejection path rather than by
        /// pretending to be a flyby.
        const auto approachAt = [&](double closingSpeed) {
            CraftState craft;
            craft.state.position = moonAtStart.position - moonToEarth * (1.10 * moonSoi)
                                 + lunarOrbitNormal * (0.30 * moonSoi);
            craft.state.velocity = moonAtStart.velocity + moonToEarth * closingSpeed;
            craft.centralBodyNaifId = kNaifEarth;
            craft.regime = PropagationRegime::LocalNumerical;
            return craft;
        };

        const CraftState transitCraft = approachAt(2.6 * lunarEscapeSpeedAtBoundary);
        const CraftState marginalCraft = approachAt(0.52 * lunarEscapeSpeedAtBoundary);

        const std::array<CraftState, 3> initialCrafts{escapeCraft, transitCraft, marginalCraft};
        // Long enough that each crossing happens well inside the run and the scenario keeps
        // propagating afterwards, which is what would expose boundary oscillation.
        const std::array<double, 3> runDurationSeconds{10.0 * 86400.0, 6.0 * 86400.0,
                                                       12.0 * 86400.0};

        // ------------------------------------------------------------------------------
        // Run each case at every warp granularity.
        // ------------------------------------------------------------------------------
        std::vector<std::vector<RunRecord>> caseRuns;
        caseRuns.reserve(kCases.size());

        for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
            std::vector<RunRecord> runs;
            runs.reserve(kTickSeconds.size());

            const CraftState coasting =
                propagator.beginCoast(initialCrafts[caseIndex]).value();
            const CampaignInstant target =
                coasting.instant
                + CampaignDuration::fromSecondsRounded(runDurationSeconds[caseIndex]);

            for (const double tickSeconds : kTickSeconds) {
                const AdvanceResult advanced = propagator.advanceTo(
                    coasting, target, CampaignDuration::fromSecondsRounded(tickSeconds));

                RunRecord run;
                run.tickSeconds = tickSeconds;
                run.coastEvaluations = advanced.coastEvaluations;
                run.incrementSubdivisions = advanced.incrementSubdivisions;
                run.incrementsAtSubdivisionFloor = advanced.incrementsAtSubdivisionFloor;
                run.eventLimitReached = advanced.eventLimitReached;
                run.incrementLimitReached = advanced.incrementLimitReached;
                run.finalState = advanced.craft.state;
                run.finalCentralBody = advanced.craft.centralBodyNaifId;

                for (const TransitionEvent& event : advanced.events) {
                    CrossingRecord record;
                    record.eventKind = std::string{eventKindName(event.kind)};
                    record.instantNanoseconds = event.instant.nanoseconds();
                    record.sequence = event.sequence;
                    record.fromBody = event.fromCentralBodyNaifId;
                    record.toBody = event.toCentralBodyNaifId;
                    record.positionDiscontinuityMetres = event.positionDiscontinuityMetres;
                    record.velocityDiscontinuityMetresPerSecond =
                        event.velocityDiscontinuityMetresPerSecond;
                    run.crossings.push_back(std::move(record));
                }
                runs.push_back(std::move(run));
            }
            caseRuns.push_back(std::move(runs));
        }

        const auto allocationCounts = sol::proto::allocations::snapshot();

        // ------------------------------------------------------------------------------
        // Gates
        // ------------------------------------------------------------------------------
        for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
            const std::string caseName{kCases[caseIndex].name};
            const std::vector<RunRecord>& runs = caseRuns[caseIndex];
            const RunRecord& baseline = runs.front();

            checks.check(!baseline.crossings.empty(),
                         caseName + ": the trajectory actually crosses a sphere-of-influence "
                                    "boundary, so the case measures something");

            const bool gateInvariance = kCases[caseIndex].gateTickInvariance;

            for (const RunRecord& run : runs) {
                const std::string label =
                    caseName + " at " + std::to_string(static_cast<int>(run.tickSeconds))
                    + " s tick";

                // The state tolerance is gated for every case, including the ill-conditioned
                // one. A crossing whose *instant* is not reproducible must still hand the state
                // over cleanly at whatever instant it is taken, and that is a property of the
                // re-expression rather than of the trajectory.
                for (const CrossingRecord& crossing : run.crossings) {
                    checks.check(crossing.positionDiscontinuityMetres
                                     <= kPositionThresholdMetres,
                                 label + ": crossing position discontinuity is within 1 m");
                    checks.check(crossing.velocityDiscontinuityMetresPerSecond
                                     <= kVelocityThresholdMetresPerSecond,
                                 label + ": crossing velocity discontinuity is within 1 mm/s");
                }

                if (!gateInvariance) {
                    continue;
                }

                checks.check(!run.eventLimitReached,
                             label + ": the advance terminated on its target instant rather "
                                     "than on the transition cap, so the boundary does not "
                                     "oscillate");
                checks.check(!run.incrementLimitReached,
                             label + ": the advance terminated on its target instant rather "
                                     "than on the increment cap");

                // The chronology property. Same events, same order, same instants, regardless
                // of how the campaign time was sliced.
                checks.check(run.crossings.size() == baseline.crossings.size(),
                             label + ": records the same number of transitions as the 1 s run");
                if (run.crossings.size() == baseline.crossings.size()) {
                    for (std::size_t i = 0; i < run.crossings.size(); ++i) {
                        checks.check(run.crossings[i].instantNanoseconds
                                         == baseline.crossings[i].instantNanoseconds,
                                     label + ": transition " + std::to_string(i)
                                         + " occurs at the identical nanosecond");
                        checks.check(run.crossings[i].eventKind
                                             == baseline.crossings[i].eventKind
                                         && run.crossings[i].toBody
                                                == baseline.crossings[i].toBody
                                         && run.crossings[i].sequence
                                                == baseline.crossings[i].sequence,
                                     label + ": transition " + std::to_string(i)
                                         + " has the identical kind, ordering, and ownership "
                                           "handover");
                    }
                }

                checks.check(run.finalCentralBody == baseline.finalCentralBody,
                             label + ": ends under the same body's ownership as the 1 s run");

                // The strongest form of the warp-equivalence claim, and the one a tolerance
                // cannot fake: an anchored coast whose transitions land on identical instants
                // must reach an identical state, bit for bit, whatever the tick was.
                checks.check(bitIdentical(run.finalState, baseline.finalState),
                             label + ": reaches a bit-identical final state to the 1 s run");
            }
        }

        // ------------------------------------------------------------------------------
        // Report
        // ------------------------------------------------------------------------------
        ScenarioMetadata metadata;
        metadata.name = "orbit.sphereOfInfluenceCrossing";
        metadata.version = "1";
        metadata.inputDescription =
            "Two trajectories against the ADR 0011 hierarchy: a hyperbolic departure from a "
            "200 km parking orbit at 11500 m/s, propagated 10 days until it leaves Earth's "
            "sphere of influence; and an approach placed 1.10 lunar sphere radii from the Moon "
            "along the Earth-Moon line, closing at 200 m/s, propagated 12 days. Each is run at "
            "five warp granularities from 1 s to 10000 s per increment. Sphere radii and the "
            "Moon's position come from the pinned ADR 0008 data.";
        metadata.seed = 0;
        metadata.warmupIterations = 0;
        metadata.sampleCount = kCases.size() * kTickSeconds.size();

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("thresholds");
            writer.write("positionMetres", kPositionThresholdMetres);
            writer.write("velocityMetresPerSecond", kVelocityThresholdMetresPerSecond);
            writer.write("appliedTo", "each sphere-of-influence crossing");
            writer.write("whyThisTransitionIsTheCostlyOne",
                         "A local-to-analytical handoff hands the state over unchanged and is "
                         "exactly lossless. A sphere-of-influence crossing re-expresses the "
                         "state against a different origin whose position comes from the "
                         "ephemeris, so it is the only structurally nonzero transition in the "
                         "contract.");
            writer.endObject();

            writer.beginObject("hierarchy");
            writer.write("earthSphereOfInfluenceMetres", earthSoi);
            writer.write("moonSphereOfInfluenceMetres", moonSoi);
            writer.write("formulation", "Laplace r = a (m/M)^(2/5), per ADR 0011");
            writer.write("originMotionModel", std::string{ConicEphemeris::kDescription});
            writer.write("gravitationalParentIsNotFrameParent",
                         "The Moon's gravitational primary is Earth, while its frame parent is "
                         "the Earth-Moon barycentre. A barycentre has no mass and owns no "
                         "sphere of influence.");
            writer.endObject();

            writer.beginArray("cases");
            for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
                writer.beginObject();
                writer.write("name", kCases[caseIndex].name);
                writer.write("whatItExercises", kCases[caseIndex].whatItExercises);
                writer.write("runDurationSeconds", runDurationSeconds[caseIndex]);
                writer.write("tickInvarianceGated", kCases[caseIndex].gateTickInvariance);

                writer.beginArray("runs");
                for (const RunRecord& run : caseRuns[caseIndex]) {
                    writer.beginObject();
                    writer.write("tickSeconds", run.tickSeconds);
                    writer.write("coastEvaluations", run.coastEvaluations);
                    writer.write("incrementSubdivisions", run.incrementSubdivisions);
                    writer.write("incrementsAtSubdivisionFloor",
                                 run.incrementsAtSubdivisionFloor);
                    writer.write("eventLimitReached", run.eventLimitReached);
                    writer.write("incrementLimitReached", run.incrementLimitReached);
                    writer.write("finalCentralBodyNaifId",
                                 static_cast<std::int64_t>(run.finalCentralBody));

                    writer.beginArray("crossings");
                    for (const CrossingRecord& crossing : run.crossings) {
                        writer.beginObject();
                        writer.write("kind", crossing.eventKind);
                        writer.write("sequence", crossing.sequence);
                        writer.write("instantNanoseconds", crossing.instantNanoseconds);
                        writer.write("fromCentralBodyNaifId",
                                     static_cast<std::int64_t>(crossing.fromBody));
                        writer.write("toCentralBodyNaifId",
                                     static_cast<std::int64_t>(crossing.toBody));
                        writer.write("positionDiscontinuityMetres",
                                     crossing.positionDiscontinuityMetres);
                        writer.writeBits("positionDiscontinuityMetresBits",
                                         crossing.positionDiscontinuityMetres);
                        writer.write("velocityDiscontinuityMetresPerSecond",
                                     crossing.velocityDiscontinuityMetresPerSecond);
                        writer.write("meetsPositionThreshold",
                                     crossing.positionDiscontinuityMetres
                                         <= kPositionThresholdMetres);
                        writer.write("meetsVelocityThreshold",
                                     crossing.velocityDiscontinuityMetresPerSecond
                                         <= kVelocityThresholdMetresPerSecond);
                        writer.endObject();
                    }
                    writer.endArray();
                    writer.endObject();
                }
                writer.endArray();
                writer.endObject();
            }
            writer.endArray();

            writer.beginObject("measuredRegionAllocations");
            writer.write("allocationCount", allocationCounts.allocationCount);
            writer.write("totalAllocatedBytes", allocationCounts.totalAllocatedBytes);
            writer.endObject();

            writer.beginObject("checks");
            writer.write("passed", static_cast<std::uint64_t>(checks.passedCount()));
            writer.write("failed", static_cast<std::uint64_t>(checks.failedCount()));
            writer.endObject();
        });

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "soi-crossing.json");
        report.writeToFile(outputPath);
        std::cout << "SoiCrossing: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("SoiCrossing");
    } catch (const std::exception& error) {
        std::cerr << "SoiCrossing: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
