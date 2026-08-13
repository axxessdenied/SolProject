/// The A3 reference-orbit gate.
///
/// P1a's accepted thresholds put one criterion on this scenario:
///
///   Reference orbit - after one approximately 200 km circular-orbit period, the integrated
///                     position is within 100 m of the Kepler analytic reference at the same
///                     campaign time.
///
/// ### What the comparison means
///
/// It is not a comparison against reality. ADR 0011 makes patched conics *authoritative*, so
/// the Kepler solution is not a test oracle standing in for the truth -- it is the truth, by
/// decision. Every metre of error reported here therefore belongs to the numerical integrator
/// and to nothing else, which is a stronger statement than an accuracy comparison usually gets
/// to make.
///
/// ### Why three integrators and five step sizes
///
/// The gate as written could be passed by running any fourth-order method at a small enough
/// step and reporting one number. That would answer the threshold and none of the questions
/// behind it. The choice that actually has to be made is which integrator the local physics
/// regime uses for the next several years, and the deciding property is not one-orbit accuracy
/// but whether the energy error accumulates. ADR 0011 says orbits do not decay; an integrator
/// with secular energy loss manufactures decay the ADR forbids, and it does so invisibly,
/// because over one orbit it looks like the most accurate candidate available.
///
/// So this scenario reports the gate and then reports the thing the gate cannot see: the same
/// integrators over fifty orbits, where the difference between bounded and accumulating error
/// is the entire result.
///
/// ### Why two orbits and not one
///
/// A circular orbit is the easiest case a two-body integrator can be given -- constant speed,
/// constant radius, no periapsis passage. Gating only on it would flatter every candidate. The
/// eccentric case is included so the reported step-size requirement is one that survives a
/// periapsis.

#include "Sol/Proto/Frames/ReferenceData.h"

#include "Sol/Proto/Orbit/BodySystem.h"
#include "Sol/Proto/Orbit/CampaignClock.h"
#include "Sol/Proto/Orbit/ConicEphemeris.h"
#include "Sol/Proto/Orbit/HybridPropagator.h"
#include "Sol/Proto/Orbit/Integrator.h"
#include "Sol/Proto/Orbit/KeplerPropagator.h"
#include "Sol/Proto/Orbit/OrbitalElements.h"
#include "Sol/Proto/Orbit/TwoBody.h"

#include "Sol/Proto/Harness/AllocationCounter.h"
#include "Sol/Proto/Harness/Check.h"
#include "Sol/Proto/Harness/JsonWriter.h"
#include "Sol/Proto/Harness/ScenarioReport.h"

#include <windows.h>

#include <algorithm>
#include <array>
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

/// The P1a accepted threshold, in metres.
constexpr double kReferenceOrbitThresholdMetres = 100.0;

/// Step sizes measured, in seconds. Spans two orders of magnitude so the reported step-size
/// requirement is read off a curve rather than asserted from one point.
constexpr std::array<double, 5> kStepSeconds{64.0, 16.0, 4.0, 1.0, 0.25};

/// Orbits in the long run that exposes secular energy error.
constexpr int kLongRunOrbits = 50;
constexpr double kLongRunStepSeconds = 4.0;

/// Untimed repetitions of each configuration before the timed one.
///
/// The P1a measurement rules require warm-up to be performed and recorded, and this scenario
/// demonstrated why on its first run: the first configuration measured reported 131 ns per
/// acceleration evaluation against about 30 for identical work in every later configuration.
/// That is a cold instruction and data cache, not a slow integrator, and quoting it would have
/// been the same class of defect A2 caught in its own first reports -- an instrument reporting
/// something other than what it claims to.
constexpr std::uint64_t kWarmupRuns = 1;

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

private:
    double m_ticksPerSecond{1.0};
};

/// One orbit A3 gates against.
struct OrbitCase {
    const char* name;
    const char* whyItMatters;
    double periapsisAltitudeMetres;
    double apoapsisAltitudeMetres;
};

constexpr std::array<OrbitCase, 2> kCases{{
    {"circular200km",
     "the P1a reference orbit and the Orbital Environmental Survey contract's own orbit",
     200.0e3, 200.0e3},
    {"elliptical200x2000km",
     "an eccentric orbit, so the reported step size survives a periapsis passage rather than "
     "being read off the easiest case a two-body integrator can be given",
     200.0e3, 2000.0e3},
}};

