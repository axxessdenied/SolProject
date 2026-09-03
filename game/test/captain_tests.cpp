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

// A HALF-SECOND STEP, AND IT IS NOT A SHORTCUT. A haul is quoted at
// `traderLegSeconds * 2 + hops * jumpSeconds` = 180 s at minimum and 280 s
// across five gates, so a leg at 1/60 is seventeen thousand frames of full
// physics for a fact the coarse layer settles on its own clock. Half a second
// keeps every coarse consumer honest - the economy still steps at its own one
// second interval internally - while costing a few hundred ticks a leg.
constexpr double kCoarseStep = 0.5;

// Runs the world until `captain` next parks, or until the budget runs out.
// Returns false on the budget, which is what makes a stalled haul a failing
// test rather than a hanging one.
[[nodiscard]] bool runUntilParked(SpaceWorld& world, std::size_t captain, int maxSteps = 2000)
{
    for (int i = 0; i < maxSteps; ++i) {
        world.tick(kCoarseStep);
        if (captain >= world.captains().size()) {
            return false;
        }
        if (world.captains()[captain].haul.leg.phase == sol::sim::TraderPhase::Idle) {
            return true;
        }
    }
    return false;
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

// ---------------------------------------------------------------------------
// Stage B: the itinerant half.
// ---------------------------------------------------------------------------

// ⚑⚑⚑⚑ THE CLAIM THIS ONE EXISTS FOR IS THE ARITHMETIC OF RULING 6, AND IT IS
// PINNED BY AN IDENTITY RATHER THAN BY A NUMBER. What the player's purse gained
// over a round trip must equal what the captain's own ledger says they earned:
// the purse pays `outlay` at the buy and receives `gross - cut` at the sale,
// and `earned` accumulates `profit - cut` - so the two agree only if the cost
// basis, the sale and the cut are all booked against the same haul. Get any one
// of the three wrong and the two numbers separate.
//
// ⚑⚑⚑ AND THE SECOND HALF IS THE RULING ITSELF: `paid / (earned + paid)` is
// the captain's cut EXACTLY, because `earned + paid` is the profit. A cut taken
// off the SALE instead - the alternative the user declined - fails this by the
// whole cost of the cargo, which on a thin margin is most of the trade.
SOL_TEST(a_haul_pays_the_player_the_profit_and_the_captain_a_cut_of_it)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Trade));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));

    const std::size_t hull = fixture.buyAnyShip();
    SOL_REQUIRE(hull != 0);
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, hull));

    // Market intel is what a fleet's reach is made of: a captain can only be
    // sent where the player remembers a price from, so buying the report is
    // the same act that widens the list on the Crew tab.
    fixture.world().addCredits(1'000'000.0);
    SOL_REQUIRE(fixture.world().buyMarketIntel());
    std::vector<SpaceWorld::HaulDestination> places;
    fixture.world().haulDestinations(places);
    SOL_REQUIRE(!places.empty());
    std::printf("  %zu destination(s); nearest %s (%s) at %u jump(s)\n",
                places.size(),
                places[0].station.c_str(),
                places[0].system.c_str(),
                places[0].hops);

    // Walk the destinations until one of them is worth running. A pair of
    // stations with no margin either way is a fact about the galaxy, not a
    // defect - so the test looks for a live route rather than asserting the
    // first one is.
    bool paidOut = false;
    for (std::size_t p = 0; p < places.size() && !paidOut; ++p) {
        SOL_REQUIRE(fixture.walkIn(yard));
        if (!fixture.world().orderHaul(0, places[p].market)) {
            continue;
        }
        for (int legs = 0; legs < 4 && !paidOut; ++legs) {
            const double before = fixture.world().playerCredits();
            const double earnedBefore = fixture.world().captains()[0].haul.earned;
            SOL_REQUIRE(runUntilParked(fixture.world(), 0));
            const game::Captain& captain = fixture.world().captains()[0];
            const double gained = fixture.world().playerCredits() - before;
            const double booked = captain.haul.earned - earnedBefore;
            // The identity. A tenth of a credit of slack over a five-figure
            // trade is float, not a bug.
            SOL_CHECK(std::abs(gained - booked) < 0.5);
            paidOut = captain.haul.paid > 0.0;
        }
        if (!paidOut) {
            SOL_REQUIRE(fixture.world().cancelOrder(0));
            (void)runUntilParked(fixture.world(), 0);
        }
    }
    SOL_REQUIRE(paidOut); // no route in reach ever made money: that is a finding

    const game::Captain& captain = fixture.world().captains()[0];
    const double profit = captain.haul.earned + captain.haul.paid;
    SOL_REQUIRE(profit > 0.0);
    const double share = captain.haul.paid / profit;
    std::printf("  %s took %.1f%% of %.0f cr profit (their cut is %.1f%%)\n",
                captain.name.c_str(),
                share * 100.0,
                profit,
                static_cast<double>(captain.cut) * 100.0);
    SOL_CHECK(std::abs(share - static_cast<double>(captain.cut)) < 0.01);
    // The anti-vacuity half: a cut of ZERO would satisfy the identity above and
    // every ruling-6 assertion, and would mean the captain works for nothing.
    SOL_CHECK(captain.cut >= SpaceWorld::kCaptainCutMin);
}

