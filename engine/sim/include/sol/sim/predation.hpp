#pragma once

// Which hauler a raider goes for (engine plan Phase 8x, stage 4).
//
// Stage 3's rule was that a trader can only be LOST somewhere it could have
// been seen, so attrition rolls on the depart and arrive legs and never
// between gates. This is the same rule from the other side: a trader can only
// be HUNTED where it is flying itself. Through the middle of a leg the coarse
// record owns its position and moves it far faster than any hull travels — the
// schedule crosses 600,000 km in twenty seconds, where a fighter's cruise
// drive needs a hundred — so a hunter that locked onto a paced hauler would be
// in a stern chase it can never win, teleporting away from it once a tick and
// never firing a shot. Ends of a leg are the whole of a hauler's exposure, on
// both layers, and that is what makes an escort contract coherent.
//
// It lives here as a pure header for docking.hpp's and trade_route.hpp's
// reason: game/src has no test suite, so a rule that cannot be asserted
// headlessly gets verified by screenshot forever.

#include "sol/core/math/math.hpp"

#include <cstdint>
#include <span>

namespace sol::sim {

inline constexpr std::uint32_t kNoPrey = 0xffff'ffffu;

// One hauler a hunter could go for. `index` is the caller's own handle (the
// game passes an entity index) and comes back untouched, so nothing in here
// has to know what an entity is.
struct PreyCandidate
{
    std::uint32_t index = kNoPrey;
    core::DVec3 position;
    std::uint32_t faction = 0;
    // The coarse record is flying it this tick. Uncatchable — see above.
    bool paced = false;
    // Flying the arrive leg: in from a gate toward a station in this system,
    // rather than out from a station toward a gate.
    bool inbound = false;
};

// How far a hunter will travel for a hauler: a full system transit, which
// generation's own comment defines as 2 * gateDistance and which the
// decisions/005 cruise-leg budget is tuned against.
//
// ⚑ This is the bound being honest about itself. The first version budgeted
// the hunter's cruise drive against the hauler's 35 s approach window, on the
// reasoning that a chase it could not finish inside that window was one it
// should not start — and a drive killed it: the nearest hauler in the shipped
// starting system was 886,783 km away, three times that budget, so no raider
// ever hunted anything. The budget was measuring the wrong thing. A hauler
// does not vanish when its window shuts; it goes back on the record's
// schedule, pops out at the far end and, if it is arriving, parks 250 m off a
// station and waits. The hunter re-decides at 2 Hz and drops prey the record
// picks up again, so a long chase costs it nothing. **The real limit is the
// LOD bubble: prey in another system has no body to hunt.** So the reach is
// that bubble's own diameter, and it says no only to a lock that geometry
// already made impossible.
[[nodiscard]] inline double preyReach(double gateDistance)
{
    return gateDistance > 0.0 ? 2.0 * gateDistance : 0.0;
}

// The best hauler within `reach` that `hostile` marks as fair game, or
// kNoPrey. `hostile` is the hunter's own row — indexed by faction, non-zero
// meaning it would attack them — so this stays free of who is at war with
// whom, and the caller builds that row from the same test the coarse layer
// uses to choose a system to raid.
//
// ⚑ Inbound first, then nearest — and the first half is what makes predation
// happen at all. A drive found a raider locking onto the nearest hauler, an
// outbound one, cruising after it and losing it within seconds: an outbound
// hauler is heading for a gate, and when it reaches one its leg becomes a
// jump, its body is deleted and the chase was for nothing. An inbound one is
// heading for a station in this system, where it will stop and hold. It is
// also the laden one — the outbound leg of a coarse haul is the deadhead —
// so the ship that is worth taking and the ship that will still be there when
// you arrive are the same ship. That is a pirate's own calculus, not a
// tuning knob.
//
// Distance still decides within a tier. Cargo does not: a coarse trader's
// hold is real state, but reading it here would make a raider omniscient
// about a ship it has never been near, where which way it is pointed is
// something anyone can see.
[[nodiscard]] inline std::uint32_t choosePrey(const core::DVec3& hunter,
                                              double reach,
                                              std::span<const PreyCandidate> candidates,
                                              std::span<const std::uint8_t> hostile)
{
    std::uint32_t best = kNoPrey;
    double bestDistance = 0.0;
    bool bestInbound = false;
    for (const PreyCandidate& candidate : candidates) {
        if (candidate.paced || candidate.faction >= hostile.size() || hostile[candidate.faction] == 0) {
            continue;
        }
        const double distance = length(candidate.position - hunter);
        if (distance > reach) {
            continue;
        }
        // Ties go to the earlier candidate, which is spawn order, so two
        // raiders handed the same list pick the same hauler.
        const bool better = best == kNoPrey || (candidate.inbound && !bestInbound) ||
                            (candidate.inbound == bestInbound && distance < bestDistance);
        if (better) {
            bestDistance = distance;
            bestInbound = candidate.inbound;
            best = candidate.index;
        }
    }
    return best;
}

} // namespace sol::sim
