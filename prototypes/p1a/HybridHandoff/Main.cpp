/// The A3 local-to-analytical handoff gate.
///
/// P1a's accepted thresholds put one criterion on this scenario:
///
///   Local <-> analytical handoff - state discontinuity no greater than 1 m position and
///                                  1 mm/s velocity per transition.
///
/// ### The gate is nearly a tautology, and saying so is part of the result
///
/// If a coast anchors on the Cartesian state it was handed, the discontinuity is exactly zero,
/// because nothing is recomputed. Reporting that alone would clear a 1 m gate without having
/// examined anything, and P1a's measurement rules are explicit that a threshold must not be
/// answered by choosing what to measure.
///
/// So this scenario measures the gate against the two anchor representations a real design has
/// to choose between (see AnchorRepresentation): the Cartesian state, whose discontinuity is
/// structurally zero, and classical elements, which are what an orbital map draws and what a
/// save file wants to hold, and which cannot represent a state exactly. The gate then answers a
/// question that has two possible answers.
///
/// ### Three quantities, deliberately not conflated
///
///   Discontinuity - the jump at the instant of transition. This is what the gate bounds, and
///                   it is a self-consistency property of the handoff.
///   Cycling drift - what repeated transitions accumulate. A per-transition discontinuity
///                   inside tolerance can still be ruinous if a craft transitions thousands of
///                   times across a campaign, which a docked or intermittently-thrusting craft
///                   will. This is the number A2's iterated-conversion result taught us to ask
///                   for.
///   Divergence    - how far the numerical integrator drifts from the conic while both are
///                   running. Not a handoff cost at all: it belongs to the integrator, and
///                   ReferenceOrbit owns it. Measured here only so it cannot be mistaken for
///                   one of the other two.
///
/// ### Rejection coverage
///
/// A3's done criteria require that invalid transitions be "rejected explicitly rather than
/// producing a silent discontinuity". Every rejection reason is constructed and exercised here,
/// and the scenario fails if any condition is accepted or returns the wrong reason. A rejection
/// path that is never executed is not a safeguard.

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

/// P1a accepted thresholds, in SI units.
constexpr double kPositionThresholdMetres = 1.0;
constexpr double kVelocityThresholdMetresPerSecond = 1.0e-3;

/// Transition points sampled around one orbit.
///
/// Sixty-four rather than one, because a handoff is not a single event in a design: it happens
/// wherever the player leaves the atmosphere, cuts thrust, or times out of physics range, and a
/// discontinuity that depended on orbital phase would be invisible in a single-point
/// measurement.
constexpr int kPhaseSamples = 64;

/// Cycles in the repeated-handoff measurement.
constexpr int kCycleCount = 100'000;

/// The two anchor representations compared.
constexpr std::array<AnchorRepresentation, 2> kAnchorRepresentations{
    AnchorRepresentation::CartesianState,
    AnchorRepresentation::ClassicalElements,
};

struct OrbitCase {
    const char* name;
    double periapsisAltitudeMetres;
    double apoapsisAltitudeMetres;
};

constexpr std::array<OrbitCase, 3> kCases{{
    {"circular200km", 200.0e3, 200.0e3},
    {"elliptical200x2000km", 200.0e3, 2000.0e3},
    {"highlyEccentric200x100000km", 200.0e3, 100000.0e3},
}};

[[nodiscard]] TwoBodyState initialStateFor(const OrbitCase& orbitCase, double earthRadius,
                                           double mu) noexcept
{
    const double periapsis = earthRadius + orbitCase.periapsisAltitudeMetres;
    const double apoapsis = earthRadius + orbitCase.apoapsisAltitudeMetres;
    const double semiMajorAxis = 0.5 * (periapsis + apoapsis);
    const double speedAtPeriapsis = std::sqrt(mu * (2.0 / periapsis - 1.0 / semiMajorAxis));

    TwoBodyState state;
    state.position = Vec3{periapsis, 0.0, 0.0};
    state.velocity = Vec3{0.0, speedAtPeriapsis, 0.0};
    return state;
}

/// Discontinuity of one transition.
struct Discontinuity {
    double positionMetres{0.0};
    double velocityMetresPerSecond{0.0};
};

[[nodiscard]] Discontinuity difference(const TwoBodyState& before,
                                       const TwoBodyState& after) noexcept
{
    Discontinuity discontinuity;
    discontinuity.positionMetres = distance(before.position, after.position);
    discontinuity.velocityMetresPerSecond = distance(before.velocity, after.velocity);
    return discontinuity;
}