// ⚑⚑⚑⚑ A HULL ON A ROUTE IS OUT OF REACH THROUGH ALL THREE DOORS, and the
// third one is the hole stage A left. `sellShip` grew a captain guard and
// `switchShip` beside it did not - and taking the active seat in a captain's
// hull is worse than untidy: it sets `captain.ship == m_activeShip`, which the
// LOADER refuses outright, so the game would write a save it then declines to
// open with nothing at either end saying why.
SOL_TEST(a_hull_a_captain_is_flying_cannot_be_sold_boarded_or_handed_back)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Trade));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));

    const std::size_t hull = fixture.buyAnyShip();
    SOL_REQUIRE(hull != 0);
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, hull));

    // ⚑⚑ PARKED AND UNORDERED, THE SWITCH IS ALREADY REFUSED - and this is the
    // assertion stage A was missing, before any route exists. Without the guard
    // this call SUCCEEDS and the resulting save cannot be loaded.
    std::string why;
    SOL_CHECK(!fixture.world().switchShip(hull, &why));
    SOL_CHECK(why.find(fixture.world().captains()[0].name) != std::string::npos);
    SOL_CHECK(fixture.world().activeShipIndex() != hull);

    fixture.world().addCredits(1'000'000.0);
    SOL_REQUIRE(fixture.world().buyMarketIntel());
    std::vector<SpaceWorld::HaulDestination> places;
    fixture.world().haulDestinations(places);
    SOL_REQUIRE(!places.empty());
    SOL_REQUIRE(fixture.world().orderHaul(0, places[0].market));

    // One tick puts them on the leg: `captainThink` runs on the first tick
    // after the order and `beginCaptainTransit` unparks the hull.
    fixture.world().tick(kCoarseStep);
    SOL_REQUIRE(fixture.world().captains()[0].haul.leg.phase == sol::sim::TraderPhase::InTransit);
    SOL_REQUIRE(fixture.world().fleet()[hull].storedSystem == kNone);

    SOL_CHECK(!fixture.world().sellShip(hull));
    SOL_CHECK(!fixture.world().switchShip(hull));
    SOL_CHECK(!fixture.world().recallCaptain(0));
    SOL_CHECK(!fixture.world().dismissCaptain(0));
    // ...and the fleet still has it, which is the half that says the refusals
    // above are guards rather than silent no-ops on a hull already gone.
    SOL_CHECK(fixture.world().fleet().size() == 2);
    SOL_CHECK(fixture.world().captains()[0].ship == static_cast<std::uint32_t>(hull));
}

