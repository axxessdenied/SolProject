#pragma once

// What somebody knows worth telling you (engine plan Phase 8s, widened by Phase
// 35 stage B). Which market a trader would name, which site a fighter or a
// patrol would point at, and - since the Bar - which shortage, which raid, which
// war and which departing hauler a barkeep would mention.
//
// ⚑⚑ A BARKEEP IS NOT A PILOT AND THIS FILE IS STILL THE RIGHT HOME, BECAUSE
// WHAT IS SHARED IS THE RULING BELOW RATHER THAN THE MOUTH. Splitting bar talk
// into a file of its own would have copied that ruling into a second place to
// drift, and the two halves already share the one rule that matters: the engine
// picks the fact and the script picks the words.
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
#include "sol/sim/missions.hpp"
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

// ---------------------------------------------------------------------------
// Bar talk (Phase 35 stage B). Which one of a room's many true facts is worth
// a sentence.
//
// ⚑⚑⚑⚑ SELECTION IS THE WHOLE PROBLEM HERE AND ITS FAILURE MODE IS SILENT.
// MEASURED at seed 1701 over the 62 docks with a room: at t=0 three of the four
// sources are empty, and by two sim hours the median room stands on 155 live
// candidates and the busiest on 219. Any rule at all produces a line; a BAD
// rule produces a line that is true, irrelevant, and indistinguishable from a
// good one - and no test will say so. So each rule below consults what the
// player already knows, and each one names the shipped rule it copies, because
// both of the rules above had already solved a version of this.
//
// ⚑ All four are deterministic for a given state, for `chooseMarketTip`'s
// reason: a test can then state the preference rather than sample it. Which
// KIND of line gets spent, and the words it is spent in, belong to the
// `bar_talk` hook - the same split, for the same reason.
// ---------------------------------------------------------------------------

// Where a (system, station) pair sits in the market table, or false. A haul
// names a station and market memory is keyed by market, so something has to
// join them; it lives here rather than in the caller because both sides are
// arguments to the rule below and neither is the caller's own bookkeeping.
[[nodiscard]] inline bool marketAt(std::span<const StationMarket> markets,
                                   std::uint32_t system,
                                   std::uint32_t station,
                                   std::uint32_t* out)
{
    for (std::uint32_t index = 0; index < markets.size(); ++index) {
        if (markets[index].systemIndex == system && markets[index].stationIndex == station) {
            if (out != nullptr) {
                *out = index;
            }
            return true;
        }
    }
    return false;
}

// Which shortage a barkeep would name: an index into `hauls`, or false.
//
// ⚑ NEVER ONE IN THIS SYSTEM, AND NEVER ONE WHOSE PRICES THE PLAYER ALREADY
// HOLDS FRESH. Both halves are `chooseMarketTip`'s rules restated rather than
// new ones - a market you can reach without leaving the system is a window
// rather than a tip, and "a fresh memory is not worth a conversation; a stale
// one is" is the line that stops this reading as a random-fact generator. It is
// the single most important filter in this file, because a shortage is by far
// the largest of the four pools: 8,656 of 8,884 candidates at two sim hours.
//
// Then the most SEVERE, because a barely-short market is not worth mentioning
// and severity is what makes the trip pay; ties nearest; ties by index.
[[nodiscard]] inline bool chooseShortageTalk(std::span<const HaulCandidate> hauls,
                                             std::span<const StationMarket> markets,
                                             const SurveySim& survey,
                                             double now,
                                             std::uint32_t* outIndex)
{
    bool found = false;
    std::uint32_t best = 0;
    float bestSeverity = 0.0f;
    std::uint32_t bestJumps = 0;
    for (std::uint32_t i = 0; i < hauls.size(); ++i) {
        const HaulCandidate& haul = hauls[i];
        if (haul.jumps == 0) {
            continue; // in this system: fly over and read the board yourself
        }
        std::uint32_t market = 0;
        if (marketAt(markets, haul.system, haul.station, &market)) {
            const MarketMemory* memory = survey.remembered(market);
            if (memory != nullptr && !survey.isStale(*memory, now)) {
                continue; // you already hold today's prices for that dock
            }
        }
        const bool better = !found || haul.severity > bestSeverity ||
                            (haul.severity == bestSeverity && haul.jumps < bestJumps);
        if (better) {
            found = true;
            best = i;
            bestSeverity = haul.severity;
            bestJumps = haul.jumps;
        }
    }
    if (found && outIndex != nullptr) {
        *outIndex = best;
    }
    return found;
}

