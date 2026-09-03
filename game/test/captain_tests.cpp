// Captains as people (engine plan Phase 39 stage A).
//
// ⚑⚑⚑⚑ THE CLAIM THIS FILE EXISTS FOR IS THE ROSTER'S STABILITY, AND IT IS THE
// ONE THING NOTHING ELSE IN THE TREE WOULD NOTICE. Who is standing in a crew
// hall is derived from the seed and never saved, so the only thing that keeps a
// hall's three candidates the same three across a hire, a save and a reload is
// that `captainCandidates` takes all four of its draws BEFORE it skips anybody.
// Get that wrong and hiring the first captain in a hall silently re-rolls the
// two beside him - every existing test stays green, the save round-trips
// perfectly, and the player watches two strangers replace the people they were
// deciding between. It is `assignCast`'s own rule ("the draws are taken for
// every station, including the ones with no room") arriving one phase later, and
// the anti-vacuity half is what makes the assertion mean anything: the OTHER
// two must be unchanged, not merely present.
//
// ⚑⚑⚑ AND THE SECOND CLAIM IS THAT A FLEET INDEX IS A MOVING TARGET.
// `Captain::ship` is an index into `m_fleet`, and `sellShip` erases from the
// middle of it - the same hazard `m_activeShip` has carried since Phase 8a and
// the same fix. Without the shift a captain silently inherits whichever hull
// slid into the slot, which is a ship changing hands with nothing said.

#include "space_world.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

using game::Captain;
using game::CaptainCandidate;
using game::SpaceWorld;
using sol::assets::DefDatabase;
using sol::assets::StationScreen;

namespace {

constexpr std::uint32_t kNone = 0xffff'ffffu;

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

// A dock in the shipped galaxy, and which screens it offers. The tests need
// two kinds - one with a crew hall and one without - and the shipped galaxy is
// where both are found rather than authored, so a recipe change that stopped
// producing either would fail here rather than pass vacuously.
struct Dock
{
    std::uint32_t system = kNone;
    std::uint32_t station = 0;
};

struct Fixture
{
    DefDatabase defs;
    SpaceWorld space;

    Fixture()
    {
        SOL_REQUIRE(loadShippedDefs(defs));
        space.spawn(game::kDefaultUniverseSeed);
        // applyDefs BEFORE generateUniverse: `resetFleetToStarter` runs inside
        // the generate and reads the hull's mounts, so a world that learns its
        // defs afterwards starts with an empty ship.
        space.applyDefs(defs);
        SOL_REQUIRE(space.generateUniverse(defs));
    }

    [[nodiscard]] SpaceWorld& world() { return space; }

    // The first dock whose screen mask has every bit in `wanted` and none in
    // `forbidden`.
    [[nodiscard]] Dock findDock(std::uint32_t wanted, std::uint32_t forbidden = 0, Dock skip = {})
    {
        const SpaceWorld& w = space;
        for (std::uint32_t s = 0; s < w.galaxy().systems.size(); ++s) {
            for (std::uint32_t t = 0; t < w.galaxy().systems[s].stations.size(); ++t) {
                if (s == skip.system && t == skip.station) {
                    continue; // ⚑ the caller wants a DIFFERENT dock, and the
                              // first match for two overlapping masks is very
                              // often the same one - which is how a test that
                              // means 'stand somewhere else' quietly stands
                              // exactly where it was.
                }
                const std::uint32_t screens = w.stationScreens(s, t);
                if ((screens & wanted) == wanted && (screens & forbidden) == 0) {
                    return {s, t};
                }
            }
        }
        return {};
    }

    // Stand on a dock. Every captain call is docked-gated, so this is the
    // preamble of nearly every test below.
    [[nodiscard]] bool walkIn(const Dock& dock)
    {
        SpaceWorld& w = space;
        if (w.isDocked() && !w.undock()) {
            return false;
        }
        return w.enterSystem(dock.system) && w.warpToStationOffset(dock.station, {100.0, 0.0, 0.0}) &&
               w.tryDockNearestStation(1000.0);
    }

    // Buy any hull this yard will sell, and leave it parked. Returns its fleet
    // index, or 0 (the starter, which is always active) on failure.
    [[nodiscard]] std::size_t buyAnyShip()
    {
        SpaceWorld& w = space;
        w.addCredits(1'000'000.0);
        const std::size_t before = w.fleet().size();
        for (const sol::assets::ShipDef& def : defs.ships()) {
            if (w.buyShip(def.id.c_str())) {
                return before;
            }
        }
        return 0;
    }
};

constexpr std::uint32_t screenBit(StationScreen screen)
{
    return 1u << static_cast<std::uint32_t>(screen);
}

} // namespace

