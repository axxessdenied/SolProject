#include "Sol/Proto/Orbit/HybridPropagator.h"

#include "Sol/Proto/Orbit/KeplerPropagator.h"
#include "Sol/Proto/Orbit/OrbitalElements.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sol::proto::orbit {
namespace {

using frames::TdbEpoch;
using frames::Vec3;

[[nodiscard]] double positionDifference(const TwoBodyState& a, const TwoBodyState& b) noexcept
{
    return frames::distance(a.position, b.position);
}

[[nodiscard]] double velocityDifference(const TwoBodyState& a, const TwoBodyState& b) noexcept
{
    return frames::distance(a.velocity, b.velocity);
}

} // namespace

std::string_view regimeName(PropagationRegime regime) noexcept
{
    switch (regime) {
    case PropagationRegime::LocalNumerical:  return "local-numerical";
    case PropagationRegime::AnalyticalCoast: return "analytical-coast";
    }
    return "unknown";
}

std::string_view rejectionName(HandoffRejection rejection) noexcept
{
    switch (rejection) {
    case HandoffRejection::InsideAtmosphere:  return "inside-atmosphere";
    case HandoffRejection::ThrustActive:      return "thrust-active";
    case HandoffRejection::DegenerateConic:   return "degenerate-conic";
    case HandoffRejection::OutsideCentralBodySphereOfInfluence:
        return "outside-central-body-sphere-of-influence";
    case HandoffRejection::BelowSurface:              return "below-surface";
    case HandoffRejection::AlreadyInRequestedRegime:  return "already-in-requested-regime";
    case HandoffRejection::UnknownCentralBody:        return "unknown-central-body";
    case HandoffRejection::AnchorCentralBodyMismatch: return "anchor-central-body-mismatch";
    }
    return "unknown";
}

std::string_view coastEvaluationName(CoastEvaluation evaluation) noexcept
{
    switch (evaluation) {
    case CoastEvaluation::AnchoredFromEpoch:   return "anchored-from-epoch";
    case CoastEvaluation::SteppedFromPrevious: return "stepped-from-previous";
    }
    return "unknown";
}

std::string_view anchorRepresentationName(AnchorRepresentation representation) noexcept
{
    switch (representation) {
    case AnchorRepresentation::CartesianState:    return "cartesian-state";
    case AnchorRepresentation::ClassicalElements: return "classical-elements";
    }
    return "unknown";
}

std::string_view eventKindName(EventKind kind) noexcept
{
    switch (kind) {
    case EventKind::CoastBegan:               return "coast-began";
    case EventKind::CoastEnded:               return "coast-ended";
    case EventKind::SphereOfInfluenceEntered: return "sphere-of-influence-entered";
    case EventKind::SphereOfInfluenceExited:  return "sphere-of-influence-exited";
    }
    return "unknown";
}

HybridPropagator::HybridPropagator(const BodySystem& system, const ConicEphemeris& ephemeris,
                                   Settings settings) noexcept
    : m_system{&system}, m_ephemeris{&ephemeris}, m_settings{settings}
{
}

double HybridPropagator::centralGravitationalParameter(const CraftState& craft) const
{
    return m_system->body(craft.centralBodyNaifId).gravitationalParameter;
}

