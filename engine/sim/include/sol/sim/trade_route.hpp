#pragma once

// Where a coarse trader's body goes (engine plan Phase 8x, stage 2).
//
// Economy::route() says which in-system leg a trader is flying and how far
// along it is; this turns that into geometry. It lives here as a pure header
// for docking.hpp's and pilot_tips.hpp's reason: game/src has no test suite,
// so a rule that cannot be asserted headlessly gets verified by screenshot
// forever — and both rules below are the kind that look right in a screenshot
// while being wrong somewhere the player has not flown yet.
//
// The leg itself is not invented here. traderLegSeconds is "in-system travel
// per endpoint" and jumpSeconds is per gate transit, so a haul has always
// been depart -> jumps -> arrive; a departing trader runs from its origin
// station out to a gate, and an arriving one runs from a gate in to its
// destination station.

#include "sol/sim/economy.hpp"
#include "sol/sim/universe.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace sol::sim {

inline constexpr std::uint32_t kNoGate = 0xffff'ffffu;

// The gate in `gates` that starts a shortest path from `here` to `target`,
// or kNoGate when none does.
//
// A hop count alone cannot answer this — it says how far the destination is,
// not which door leads there. The test is one step of the same BFS the hop
// table was built with: a gate is on a shortest path exactly when standing on
// its far side leaves you one hop closer than you are now. `hops` is any
// callable (from, to) -> hop count, so this stays independent of where the
// table lives and can be tested against a hand-written graph.
//
// Ties are broken by gate order, which is generation order, so the choice is
// deterministic for a seed and two traders on the same route agree.
template <typename HopFn>
[[nodiscard]] std::uint32_t gateTowardSystem(std::span<const GateSpec> gates, std::uint32_t here,
                                             std::uint32_t target, HopFn hops)
{
    if (here == target) {
        return kNoGate; // already there: the haul never leaves this system
    }
    const std::uint32_t distance = hops(here, target);
    if (distance == 0) {
        return kNoGate;
    }
    for (std::uint32_t i = 0; i < gates.size(); ++i) {
        if (hops(gates[i].toSystem, target) + 1 == distance) {
            return i;
        }
    }
    return kNoGate;
}

// How far along the whole station-to-station run a hopless trader is.
//
// A haul between two stations in one system never touches a gate, so it is a
// single straight run — and the leg windows split it exactly in half, because
// legTotal is traderLegSeconds twice with no gate time between. The depart
// leg's progress therefore folds into the first half and the arrive leg's
// into the second. Reading the arrive leg's progress raw would snap the
// trader back to its origin the instant it crossed the midpoint, which is a
// teleport in plain sight of anyone watching it cross.
[[nodiscard]] inline float hoplessProgress(TraderLeg leg, float progress)
{
    const float clamped = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    return leg == TraderLeg::Arrive ? 0.5f + clamped * 0.5f : clamped * 0.5f;
}

// The point `progress` of the way from `from` to `to`.
[[nodiscard]] inline core::DVec3 legPoint(const core::DVec3& from, const core::DVec3& to,
                                          float progress)
{
    const double t = static_cast<double>(progress);
    return from + (to - from) * t;
}

// How far a trader still has to go, on the record's schedule rather than on
// its engines.
//
// ⚑ The two clocks do not agree, and cannot. A leg is traderLegSeconds long
// however far it is, while a hull crosses it at its own cruise speed: a
// freighter tops out at 3,000 km/s, so the 600,000 km from a station to a
// gate takes it about 260 seconds against a leg quoted at 90. Left to fly
// freely a hauler is three times too slow, is still mid-lane when its books
// say it arrived, and is deleted in deep space having never reached anything.
// That is why an uncapped system full of bodies could still show an empty sky
// at every station.
//
// So the record paces the middle, where nobody can see it — at these speeds a
// hull is far below a pixel — and the ship flies for itself at both ends,
// where it can be watched. `approachDistance` is where steerTravel's own
// profile drops a hauler back into its normal envelope (5.76 km for the
// shipped freighter, hence 6 km), and `approachSeconds` is the time that
// final approach actually takes. Inside either window this returns a distance
// no caller should use: the ship owns its position there.
[[nodiscard]] inline double scheduledLaneDistance(double remainingSeconds, double legSeconds,
                                                  double legLength, double approachDistance,
                                                  double approachSeconds)
{
    if (legSeconds <= 0.0) {
        return 0.0;
    }
    const double approach = std::min(approachSeconds, legSeconds * 0.4);
    const double dA = std::min(approachDistance, legLength * 0.25);
    const double remaining = std::clamp(remainingSeconds, 0.0, legSeconds);
    if (remaining <= approach) {
        return dA * (approach > 0.0 ? remaining / approach : 0.0);
    }
    return dA + (legLength - dA) * (remaining - approach) / (legSeconds - approach);
}