// ⚑⚑⚑⚑ THE CONTROL WORLD THE PHASE'S RISK REGISTER ASKS FOR, IN THE WORDS IT
// ASKS FOR THEM: "the same captain, observed and unobserved, must agree on
// position, cargo and credits". Two worlds off one seed run the same order for
// the same time; in one the player stands in the system the haul departs
// through, so the bubble is instantiated and a hull appears in the sky. The
// records must not diverge by so much as a unit of ore - the body is a VIEW,
// and the moment it owns any of the haul this fails.
//
// ⚑⚑ AND THE ANTI-VACUITY HALF IS THE WHOLE TEST: a promotion that never
// happened would pass the agreement trivially. `captainPuppetInfo` must be
// non-empty on the observed side, or the two worlds agree because nothing
// happened in either.
SOL_TEST(a_watched_haul_and_an_unwatched_one_agree_and_the_watched_one_has_a_hull)
{
    const auto arm = [](Fixture& fixture, const Dock& yard, std::uint32_t& outMarket) {
        SpaceWorld& world = fixture.world();
        SOL_REQUIRE(fixture.walkIn(yard));
        const std::size_t hull = fixture.buyAnyShip();
        SOL_REQUIRE(hull != 0);
        SOL_REQUIRE(world.hireCaptain(0));
        SOL_REQUIRE(world.assignCaptain(0, hull));
        world.addCredits(1'000'000.0);
        SOL_REQUIRE(world.buyMarketIntel());
        std::vector<SpaceWorld::HaulDestination> places;
        world.haulDestinations(places);
        SOL_REQUIRE(!places.empty());
        outMarket = places[0].market;
        SOL_REQUIRE(world.orderHaul(0, places[0].market));
    };

    Fixture watched;
    Fixture blind;
    const Dock yard = watched.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Trade));
    SOL_REQUIRE(yard.system != kNone);
    std::uint32_t marketA = kNone;
    std::uint32_t marketB = kNone;
    arm(watched, yard, marketA);
    arm(blind, yard, marketB);
    SOL_REQUIRE(marketA == marketB); // one seed, one galaxy, one nearest market

    // The watched player undocks and stays put: their own system is
    // instantiated, and the captain's outbound leg runs through it.
    SOL_REQUIRE(watched.world().undock());

    bool sawHull = false;
    std::vector<game::CaptainPuppetInfo> bodies;
    for (int i = 0; i < 400; ++i) {
        watched.world().tick(kCoarseStep);
        blind.world().tick(kCoarseStep);
        watched.world().captainPuppetInfo(bodies);
        if (!bodies.empty()) {
            if (!sawHull) {
                std::printf("  %s's %s promoted at %.0f km%s\n",
                            bodies[0].name.c_str(),
                            bodies[0].ship.c_str(),
                            bodies[0].distance / 1000.0,
                            bodies[0].paced ? ", on the record's schedule" : "");
            }
            sawHull = true;
        }
        const game::Captain& a = watched.world().captains()[0];
        const game::Captain& b = blind.world().captains()[0];
        // Position, cargo and credits, checked EVERY step rather than at the
        // end: a divergence that healed itself would otherwise pass.
        SOL_REQUIRE(a.haul.leg.phase == b.haul.leg.phase);
        SOL_REQUIRE(std::abs(a.haul.leg.travelRemaining - b.haul.leg.travelRemaining) < 1.0e-9);
        SOL_REQUIRE(a.haul.leg.cargo == b.haul.leg.cargo);
        SOL_REQUIRE(a.haul.leg.market == b.haul.leg.market);
    }
    SOL_CHECK(sawHull);
    // The credits agree too, and they are the number a doubled income would
    // separate first.
    SOL_CHECK(std::abs(watched.world().playerCredits() - blind.world().playerCredits()) < 0.5);
    SOL_CHECK(std::abs(watched.world().captains()[0].haul.earned - blind.world().captains()[0].haul.earned) <
              0.5);
}