std::expected<void, HandoffRejection> HybridPropagator::coastEligibility(
    const CraftState& craft) const
{
    if (craft.regime == PropagationRegime::AnalyticalCoast) {
        return std::unexpected(HandoffRejection::AlreadyInRequestedRegime);
    }

    const GravitationalBody* body = nullptr;
    for (const GravitationalBody& candidate : m_system->bodies()) {
        if (candidate.naifId == craft.centralBodyNaifId) {
            body = &candidate;
            break;
        }
    }
    if (body == nullptr) {
        return std::unexpected(HandoffRejection::UnknownCentralBody);
    }

    // Thrust first: it is the cheapest test and the one whose violation is least visible in the
    // resulting numbers, because a thrusting craft on a conic looks like a perfectly ordinary
    // orbit that happens to be the wrong one.
    if (craft.thrustActive) {
        return std::unexpected(HandoffRejection::ThrustActive);
    }

    const double r = radius(craft.state);
    if (r <= body->meanRadiusMetres && body->meanRadiusMetres > 0.0) {
        return std::unexpected(HandoffRejection::BelowSurface);
    }
    if (r < body->atmosphereLimitRadiusMetres) {
        return std::unexpected(HandoffRejection::InsideAtmosphere);
    }
    if (r > body->sphereOfInfluenceRadiusMetres) {
        return std::unexpected(HandoffRejection::OutsideCentralBodySphereOfInfluence);
    }

    // The conic itself must exist. A radial trajectory has no orbital plane, and the universal
    // Kepler solver would return a state for it that is not wrong so much as meaningless.
    const OrbitalElements elements =
        elementsFromState(craft.state, body->gravitationalParameter);
    if (elements.shape == OrbitShape::Degenerate) {
        return std::unexpected(HandoffRejection::DegenerateConic);
    }

    return {};
}

std::expected<CraftState, HandoffRejection> HybridPropagator::beginCoast(
    const CraftState& craft) const
{
    if (const auto eligible = coastEligibility(craft); !eligible.has_value()) {
        return std::unexpected(eligible.error());
    }

    CraftState coasting = craft;
    coasting.regime = PropagationRegime::AnalyticalCoast;
    coasting.anchor.instant = craft.instant;
    coasting.anchor.centralBodyNaifId = craft.centralBodyNaifId;

    if (m_settings.anchorRepresentation == AnchorRepresentation::CartesianState) {
        // The handed-over state itself, not a re-derivation of it. This is what makes the
        // regime-change discontinuity zero rather than small.
        coasting.anchor.state = craft.state;
    } else {
        // Through classical elements and back. The round trip is the whole cost of an
        // element-anchored design, and performing it here rather than storing the elements
        // alongside the state is deliberate: a design that kept both would not be an
        // element-anchored design, and measuring it would flatter the option under test.
        const double mu = centralGravitationalParameter(craft);
        coasting.anchor.state = stateFromElements(elementsFromState(craft.state, mu), mu);
    }

    // The craft's own state follows its anchor. Otherwise an element-anchored coast would keep
    // a Cartesian state its own conic does not pass through, and the discontinuity would be
    // hidden until the first evaluation rather than taken at the transition where it belongs.
    coasting.state = coasting.anchor.state;
    return coasting;
}

std::expected<CraftState, HandoffRejection> HybridPropagator::endCoast(
    const CraftState& craft) const
{
    if (craft.regime == PropagationRegime::LocalNumerical) {
        return std::unexpected(HandoffRejection::AlreadyInRequestedRegime);
    }

    CraftState local = craft;
    local.regime = PropagationRegime::LocalNumerical;
    local.anchor = CoastAnchor{};
    return local;
}

std::expected<CraftState, HandoffRejection> HybridPropagator::rebaseTo(
    const CraftState& craft, int newCentralBodyNaifId) const
{
    if (newCentralBodyNaifId == craft.centralBodyNaifId) {
        return craft;
    }

    const TdbEpoch epoch = toTdb(craft.instant, m_settings.campaignEpochTdb);

    // Where the old centre sits relative to the new one. Adding it re-expresses the craft's
    // state without ever forming a barycentric magnitude, which is the same precision argument
    // that selected A2's hierarchical frame model.
    TwoBodyState offset;
    try {
        offset = m_ephemeris->stateRelativeTo(craft.centralBodyNaifId, newCentralBodyNaifId,
                                              epoch);
    } catch (const std::exception&) {
        return std::unexpected(HandoffRejection::UnknownCentralBody);
    }

    CraftState rebased = craft;
    rebased.centralBodyNaifId = newCentralBodyNaifId;
    rebased.state.position = craft.state.position + offset.position;
    rebased.state.velocity = craft.state.velocity + offset.velocity;

    // Re-anchor. A conic is about a body, so an anchor that survived the change of body would
    // describe an orbit around something the craft is no longer orbiting.
    if (rebased.regime == PropagationRegime::AnalyticalCoast) {
        rebased.anchor.state = rebased.state;
        rebased.anchor.instant = rebased.instant;
        rebased.anchor.centralBodyNaifId = newCentralBodyNaifId;
    }
    return rebased;
}