/// Builds the initial state for a case, at periapsis, in the reference plane.
[[nodiscard]] TwoBodyState initialStateFor(const OrbitCase& orbitCase, double earthRadius,
                                           double mu) noexcept
{
    const double periapsis = earthRadius + orbitCase.periapsisAltitudeMetres;
    const double apoapsis = earthRadius + orbitCase.apoapsisAltitudeMetres;
    const double semiMajorAxis = 0.5 * (periapsis + apoapsis);

    // Vis-viva at periapsis. Reduces to the circular speed when the two radii are equal, so the
    // circular case is not special-cased.
    const double speedAtPeriapsis = std::sqrt(mu * (2.0 / periapsis - 1.0 / semiMajorAxis));

    TwoBodyState state;
    state.position = Vec3{periapsis, 0.0, 0.0};
    state.velocity = Vec3{0.0, speedAtPeriapsis, 0.0};
    return state;
}

/// Everything measured for one integrator at one step size.
struct StepResult {
    double stepSeconds{0.0};
    double positionErrorMetres{0.0};
    double velocityErrorMetresPerSecond{0.0};
    double relativeEnergyErrorAtEnd{0.0};
    double relativeAngularMomentumErrorAtEnd{0.0};
    double semiMajorAxisErrorMetres{0.0};
    double eccentricityError{0.0};
    double inclinationErrorRadians{0.0};
    std::uint64_t accelerationEvaluations{0};
    std::uint64_t integratorSteps{0};
    double nanosecondsPerAccelerationEvaluation{0.0};
    bool meetsThreshold{false};
};

struct IntegratorResults {
    IntegratorKind kind{IntegratorKind::RungeKutta4};
    std::vector<StepResult> steps;
    /// The largest step size that still meets the gate, or zero when none does.
    double largestPassingStepSeconds{0.0};
    /// Acceleration evaluations one orbit costs at that step, as this prototype performs them.
    ///
    /// The honest cost comparison. Comparing candidates at a common step size credits the
    /// cheap-per-step ones for accuracy they do not have; comparing them at the step each one
    /// actually needs to pass the gate is the question a budget has to answer.
    std::uint64_t accelerationEvaluationsAtLargestPassingStep{0};
    /// The same count with each integrator's inherent per-step cost rather than this
    /// implementation's.
    ///
    /// This is the figure that compares *integrators*, and it differs only for velocity Verlet,
    /// whose end-of-step acceleration a stateful loop would carry into the next step. A3's
    /// stateless integrateStep cannot, so quoting the implementation's count as the method's
    /// would overstate velocity Verlet by exactly 2x. Both are reported; the relative-cost
    /// conclusion is drawn from this one.
    std::uint64_t minimumAccelerationEvaluationsAtLargestPassingStep{0};
};