// ⚑⚑ A CANCEL LANDS THE SHIP RATHER THAN STOPPING IT WHERE IT IS. Standing a
// captain down mid-leg would leave a laden hull between two gates with nothing
// that knows how to park it, which is the "falls between the two
// representations" defect the risk register names - reached by a button.
SOL_TEST(a_cancelled_order_finishes_the_leg_and_parks_the_hull_where_it_landed)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Trade));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));
    const std::size_t hull = fixture.buyAnyShip();
    SOL_REQUIRE(hull != 0);
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, hull));
    fixture.world().addCredits(1'000'000.0);
    SOL_REQUIRE(fixture.world().buyMarketIntel());
    std::vector<SpaceWorld::HaulDestination> places;
    fixture.world().haulDestinations(places);
    SOL_REQUIRE(!places.empty());
    SOL_REQUIRE(fixture.world().orderHaul(0, places[0].market));

    fixture.world().tick(kCoarseStep);
    SOL_REQUIRE(fixture.world().captains()[0].haul.leg.phase == sol::sim::TraderPhase::InTransit);
    SOL_REQUIRE(fixture.world().cancelOrder(0));
    // Not immediate: the order is still standing while the leg is flown.
    SOL_CHECK(fixture.world().captains()[0].order.stopping);
    SOL_CHECK(fixture.world().captains()[0].order.kind == game::OrderKind::Haul);

    SOL_REQUIRE(runUntilParked(fixture.world(), 0));
    // One more tick for the idle pass that reads `stopping`.
    fixture.world().tick(kCoarseStep);
    const game::Captain& captain = fixture.world().captains()[0];
    SOL_CHECK(captain.order.kind == game::OrderKind::None);
    SOL_CHECK(captain.haul.leg.phase == sol::sim::TraderPhase::Idle);
    // ⚑ AND THE HULL IS SOMEWHERE, which is what makes "fly to them and find
    // them where the screen said" answerable. A hull left with `kNoIndex` here
    // is one no dock can ever hand back.
    const std::uint32_t at = fixture.world().fleet()[hull].storedSystem;
    SOL_REQUIRE(at < fixture.world().galaxy().systems.size());
    SOL_CHECK(fixture.world().captainSystem(0) == at);
    std::printf("  stood down at %s\n", fixture.world().galaxy().systems[at].name.c_str());
    // Cancelling again is refused: there is nothing left to stand down.
    SOL_CHECK(!fixture.world().cancelOrder(0));
}

// ⚑⚑ AN ORDER IS ONLY GIVEN WHERE BOTH OF YOU ARE, AND ONLY TO SOMEWHERE YOU
// HAVE SEEN. Both halves matter: the first is `assignCaptain`'s rule (a route
// starts where the hull is, so a list measured from another dock is a list of
// the wrong distances), and the second is what keeps this from being a
// dropdown of all eighty systems.
SOL_TEST(a_route_is_given_on_the_hulls_own_dock_and_only_to_a_market_you_remember)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Trade));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));
    const std::size_t hull = fixture.buyAnyShip();
    SOL_REQUIRE(hull != 0);
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, hull));

    // Before any intel, the only market the player has ever read is the one
    // they are standing on - and a run to where you are standing is not a run.
    std::vector<SpaceWorld::HaulDestination> places;
    fixture.world().haulDestinations(places);
    const std::size_t knownAtFirst = places.size();
    std::printf("  %zu destination(s) before intel\n", knownAtFirst);

    fixture.world().addCredits(1'000'000.0);
    SOL_REQUIRE(fixture.world().buyMarketIntel());
    fixture.world().haulDestinations(places);
    SOL_REQUIRE(places.size() > knownAtFirst); // the report is what widened it
    for (const SpaceWorld::HaulDestination& place : places) {
        SOL_CHECK(place.market != fixture.world().dockedMarket());
        SOL_CHECK(place.hops <= fixture.world().economy().params().maxTradeJumps);
    }
    // Sorted nearest first, which is the only ordering a player can navigate.
    for (std::size_t i = 1; i < places.size(); ++i) {
        SOL_CHECK(places[i - 1].hops <= places[i].hops);
    }

    // A market index that is real but never seen is refused. ⚑ Found by
    // looking for one the ledger has no row for, rather than by using an
    // out-of-range number - which would be caught by the bounds check instead
    // and would leave the knowledge rule untested.
    std::uint32_t unseen = kNone;
    for (std::uint32_t m = 0; m < fixture.world().economy().markets().size(); ++m) {
        if (std::none_of(places.begin(),
                         places.end(),
                         [m](const SpaceWorld::HaulDestination& d) { return d.market == m; }) &&
            m != fixture.world().dockedMarket()) {
            unseen = m;
            break;
        }
    }
    SOL_REQUIRE(unseen != kNone);
    SOL_CHECK(!fixture.world().orderHaul(0, unseen));

    // And from another dock entirely, the hull is not here and nothing can be
    // ordered - even to a market this player does remember.
    const std::uint32_t reachable = places[0].market;
    const Dock elsewhere = fixture.findDock(screenBit(StationScreen::Crew), 0, yard);
    SOL_REQUIRE(elsewhere.system != kNone);
    SOL_REQUIRE(fixture.walkIn(elsewhere));
    SOL_CHECK(!fixture.world().orderHaul(0, reachable));
    SOL_CHECK(fixture.world().captains()[0].order.kind == game::OrderKind::None);
}