/// Results for one orbit case under one anchor representation.
struct HandoffResults {
    std::string caseName;
    AnchorRepresentation representation{AnchorRepresentation::CartesianState};

    Discontinuity worstIntoCoast;
    Discontinuity worstOutOfCoast;
    Discontinuity worstRoundTrip;
    /// Orbital phase, in radians, at which the worst round-trip discontinuity occurred.
    double worstRoundTripPhaseRadians{0.0};

    /// Accumulated departure from the starting state after kCycleCount begin/end cycles, with
    /// no time advancing. Isolates the transition's own cost from every other source.
    double cyclingPositionDriftMetres{0.0};
    double cyclingVelocityDriftMetresPerSecond{0.0};
    /// Cycles after which the state stopped changing bit for bit. Zero means it never settled.
    std::uint64_t cyclesToBitwiseFixedPoint{0};

    /// Element degradation across the cycles, which is what an element-anchored design actually
    /// loses: not position directly, but the orbit's identity.
    double semiMajorAxisDriftMetres{0.0};
    double eccentricityDrift{0.0};

    bool meetsPositionThreshold{false};
    bool meetsVelocityThreshold{false};
};

/// One constructed invalid transition and the reason it must produce.
struct RejectionCase {
    const char* name;
    const char* why;
    HandoffRejection expected;
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
        const double earthRadius = earth.meanRadiusMetres;

        HybridPropagator::Settings baseSettings;
        baseSettings.campaignEpochTdb =
            data.timeScales().utcToTdb(UtcDateTime{2026, 1, 1, 0, 0, 0.0});

        sol::proto::allocations::reset();

        // ------------------------------------------------------------------------------
        // Discontinuity and cycling, per case, per anchor representation.
        // ------------------------------------------------------------------------------
        std::vector<HandoffResults> allResults;
        allResults.reserve(kCases.size() * kAnchorRepresentations.size());

        for (const OrbitCase& orbitCase : kCases) {
            const TwoBodyState initial = initialStateFor(orbitCase, earthRadius, mu);
            const OrbitalElements initialElements = elementsFromState(initial, mu);

            for (const AnchorRepresentation representation : kAnchorRepresentations) {
                HybridPropagator::Settings settings = baseSettings;
                settings.anchorRepresentation = representation;
                const HybridPropagator propagator{system, ephemeris, settings};

                HandoffResults results;
                results.caseName = orbitCase.name;
                results.representation = representation;

                // Sample transitions all the way round the orbit.
                for (int sample = 0; sample < kPhaseSamples; ++sample) {
                    const double phaseSeconds =
                        initialElements.periodSeconds * sample / kPhaseSamples;
                    const TwoBodyState atPhase =
                        propagateKepler(initial, mu, phaseSeconds).state;

                    CraftState craft;
                    craft.state = atPhase;
                    craft.centralBodyNaifId = kNaifEarth;
                    craft.instant = CampaignInstant::fromSecondsRounded(phaseSeconds);
                    craft.regime = PropagationRegime::LocalNumerical;

                    const auto coasting = propagator.beginCoast(craft);
                    if (!coasting.has_value()) {
                        checks.fail(std::string{orbitCase.name}
                                    + ": a valid transition was refused");
                        continue;
                    }
                    const Discontinuity intoCoast =
                        difference(craft.state, coasting.value().state);

                    const auto back = propagator.endCoast(coasting.value());
                    const Discontinuity outOfCoast =
                        difference(coasting.value().state, back.value().state);
                    const Discontinuity roundTrip = difference(craft.state, back.value().state);

                    results.worstIntoCoast.positionMetres =
                        std::max(results.worstIntoCoast.positionMetres, intoCoast.positionMetres);
                    results.worstIntoCoast.velocityMetresPerSecond =
                        std::max(results.worstIntoCoast.velocityMetresPerSecond,
                                 intoCoast.velocityMetresPerSecond);
                    results.worstOutOfCoast.positionMetres =
                        std::max(results.worstOutOfCoast.positionMetres,
                                 outOfCoast.positionMetres);
                    results.worstOutOfCoast.velocityMetresPerSecond =
                        std::max(results.worstOutOfCoast.velocityMetresPerSecond,
                                 outOfCoast.velocityMetresPerSecond);

                    if (roundTrip.positionMetres > results.worstRoundTrip.positionMetres) {
                        results.worstRoundTrip.positionMetres = roundTrip.positionMetres;
                        results.worstRoundTripPhaseRadians =
                            elementsFromState(atPhase, mu).trueLongitude;
                    }
                    results.worstRoundTrip.velocityMetresPerSecond =
                        std::max(results.worstRoundTrip.velocityMetresPerSecond,
                                 roundTrip.velocityMetresPerSecond);
                }

                // Repeated transitions at a fixed instant. Time does not advance, so anything
                // that accumulates here is the transition's own cost and nothing else.
                CraftState cycling;
                cycling.state = initial;
                cycling.centralBodyNaifId = kNaifEarth;
                cycling.regime = PropagationRegime::LocalNumerical;

                TwoBodyState previous = cycling.state;
                for (int cycle = 0; cycle < kCycleCount; ++cycle) {
                    cycling = propagator.beginCoast(cycling).value();
                    cycling = propagator.endCoast(cycling).value();
                    if (results.cyclesToBitwiseFixedPoint == 0 && cycle > 0
                        && bitIdentical(previous, cycling.state)) {
                        results.cyclesToBitwiseFixedPoint =
                            static_cast<std::uint64_t>(cycle);
                    }
                    previous = cycling.state;
                }

                const Discontinuity cyclingDrift = difference(initial, cycling.state);
                results.cyclingPositionDriftMetres = cyclingDrift.positionMetres;
                results.cyclingVelocityDriftMetresPerSecond =
                    cyclingDrift.velocityMetresPerSecond;

                const OrbitalElements finalElements = elementsFromState(cycling.state, mu);
                results.semiMajorAxisDriftMetres =
                    finalElements.semiMajorAxis - initialElements.semiMajorAxis;
                results.eccentricityDrift =
                    finalElements.eccentricity - initialElements.eccentricity;

                results.meetsPositionThreshold =
                    results.worstRoundTrip.positionMetres <= kPositionThresholdMetres;
                results.meetsVelocityThreshold =
                    results.worstRoundTrip.velocityMetresPerSecond
                    <= kVelocityThresholdMetresPerSecond;

                allResults.push_back(std::move(results));
            }
        }