std::expected<CraftState, HandoffRejection> HybridPropagator::evaluateCoastAt(
    const CraftState& craft, CampaignInstant target) const
{
    if (craft.regime != PropagationRegime::AnalyticalCoast) {
        return std::unexpected(HandoffRejection::AlreadyInRequestedRegime);
    }
    if (craft.anchor.centralBodyNaifId != craft.centralBodyNaifId) {
        return std::unexpected(HandoffRejection::AnchorCentralBodyMismatch);
    }

    const double mu = centralGravitationalParameter(craft);

    CraftState evaluated = craft;
    evaluated.instant = target;

    if (m_settings.coastEvaluation == CoastEvaluation::AnchoredFromEpoch) {
        const double elapsed = (target - craft.anchor.instant).seconds();
        evaluated.state = propagateKepler(craft.anchor.state, mu, elapsed).state;
    } else {
        const double elapsed = (target - craft.instant).seconds();
        evaluated.state = propagateKepler(craft.state, mu, elapsed).state;
    }
    return evaluated;
}

int HybridPropagator::sphereOfInfluenceOwnerAt(const CraftState& craft) const
{
    const int current = craft.centralBodyNaifId;
    const double r = radius(craft.state);
    const double hysteresis = m_settings.boundaryHysteresisFraction;

    // Exit is tested before entry. The order is not arbitrary: a craft that has left its
    // central body's sphere is no longer governed by it at all, so any question about a
    // sibling's sphere is being asked in a frame that has already stopped applying.
    //
    // The thresholds are asymmetric by the hysteresis fraction: leaving requires passing
    // outside r_soi * (1 + f) and entering requires passing inside r_soi * (1 - f), so the band
    // between them belongs to whoever already owns the craft.
    if (!m_system->isRoot(current)
        && r > m_system->sphereOfInfluenceRadius(current) * (1.0 + hysteresis)) {
        return m_system->primaryOf(current);
    }

    const TdbEpoch epoch = toTdb(craft.instant, m_settings.campaignEpochTdb);
    for (const GravitationalBody& candidate : m_system->bodies()) {
        if (candidate.naifId == current) {
            continue;
        }
        if (m_system->primaryOf(candidate.naifId) != current) {
            continue;
        }
        const TwoBodyState candidateState =
            m_ephemeris->stateRelativeTo(candidate.naifId, current, epoch);
        if (frames::distance(craft.state.position, candidateState.position)
            < candidate.sphereOfInfluenceRadiusMetres * (1.0 - hysteresis)) {
            return candidate.naifId;
        }
    }
    return current;
}

double HybridPropagator::boundaryClearanceMetres(const CraftState& craft) const
{
    const int current = craft.centralBodyNaifId;
    const double r = radius(craft.state);
    const double hysteresis = m_settings.boundaryHysteresisFraction;

    double clearance = std::numeric_limits<double>::infinity();

    if (!m_system->isRoot(current)) {
        clearance = m_system->sphereOfInfluenceRadius(current) * (1.0 + hysteresis) - r;
    }

    const TdbEpoch epoch = toTdb(craft.instant, m_settings.campaignEpochTdb);
    for (const GravitationalBody& candidate : m_system->bodies()) {
        if (candidate.naifId == current
            || m_system->primaryOf(candidate.naifId) != current) {
            continue;
        }
        const TwoBodyState candidateState =
            m_ephemeris->stateRelativeTo(candidate.naifId, current, epoch);
        const double distanceToCandidate =
            frames::distance(craft.state.position, candidateState.position);
        clearance = std::min(clearance,
                             distanceToCandidate
                                 - candidate.sphereOfInfluenceRadiusMetres * (1.0 - hysteresis));
    }
    return clearance;
}