// ⚑⚑⚑ A SAVE TAKEN MID-HAUL LOADS INTO THE SAME HAUL. The order is what the
// player told them and the leg is where they are; without both, a reload
// teleports a laden hull to one end of a run the player already paid for.
SOL_TEST(a_save_carries_the_order_and_the_leg_and_refuses_a_clock_that_runs_backwards)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Trade));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));
    const std::size_t hull = fixture.buyAnyShip();
    SOL_REQUIRE(hull != 0);
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, hull));
    fixture.world().addCredits(1'000'000.0);
    SOL_REQUIRE(fixture.world().buyMarketIntel());
    std::vector<SpaceWorld::HaulDestination> places;
    fixture.world().haulDestinations(places);
    SOL_REQUIRE(!places.empty());
    SOL_REQUIRE(fixture.world().orderHaul(0, places[0].market));
    for (int i = 0; i < 40; ++i) {
        fixture.world().tick(kCoarseStep);
    }
    const game::Captain& before = fixture.world().captains()[0];
    SOL_REQUIRE(before.haul.leg.phase == sol::sim::TraderPhase::InTransit);
    const game::CaptainOrder order = before.order;
    const sol::sim::EconomyTrader leg = before.haul.leg;
    const double earned = before.haul.earned;
    const double outlay = before.haul.outlay;

    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/captains-b";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/haul.sav";
    SOL_REQUIRE(fixture.world().saveTo(path.c_str(), "Haul"));

    Fixture reloaded;
    SOL_REQUIRE(reloaded.world().loadFrom(path.c_str()));
    SOL_REQUIRE(reloaded.world().captains().size() == 1);
    const game::Captain& after = reloaded.world().captains()[0];
    SOL_CHECK(after.order.kind == order.kind);
    SOL_CHECK(after.order.marketA == order.marketA);
    SOL_CHECK(after.order.marketB == order.marketB);
    SOL_CHECK(after.haul.leg.phase == leg.phase);
    SOL_CHECK(after.haul.leg.origin == leg.origin);
    SOL_CHECK(after.haul.leg.market == leg.market);
    SOL_CHECK(after.haul.leg.travelRemaining == leg.travelRemaining);
    SOL_CHECK(after.haul.leg.legTotal == leg.legTotal);
    SOL_CHECK(after.haul.leg.cargo == leg.cargo);
    SOL_CHECK(after.haul.outlay == outlay);
    SOL_CHECK(after.haul.earned == earned);
    // The leg it resumes on is the leg it was flying, which is the fact the
    // save exists for - a route that restarted would be free money.
    SOL_CHECK(reloaded.world().captainRoute(0).leg == fixture.world().captainRoute(0).leg);
    std::printf("  resumed %.0f s into a %.0f s leg with %.0f aboard\n",
                leg.legTotal - leg.travelRemaining,
                leg.legTotal,
                static_cast<double>(leg.cargo));

    // ⚑ AND A CLOCK THAT READS PAST THE END OF ITS OWN LEG IS REFUSED. Written
    // by finding the `legTotal` that follows this captain's `travelRemaining`
    // on disk and doubling the remainder - the same 'patch the field you mean'
    // discipline the two-hulls check above uses, and for the same reason.
    std::vector<std::uint8_t> bytes;
    SOL_REQUIRE(sol::platform::readFileBytes(path.c_str(), bytes));
    std::size_t found = 0;
    std::size_t at = 0;
    for (std::size_t i = 0; i + 16 <= bytes.size(); ++i) {
        double a = 0.0;
        double b = 0.0;
        std::memcpy(&a, bytes.data() + i, 8);
        std::memcpy(&b, bytes.data() + i + 8, 8);
        if (a == leg.travelRemaining && b == leg.legTotal) {
            ++found;
            at = i;
        }
    }
    SOL_REQUIRE(found == 1);
    const double impossible = leg.legTotal * 2.0;
    std::memcpy(bytes.data() + at, &impossible, 8);
    const std::string broken = dir + "/backwards.sav";
    SOL_REQUIRE(sol::platform::writeFileBytes(broken.c_str(), bytes.data(), bytes.size()));
    Fixture refused;
    SOL_CHECK(!refused.world().loadFrom(broken.c_str()));
}