        // ------------------------------------------------------------------------------
        // Divergence: the integrator against the conic, which is not a handoff cost.
        // ------------------------------------------------------------------------------
        const TwoBodyState divergenceInitial = initialStateFor(kCases[0], earthRadius, mu);
        const double divergencePeriod =
            elementsFromState(divergenceInitial, mu).periodSeconds;

        HybridPropagator::Settings divergenceSettings = baseSettings;
        divergenceSettings.integrator = IntegratorKind::RungeKutta4;
        divergenceSettings.localStep = CampaignDuration::fromSecondsRounded(1.0);
        const HybridPropagator divergencePropagator{system, ephemeris, divergenceSettings};

        CraftState numerical;
        numerical.state = divergenceInitial;
        numerical.centralBodyNaifId = kNaifEarth;
        numerical.regime = PropagationRegime::LocalNumerical;

        const CampaignDuration oneOrbit =
            CampaignDuration::fromSecondsRounded(divergencePeriod);
        const AdvanceResult integrated =
            divergencePropagator.advanceTo(numerical, numerical.instant + oneOrbit, oneOrbit);
        const TwoBodyState conic =
            propagateKepler(divergenceInitial, mu, oneOrbit.seconds()).state;
        const double divergenceOverOneOrbit =
            distance(integrated.craft.state.position, conic.position);

        // ------------------------------------------------------------------------------
        // Rejection coverage.
        // ------------------------------------------------------------------------------
        const HybridPropagator propagator{system, ephemeris, baseSettings};

        const auto orbitingCraft = [&](double altitudeMetres) {
            const double r = earthRadius + altitudeMetres;
            CraftState craft;
            craft.state.position = Vec3{r, 0.0, 0.0};
            craft.state.velocity = Vec3{0.0, circularSpeed(r, mu), 0.0};
            craft.centralBodyNaifId = kNaifEarth;
            craft.regime = PropagationRegime::LocalNumerical;
            return craft;
        };

        constexpr std::array<RejectionCase, 7> kRejectionCases{{
            {"thrustActive",
             "a conic solves the unforced two-body problem, so coasting under thrust is not an "
             "approximation of the trajectory -- it is a different problem",
             HandoffRejection::ThrustActive},
            {"insideAtmosphere",
             "ADR 0011 keeps aerodynamic forces in play below the atmosphere limit, so a conic "
             "coast there would silently delete drag from a regime the ADR says has drag",
             HandoffRejection::InsideAtmosphere},
            {"degenerateConic",
             "a radial trajectory has no orbital plane, so no conic represents it",
             HandoffRejection::DegenerateConic},
            {"outsideSphereOfInfluence",
             "the craft is not inside the sphere it claims to orbit, so ADR 0011's one-body rule "
             "has already been violated",
             HandoffRejection::OutsideCentralBodySphereOfInfluence},
            {"belowSurface", "the craft has already arrived", HandoffRejection::BelowSurface},
            {"alreadyCoasting", "the regime is already the one being requested",
             HandoffRejection::AlreadyInRequestedRegime},
            {"unknownCentralBody", "the body is not in the ADR 0011 hierarchy",
             HandoffRejection::UnknownCentralBody},
        }};