// ⚑ The gate, and its anti-vacuity half in the same test: a hall offers people
// and a dock without one offers nobody. Without the second assertion an empty
// roster everywhere would read as a pass.
SOL_TEST(a_crew_hall_offers_captains_and_a_dock_without_one_offers_nobody)
{
    Fixture fixture;
    std::vector<CaptainCandidate> hall;

    const Dock withHall = fixture.findDock(screenBit(StationScreen::Crew));
    SOL_REQUIRE(withHall.system != kNone);
    SOL_REQUIRE(fixture.walkIn(withHall));
    fixture.world().captainCandidates(hall);
    SOL_CHECK(hall.size() == SpaceWorld::kCaptainsPerHall);
    for (const CaptainCandidate& who : hall) {
        SOL_CHECK(!who.name.empty());
        SOL_CHECK(!who.trade.empty());
        SOL_CHECK(who.who != 0);
        SOL_CHECK(who.cut >= SpaceWorld::kCaptainCutMin && who.cut <= SpaceWorld::kCaptainCutMax);
    }
    std::printf("  %s: %s (%s, %.0f%%)\n",
                fixture.world().dockedStationName(),
                hall[0].name.c_str(),
                hall[0].trade.c_str(),
                static_cast<double>(hall[0].cut) * 100.0);

    const Dock without = fixture.findDock(0, screenBit(StationScreen::Crew));
    SOL_REQUIRE(without.system != kNone);
    SOL_REQUIRE(fixture.walkIn(without));
    fixture.world().captainCandidates(hall);
    SOL_CHECK(hall.empty());

    // And nobody is looking for a berth while you are flying, either.
    SOL_REQUIRE(fixture.world().undock());
    fixture.world().captainCandidates(hall);
    SOL_CHECK(hall.empty());
}

// ⚑⚑ WALKING OUT AND BACK IN FINDS THE SAME PEOPLE. The roster is a pure
// function of the dock and the seed, so this is what makes "remember where the
// cheap captain was" a thing a player can do.
SOL_TEST(a_hall_offers_the_same_people_every_time_you_walk_in)
{
    Fixture fixture;
    const Dock hall = fixture.findDock(screenBit(StationScreen::Crew));
    SOL_REQUIRE(hall.system != kNone);

    SOL_REQUIRE(fixture.walkIn(hall));
    std::vector<CaptainCandidate> first;
    fixture.world().captainCandidates(first);
    SOL_REQUIRE(first.size() == SpaceWorld::kCaptainsPerHall);

    SOL_REQUIRE(fixture.world().undock());
    SOL_REQUIRE(fixture.walkIn(hall));
    std::vector<CaptainCandidate> again;
    fixture.world().captainCandidates(again);
    SOL_REQUIRE(again.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        SOL_CHECK(again[i].who == first[i].who);
        SOL_CHECK(again[i].name == first[i].name);
        SOL_CHECK(again[i].cut == first[i].cut);
    }

    // A DIFFERENT hall is different people, which is the anti-vacuity: a
    // generator that ignored the dock would pass everything above.
    const SpaceWorld& w = fixture.world();
    Dock other;
    for (std::uint32_t s = 0; s < w.galaxy().systems.size() && other.system == kNone; ++s) {
        for (std::uint32_t t = 0; t < w.galaxy().systems[s].stations.size(); ++t) {
            if ((s != hall.system || t != hall.station) &&
                (w.stationScreens(s, t) & screenBit(StationScreen::Crew)) != 0) {
                other = {s, t};
                break;
            }
        }
    }
    SOL_REQUIRE(other.system != kNone);
    SOL_REQUIRE(fixture.walkIn(other));
    std::vector<CaptainCandidate> elsewhere;
    fixture.world().captainCandidates(elsewhere);
    SOL_REQUIRE(!elsewhere.empty());
    SOL_CHECK(elsewhere[0].who != first[0].who);

    // ⚑⚑⚑ AND THE PAIR THAT ACTUALLY PINS THE STATION HALF OF THE SEED:
    // TWO CREW HALLS IN ONE SYSTEM. The check above is satisfied by the system
    // alone, so a roster that folded in only `m_currentSystem` would pass it -
    // and then every dock in a system would offer the same three people, which
    // is a bug a player meets on their second landing.
    Dock a;
    Dock b;
    for (std::uint32_t s = 0; s < w.galaxy().systems.size() && b.system == kNone; ++s) {
        a = Dock{};
        for (std::uint32_t t = 0; t < w.galaxy().systems[s].stations.size(); ++t) {
            if ((w.stationScreens(s, t) & screenBit(StationScreen::Crew)) == 0) {
                continue;
            }
            if (a.system == kNone) {
                a = {s, t};
            } else {
                b = {s, t};
                break;
            }
        }
    }
    SOL_REQUIRE(b.system != kNone); // the shipped galaxy has such a pair
    SOL_REQUIRE(fixture.walkIn(a));
    std::vector<CaptainCandidate> here;
    fixture.world().captainCandidates(here);
    SOL_REQUIRE(fixture.walkIn(b));
    std::vector<CaptainCandidate> nextDoor;
    fixture.world().captainCandidates(nextDoor);
    SOL_REQUIRE(!here.empty() && !nextDoor.empty());
    std::printf("  same system, two halls: %s vs %s\n", here[0].name.c_str(), nextDoor[0].name.c_str());
    SOL_CHECK(here[0].who != nextDoor[0].who);
    SOL_CHECK(here[0].name != nextDoor[0].name);
}