double HybridPropagator::boundaryClosingSpeedBound(const CraftState& craft) const
{
    const int current = craft.centralBodyNaifId;
    double bound = speed(craft.state);

    const TdbEpoch epoch = toTdb(craft.instant, m_settings.campaignEpochTdb);
    for (const GravitationalBody& candidate : m_system->bodies()) {
        if (candidate.naifId == current
            || m_system->primaryOf(candidate.naifId) != current) {
            continue;
        }
        const TwoBodyState candidateState =
            m_ephemeris->stateRelativeTo(candidate.naifId, current, epoch);
        bound += frames::length(candidateState.velocity);
    }
    return bound;
}

TwoBodyState HybridPropagator::conicStateAt(const CraftState& craft,
                                            CampaignInstant target) const
{
    const double mu = centralGravitationalParameter(craft);

    // Propagate from the coast anchor when there is one, rather than from the craft's current
    // state.
    //
    // This is what makes a crossing instant tick-invariant, and getting it wrong was a real
    // defect rather than a theoretical one. When the probe propagated from wherever the current
    // increment happened to start, the crossing predicate was not a function of the instant
    // alone -- it depended on which increment was asking. Two runs of the same trajectory at
    // different warp factors then bracketed the boundary differently and disagreed about when
    // it was crossed, by as much as twenty-three minutes over a three-day escape. Anchoring the
    // probe makes the predicate a pure function of the instant, so any interval that brackets
    // the crossing converges to the same nanosecond.
    //
    // A craft in the local numerical regime has no anchor, so its probe still runs from the
    // current state. Crossings detected there are therefore only as reproducible as the
    // integrator's path to them. That is an accepted limitation rather than an oversight: a
    // sphere-of-influence boundary is hundreds of thousands of kilometres from any atmosphere,
    // so a craft reaching one is coasting.
    if (craft.regime == PropagationRegime::AnalyticalCoast
        && m_settings.coastEvaluation == CoastEvaluation::AnchoredFromEpoch
        && craft.anchor.centralBodyNaifId == craft.centralBodyNaifId) {
        const double elapsedFromAnchor = (target - craft.anchor.instant).seconds();
        return propagateKepler(craft.anchor.state, mu, elapsedFromAnchor).state;
    }

    const double elapsed = (target - craft.instant).seconds();
    return propagateKepler(craft.state, mu, elapsed).state;
}