// Which raid a barkeep would name - WHO HIT WHERE - as an index into
// `bounties`, or false.
//
// ⚑⚑ THIS IS NOT A BOUNTY ON THE PLAYER AND THE DISTINCTION IS LOAD-BEARING.
// `BountyCandidate` is a system whose raid intensity is still warm paired with
// the clan that hit it last; there is nothing to be WANTED FOR until Phase 36,
// which holds bounties, fines and legal consequence deliberately. A barkeep may
// say who raided where. A barkeep may not say what you are worth, and no line
// written against this function may promise it.
//
// ⚑⚑⚑ A SYSTEM THE PLAYER CAN PLACE IS PREFERRED AND IS NOT REQUIRED, AND
// THAT DIFFERENCE WAS MEASURED RATHER THAN ARGUED. `choosePlaceTip` REFUSES a
// system the player cannot place, and the temptation is to copy it - but that
// rule earns its refusal by WRITING A BOOKMARK, and a marker you cannot place
// is one you cannot navigate to. A sentence writes nothing. Built as a refusal
// it measured, over the 62 rooms at seed 1701: the raid line sayable at 21
// rooms against 55-62 ungated, and the war line below at ZERO rooms against
// 19-37 - and Phase 8z is why, because arrival stopped charting the neighbours,
// so a player knows only the systems whose gates they have identified.
//
// So it is a TIER, which is this file's own idiom rather than a new one:
// `chooseMarketTip` already ranks in two tiers and says "a known market never
// displaces an unknown one". Placeable first, then the WARMEST, because
// intensity is how recently it happened; ties nearest; ties by index.
[[nodiscard]] inline bool
chooseRaidTalk(std::span<const BountyCandidate> bounties, const SurveySim& survey, std::uint32_t* outIndex)
{
    bool found = false;
    std::uint32_t best = 0;
    bool bestPlaceable = false;
    float bestIntensity = 0.0f;
    std::uint32_t bestJumps = 0;
    for (std::uint32_t i = 0; i < bounties.size(); ++i) {
        const BountyCandidate& raid = bounties[i];
        const bool placeable = survey.knowledge(raid.system) != KnowledgeState::Unknown;
        if (found && !placeable && bestPlaceable) {
            continue; // a system you cannot find never displaces one you can
        }
        const bool better = !found || (placeable && !bestPlaceable) || raid.intensity > bestIntensity ||
                            (raid.intensity == bestIntensity && raid.jumps < bestJumps);
        if (better) {
            found = true;
            best = i;
            bestPlaceable = placeable;
            bestIntensity = raid.intensity;
            bestJumps = raid.jumps;
        }
    }
    if (found && outIndex != nullptr) {
        *outIndex = best;
    }
    return found;
}

// Which war a barkeep would name: an index into `fronts`, or false.
//
// ⚑⚑ FEED THIS `MissionSim::frontCandidates`, NOT `contestCandidates`. The
// second is gated on the board's owner being a party to the fight, which is a
// rule about who will PAY - and that gate is exactly why a war front is the one
// thing a bar can tell you that a board structurally cannot. Handing this the
// gated list would refund that difference silently, which is the shape of
// failure this phase has already met twice.
//
// Same placeable-first tier as the raid rule and for the same measured reason -
// this is the line the refusal would have silenced outright - then the heaviest
// PRESSURE, because a contest about to resolve is the one people are talking
// about; ties nearest; ties by index.
[[nodiscard]] inline bool
chooseFrontTalk(std::span<const ContestCandidate> fronts, const SurveySim& survey, std::uint32_t* outIndex)
{
    bool found = false;
    std::uint32_t best = 0;
    bool bestPlaceable = false;
    float bestPressure = 0.0f;
    std::uint32_t bestJumps = 0;
    for (std::uint32_t i = 0; i < fronts.size(); ++i) {
        const ContestCandidate& front = fronts[i];
        const bool placeable = survey.knowledge(front.system) != KnowledgeState::Unknown;
        if (found && !placeable && bestPlaceable) {
            continue;
        }
        const bool better = !found || (placeable && !bestPlaceable) || front.pressure > bestPressure ||
                            (front.pressure == bestPressure && front.jumps < bestJumps);
        if (better) {
            found = true;
            best = i;
            bestPlaceable = placeable;
            bestPressure = front.pressure;
            bestJumps = front.jumps;
        }
    }
    if (found && outIndex != nullptr) {
        *outIndex = best;
    }
    return found;
}

// Which departing hauler a barkeep would point at: an index into `escorts`, or
// false.
//
// ⚑⚑ NO KNOWLEDGE GATE, DELIBERATELY, AND IT IS THE ONE RULE HERE THAT NEEDS
// NONE. Every escort candidate is a ship leaving THIS system right now, so the
// player can walk out and watch it go: there is nothing to place and nothing
// they could already know. It is also the only one of the four sources with
// anything at all at t=0 - MEASURED: 64 galaxy-wide, one at the median room,
// against zero shortages, zero raids and zero wars - which is what keeps a live
// line sayable in a brand-new game.
//
// LADEN FIRST, because a deadheading hauler has no cargo worth naming and its
// `commodity` field stays set from its last run - the trap `sol.traders()` fell
// into and the board's escort marshalling has a guard against. Then the most
// DANGEROUS destination, because that is what makes the run worth remarking on
// and it is read from the same number attrition rolls against; ties by index.
[[nodiscard]] inline bool chooseHaulerTalk(std::span<const EscortCandidate> escorts, std::uint32_t* outIndex)
{
    bool found = false;
    std::uint32_t best = 0;
    bool bestLaden = false;
    float bestDanger = 0.0f;
    for (std::uint32_t i = 0; i < escorts.size(); ++i) {
        const EscortCandidate& run = escorts[i];
        const bool laden = run.cargo > 0.0f;
        if (found && bestLaden && !laden) {
            continue; // a deadhead never displaces a loaded run
        }
        const bool better = !found || (laden && !bestLaden) || run.danger > bestDanger;
        if (better) {
            found = true;
            best = i;
            bestLaden = laden;
            bestDanger = run.danger;
        }
    }
    if (found && outIndex != nullptr) {
        *outIndex = best;
    }
    return found;
}

} // namespace sol::sim