// ⚑⚑⚑⚑ THE TEST THIS FILE EXISTS FOR. Hiring the middle candidate must take
// exactly that person off the roster and leave the other two EXACTLY as they
// were - same key, same name, same cut. A `captainCandidates` that skipped the
// hired slot before drawing would shift the stream and re-roll everybody after
// it, and every other assertion in this file would still pass.
SOL_TEST(hiring_one_captain_leaves_the_others_in_the_hall_unchanged)
{
    Fixture fixture;
    const Dock hall = fixture.findDock(screenBit(StationScreen::Crew));
    SOL_REQUIRE(hall.system != kNone);
    SOL_REQUIRE(fixture.walkIn(hall));

    std::vector<CaptainCandidate> before;
    fixture.world().captainCandidates(before);
    SOL_REQUIRE(before.size() == 3);

    SOL_REQUIRE(fixture.world().hireCaptain(1));
    SOL_REQUIRE(fixture.world().captains().size() == 1);
    const Captain& hired = fixture.world().captains().front();
    SOL_CHECK(hired.who == before[1].who);
    SOL_CHECK(hired.name == before[1].name);
    SOL_CHECK(hired.cut == before[1].cut);
    SOL_CHECK(hired.ship == kNone); // hired, and with nothing to fly yet

    std::vector<CaptainCandidate> after;
    fixture.world().captainCandidates(after);
    SOL_REQUIRE(after.size() == 2);
    SOL_CHECK(after[0].who == before[0].who);
    SOL_CHECK(after[0].name == before[0].name);
    SOL_CHECK(after[0].cut == before[0].cut);
    // The survivor from BEYOND the hired slot is the discriminating one: a
    // shifted stream moves this person and nothing else notices.
    SOL_CHECK(after[1].who == before[2].who);
    SOL_CHECK(after[1].name == before[2].name);
    SOL_CHECK(after[1].cut == before[2].cut);

    // ⚑ And dismissing puts them back on the market, which is what "no second
    // list of who has been taken" buys.
    SOL_REQUIRE(fixture.world().dismissCaptain(0));
    fixture.world().captainCandidates(after);
    SOL_REQUIRE(after.size() == 3);
    SOL_CHECK(after[1].who == before[1].who);
}

