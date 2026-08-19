#include <sol/sim/predation.hpp>

#include <sol/test/test.hpp>

#include <cstdint>
#include <vector>

using sol::core::DVec3;
using sol::sim::choosePrey;
using sol::sim::kNoPrey;
using sol::sim::PreyCandidate;
using sol::sim::preyReach;

namespace {

// Three factions: 0 hunts nobody, 1 is fair game, 2 is not. The hunter's row
// is what the game builds from "at war with, or hostile to" - the same test
// FactionSim::raidCandidates uses to choose a system to raid.
std::vector<std::uint8_t> hostileToOne()
{
    return {0, 1, 0};
}

PreyCandidate hauler(std::uint32_t index, double x, std::uint32_t faction, bool paced = false,
                     bool inbound = false)
{
    return {.index = index,
            .position = {x, 0.0, 0.0},
            .faction = faction,
            .paced = paced,
            .inbound = inbound};
}

} // namespace

SOL_TEST(predation_takes_the_nearest_hauler_it_is_hostile_to)
{
    const std::vector<std::uint8_t> hostile = hostileToOne();
    const std::vector<PreyCandidate> candidates{
        hauler(10, 9'000.0, 1),
        hauler(11, 3'000.0, 1),
        hauler(12, 5'000.0, 1),
    };
    SOL_CHECK(choosePrey({}, 1.0e6, candidates, hostile) == 11);

    // Two at the same range: spawn order decides, so a wing of raiders handed
    // the same list converges on one hauler rather than scattering.
    const std::vector<PreyCandidate> tied{hauler(20, 4'000.0, 1), hauler(21, 4'000.0, 1)};
    SOL_CHECK(choosePrey({}, 1.0e6, tied, hostile) == 20);
}

SOL_TEST(predation_prefers_a_hauler_coming_in_over_a_nearer_one_going_out)
{
    const std::vector<std::uint8_t> hostile = hostileToOne();
    // ⚑ The rule a drive taught. The near hauler is outbound: it is running
    // for a gate, and when it gets there its body is deleted and the chase
    // was for nothing. The far one is inbound, so it is heading for a station
    // in this system and will stop there - and on a coarse haul the inbound
    // leg is the laden one, so it is also the only one worth taking.
    const std::vector<PreyCandidate> candidates{
        hauler(10, 2'000.0, 1, false, false),
        hauler(11, 90'000.0, 1, false, true),
    };
    SOL_CHECK(choosePrey({}, 1.0e6, candidates, hostile) == 11);

    // Distance still decides inside a tier - the preference is a tie-break
    // between two kinds of prey, not a licence to cross the system for the
    // first inbound hauler when a nearer one is doing the same thing.
    const std::vector<PreyCandidate> twoInbound{
        hauler(10, 90'000.0, 1, false, true),
        hauler(11, 30'000.0, 1, false, true),
    };
    SOL_CHECK(choosePrey({}, 1.0e6, twoInbound, hostile) == 11);

    // ...and with nothing inbound at all, an outbound hauler is still prey:
    // a chase that may not pay off beats no chase.
    const std::vector<PreyCandidate> allOutbound{
        hauler(10, 90'000.0, 1),
        hauler(11, 30'000.0, 1),
    };
    SOL_CHECK(choosePrey({}, 1.0e6, allOutbound, hostile) == 11);

    // An inbound hauler the record is pacing is still uncatchable. The tiers
    // rank prey; they do not resurrect a rule the geometry already settled.
    const std::vector<PreyCandidate> pacedInbound{
        hauler(10, 2'000.0, 1, true, true),
        hauler(11, 90'000.0, 1, false, false),
    };
    SOL_CHECK(choosePrey({}, 1.0e6, pacedInbound, hostile) == 11);
}

SOL_TEST(predation_ignores_haulers_it_has_no_quarrel_with)
{
    const std::vector<std::uint8_t> hostile = hostileToOne();
    // The nearest three are its own and a neutral's; only the far one is prey.
    const std::vector<PreyCandidate> candidates{
        hauler(10, 100.0, 0),
        hauler(11, 200.0, 2),
        hauler(12, 8'000.0, 1),
    };
    SOL_CHECK(choosePrey({}, 1.0e6, candidates, hostile) == 12);

    // Nothing hostile in the sky at all is an answer, not a failure: the
    // caller falls through to whatever it does when there is nothing to take.
    const std::vector<PreyCandidate> friends{hauler(10, 100.0, 0), hauler(11, 200.0, 2)};
    SOL_CHECK(choosePrey({}, 1.0e6, friends, hostile) == kNoPrey);

    // A faction the hunter's row does not cover cannot be attacked by
    // accident - an unaffiliated body is not automatically prey.
    const std::vector<PreyCandidate> stranger{hauler(10, 100.0, 7)};
    SOL_CHECK(choosePrey({}, 1.0e6, stranger, hostile) == kNoPrey);
}

SOL_TEST(predation_never_chases_a_hauler_the_record_is_flying)
{
    const std::vector<std::uint8_t> hostile = hostileToOne();
    // ⚑ The load-bearing rule. The near hauler is in the paced middle of its
    // leg, where the schedule moves it faster than any hull travels: locking
    // onto it would be a chase that never closes and never fires. The one
    // twice as far away is flying itself, so it is the one that can be taken.
    const std::vector<PreyCandidate> candidates{
        hauler(10, 1'000.0, 1, true),
        hauler(11, 2'000.0, 1),
    };
    SOL_CHECK(choosePrey({}, 1.0e6, candidates, hostile) == 11);

    // Every hauler in the system paced: nobody is huntable, which is the same
    // answer as an empty sky and is exactly what stage 3 says about attrition
    // in the middle of a haul.
    const std::vector<PreyCandidate> allPaced{
        hauler(10, 1'000.0, 1, true),
        hauler(11, 2'000.0, 1, true),
    };
    SOL_CHECK(choosePrey({}, 1.0e6, allPaced, hostile) == kNoPrey);
}

SOL_TEST(predation_reach_is_a_full_system_transit)
{
    // The shipped gateDistance is 6e8, and generation's own comment says
    // 2*gateDistance bounds a full system transit - so a raider hunts
    // anywhere in the system it is standing in and the LOD bubble does the
    // limiting. ⚑ This replaced a reach budgeted against the hauler's 35 s
    // approach window, which a live drive proved fired on nothing at all: the
    // nearest hauler in the starting system was 886,783 km out, three times
    // that budget.
    const double reach = preyReach(6.0e8);
    SOL_CHECK(reach == 1.2e9);
    SOL_CHECK(reach > 8.87e8); // the hauler that killed the first derivation

    const std::vector<std::uint8_t> hostile = hostileToOne();
    const std::vector<PreyCandidate> candidates{hauler(10, reach * 1.5, 1)};
    SOL_CHECK(choosePrey({}, reach, candidates, hostile) == kNoPrey);

    // Exactly at reach still counts: the boundary belongs to the hunter, so
    // there is no band where prey is visible and refused for no stated reason.
    const std::vector<PreyCandidate> onTheEdge{hauler(10, reach, 1)};
    SOL_CHECK(choosePrey({}, reach, onTheEdge, hostile) == 10);

    // A galaxy with no gate distance at all has no reach and hunts nothing,
    // rather than being handed a negative one.
    SOL_CHECK(preyReach(0.0) == 0.0);
    SOL_CHECK(choosePrey({}, preyReach(0.0), candidates, hostile) == kNoPrey);
}