// Which hull a hauler is drawn in, out of the rosters the factions already
// carry (engine plan Phase 8x, stage 6).
//
// ⚑ The rule is that the ship can carry what it is carrying. That is not
// decoration: a coarse haul is laden on the way in and a deadhead on the way
// out (stage 4's own finding), so keying the hull off the load makes the sky
// readable — a big hull in the lane is cargo worth taking, and a small one is
// a ship on its way to fetch some. A raider, an escort and the player all read
// the same thing off it, and none of them has to be told.
//
// The smallest hull that covers the load wins, because a faction that owns a
// freighter and a shuttle does not send the freighter to move forty units.
// Nothing covering it takes the biggest there is: a roster is data, and data
// that cannot carry a shipped cargo must still answer with a ship.
//
// `traderIndex` breaks ties, so a faction listing two hulls of equal capacity
// uses both, and one hauler keeps its own answer for as long as its load does
// — which is the whole leg, because cargo is bought at one end and sold at
// the other.
[[nodiscard]] inline std::uint32_t chooseTraderHull(std::span<const float> capacities, float cargo,
                                                    std::uint32_t traderIndex)
{
    if (capacities.empty()) {
        return 0;
    }
    std::uint32_t best = static_cast<std::uint32_t>(capacities.size());
    std::uint32_t ties = 0;
    std::uint32_t biggest = 0;
    for (std::uint32_t i = 0; i < capacities.size(); ++i) {
        if (capacities[i] > capacities[biggest]) {
            biggest = i;
        }
        if (capacities[i] < cargo) {
            continue;
        }
        if (best >= capacities.size() || capacities[i] < capacities[best]) {
            best = i;
            ties = 1;
        } else if (capacities[i] == capacities[best]) {
            ++ties;
        }
    }
    if (best >= capacities.size()) {
        return biggest;
    }
    if (ties <= 1) {
        return best;
    }
    // Rotate through the equals rather than always taking the first, so a
    // roster of same-sized hulls is a mixed sky instead of one repeated ship.
    std::uint32_t wanted = traderIndex % ties;
    for (std::uint32_t i = 0; i < capacities.size(); ++i) {
        if (capacities[i] == capacities[best] && wanted-- == 0) {
            return i;
        }
    }
    return best;
}

// Where in its lane a trader flies, so that two on the same leg are not at
// the same point.
//
// ⚑ This is not decoration. The coarse fleet converges: a dozen haulers can
// leave one station on one tick bound for one destination, and the record
// puts every one of them at the same progress along the same line. In the
// bubble that is not a tight formation, it is a dozen hulls sharing
// coordinates — and the collision resolver depenetrates a zero-distance pair
// along an arbitrary axis, so the stack throws itself apart hard enough to do
// impact damage and the convoy annihilates itself in the first tick it
// exists. Measured, not feared: an uncapped spawn without this lost most of a
// 45-ship fleet to mutual destruction on arrival, wrecks and all.
//
// The slots are a sunflower spiral keyed on the trader's own index: every
// trader gets a distinct place, the spacing stays near `spacing` however many
// there are, and it is stable, so a hauler keeps its slot for a whole leg
// instead of shuffling each time the set is reconciled.
[[nodiscard]] inline core::DVec3 laneSlotOffset(std::uint32_t traderIndex,
                                                const core::DVec3& laneDirection, double spacing)
{
    const auto cross3 = [](const core::DVec3& a, const core::DVec3& b) {
        return core::DVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const double laneLength = length(laneDirection);
    const core::DVec3 forward =
        laneLength > 0.0 ? laneDirection * (1.0 / laneLength) : core::DVec3{0.0, 0.0, 1.0};
    // Any world axis that is not nearly parallel to the lane gives a stable
    // basis; picking by the smallest component is what keeps it well
    // conditioned for a lane pointing any direction at all.
    const core::DVec3 axis = std::abs(forward.x) < 0.9 ? core::DVec3{1.0, 0.0, 0.0}
                                                       : core::DVec3{0.0, 1.0, 0.0};
    const core::DVec3 u = normalize(cross3(forward, axis));
    const core::DVec3 v = cross3(forward, u);
    constexpr double kGoldenAngle = 2.399963229728653;
    const double angle = kGoldenAngle * static_cast<double>(traderIndex);
    const double radius = spacing * std::sqrt(static_cast<double>(traderIndex) + 1.0);
    return u * (radius * std::cos(angle)) + v * (radius * std::sin(angle));
}

} // namespace sol::sim