// ⚑⚑⚑ A CAPTAIN TAKES A HULL, AND ONLY WHERE BOTH OF THEM ARE STANDING. The
// station rule is `sellShip`'s and `switchShip`'s, and here it is the fiction
// as well as the bookkeeping: somebody has to physically walk onto the ship.
SOL_TEST(a_captain_takes_a_hull_parked_here_and_no_other)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));
    // Printed because the drive needs it: this is the shipped galaxy's first
    // dock that both sells hulls and has a crew hall, which is the one place a
    // player can do the whole of this stage without flying anywhere.
    std::printf("  yard+hall: system %u station %u (%s)\n",
                yard.system,
                yard.station,
                fixture.world().dockedStationName());

    const std::size_t bought = fixture.buyAnyShip();
    SOL_REQUIRE(bought != 0);
    SOL_REQUIRE(fixture.world().fleet().size() == 2);
    SOL_REQUIRE(fixture.world().hireCaptain(0));

    // The hull you are FLYING is not one you can hand over.
    SOL_CHECK(!fixture.world().assignCaptain(0, fixture.world().activeShipIndex()));
    SOL_CHECK(fixture.world().captainOf(bought) == nullptr);

    SOL_REQUIRE(fixture.world().assignCaptain(0, bought));
    SOL_REQUIRE(fixture.world().captainOf(bought) != nullptr);
    SOL_CHECK(fixture.world().captainOf(bought)->name == fixture.world().captains()[0].name);

    // A second captain cannot be given the same hull, and the first cannot be
    // given a second one.
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().captains().size() == 2);
    SOL_CHECK(!fixture.world().assignCaptain(1, bought));
    SOL_CHECK(!fixture.world().assignCaptain(0, bought));

    // ⚑⚑ A SECOND FREE HULL, PARKED AT THE YARD, IS WHAT MAKES THE STATION
    // RULE TESTABLE AT ALL. Trying to give away `bought` from another dock
    // proves nothing: `assignCaptain` refuses it because somebody already holds
    // it, so the station check could be deleted and every assertion would still
    // pass. This hull is free, so "you are not standing where it is" is the only
    // thing left that can say no.
    const std::size_t spare = fixture.buyAnyShip();
    SOL_REQUIRE(spare == 2);
    SOL_REQUIRE(fixture.world().captainOf(spare) == nullptr);

    // Stand somewhere else and neither the give nor the recall is available.
    const Dock elsewhere = fixture.findDock(screenBit(StationScreen::Crew), 0, yard);
    SOL_REQUIRE(elsewhere.system != kNone);
    SOL_REQUIRE(elsewhere.system != yard.system || elsewhere.station != yard.station);
    SOL_REQUIRE(fixture.walkIn(elsewhere));
    SOL_CHECK(!fixture.world().recallCaptain(0));
    SOL_CHECK(!fixture.world().assignCaptain(1, spare));
    SOL_CHECK(fixture.world().captainOf(spare) == nullptr);
    SOL_CHECK(!fixture.world().assignCaptain(1, bought));

    // ...and back at the yard the same call lands, which is what keeps the
    // refusals above a rule rather than a wall.
    SOL_REQUIRE(fixture.walkIn(yard));
    SOL_REQUIRE(fixture.world().assignCaptain(1, spare));
    SOL_REQUIRE(fixture.world().recallCaptain(1));

    SOL_REQUIRE(fixture.walkIn(yard));
    SOL_REQUIRE(fixture.world().recallCaptain(0));
    SOL_CHECK(fixture.world().captainOf(bought) == nullptr);
    SOL_CHECK(fixture.world().captains()[0].ship == kNone);
}

// ⚑⚑⚑⚑ A HULL SOMEBODY IS HOLDING IS NEITHER SELLABLE NOR ABANDONABLE. In
// stage A this is tidiness; from stage B on the hull is two jumps away flying a
// route, and either hole would delete a ship mid-haul with nothing said.
SOL_TEST(a_captain_holding_a_ship_can_be_neither_dismissed_nor_sold_out_from_under)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));

    const std::size_t bought = fixture.buyAnyShip();
    SOL_REQUIRE(bought != 0);
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, bought));

    const double credits = fixture.world().playerCredits();
    SOL_CHECK(!fixture.world().sellShip(bought));
    SOL_CHECK(fixture.world().fleet().size() == 2);
    SOL_CHECK(fixture.world().playerCredits() == credits); // and no refund landed
    SOL_CHECK(!fixture.world().dismissCaptain(0));
    SOL_CHECK(fixture.world().captains().size() == 1);

    // Recall first, and both become legal - which is what makes the refusals
    // above a rule rather than a wall.
    SOL_REQUIRE(fixture.world().recallCaptain(0));
    SOL_REQUIRE(fixture.world().sellShip(bought));
    SOL_REQUIRE(fixture.world().dismissCaptain(0));
    SOL_CHECK(fixture.world().captains().empty());
}