        std::vector<std::pair<const RejectionCase*, HandoffRejection>> rejectionOutcomes;
        std::vector<bool> rejectionCorrect;

        for (const RejectionCase& rejectionCase : kRejectionCases) {
            CraftState craft = orbitingCraft(200.0e3);
            const std::string name{rejectionCase.name};

            if (name == "thrustActive") {
                craft.thrustActive = true;
            } else if (name == "insideAtmosphere") {
                craft = orbitingCraft(100.0e3);
            } else if (name == "degenerateConic") {
                craft.state.velocity = Vec3{-100.0, 0.0, 0.0};
            } else if (name == "outsideSphereOfInfluence") {
                craft.state.position = Vec3{2.0e9, 0.0, 0.0};
            } else if (name == "belowSurface") {
                craft.state.position = Vec3{1.0e6, 0.0, 0.0};
            } else if (name == "alreadyCoasting") {
                craft = propagator.beginCoast(craft).value();
            } else if (name == "unknownCentralBody") {
                craft.centralBodyNaifId = 599;
            }

            const auto outcome = propagator.coastEligibility(craft);
            const bool refusedCorrectly =
                !outcome.has_value() && outcome.error() == rejectionCase.expected;
            rejectionOutcomes.emplace_back(
                &rejectionCase,
                outcome.has_value() ? HandoffRejection::AlreadyInRequestedRegime
                                    : outcome.error());
            rejectionCorrect.push_back(refusedCorrectly);

            checks.check(refusedCorrectly,
                         name + ": refused explicitly, with the reason that names the condition");
        }

        const auto allocationCounts = sol::proto::allocations::snapshot();

        // ------------------------------------------------------------------------------
        // Gates
        // ------------------------------------------------------------------------------
        for (const HandoffResults& results : allResults) {
            const std::string label =
                results.caseName + ", "
                + std::string{anchorRepresentationName(results.representation)};
            checks.check(results.meetsPositionThreshold,
                         label + ": worst transition position discontinuity is within 1 m");
            checks.check(results.meetsVelocityThreshold,
                         label + ": worst transition velocity discontinuity is within 1 mm/s");

            if (results.representation == AnchorRepresentation::CartesianState) {
                // The structural claim, asserted as an exact zero rather than as "small". If
                // this ever becomes nonzero, the handoff has started recomputing something.
                checks.check(results.worstRoundTrip.positionMetres == 0.0
                                 && results.worstRoundTrip.velocityMetresPerSecond == 0.0,
                             label + ": a state-anchored transition is exactly lossless at every "
                                     "sampled orbital phase");
                checks.check(results.cyclingPositionDriftMetres == 0.0,
                             label + ": 100000 transition cycles accumulate exactly nothing");
            } else {
                // The element-anchored path must reach a fixed point, or repeated transitions
                // random-walk and the per-transition number says nothing about a campaign.
                checks.check(results.cyclesToBitwiseFixedPoint > 0,
                             label + ": repeated element-anchored transitions reach a bitwise "
                                     "fixed point rather than accumulating without bound");
            }
        }

        checks.check(divergenceOverOneOrbit > 0.0,
                     "the integrator does diverge from the conic, so the zero discontinuity "
                     "above is a property of the handoff and not of the two regimes agreeing");

        // ------------------------------------------------------------------------------
        // Report
        // ------------------------------------------------------------------------------
        ScenarioMetadata metadata;
        metadata.name = "orbit.hybridHandoff";
        metadata.version = "1";
        metadata.inputDescription =
            "Three Earth orbits -- 200 km circular, 200 x 2000 km, and 200 x 100000 km -- each "
            "handed between the local numerical regime and the analytical coast at 64 orbital "
            "phases, under both anchor representations. Each configuration then performs 100000 "
            "transition cycles at a fixed instant to isolate what repeated handoffs accumulate. "
            "Every eligibility rejection reason is constructed and exercised.";
        metadata.seed = 0;
        metadata.warmupIterations = 0;
        metadata.sampleCount = static_cast<std::uint64_t>(kPhaseSamples);