// ⚑⚑⚑⚑ AN ORDERED CAPTAIN WHO HAS NOT LEFT YET IS STILL NOT YOURS TO TAKE THE
// SHIP BACK FROM, AND THE LIVE DRIVE IS WHAT FOUND THIS. Every test above
// checks the refusals against a hull IN TRANSIT, where `storedSystem` is
// `kNoIndex` and so all three doors shut on the stored-ship rule alone. The
// window this covers is the one between giving a route and the first tick: the
// hull is still parked on your dock, so every one of those conditions passes.
//
// ⚑⚑⚑ AND WHAT IT PRODUCES IS NOT UNTIDINESS, IT IS AN UNLOADABLE SAVE. Recall
// leaves `ship == kNoIndex` beside a live `order.kind`, which is the exact pair
// the v40 loader refuses - so the game would write a file it then declines to
// open, with nothing at either end saying why. Same shape as `switchShip`
// above, found the same way stage A's clipped price cell was: by flying it.
SOL_TEST(a_captain_with_orders_keeps_the_ship_even_before_the_first_tick)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Trade));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));
    const std::size_t hull = fixture.buyAnyShip();
    SOL_REQUIRE(hull != 0);
    SOL_REQUIRE(fixture.world().hireCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, hull));
    fixture.world().addCredits(1'000'000.0);
    SOL_REQUIRE(fixture.world().buyMarketIntel());
    std::vector<SpaceWorld::HaulDestination> places;
    fixture.world().haulDestinations(places);
    SOL_REQUIRE(!places.empty());

    // Recall works right up to the moment an order exists...
    SOL_REQUIRE(fixture.world().recallCaptain(0));
    SOL_REQUIRE(fixture.world().assignCaptain(0, hull));

    // ...and not after. NOT ONE TICK IS RUN HERE: the hull is still parked on
    // this dock, which is what makes this a different window from every other
    // refusal in this file.
    SOL_REQUIRE(fixture.world().orderHaul(0, places[0].market));
    SOL_REQUIRE(fixture.world().captains()[0].haul.leg.phase == sol::sim::TraderPhase::Idle);
    SOL_REQUIRE(fixture.world().fleet()[hull].storedSystem == fixture.world().currentSystemIndex());
    std::string why;
    SOL_CHECK(!fixture.world().recallCaptain(0, &why));
    SOL_CHECK(why.find("stand them down") != std::string::npos);
    SOL_CHECK(!fixture.world().dismissCaptain(0));
    SOL_CHECK(fixture.world().captains().size() == 1);
    SOL_CHECK(fixture.world().captains()[0].ship == static_cast<std::uint32_t>(hull));

    // The state the loader refuses must therefore be unreachable, and the
    // round trip is what says so rather than a comment.
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/captains-b";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/ordered.sav";
    SOL_REQUIRE(fixture.world().saveTo(path.c_str(), "Ordered"));
    Fixture reloaded;
    SOL_REQUIRE(reloaded.world().loadFrom(path.c_str()));
    SOL_CHECK(reloaded.world().captains()[0].order.kind == game::OrderKind::Haul);

    // Standing down first is the way through, and then the hull comes back.
    SOL_REQUIRE(fixture.world().cancelOrder(0));
    SOL_CHECK(fixture.world().captains()[0].order.kind == game::OrderKind::None);
    SOL_REQUIRE(fixture.world().recallCaptain(0));
    SOL_REQUIRE(fixture.world().dismissCaptain(0));
    SOL_CHECK(fixture.world().captains().empty());
}
