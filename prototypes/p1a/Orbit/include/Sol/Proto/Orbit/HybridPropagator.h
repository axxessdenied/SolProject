#pragma once

#include "Sol/Proto/Orbit/BodySystem.h"
#include "Sol/Proto/Orbit/CampaignClock.h"
#include "Sol/Proto/Orbit/ConicEphemeris.h"
#include "Sol/Proto/Orbit/Integrator.h"
#include "Sol/Proto/Orbit/TwoBody.h"

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace sol::proto::orbit {

/// Which of the two regimes owns a craft's state.
///
/// "Owns" is the operative word and it is the whole transition contract in one sentence:
/// exactly one regime is authoritative at any instant, and the other holds nothing. There is no
/// period during which both are running and being reconciled, because reconciling two
/// authoritative states is how the discontinuities this increment exists to bound get
/// introduced.
enum class PropagationRegime : std::uint8_t {
    /// Fixed-step numerical integration. The regime in which non-gravitational forces --
    /// thrust, and the aerodynamic forces ADR 0011 explicitly does not exclude from flight --
    /// are permitted to act.
    LocalNumerical,
    /// Closed-form conic evaluation about the central body. Gravity only, by definition.
    AnalyticalCoast,
};

[[nodiscard]] std::string_view regimeName(PropagationRegime regime) noexcept;

/// Why a requested transition was refused.
///
/// A3's done criteria require that invalid transition conditions be "rejected explicitly rather
/// than producing a silent discontinuity". This enum is that rejection. Each value names a
/// specific physical or numerical condition under which the analytical coast would produce a
/// state that looks reasonable and is wrong, which is the failure mode a tolerance check cannot
/// catch -- the wrong answer is smooth.
enum class HandoffRejection : std::uint8_t {
    /// Below the body's atmosphere limit, where ADR 0011 keeps aerodynamic forces in play. A
    /// conic coast here would silently delete drag from a regime the ADR says has drag.
    InsideAtmosphere,
    /// Thrust is active. A conic is a solution of the unforced two-body problem, so coasting
    /// under thrust does not approximate the trajectory badly -- it solves a different problem.
    ThrustActive,
    /// Zero angular momentum or zero radius: no conic exists. See OrbitShape::Degenerate.
    DegenerateConic,
    /// The craft is not inside the sphere of influence it claims to be orbiting, so the ADR
    /// 0011 rule that exactly one body gravitates has already been violated.
    OutsideCentralBodySphereOfInfluence,
    /// Below the central body's surface. Physically the craft has already arrived.
    BelowSurface,
    /// The craft is already in the regime being requested.
    AlreadyInRequestedRegime,
    /// The central body is not in the hierarchy.
    UnknownCentralBody,
    /// A coast anchor survives from before a change of central body, so the conic it
    /// propagates is about the wrong body. Structurally impossible if every rebase re-anchors,
    /// which is why this exists: it is the check that the invariant held, not a case to
    /// recover from.
    AnchorCentralBodyMismatch,
};

[[nodiscard]] std::string_view rejectionName(HandoffRejection rejection) noexcept;

/// How a coast evaluates its state at a later instant.
///
/// The decisive architectural choice for time warp, and A3 measures both rather than assuming
/// one. It is the same question A2 asked about frame conversion -- re-derive from one stored
/// authoritative state, or consume the previous result -- arriving in a second subsystem, which
/// is itself worth noticing.
enum class CoastEvaluation : std::uint8_t {
    /// Propagate from the anchor stored when the coast began, by the total elapsed time.
    ///
    /// Warp-invariant by construction: the result at a given instant does not depend on how
    /// many increments were used to reach it, because the increments are never composed.
    AnchoredFromEpoch,
    /// Propagate from the previous evaluation's result, by the increment.
    ///
    /// The intuitive implementation, and the one that makes warp factor a physical parameter:
    /// each increment applies its own rounding, so a run at one tick size and a run at another
    /// visit different states.
    SteppedFromPrevious,
};

[[nodiscard]] std::string_view coastEvaluationName(CoastEvaluation evaluation) noexcept;

/// What a coast anchor stores.
///
/// The other decision the transition contract has to make, and the one that decides whether the
/// handoff gate is a measurement or a tautology.
///
/// Anchoring on the Cartesian state makes the local-to-analytical discontinuity exactly zero,
/// because the state is handed over rather than recomputed. That is not a clever result -- it
/// is what "handed over" means -- and reporting it alone would answer P1a's 1 m gate without
/// having examined anything.
///
/// Anchoring on classical elements is the obvious alternative and is what many designs choose:
/// elements are compact, they are what an orbital map displays, they are what a save file wants
/// to hold, and they make the orbit's shape directly inspectable. They also cannot represent a
/// Cartesian state exactly, so every transition through them costs a conversion round trip.
/// A3 measures that cost rather than assuming it is negligible or assuming it is fatal.
enum class AnchorRepresentation : std::uint8_t {
    /// Store the handed-over position and velocity verbatim.
    CartesianState,
    /// Store classical elements, and rebuild the state from them.
    ClassicalElements,
};

[[nodiscard]] std::string_view anchorRepresentationName(AnchorRepresentation representation) noexcept;

/// The state a coast propagates from.
struct CoastAnchor {
    TwoBodyState state{};
    CampaignInstant instant{};
    /// The body the anchor state is relative to. Carried explicitly so a rebase cannot leave an
    /// anchor pointing at the previous central body -- which would be a silent discontinuity of
    /// hundreds of thousands of kilometres, and exactly the class of error the explicit
    /// rejections above exist to make impossible.
    int centralBodyNaifId{0};
};

/// A craft's complete authoritative state under ADR 0011.
struct CraftState {
    /// Position and velocity relative to the central body's centre, on ICRF-parallel axes.
    TwoBodyState state{};
    int centralBodyNaifId{0};
    CampaignInstant instant{};
    PropagationRegime regime{PropagationRegime::LocalNumerical};
    /// Whether a non-gravitational force is acting. A3 never models the force itself; the flag
    /// exists so the eligibility rule that depends on it is real code rather than a comment.
    bool thrustActive{false};
    /// Meaningful only while coasting.
    CoastAnchor anchor{};
};

/// What kind of transition an event records.
enum class EventKind : std::uint8_t {
    CoastBegan,
    CoastEnded,
    SphereOfInfluenceEntered,
    SphereOfInfluenceExited,
};

[[nodiscard]] std::string_view eventKindName(EventKind kind) noexcept;

/// One transition, with the discontinuity it introduced.
struct TransitionEvent {
    EventKind kind{EventKind::CoastBegan};
    CampaignInstant instant{};
    /// Monotonic within one advance. The chronology A3's determinism criterion is asserted
    /// against: identical inputs must reproduce this ordering exactly, not merely the final
    /// state.
    std::uint64_t sequence{0};
    int fromCentralBodyNaifId{0};
    int toCentralBodyNaifId{0};
    /// Position and velocity discontinuity introduced by this transition, measured through the
    /// real code path rather than asserted.
    ///
    /// For a regime change these are zero by construction -- the state is handed over, not
    /// recomputed -- and A3 reports the measured zero rather than skipping the measurement,
    /// because "zero by construction" is a claim about the code that the code should be made to
    /// demonstrate.
    ///
    /// For a sphere-of-influence change they are not zero: the state is re-expressed against a
    /// new origin whose position comes from the ephemeris, and the discontinuity is the
    /// round-trip error of that re-expression.
    double positionDiscontinuityMetres{0.0};
    double velocityDiscontinuityMetresPerSecond{0.0};
};

/// Result of advancing a craft over an interval of campaign time.
struct AdvanceResult {
    CraftState craft{};
    std::vector<TransitionEvent> events;
    std::uint64_t integratorSteps{0};
    std::uint64_t coastEvaluations{0};
    std::uint64_t accelerationEvaluations{0};
    /// Coast evaluations that were refused. Structurally zero -- the only refusals possible are
    /// invariant violations the propagator itself maintains -- so a nonzero value here is a bug
    /// report, and it is counted rather than swallowed because the alternative is an advance
    /// that quietly stops moving.
    std::uint64_t coastEvaluationFailures{0};
    /// Increments halved because the craft could have reached a boundary inside them. The cost
    /// of conservative boundary stepping, measured rather than assumed negligible.
    std::uint64_t incrementSubdivisions{0};
    /// Increments that reached the subdivision floor while still not provably safe. Nonzero
    /// means a boundary excursion shorter than the floor could have been missed in this run.
    std::uint64_t incrementsAtSubdivisionFloor{0};
    /// True when the advance stopped on its increment cap rather than at its target instant.
    /// The result is a partial advance and must not be compared against a complete one.
    bool incrementLimitReached{false};
    /// True when the advance stopped because it hit its transition-event cap rather than
    /// because it reached the target instant. Reported rather than thrown: an advance that
    /// silently returned early would look like a successful short run.
    bool eventLimitReached{false};
};

/// The ADR 0011 hybrid propagator: fixed-step local integration, closed-form analytical coast,
/// explicit eligibility, bidirectional handoff, and sphere-of-influence crossings as discrete
/// scheduled events.
class HybridPropagator {
public:
    struct Settings {
        IntegratorKind integrator{IntegratorKind::Yoshida4};
        /// Fixed local-physics step. Never adaptive; see Integrator.h for why.
        CampaignDuration localStep{CampaignDuration::fromNanoseconds(15'625'000)}; // 1/64 s
        CoastEvaluation coastEvaluation{CoastEvaluation::AnchoredFromEpoch};
        AnchorRepresentation anchorRepresentation{AnchorRepresentation::CartesianState};
        /// TDB instant the campaign clock's zero corresponds to, for ephemeris lookups.
        frames::TdbEpoch campaignEpochTdb{};
        /// Upper bound on transitions in one advance, so a craft sitting exactly on a boundary
        /// terminates instead of looping.
        std::uint64_t maxEventsPerAdvance{64};

        /// Dead band around a sphere-of-influence boundary, as a fraction of its radius.
        ///
        /// Exit requires crossing outward past r_soi * (1 + f); entry requires crossing inward
        /// past r_soi * (1 - f). Without it, ownership changes hands whenever rounding tips a
        /// comparison, and a craft grazing a boundary chatters: A3 measured a craft leaving the
        /// Moon's sphere and re-entering it fifteen milliseconds later, then repeating.
        ///
        /// A dead band is defensible here in a way it would not be in an exact model, because
        /// the sphere of influence is already an approximation -- the Laplace radius is a
        /// convenient place to switch primaries, not a physical surface. Ownership persisting a
        /// few kilometres past it changes nothing the model claims. At the default it is 92 km
        /// on Earth's 9.2e8 m sphere and 6.6 km on the Moon's.
        double boundaryHysteresisFraction{1.0e-4};

        /// Whether an increment is subdivided when the craft could reach a boundary inside it.
        ///
        /// Sampling the boundary test only at increment ends lets a warp tick step straight over
        /// a short excursion. A3 measured this too: a craft that left the Moon's sphere and
        /// returned within one 1000 s tick was never seen to leave, and the run diverged from
        /// the same trajectory at finer ticks from that point on. With this enabled the
        /// increment is halved until the craft provably cannot reach a boundary within it.
        bool conservativeBoundaryStepping{true};

        /// Floor on subdivision, so a craft that sits arbitrarily close to a boundary
        /// terminates. Excursions shorter than this can still be missed, which is the residual
        /// limitation and is reported rather than hidden.
        CampaignDuration minimumIncrement{CampaignDuration::fromNanoseconds(1'000'000)};

        /// Upper bound on increments in one advance.
        ///
        /// Conservative subdivision has a worst case: a craft whose orbit runs tangent to a
        /// boundary keeps its clearance near zero, so every increment shrinks to the floor and
        /// the advance crawls. That is a real trajectory, not a contrived one -- a marginal
        /// capture produces it -- so the advance is bounded and reports that it was, rather
        /// than appearing to hang.
        std::uint64_t maxIncrementsPerAdvance{2'000'000};
    };

    HybridPropagator(const BodySystem& system, const ConicEphemeris& ephemeris,
                     Settings settings) noexcept;

    /// The eligibility rules, evaluated without changing anything.
    ///
    /// Separated from beginCoast so a caller -- or a scenario measuring the rules themselves --
    /// can ask why a coast is unavailable without attempting one.
    [[nodiscard]] std::expected<void, HandoffRejection> coastEligibility(
        const CraftState& craft) const;

    /// Hands the craft from local numerical propagation to the analytical coast.
    [[nodiscard]] std::expected<CraftState, HandoffRejection> beginCoast(
        const CraftState& craft) const;

    /// Hands the craft back to local numerical propagation.
    ///
    /// Has no eligibility rules of its own beyond already coasting: returning to a numerical
    /// integrator is always valid, because the integrator is a superset of what the coast can
    /// represent. The asymmetry is deliberate and is the reason the handoff is safe in one
    /// direction and gated in the other.
    [[nodiscard]] std::expected<CraftState, HandoffRejection> endCoast(
        const CraftState& craft) const;

    /// Re-expresses the craft's state relative to @p newCentralBodyNaifId.
    ///
    /// The operation a sphere-of-influence crossing performs. State ownership passes from one
    /// body to the other at a single instant with no overlap, which is what ADR 0011's "discrete,
    /// scheduled events with explicit state ownership on each side" means in code.
    ///
    /// A coasting craft is re-anchored against the new body, and its eligibility is **re-checked
    /// against that body**: a state that is a valid conic about Earth can be radial about the
    /// Moon. When the new conic is degenerate the returned craft is in LocalNumerical, which
    /// represents radial motion without difficulty. This is the only operation that changes a
    /// craft's central body, so performing the re-check here rather than in advanceTo makes the
    /// guarantee hold for every caller instead of one call site.
    [[nodiscard]] std::expected<CraftState, HandoffRejection> rebaseTo(
        const CraftState& craft, int newCentralBodyNaifId) const;

    /// Evaluates a coasting craft's state at @p target.
    ///
    /// Requires the craft to be coasting. The evaluation mode in Settings decides whether the
    /// propagation runs from the anchor or from the craft's current state.
    [[nodiscard]] std::expected<CraftState, HandoffRejection> evaluateCoastAt(
        const CraftState& craft, CampaignInstant target) const;

    /// Advances @p craft to @p target in increments of @p tick.
    ///
    /// @p tick is the time-warp granularity: how much campaign time each advance increment
    /// covers. A real-time run uses a small tick and a warped run a large one, and the
    /// warp-equivalence gate is the claim that the two reach the same state.
    ///
    /// Sphere-of-influence crossings are detected per increment and then refined to the
    /// nanosecond by bisection on the conic, so the transition instant does not depend on the
    /// tick that happened to straddle it. That refinement is what makes warp equivalence
    /// achievable at all: a crossing quantised to the tick would move by up to the tick's worth
    /// of trajectory when the warp factor changed.
    [[nodiscard]] AdvanceResult advanceTo(const CraftState& craft, CampaignInstant target,
                                          CampaignDuration tick) const;

    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }

    /// The gravitational parameter of @p craft's central body.
    [[nodiscard]] double centralGravitationalParameter(const CraftState& craft) const;

private:
    /// Which body's sphere of influence @p craft is in, given its current central body.
    ///
    /// Returns the craft's current central body when no transition is due. Only ever moves one
    /// level, because a single increment that crossed two boundaries would have to order them,
    /// and the refinement loop in advanceTo handles that by taking the levels one at a time.
    [[nodiscard]] int sphereOfInfluenceOwnerAt(const CraftState& craft) const;

    /// Shortest distance from @p craft to any boundary it could cross, in metres, measured
    /// against the hysteresis-adjusted thresholds the owner test actually uses. Zero or
    /// negative when a transition is already due.
    [[nodiscard]] double boundaryClearanceMetres(const CraftState& craft) const;

    /// Upper bound on how fast @p craft can close on a boundary, in m/s: its own speed plus the
    /// speed of any body whose sphere it might enter. Deliberately an over-estimate, because
    /// the subdivision built on it is only conservative if the bound is.
    [[nodiscard]] double boundaryClosingSpeedBound(const CraftState& craft) const;

    /// Propagates a state to @p target on the conic, regardless of regime.
    ///
    /// Used by the crossing refinement. Legitimate for a craft in either regime: a boundary
    /// crossing happens far outside any atmosphere, so the motion there is two-body and
    /// drag-free by ADR 0011 and the conic is the correct interpolant rather than an
    /// approximation to the integrator.
    [[nodiscard]] TwoBodyState conicStateAt(const CraftState& craft,
                                            CampaignInstant target) const;

    const BodySystem* m_system;
    const ConicEphemeris* m_ephemeris;
    Settings m_settings;
};

} // namespace sol::proto::orbit