        ScenarioReport report(metadata);
        report.addResultWriter([&](JsonWriter& writer) {
            writer.beginObject("thresholds");
            writer.write("positionMetres", kPositionThresholdMetres);
            writer.write("velocityMetresPerSecond", kVelocityThresholdMetresPerSecond);
            writer.write("appliedTo", "each transition, not to a run total");
            writer.write("whyTwoAnchorRepresentationsAreMeasured",
                         "A coast anchored on the Cartesian state it was handed has a "
                         "structurally zero discontinuity, so gating only on that would clear "
                         "the threshold without examining anything. Classical elements are the "
                         "alternative a real design has to weigh -- they are what an orbital map "
                         "draws and what a save file wants to hold -- and they cannot represent "
                         "a state exactly.");
            writer.endObject();

            writer.beginObject("quantitiesNotConflated");
            writer.write("discontinuity",
                         "the jump at the instant of transition; this is what the gate bounds");
            writer.write("cyclingDrift",
                         "what repeated transitions accumulate; a per-transition value inside "
                         "tolerance can still be ruinous across a campaign");
            writer.write("divergence",
                         "how far the numerical integrator drifts from the conic while both run; "
                         "this belongs to the integrator, not to the handoff, and ReferenceOrbit "
                         "owns it");
            writer.endObject();

            writer.beginArray("configurations");
            for (const HandoffResults& results : allResults) {
                writer.beginObject();
                writer.write("case", results.caseName);
                writer.write("anchorRepresentation",
                             std::string{anchorRepresentationName(results.representation)});

                writer.beginObject("worstIntoCoast");
                writer.write("positionMetres", results.worstIntoCoast.positionMetres);
                writer.write("velocityMetresPerSecond",
                             results.worstIntoCoast.velocityMetresPerSecond);
                writer.endObject();

                writer.beginObject("worstOutOfCoast");
                writer.write("positionMetres", results.worstOutOfCoast.positionMetres);
                writer.write("velocityMetresPerSecond",
                             results.worstOutOfCoast.velocityMetresPerSecond);
                writer.endObject();

                writer.beginObject("worstRoundTrip");
                writer.write("positionMetres", results.worstRoundTrip.positionMetres);
                writer.writeBits("positionMetresBits", results.worstRoundTrip.positionMetres);
                writer.write("velocityMetresPerSecond",
                             results.worstRoundTrip.velocityMetresPerSecond);
                writer.write("atTrueLongitudeRadians", results.worstRoundTripPhaseRadians);
                writer.write("meetsPositionThreshold", results.meetsPositionThreshold);
                writer.write("meetsVelocityThreshold", results.meetsVelocityThreshold);
                writer.endObject();

                writer.beginObject("cycling");
                writer.write("cycles", static_cast<std::int64_t>(kCycleCount));
                writer.write("positionDriftMetres", results.cyclingPositionDriftMetres);
                writer.write("velocityDriftMetresPerSecond",
                             results.cyclingVelocityDriftMetresPerSecond);
                writer.write("cyclesToBitwiseFixedPoint", results.cyclesToBitwiseFixedPoint);
                writer.write("semiMajorAxisDriftMetres", results.semiMajorAxisDriftMetres);
                writer.write("eccentricityDrift", results.eccentricityDrift);
                writer.endObject();
                writer.endObject();
            }
            writer.endArray();

            writer.beginObject("divergence");
            writer.write("integrator", "runge-kutta-4");
            writer.write("stepSeconds", 1.0);
            writer.write("overOneOrbitMetres", divergenceOverOneOrbit);
            writer.write("note",
                         "Reported so the zero discontinuity above cannot be mistaken for the "
                         "two regimes agreeing. They do not agree; the handoff simply does not "
                         "ask them to.");
            writer.endObject();

            writer.beginArray("rejectionCoverage");
            for (std::size_t i = 0; i < rejectionOutcomes.size(); ++i) {
                writer.beginObject();
                writer.write("case", rejectionOutcomes[i].first->name);
                writer.write("why", rejectionOutcomes[i].first->why);
                writer.write("expectedReason",
                             std::string{rejectionName(rejectionOutcomes[i].first->expected)});
                writer.write("actualReason",
                             std::string{rejectionName(rejectionOutcomes[i].second)});
                writer.write("refusedCorrectly", rejectionCorrect[i]);
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

        const auto outputPath = sol::proto::parseOutputPath(argc, argv, "hybrid-handoff.json");
        report.writeToFile(outputPath);
        std::cout << "HybridHandoff: wrote " << outputPath.generic_string() << '\n';

        return checks.summarize("HybridHandoff");
    } catch (const std::exception& error) {
        std::cerr << "HybridHandoff: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