// ⚑⚑⚑⚑ SELLING A HULL BELOW A CAPTAIN'S RENUMBERS WHAT THEY ARE FLYING.
// `Captain::ship` is a fleet index and `sellShip` erases from the middle, so
// without the shift the captain keeps a number that now names somebody else's
// ship. The starter is index 0 and active, so the two bought hulls are 1 and 2:
// give the captain 2, sell 1, and 2 must follow them down to 1.
SOL_TEST(selling_a_hull_below_a_captains_renumbers_the_one_they_hold)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));

    SOL_REQUIRE(fixture.buyAnyShip() == 1);
    SOL_REQUIRE(fixture.buyAnyShip() == 2);
    SOL_REQUIRE(fixture.world().fleet().size() == 3);
    const std::string held = fixture.world().fleet()[2].defId;

    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, 2));
    SOL_REQUIRE(fixture.world().sellShip(1));

    SOL_REQUIRE(fixture.world().fleet().size() == 2);
    SOL_CHECK(fixture.world().captains()[0].ship == 1);
    // The hull they hold is still the SAME hull, which is the whole claim.
    SOL_CHECK(fixture.world().fleet()[1].defId == held);
    SOL_REQUIRE(fixture.world().captainOf(1) != nullptr);
    SOL_CHECK(fixture.world().captainOf(0) == nullptr); // not the one you fly
}

// ⚑⚑ A SAVE CARRIES THE PEOPLE, AND REFUSES A FILE THAT PUTS TWO OF THEM ON ONE
// HULL. The refusal is the half worth having: `Captain::ship` is an index, and a
// file naming the same one twice describes a fleet this code cannot represent -
// which would otherwise show up as two captains apparently flying one ship.
SOL_TEST(a_save_carries_the_captains_and_refuses_two_of_them_on_one_hull)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));

    const std::size_t bought = fixture.buyAnyShip();
    SOL_REQUIRE(bought != 0);
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().hireCaptain(0)); // the next one along
    SOL_REQUIRE(fixture.world().assignCaptain(1, bought));
    const std::string name = fixture.world().captains()[1].name;
    const std::uint64_t who = fixture.world().captains()[1].who;
    const float cut = fixture.world().captains()[1].cut;

    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/captains";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/captains.sav";
    SOL_REQUIRE(fixture.world().saveTo(path.c_str(), "Captains"));

    Fixture reloaded;
    SOL_REQUIRE(reloaded.world().loadFrom(path.c_str()));
    SOL_REQUIRE(reloaded.world().captains().size() == 2);
    const Captain& back = reloaded.world().captains()[1];
    SOL_CHECK(back.name == name);
    SOL_CHECK(back.who == who);
    SOL_CHECK(back.cut == cut);
    SOL_CHECK(back.ship == static_cast<std::uint32_t>(bought));
    SOL_CHECK(reloaded.world().captains()[0].ship == kNone);
    // And the hall they were hired from still knows they are taken.
    SOL_REQUIRE(reloaded.walkIn(yard));
    std::vector<CaptainCandidate> hall;
    reloaded.world().captainCandidates(hall);
    SOL_CHECK(hall.size() == 1);
    SOL_CHECK(
        std::none_of(hall.begin(), hall.end(), [who](const CaptainCandidate& c) { return c.who == who; }));

    // Now corrupt the pairing on disk and watch the load refuse it: the
    // UNASSIGNED captain is given the same hull as the assigned one, which is
    // the exact state the check exists for.
    //
    // ⚑ The record is `name, trade, who, ship, cut`, so the eight bytes of a
    // captain's `who` followed by the four of `kNoIndex` locate that captain's
    // `ship` field exactly - no string decoding, and a 96-bit needle that
    // cannot plausibly match anything else in the file. Asserting it is found
    // ONCE is what keeps this a patch of the field it means rather than of
    // whatever happened to look like it.
    const std::uint64_t idle = reloaded.world().captains()[0].who;
    std::vector<std::uint8_t> bytes;
    SOL_REQUIRE(sol::platform::readFileBytes(path.c_str(), bytes));
    std::uint8_t needle[12] = {};
    std::memcpy(needle, &idle, 8);
    std::memcpy(needle + 8, &kNone, 4);
    const std::uint32_t want = static_cast<std::uint32_t>(bought);
    std::size_t found = 0;
    std::size_t at = 0;
    for (std::size_t i = 0; i + sizeof(needle) <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, needle, sizeof(needle)) == 0) {
            ++found;
            at = i + 8;
        }
    }
    SOL_REQUIRE(found == 1);
    std::memcpy(bytes.data() + at, &want, 4);
    const std::string broken = dir + "/twohulls.sav";
    SOL_REQUIRE(sol::platform::writeFileBytes(broken.c_str(), bytes.data(), bytes.size()));
    Fixture refused;
    SOL_CHECK(!refused.world().loadFrom(broken.c_str()));
}
