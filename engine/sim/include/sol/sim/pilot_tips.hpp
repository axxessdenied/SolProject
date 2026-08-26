#pragma once

// What a pilot knows worth telling you (engine plan Phase 8s). Which market a
// trader would name, and which site a fighter or a patrol would point at.
//
// Pure and header-only for the same reason sol/sim/docking.hpp and
// sol/ui/map_projection.hpp are: game/src has no test suite, so a selection
// rule that lives there gets verified by screenshot forever. Everything below
// is a function of state the caller already holds.
//
// ⚑ These pick the FACT. The pilot_hail hook picks the words and which KIND of
// tip is offered, and that split is deliberate: a berth was one of four
// interchangeable integers a script could not get meaningfully wrong, but a tip
// is a claim about the galaxy. A hook allowed to name the position could
// bookmark interstellar space, and the player would have no way to tell a bug
// from a lying pilot.

#include "sol/core/math/math.hpp"
#include "sol/sim/economy.hpp"
#include "sol/sim/survey.hpp"
#include "sol/sim/universe.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace sol::sim {

// The Bookmark::label tier a rumour gets. That field has been serialised and
// never set since Phase 8h — declared as "colour/category tier; 0 = plain" with
// every bookmark in the game at 0 — and a place somebody told you about is
// exactly the second category it was declared for.
inline constexpr std::uint32_t kTipLabel = 1;

// Two tips must not land on the same site. A bookmark already within this of a
// signal means somebody has been pointed at it (or the player found it), so the
// next pilot names something else instead of repeating the last one.
inline constexpr double kTipDuplicateRange = 1.0e3;

// A site a pilot could point at.
struct TipSite
{
    std::uint32_t system = 0;
    std::uint32_t signal = 0;
    core::DVec3 position; // system barycenter frame, meters
};

// Which market a trader would name, or false when they have nothing to add.
//
// ⚑ Never a market in the system the player is standing in. That is not a tip,
// it is a window: they can fly over and read the board themselves, and naming
// it would spend the pilot's one answer on nothing. It is also the rule
// buyMarketIntel already follows for its own reason, so this is the same fact
// stated twice rather than a new one.
//
// Preference, in order: a market the player remembers NOTHING about, nearest
// first; failing that, the STALEST memory they hold. Ties break on market index
// so the answer is stable for a given state.
//
// Deliberately not seeded. The rule advances itself — a tip writes the memory
// it just named, so the next trader's best answer is the next market along —
// and a deterministic pick is what lets a test state the preference rather than
// sample it.
//
// `hops` is the BFS row from the pilot's system, capped, in the caller's own
// encoding; anything above `maxHops` is out of a pilot's world. The caller
// already computes it to price station intel, which is why it is passed rather
// than recomputed here against a second definition of "nearby".
[[nodiscard]] inline bool chooseMarketTip(std::span<const StationMarket> markets,
                                          std::span<const std::uint8_t> hops,
                                          std::uint32_t maxHops,
                                          const SurveySim& survey,
                                          double now,
                                          std::uint32_t* outMarket)
{
    bool found = false;
    bool bestUnknown = false;
    std::uint32_t bestHops = 0;
    double bestTakenAt = 0.0;
    std::uint32_t best = 0;
    for (std::uint32_t index = 0; index < markets.size(); ++index) {
        const std::uint32_t system = markets[index].systemIndex;
        if (system >= hops.size()) {
            continue;
        }
        const std::uint32_t distance = hops[system];
        if (distance == 0 || distance > maxHops) {
            continue; // 0 is where the player already is; see the note above
        }
        const MarketMemory* memory = survey.remembered(index);
        const bool unknown = memory == nullptr;
        // A fresh memory is not worth a conversation; a stale one is, which is
        // the same line the Trade tab already greys a price at.
        if (!unknown && !survey.isStale(*memory, now)) {
            continue;
        }
        const double takenAt = unknown ? 0.0 : memory->takenAt;
        if (!found || (unknown && !bestUnknown)) {
            found = true;
            bestUnknown = unknown;
            bestHops = distance;
            bestTakenAt = takenAt;
            best = index;
            continue;
        }
        if (unknown != bestUnknown) {
            continue; // a known market never displaces an unknown one
        }
        const bool better = unknown ? distance < bestHops : takenAt < bestTakenAt;
        if (better) {
            bestHops = distance;
            bestTakenAt = takenAt;
            best = index;
        }
    }
    if (found && outMarket != nullptr) {
        *outMarket = best;
    }
    return found;
}

// Whether anything the player already knows about sits on top of `position`:
// a bookmark they dropped themselves, or one an earlier pilot's tip left.
[[nodiscard]] inline bool
siteAlreadyMarked(const SurveySim& survey, std::uint32_t system, const core::DVec3& position)
{
    for (const Bookmark& bookmark : survey.bookmarks()) {
        if (bookmark.system == system && length(bookmark.position - position) <= kTipDuplicateRange) {
            return true;
        }
    }
    return false;
}

// Which site a fighter or a patrol would point at, or false when they have
// nothing left to tell.
//
// An UNRESOLVED signal in a system the player has at least Charted — a rumour
// about a place you have already emptied is not a rumour — with this system
// first and then nearest by hops. Sites the player has already discovered
// themselves are skipped: a pilot naming a contact already on the HUD is
// telling you nothing, and it would look like a bug rather than a shrug.
//
// `scratch` is the caller's signal buffer, reused across systems the way every
// other signalsFor() caller does rather than allocating per candidate.
[[nodiscard]] inline bool choosePlaceTip(const Galaxy& galaxy,
                                         const SurveySim& survey,
                                         std::span<const std::uint8_t> hops,
                                         std::uint32_t maxHops,
                                         std::vector<SignalSpec>& scratch,
                                         TipSite* out)
{
    bool found = false;
    std::uint32_t bestHops = 0;
    TipSite best;
    for (std::uint32_t system = 0; system < galaxy.systems.size() && system < hops.size(); ++system) {
        const std::uint32_t distance = hops[system];
        if (distance > maxHops || (found && distance > bestHops)) {
            continue;
        }
        if (survey.knowledge(system) == KnowledgeState::Unknown) {
            continue; // you cannot be told about a system you cannot place
        }
        survey.signalsFor(galaxy, system, scratch);
        for (std::uint32_t signal = 0; signal < scratch.size(); ++signal) {
            if (survey.signalDiscovered(system, signal) || survey.signalResolved(system, signal) ||
                siteAlreadyMarked(survey, system, scratch[signal].position)) {
                continue;
            }
            if (!found || distance < bestHops) {
                found = true;
                bestHops = distance;
                best = TipSite{.system = system, .signal = signal, .position = scratch[signal].position};
            }
            break; // one candidate per system is enough; nearer systems win
        }
    }
    if (found && out != nullptr) {
        *out = best;
    }
    return found;
}

} // namespace sol::sim