AdvanceResult HybridPropagator::advanceTo(const CraftState& craft, CampaignInstant target,
                                          CampaignDuration tick) const
{
    AdvanceResult result;
    result.craft = craft;

    if (tick.nanoseconds() <= 0) {
        return result;
    }

    std::uint64_t sequence = 0;

    /// Advances the craft to @p to within its current regime, without transition handling.
    const auto stepWithinRegime = [&](CraftState state, CampaignInstant to) -> CraftState {
        if (state.instant == to) {
            return state;
        }
        if (state.regime == PropagationRegime::AnalyticalCoast) {
            const auto evaluated = evaluateCoastAt(state, to);
            ++result.coastEvaluations;
            if (evaluated.has_value()) {
                return evaluated.value();
            }
            // Cannot happen unless an invariant broke. The instant still advances, because a
            // state that stayed put would spin this loop forever and present as a hang rather
            // than as the invariant violation it is. The counter is what makes it visible.
            ++result.coastEvaluationFailures;
            state.instant = to;
            return state;
        }

        const double mu = centralGravitationalParameter(state);
        const std::int64_t stepNanoseconds = m_settings.localStep.nanoseconds();
        std::int64_t remaining = (to - state.instant).nanoseconds();

        // Whole fixed steps first. The step duration is an exact integer count of nanoseconds
        // converted once, so every full step of a run uses bit-identical arithmetic.
        const double fullStepSeconds = m_settings.localStep.seconds();
        while (remaining >= stepNanoseconds) {
            state.state = integrateStep(m_settings.integrator, state.state, mu, fullStepSeconds);
            remaining -= stepNanoseconds;
            ++result.integratorSteps;
            result.accelerationEvaluations +=
                static_cast<std::uint64_t>(accelerationEvaluationsPerStep(m_settings.integrator));
        }
        // One truncated step to land exactly on the target instant.
        //
        // This is the single departure from strict fixed-stepping, and it is what makes
        // scheduled events possible: an event that could only be honoured at a multiple of the
        // step would move whenever the step changed. The remainder is an exact function of two
        // integer instants, so the departure costs nothing in reproducibility.
        if (remaining > 0) {
            const double remainderSeconds = static_cast<double>(remaining) * 1.0e-9;
            state.state = integrateStep(m_settings.integrator, state.state, mu, remainderSeconds);
            ++result.integratorSteps;
            result.accelerationEvaluations +=
                static_cast<std::uint64_t>(accelerationEvaluationsPerStep(m_settings.integrator));
        }
        state.instant = to;
        return state;
    };

    /// The first instant in (lo, hi] at which the sphere-of-influence owner differs from the
    /// craft's central body, found by bisection on the conic.
    ///
    /// Integer bisection on nanoseconds, so it terminates exactly rather than on a tolerance
    /// and produces the same instant on every run. Interpolating on the conic rather than by
    /// re-integrating is justified in the header: a boundary crossing is far outside any
    /// atmosphere, so ADR 0011 says the motion there is two-body and drag-free.
    const auto refineCrossing = [&](const CraftState& from, CampaignInstant lo,
                                    CampaignInstant hi) -> CampaignInstant {
        const auto ownerDiffersAt = [&](CampaignInstant when) {
            CraftState probe = from;
            probe.state = conicStateAt(from, when);
            probe.instant = when;
            return sphereOfInfluenceOwnerAt(probe) != from.centralBodyNaifId;
        };

        // The crossing is already behind the bracket's lower end. Bisection would converge to
        // lo + 1 ns and report a boundary the craft passed earlier, so the honest answer is lo
        // itself. Structurally this cannot happen once the probe is a pure function of the
        // instant -- the previous increment's own end-of-increment test would have caught it --
        // and it is handled rather than assumed because the alternative is a wrong instant that
        // looks precise.
        if (ownerDiffersAt(lo)) {
            return lo;
        }

        while ((hi - lo).nanoseconds() > 1) {
            const CampaignInstant middle = CampaignInstant::fromNanoseconds(
                lo.nanoseconds() + (hi - lo).nanoseconds() / 2);
            if (ownerDiffersAt(middle)) {
                hi = middle;
            } else {
                lo = middle;
            }
        }
        return hi;
    };

    std::uint64_t incrementCount = 0;

    while (result.craft.instant < target) {
        if (static_cast<std::uint64_t>(result.events.size())
            >= m_settings.maxEventsPerAdvance) {
            result.eventLimitReached = true;
            break;
        }
        if (++incrementCount > m_settings.maxIncrementsPerAdvance) {
            result.incrementLimitReached = true;
            break;
        }

        const CampaignInstant tickEnd = result.craft.instant + tick;
        CampaignInstant increment = tickEnd < target ? tickEnd : target;

        // Conservative boundary stepping.
        //
        // The boundary test is sampled at increment ends, so an excursion that begins and ends
        // inside one increment is invisible. Shrinking the increment until the craft provably
        // cannot reach a boundary within it removes that blind spot: clearance divided by an
        // upper bound on closing speed is a duration during which no crossing is possible,
        // whatever the trajectory does.
        if (m_settings.conservativeBoundaryStepping) {
            const double clearance = boundaryClearanceMetres(result.craft);
            const double closingSpeed = boundaryClosingSpeedBound(result.craft);
            if (clearance > 0.0 && closingSpeed > 0.0) {
                const double safeSeconds = clearance / closingSpeed;
                bool hitFloor = false;
                while ((increment - result.craft.instant).seconds() > safeSeconds) {
                    const CampaignDuration halved = CampaignDuration::fromNanoseconds(
                        (increment - result.craft.instant).nanoseconds() / 2);
                    if (halved <= m_settings.minimumIncrement) {
                        hitFloor = true;
                        break;
                    }
                    increment = result.craft.instant + halved;
                    ++result.incrementSubdivisions;
                }
                if (hitFloor) {
                    ++result.incrementsAtSubdivisionFloor;
                }
            }
        }

        const CraftState stepped = stepWithinRegime(result.craft, increment);
        const int ownerAtEnd = sphereOfInfluenceOwnerAt(stepped);

        if (ownerAtEnd == stepped.centralBodyNaifId) {
            result.craft = stepped;
            continue;
        }

        // A boundary was crossed somewhere inside this increment. Place it exactly, move the
        // craft there in its own regime, and hand ownership over at that single instant.
        const CampaignInstant crossing =
            refineCrossing(result.craft, result.craft.instant, increment);
        const CraftState atCrossing = stepWithinRegime(result.craft, crossing);

        const int newOwner = sphereOfInfluenceOwnerAt(atCrossing);
        if (newOwner == atCrossing.centralBodyNaifId) {
            // The refinement disagrees with the end-of-increment test. That can only happen if
            // the craft grazed the boundary and returned, so there is no transition to record;
            // accepting the stepped state and moving on is correct and terminates.
            result.craft = stepped;
            continue;
        }

        const auto rebased = rebaseTo(atCrossing, newOwner);
        if (!rebased.has_value()) {
            result.craft = stepped;
            continue;
        }

        // The discontinuity, measured by re-expressing back and differencing. This is the real
        // round-trip cost of a change of origin, not an estimate of it.
        const auto returned = rebaseTo(rebased.value(), atCrossing.centralBodyNaifId);

        TransitionEvent event;
        event.kind = m_system->primaryOf(atCrossing.centralBodyNaifId) == newOwner
                       ? EventKind::SphereOfInfluenceExited
                       : EventKind::SphereOfInfluenceEntered;
        event.instant = crossing;
        event.sequence = sequence++;
        event.fromCentralBodyNaifId = atCrossing.centralBodyNaifId;
        event.toCentralBodyNaifId = newOwner;
        if (returned.has_value()) {
            event.positionDiscontinuityMetres =
                positionDifference(returned.value().state, atCrossing.state);
            event.velocityDiscontinuityMetresPerSecond =
                velocityDifference(returned.value().state, atCrossing.state);
        }
        result.events.push_back(event);
        result.craft = rebased.value();

        // Re-validate the coast against the *new* central body.
        //
        // Eligibility was checked when the coast began, against the body the craft was orbiting
        // then. A change of primary can invalidate it: a state that describes a perfectly good
        // conic about Earth can be radial about the Moon, and a radial trajectory has no conic
        // at all. A3 found this the hard way -- a lunar approach on an exactly head-on course
        // was handed to the Moon and coasted on a degenerate conic, producing positions that
        // were smooth, plausible, and meaningless.
        //
        // Ending the coast is the right response rather than refusing the crossing. The
        // numerical integrator represents radial motion without difficulty; it is only the
        // closed-form conic that cannot. So ownership passes to the new body as physics
        // requires, and the regime drops to the one that can actually propagate the state.
        if (result.craft.regime == PropagationRegime::AnalyticalCoast) {
            CraftState asIfLocal = result.craft;
            asIfLocal.regime = PropagationRegime::LocalNumerical;
            if (!coastEligibility(asIfLocal).has_value()) {
                if (const auto ended = endCoast(result.craft); ended.has_value()) {
                    TransitionEvent coastEnded;
                    coastEnded.kind = EventKind::CoastEnded;
                    coastEnded.instant = crossing;
                    coastEnded.sequence = sequence++;
                    coastEnded.fromCentralBodyNaifId = newOwner;
                    coastEnded.toCentralBodyNaifId = newOwner;
                    // Handing the state to the integrator changes nothing about it, so this is
                    // measured as zero rather than assumed to be.
                    coastEnded.positionDiscontinuityMetres =
                        positionDifference(result.craft.state, ended.value().state);
                    coastEnded.velocityDiscontinuityMetresPerSecond =
                        velocityDifference(result.craft.state, ended.value().state);
                    result.events.push_back(coastEnded);
                    result.craft = ended.value();
                }
            }
        }
    }

    return result;
}

} // namespace sol::proto::orbit