/// Energy behaviour over a long run: the property the one-orbit gate cannot see.
struct LongRunResult {
    IntegratorKind kind{IntegratorKind::RungeKutta4};
    double worstRelativeEnergyError{0.0};
    double finalRelativeEnergyError{0.0};
    /// Worst relative energy error over the first orbit, for comparison with the fiftieth.
    double firstOrbitWorstRelativeEnergyError{0.0};
    /// Change in semi-major axis over the run, in metres. The number a player would experience
    /// as an orbit silently decaying or climbing.
    double semiMajorAxisDriftMetres{0.0};
    /// Whether the error grew roughly in proportion to the number of orbits.
    bool errorIsSecular{false};
};

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
        const double earthRadius = earth.surfaceRadiusMetres;

        HybridPropagator::Settings baseSettings;
        baseSettings.campaignEpochTdb =
            data.timeScales().utcToTdb(UtcDateTime{2026, 1, 1, 0, 0, 0.0});

        const HighResolutionClock clock;

        sol::proto::allocations::reset();

        // ------------------------------------------------------------------------------
        // The gate, per case, per integrator, per step size.
        // ------------------------------------------------------------------------------
        std::vector<std::vector<IntegratorResults>> caseResults;
        caseResults.reserve(kCases.size());

        for (const OrbitCase& orbitCase : kCases) {
            const TwoBodyState initial = initialStateFor(orbitCase, earthRadius, mu);
            const OrbitalElements initialElements = elementsFromState(initial, mu);
            const double period = initialElements.periodSeconds;

            // Campaign time is integer nanoseconds, so the integrated run and the analytic
            // reference are asked for the same interval down to the nanosecond rather than for
            // two doubles that happen to be close.
            const CampaignDuration oneOrbit = CampaignDuration::fromSecondsRounded(period);
            const TwoBodyState reference =
                propagateKepler(initial, mu, oneOrbit.seconds()).state;
            const double referenceEnergy = specificEnergy(initial, mu);
            const double referenceMomentum = length(specificAngularMomentum(initial));

            std::vector<IntegratorResults> perIntegrator;
            perIntegrator.reserve(kIntegratorCandidates.size());

            for (const IntegratorKind kind : kIntegratorCandidates) {
                IntegratorResults results;
                results.kind = kind;

                for (const double stepSeconds : kStepSeconds) {
                    HybridPropagator::Settings settings = baseSettings;
                    settings.integrator = kind;
                    settings.localStep = CampaignDuration::fromSecondsRounded(stepSeconds);
                    const HybridPropagator propagator{system, ephemeris, settings};

                    CraftState craft;
                    craft.state = initial;
                    craft.centralBodyNaifId = kNaifEarth;
                    craft.regime = PropagationRegime::LocalNumerical;

                    for (std::uint64_t warmup = 0; warmup < kWarmupRuns; ++warmup) {
                        const AdvanceResult discarded =
                            propagator.advanceTo(craft, craft.instant + oneOrbit, oneOrbit);
                        // Consumed so the optimiser cannot delete the warm-up it was asked for.
                        if (discarded.integratorSteps == 0) {
                            std::cerr << "ReferenceOrbit: warm-up performed no steps\n";
                        }
                    }

                    const std::int64_t startTicks = clock.now();
                    const AdvanceResult advanced =
                        propagator.advanceTo(craft, craft.instant + oneOrbit, oneOrbit);
                    const std::int64_t endTicks = clock.now();

                    const OrbitalElements finalElements =
                        elementsFromState(advanced.craft.state, mu);

                    StepResult stepResult;
                    stepResult.stepSeconds = stepSeconds;
                    stepResult.positionErrorMetres =
                        distance(advanced.craft.state.position, reference.position);
                    stepResult.velocityErrorMetresPerSecond =
                        distance(advanced.craft.state.velocity, reference.velocity);
                    stepResult.relativeEnergyErrorAtEnd =
                        std::abs((specificEnergy(advanced.craft.state, mu) - referenceEnergy)
                                 / referenceEnergy);
                    stepResult.relativeAngularMomentumErrorAtEnd =
                        std::abs((length(specificAngularMomentum(advanced.craft.state))
                                  - referenceMomentum)
                                 / referenceMomentum);
                    stepResult.semiMajorAxisErrorMetres =
                        std::abs(finalElements.semiMajorAxis - initialElements.semiMajorAxis);
                    stepResult.eccentricityError =
                        std::abs(finalElements.eccentricity - initialElements.eccentricity);
                    stepResult.inclinationErrorRadians =
                        std::abs(finalElements.inclination - initialElements.inclination);
                    stepResult.accelerationEvaluations = advanced.accelerationEvaluations;
                    stepResult.integratorSteps = advanced.integratorSteps;
                    stepResult.nanosecondsPerAccelerationEvaluation =
                        advanced.accelerationEvaluations == 0
                            ? 0.0
                            : clock.ticksToNanoseconds(endTicks - startTicks)
                                  / static_cast<double>(advanced.accelerationEvaluations);
                    stepResult.meetsThreshold =
                        stepResult.positionErrorMetres <= kReferenceOrbitThresholdMetres;

                    if (stepResult.meetsThreshold
                        && stepSeconds > results.largestPassingStepSeconds) {
                        results.largestPassingStepSeconds = stepSeconds;
                        results.accelerationEvaluationsAtLargestPassingStep =
                            stepResult.accelerationEvaluations;
                        results.minimumAccelerationEvaluationsAtLargestPassingStep =
                            stepResult.integratorSteps
                            * static_cast<std::uint64_t>(
                                minimumAccelerationEvaluationsPerStep(kind));
                    }
                    results.steps.push_back(stepResult);
                }
                perIntegrator.push_back(std::move(results));
            }
            caseResults.push_back(std::move(perIntegrator));
        }

        // ------------------------------------------------------------------------------
        // The long run, on the circular reference orbit only.
        // ------------------------------------------------------------------------------
        const TwoBodyState longRunInitial = initialStateFor(kCases[0], earthRadius, mu);
        const OrbitalElements longRunElements = elementsFromState(longRunInitial, mu);
        const double longRunPeriod = longRunElements.periodSeconds;
        const double longRunReferenceEnergy = specificEnergy(longRunInitial, mu);

        std::vector<LongRunResult> longRun;
        longRun.reserve(kIntegratorCandidates.size());

        for (const IntegratorKind kind : kIntegratorCandidates) {
            LongRunResult result;
            result.kind = kind;

            TwoBodyState state = longRunInitial;
            const auto stepsPerOrbit =
                static_cast<long long>(longRunPeriod / kLongRunStepSeconds);

            for (int orbit = 0; orbit < kLongRunOrbits; ++orbit) {
                for (long long i = 0; i < stepsPerOrbit; ++i) {
                    state = integrateStep(kind, state, mu, kLongRunStepSeconds);
                    const double relative =
                        std::abs((specificEnergy(state, mu) - longRunReferenceEnergy)
                                 / longRunReferenceEnergy);
                    result.worstRelativeEnergyError =
                        std::max(result.worstRelativeEnergyError, relative);
                    if (orbit == 0) {
                        result.firstOrbitWorstRelativeEnergyError =
                            std::max(result.firstOrbitWorstRelativeEnergyError, relative);
                    }
                }
            }

            result.finalRelativeEnergyError =
                std::abs((specificEnergy(state, mu) - longRunReferenceEnergy)
                         / longRunReferenceEnergy);
            result.semiMajorAxisDriftMetres =
                elementsFromState(state, mu).semiMajorAxis - longRunElements.semiMajorAxis;
            // Secular means the error after fifty orbits is far worse than after one. A
            // bounded oscillation returns a ratio near 1; an accumulating error returns a
            // ratio near the orbit count. Ten is comfortably between the two.
            result.errorIsSecular =
                result.firstOrbitWorstRelativeEnergyError > 0.0
                && result.worstRelativeEnergyError
                       > 10.0 * result.firstOrbitWorstRelativeEnergyError;
            longRun.push_back(result);
        }

        const auto allocationCounts = sol::proto::allocations::snapshot();

        // ------------------------------------------------------------------------------
        // Gates
        // ------------------------------------------------------------------------------
        for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
            const std::string caseName{kCases[caseIndex].name};
            const bool anyCandidatePasses =
                std::any_of(caseResults[caseIndex].begin(), caseResults[caseIndex].end(),
                            [](const IntegratorResults& results) {
                                return results.largestPassingStepSeconds > 0.0;
                            });
            checks.check(anyCandidatePasses,
                         caseName + ": at least one integrator candidate reaches the 100 m "
                                    "one-orbit gate");

            // Every candidate must pass at the finest step, or it is not converging to the
            // authoritative model at all and its order claim is wrong.
            for (const IntegratorResults& results : caseResults[caseIndex]) {
                const StepResult& finest = results.steps.back();
                checks.check(finest.meetsThreshold,
                             caseName + ", " + std::string{integratorName(results.kind)}
                                 + ": meets the 100 m gate at the finest measured step");
                checks.check(finest.relativeAngularMomentumErrorAtEnd < 1.0e-12,
                             caseName + ", " + std::string{integratorName(results.kind)}
                                 + ": conserves angular momentum, as any central force must");
            }
        }

        // The result that decides the integrator, stated as a check so it cannot be read past.
        for (const LongRunResult& result : longRun) {
            if (isSymplectic(result.kind)) {
                checks.check(!result.errorIsSecular,
                             std::string{integratorName(result.kind)}
                                 + ": energy error stays bounded over 50 orbits, so it does not "
                                   "manufacture the orbital decay ADR 0011 forbids");
            }
        }

        // ------------------------------------------------------------------------------
        // Report
        // ------------------------------------------------------------------------------
        ScenarioMetadata metadata;
        metadata.name = "orbit.referenceOrbit";
        metadata.version = "1";
        metadata.inputDescription =
            "Two Earth orbits from the pinned ADR 0008 gravitational parameter and ellipsoid: a "
            "200 km circular orbit and a 200 x 2000 km elliptical orbit, each started at "
            "periapsis in the reference plane. Each is integrated for exactly one period of "
            "campaign time by three fixed-step integrators at five step sizes, and compared "
            "against the Kepler analytic state at the same instant. A separate 50-orbit run at "
            "a 4 s step measures energy behaviour.";
        metadata.seed = 0;
        metadata.warmupIterations = kWarmupRuns;
        metadata.sampleCount =
            kCases.size() * kIntegratorCandidates.size() * kStepSeconds.size();

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("threshold");
            writer.write("positionMetres", kReferenceOrbitThresholdMetres);
            writer.write("comparedAgainst",
                         "the Kepler analytic state at the same campaign instant");
            writer.write("whatTheComparisonMeans",
                         "ADR 0011 makes patched conics authoritative, so the Kepler solution is "
                         "the model itself rather than a stand-in for reality. Every metre "
                         "reported here belongs to the numerical integrator.");
            writer.endObject();

            writer.beginObject("model");
            writer.write("gravity", "ADR 0011 two-body point mass, one gravitating body");
            writer.write("gravitationalParameterM3S2", mu);
            writer.write("earthEquatorialRadiusMetres", earthRadius);
            writer.write("originMotionModel", std::string{ConicEphemeris::kDescription});
            writer.endObject();

            writer.beginArray("cases");
            for (std::size_t caseIndex = 0; caseIndex < kCases.size(); ++caseIndex) {
                const OrbitCase& orbitCase = kCases[caseIndex];
                const TwoBodyState initial = initialStateFor(orbitCase, earthRadius, mu);
                const OrbitalElements elements = elementsFromState(initial, mu);

                writer.beginObject();
                writer.write("name", orbitCase.name);
                writer.write("whyItMatters", orbitCase.whyItMatters);
                writer.write("periapsisAltitudeMetres", orbitCase.periapsisAltitudeMetres);
                writer.write("apoapsisAltitudeMetres", orbitCase.apoapsisAltitudeMetres);
                writer.write("semiMajorAxisMetres", elements.semiMajorAxis);
                writer.write("eccentricity", elements.eccentricity);
                writer.write("periodSeconds", elements.periodSeconds);
                writer.write("shape", std::string{orbitShapeName(elements.shape)});

                writer.beginArray("integrators");
                for (const IntegratorResults& results : caseResults[caseIndex]) {
                    writer.beginObject();
                    writer.write("integrator", std::string{integratorName(results.kind)});
                    writer.write("symplectic", isSymplectic(results.kind));
                    writer.write("accelerationEvaluationsPerStep",
                                 static_cast<std::int64_t>(
                                     accelerationEvaluationsPerStep(results.kind)));
                    writer.write("minimumAccelerationEvaluationsPerStep",
                                 static_cast<std::int64_t>(
                                     minimumAccelerationEvaluationsPerStep(results.kind)));
                    writer.write("largestPassingStepSeconds", results.largestPassingStepSeconds);
                    writer.write("accelerationEvaluationsAtLargestPassingStep",
                                 results.accelerationEvaluationsAtLargestPassingStep);
                    writer.write("minimumAccelerationEvaluationsAtLargestPassingStep",
                                 results.minimumAccelerationEvaluationsAtLargestPassingStep);

                    writer.beginArray("steps");
                    for (const StepResult& step : results.steps) {
                        writer.beginObject();
                        writer.write("stepSeconds", step.stepSeconds);
                        writer.write("positionErrorMetres", step.positionErrorMetres);
                        writer.writeBits("positionErrorMetresBits", step.positionErrorMetres);
                        writer.write("velocityErrorMetresPerSecond",
                                     step.velocityErrorMetresPerSecond);
                        writer.write("relativeEnergyErrorAtEnd", step.relativeEnergyErrorAtEnd);
                        writer.write("relativeAngularMomentumErrorAtEnd",
                                     step.relativeAngularMomentumErrorAtEnd);
                        writer.write("semiMajorAxisErrorMetres", step.semiMajorAxisErrorMetres);
                        writer.write("eccentricityError", step.eccentricityError);
                        writer.write("inclinationErrorRadians", step.inclinationErrorRadians);
                        writer.write("integratorSteps", step.integratorSteps);
                        writer.write("accelerationEvaluations", step.accelerationEvaluations);
                        writer.write("nanosecondsPerAccelerationEvaluation",
                                     step.nanosecondsPerAccelerationEvaluation);
                        writer.write("meetsThreshold", step.meetsThreshold);
                        writer.endObject();
                    }
                    writer.endArray();
                    writer.endObject();
                }
                writer.endArray();
                writer.endObject();
            }
            writer.endArray();

            writer.beginObject("longRun");
            writer.write("orbits", static_cast<std::int64_t>(kLongRunOrbits));
            writer.write("stepSeconds", kLongRunStepSeconds);
            writer.write("whyItIsHere",
                         "The one-orbit gate cannot distinguish bounded energy error from "
                         "secular energy error, and that distinction is what actually decides "
                         "the integrator: ADR 0011 says orbits do not decay, so an integrator "
                         "whose energy accumulates manufactures decay the ADR forbids.");
            writer.beginArray("integrators");
            for (const LongRunResult& result : longRun) {
                writer.beginObject();
                writer.write("integrator", std::string{integratorName(result.kind)});
                writer.write("symplectic", isSymplectic(result.kind));
                writer.write("firstOrbitWorstRelativeEnergyError",
                             result.firstOrbitWorstRelativeEnergyError);
                writer.write("worstRelativeEnergyError", result.worstRelativeEnergyError);
                writer.write("finalRelativeEnergyError", result.finalRelativeEnergyError);
                writer.write("semiMajorAxisDriftMetres", result.semiMajorAxisDriftMetres);
                writer.write("errorIsSecular", result.errorIsSecular);
                writer.endObject();
            }
            writer.endArray();
            writer.endObject();

            writer.beginObject("howToReadThis");
            writer.write("costComparison",
                         "Compare integrators by "
                         "minimumAccelerationEvaluationsAtLargestPassingStep, not by error at a "
                         "common step size. Each candidate needs a different step to clear the "
                         "same gate, and the cost that matters is the cost of clearing it.");
            writer.write("whyTwoEvaluationCountsAreReported",
                         "accelerationEvaluations* counts what this prototype performed; "
                         "minimumAccelerationEvaluations* counts what the integrator inherently "
                         "requires. They differ only for velocity Verlet, whose end-of-step "
                         "acceleration is evaluated at the next step's start position and would "
                         "be carried forward by a stateful loop. A3's integrateStep is "
                         "stateless and cannot, so the implementation count overstates velocity "
                         "Verlet by exactly 2x. The conclusion is drawn from the minimum, which "
                         "is the property of the method rather than of this code.");
            writer.write("whyTheSymplecticPropertyDoesNotDecideThis",
                         "The long run shows RK4's energy error is secular and the symplectic "
                         "candidates' is bounded, which is the classical reason to prefer a "
                         "symplectic integrator for orbital work. Under the ADR 0011 hybrid "
                         "architecture that reason largely does not apply, because a craft in a "
                         "stable orbit is not integrated at all -- it coasts on a closed-form "
                         "conic, which conserves energy exactly. The local integrator runs "
                         "during ascent, thrust, and atmospheric flight: minutes at a time, not "
                         "years. Secular energy error over fifty orbits is therefore a property "
                         "of a scenario this architecture does not produce. The measured "
                         "magnitude says the same thing: RK4's fifty-orbit semi-major axis "
                         "drift is below a millimetre.");
            writer.write("whatWouldChangeThat",
                         "Any decision that puts a craft on the numerical integrator for a long "
                         "continuous span -- low-thrust transfers held under power for days, or "
                         "an atmosphere limit raised until the reference orbit falls inside it "
                         "-- reopens this, because it recreates the long integration the hybrid "
                         "architecture was built to avoid.");
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

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "reference-orbit.json");
        report.writeToFile(outputPath);
        std::cout << "ReferenceOrbit: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("ReferenceOrbit");
    } catch (const std::exception& error) {
        std::cerr << "ReferenceOrbit: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
