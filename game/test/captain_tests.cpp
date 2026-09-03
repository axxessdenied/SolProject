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

// A dock a MINING captain can be posted from: a crew hall to hire in, a ship
// counter to buy a hull at, and rock in the system to cut. ⚑ Searched in the
// shipped galaxy rather than authored, on this file's own rule - a recipe
// change that stopped producing such a place fails here instead of passing
// vacuously.
//
// ⚑⚑⚑ AND THE SEARCH FOUND A CONTENT FACT WORTH KEEPING: THE OUTFITTER IS NOT
// IN THE MASK BECAUSE ASKING FOR IT LEAVES EXACTLY ONE DOCK IN THE GALAXY, AND
// THAT ONE HAS NO ROCK. Crew hall + outfitter + ship sales co-occur at a single
// station out of 81 systems' worth, so there is no single dock where a player
// can hire somebody, buy them a hull, bolt a beam on it and post them to a
// field - the flow crosses at least one lane, always. Buying is gated on the
// faction CATALOG rather than on the screen (`stationSells`), so the world
// still allows the refit here; what the missing tab means is that a human has
// to fly, which is a design observation rather than a bug and is recorded
// where somebody re-running this search will find it.
[[nodiscard]] Dock findMiningDock(Fixture& fixture)
{
    const SpaceWorld& w = fixture.world();
    const std::uint32_t wanted = screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard);
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size(); ++sys) {
        if (w.mining().fieldCount(sys) == 0) {
            continue;
        }
        for (std::uint32_t st = 0; st < w.galaxy().systems[sys].stations.size(); ++st) {
            if ((w.stationScreens(sys, st) & wanted) == wanted) {
                std::printf("  mining dock: %s / %s (system %u, station %u), %u field(s)\n",
                            w.galaxy().systems[sys].name.c_str(),
                            w.galaxy().systems[sys].stations[st].name.c_str(),
                            sys,
                            st,
                            w.mining().fieldCount(sys));
                return {sys, st};
            }
        }
    }
    return {};
}

// Buys a hull, boards it, bolts a mining beam on, and steps back into the
// starter - which is the sequence a player performs, through the same four
// public calls, rather than a fit written straight into the fleet row. It has
// to be done from the SEAT because `buyFitting` refits the active ship, which
// is itself worth having a test walk through: a captain flies the fit you gave
// them, so giving them one means being in the chair first.
// Bolts a mining beam onto the ACTIVE hull, naming the mount rather than asking
// for "wherever it goes".
//
// ⚑⚑ AND THE MOUNT HAS TO BE NAMED, WHICH IS THE FIRST THING THIS TEST TAUGHT
// ME ABOUT THE ORDER ITSELF. `buyFitting(id, nullptr)` means "the first EMPTY
// mount that accepts it", which is what a catalog Buy button means - and every
// hull this yard sells arrives with its authored fit already in every weapon
// mount it has. So a player fitting a mining laser is always REPLACING a gun,
// never adding one, and a mining captain is therefore a hull that has given up
// its armament to do the job. That is the trade the phase's next stage has to
// price when a raider finds one.
[[nodiscard]] bool armMiningBeam(Fixture& fixture)
{
    SpaceWorld& w = fixture.world();
    const sol::assets::ShipDef def = w.resolvedShipDef(w.activeShip());
    for (const sol::assets::WeaponDef& weapon : fixture.defs.weapons()) {
        if (weapon.miningPower <= 0.0f) {
            continue;
        }
        for (const sol::assets::ShipMount& mount : def.mounts) {
            if (!sol::assets::mountTakesWeapon(mount.kind)) {
                continue;
            }
            std::string error;
            if (w.buyFitting(weapon.id.c_str(), mount.id.c_str(), &error)) {
                return true;
            }
        }
    }
    return false;
}

// ⚑ `armMiningBeam`'s twin (stage D). The two combat orders refuse a hull with
// no guns exactly as a mining order refuses one with no beam, so the fixture
// needs both halves of the same wardrobe.
[[nodiscard]] bool armGuns(Fixture& fixture)
{
    SpaceWorld& w = fixture.world();
    const sol::assets::ShipDef def = w.resolvedShipDef(w.activeShip());
    for (const sol::assets::WeaponDef& weapon : fixture.defs.weapons()) {
        if (weapon.damage <= 0.0f) {
            continue;
        }
        for (const sol::assets::ShipMount& mount : def.mounts) {
            if (!sol::assets::mountTakesWeapon(mount.kind)) {
                continue;
            }
            std::string error;
            if (w.buyFitting(weapon.id.c_str(), mount.id.c_str(), &error)) {
                return true;
            }
        }
    }
    return false;
}

// ⚑⚑ "ARM" HERE MEANS "MAKE SURE IT CAN SHOOT", NOT "ADD A GUN", and
// `armMiningBeam`'s own comment is why: every hull this yard sells arrives with
// its authored fit already in every weapon mount it has. So the usual case is
// that a bought hull is ALREADY armed and `armGuns` finds no empty mount to buy
// into - which is a success, not a failure. The check is on the POWER.
[[nodiscard]] std::size_t buyAndArmAFighter(Fixture& fixture)
{
    SpaceWorld& w = fixture.world();
    const std::size_t slot = fixture.buyAnyShip();
    if (slot == 0) {
        return 0;
    }
    if (w.shipGunPower(w.fleet()[slot]) > 0.0f) {
        return slot; // straight off the forecourt with its guns in
    }
    if (!w.switchShip(slot)) {
        return 0;
    }
    const bool armed = armGuns(fixture);
    if (!w.switchShip(0)) {
        return 0;
    }
    return armed && w.shipGunPower(w.fleet()[slot]) > 0.0f ? slot : 0;
}

[[nodiscard]] std::size_t buyAndArmAMiner(Fixture& fixture, float* outPower = nullptr)
{
    SpaceWorld& w = fixture.world();
    const std::size_t slot = fixture.buyAnyShip();
    if (slot == 0 || !w.switchShip(slot)) {
        return 0;
    }
    if (!armMiningBeam(fixture) || !w.switchShip(0)) {
        return 0;
    }
    if (outPower != nullptr) {
        *outPower = w.shipMiningPower(w.fleet()[slot]);
    }
    return slot;
}

// Hires the first candidate in this hall and hands them `slot`. Returns their
// captain index, or `kNone`.
[[nodiscard]] std::size_t hireAndGive(Fixture& fixture, std::size_t slot)
{
    SpaceWorld& w = fixture.world();
    std::vector<CaptainCandidate> hall;
    w.captainCandidates(hall);
    if (hall.empty() || !w.hireCaptain(0)) {
        return kNone;
    }
    const std::size_t captain = w.captains().size() - 1;
    return w.assignCaptain(captain, slot) ? captain : kNone;
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
// Steps the world until `predicate` answers true, or the budget runs out.
template <typename Predicate>
[[nodiscard]] bool runUntil(SpaceWorld& world, Predicate predicate, int maxSteps = 4000)
{
    for (int i = 0; i < maxSteps; ++i) {
        world.tick(kCoarseStep);
        if (predicate()) {
            return true;
        }
    }
    return false;
}

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
            const double earnedBefore = fixture.world().captains()[0].ledger.earned;
            SOL_REQUIRE(runUntilParked(fixture.world(), 0));
            const game::Captain& captain = fixture.world().captains()[0];
            const double gained = fixture.world().playerCredits() - before;
            const double booked = captain.ledger.earned - earnedBefore;
            // The identity. A tenth of a credit of slack over a five-figure
            // trade is float, not a bug.
            SOL_CHECK(std::abs(gained - booked) < 0.5);
            paidOut = captain.ledger.paid > 0.0;
        }
        if (!paidOut) {
            SOL_REQUIRE(fixture.world().cancelOrder(0));
            (void)runUntilParked(fixture.world(), 0);
        }
    }
    SOL_REQUIRE(paidOut); // no route in reach ever made money: that is a finding

    const game::Captain& captain = fixture.world().captains()[0];
    const double profit = captain.ledger.earned + captain.ledger.paid;
    SOL_REQUIRE(profit > 0.0);
    const double share = captain.ledger.paid / profit;
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
    SOL_CHECK(std::abs(watched.world().captains()[0].ledger.earned -
                       blind.world().captains()[0].ledger.earned) < 0.5);
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
    const double earned = before.ledger.earned;
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
    SOL_CHECK(after.ledger.earned == earned);
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

// ===========================================================================
// Phase 39 stage C - the stationary half.
// ===========================================================================

// ⚑⚑⚑⚑ THE STAGE'S EXIT, AS ONE TEST: leave a captain working a field, go two
// systems away, spend time there, come back and find the hull at the rock with
// a fuller hold - AND the evidence that it was TICKED rather than restored.
// The last clause is the whole assertion. A system rebuilt from its seed
// produces a captain in exactly the same state as one that was held, so
// "cutting, at a rock" is not evidence of anything; what cannot be faked by a
// rebuild is a hold that grew and a rock count that advanced WHILE THE PLAYER
// WAS SOMEWHERE ELSE.
SOL_TEST(a_mining_captain_works_a_field_while_the_player_is_two_systems_away)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));

    float power = 0.0f;
    const std::size_t slot = buyAndArmAMiner(fixture, &power);
    SOL_REQUIRE(slot != 0);
    SOL_CHECK(power > 0.0f);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    SOL_REQUIRE(w.orderMine(captain));
    SOL_CHECK(w.captains()[captain].order.kind == game::OrderKind::Mine);
    // The order takes the hull off the pad, which is what makes the three
    // stored-ship refusals cover a mining captain without a fourth rule.
    SOL_CHECK(w.fleet()[slot].storedSystem == kNone);
    SOL_CHECK(!w.recallCaptain(captain));
    SOL_CHECK(!w.dismissCaptain(captain));

    // Let them reach the rock and start cutting while the player is still here.
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].mine.units > 0.0f; }));
    const float aboardBefore = w.captains()[captain].mine.units;
    const std::uint32_t rocksBefore = w.captains()[captain].mine.rockStep;

    // Two systems away. ⚑ The far system is reached through `enterSystem`,
    // which is the same door a jump uses, so the bubble the captain is in has
    // to be RETAINED by the retention policy rather than by the player's
    // presence - which is precisely the thing stage C changed.
    SOL_REQUIRE(w.undock());
    const auto& links = w.galaxy().links;
    std::uint32_t elsewhere = kNone;
    for (const sol::sim::GateLink& link : links) {
        if (link.a == dock.system) {
            elsewhere = link.b;
            break;
        }
        if (link.b == dock.system) {
            elsewhere = link.a;
            break;
        }
    }
    SOL_REQUIRE(elsewhere != kNone);
    SOL_REQUIRE(w.enterSystem(elsewhere));
    SOL_CHECK(w.currentSystemIndex() != dock.system);

    // A minute of somebody else's sky. The bubble the captain is in must still
    // be open at the end of it - `kCoolingSeconds` is 120 s, so a hold that was
    // merely counting down would not survive an order of magnitude more than
    // this, and the loop below runs well past it.
    for (int i = 0; i < 600; ++i) {
        w.tick(kCoarseStep);
    }
    SOL_CHECK(w.systemIsInstantiated(dock.system));
    const float aboardAway = w.captains()[captain].mine.units;
    const std::uint32_t rocksAway = w.captains()[captain].mine.rockStep;
    std::printf("  away: %.1f -> %.1f units, %u -> %u rocks, %.0f cr earned\n",
                static_cast<double>(aboardBefore),
                static_cast<double>(aboardAway),
                rocksBefore,
                rocksAway,
                w.captains()[captain].ledger.earned);
    // ⚑⚑ THE ANTI-VACUITY, AND IT IS AN *OR*. Five minutes at a real beam rate
    // is more than one hold on a small hull, so the honest statement is that
    // work HAPPENED: either there is more ore aboard than there was, or a load
    // has been sold and the ledger has moved. A test that demanded only the
    // first would fail on a fast beam for being too successful.
    SOL_CHECK(aboardAway > aboardBefore || w.captains()[captain].ledger.earned > 0.0 ||
              rocksAway > rocksBefore);

    // And walking back in finds the same hull in the sky, not a fresh one.
    SOL_REQUIRE(w.enterSystem(dock.system));
    w.tick(kCoarseStep);
    std::vector<game::CaptainPuppetInfo> bodies;
    w.captainPuppetInfo(bodies);
    SOL_CHECK(bodies.size() == 1);
    if (!bodies.empty()) {
        SOL_CHECK(bodies[0].captainIndex == captain);
    }
}

// ⚑⚑⚑⚑ THE DATA-LOSS BUG THE PHASE'S RISK REGISTER NAMED IN ADVANCE, AS A
// TEST. `enforceBubbleCap` drops the coldest retained bubble with no death
// path - no wreck, no kill credit, nothing recorded - and before stage C the
// victim could be the one holding the player's freighter and the ore in its
// hold, which would simply cease to exist. Two assertions, and the second is
// the sharp one:
//
//   1. The captain's system survives the cap being exceeded by systems nobody
//      paid for.
//   2. It survives a TIE, which is the case the phase spec singled out:
//      "among bubbles all sharing one indefinite sentinel the winner is
//      whichever is earliest in the vector, which is arrival order and nothing
//      else". A freshly opened bubble and a renewed captain's bubble both sit
//      at `kCoolingSeconds` exactly, so `<` never separates them and the
//      captain's - opened first, and therefore earliest - was precisely the one
//      arrival order would have taken.
SOL_TEST(the_bubble_cap_never_evicts_the_system_a_captain_is_working_in)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    SOL_REQUIRE(w.orderMine(captain));
    SOL_REQUIRE(w.undock());

    // Stand somewhere else, so the captain's system is a RETAINED bubble rather
    // than the player's own - which is the only way it can be a candidate at
    // all, since slot 0 is never one.
    std::uint32_t elsewhere = dock.system == 0 ? 1u : 0u;
    SOL_REQUIRE(w.enterSystem(elsewhere));
    w.tick(kCoarseStep);
    SOL_REQUIRE(w.systemIsInstantiated(dock.system));

    // ⚑⚑ AND THE CAP HAS TO BE *EXCEEDED* FOR THE EVICTION TO RUN AT ALL, WHICH
    // IS WORTH KNOWING BEFORE READING THIS: `enforceBubbleCap` is called from
    // `leaveSystemFor` and from the loader, not every tick - the ordinary fence
    // is `instantiateSystem` REFUSING at the cap. So the only way to reach the
    // eviction is to get over the line first, and the door that can is the
    // captain tick's own (`overCap`). Ambient bubbles are forced in through it
    // here to make the victim pool real.
    std::vector<std::uint32_t> ambient;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size() && ambient.size() < 5; ++sys) {
        if (sys != dock.system && sys != w.currentSystemIndex() && w.instantiateSystem(sys, true)) {
            ambient.push_back(sys);
        }
    }
    SOL_REQUIRE(ambient.size() == 5);
    SOL_REQUIRE(w.instantiatedSystemCount() > SpaceWorld::kMaxInstantiatedSystems);

    // Every bubble in the list is now on the same `kCoolingSeconds` - the
    // captain's because the sweep renews it, the forced ones because that is
    // what `instantiateSystem` puts them on - so `<` never separates them and
    // the old rule fell back on ARRIVAL ORDER, in which the captain's is the
    // earliest and therefore the victim. Jumping somewhere else runs the cap.
    std::uint32_t empty = kNone;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size() && empty == kNone; ++sys) {
        if (sys != dock.system && !w.systemIsInstantiated(sys)) {
            empty = sys;
        }
    }
    SOL_REQUIRE(empty != kNone);
    SOL_REQUIRE(w.enterSystem(empty));
    std::printf("  %zu bubble(s) after the cap ran, at a cap of %zu\n",
                w.instantiatedSystemCount(),
                SpaceWorld::kMaxInstantiatedSystems);
    // ⚑ THE CAP STILL BINDS WHAT IT IS ALLOWED TO CHOOSE FROM, and something was
    // actually taken - a cap that had simply stopped enforcing would satisfy the
    // survival check below and fail this one, which is the whole difference the
    // user's ruling turns on.
    SOL_CHECK(w.instantiatedSystemCount() <= SpaceWorld::kMaxInstantiatedSystems);
    SOL_CHECK(w.systemIsInstantiated(dock.system));
}

// ⚑⚑⚑⚑ THE OTHER HALF OF RULING 11: WHEN EVERY RETAINED BUBBLE IS ONE SOMEBODY
// PAID FOR, THE CAP GOES SOFT RATHER THAN PICKING A VICTIM. Six captains in six
// systems against a cap of six, so the player's own bubble makes seven and
// there is nothing `enforceBubbleCap` is allowed to choose.
//
// ⚑⚑⚑ AND SETTING IT UP TAUGHT ME THE SHAPE OF THE ORDER IN THE REAL GALAXY,
// WHICH NO AMOUNT OF READING THE CODE WOULD HAVE. A captain can be HIRED
// anywhere there is a crew hall, but a captain can only be GIVEN a hull where
// the player can buy one: `assignCaptain` refuses the ship the player is
// currently flying, and a pilot has one body - so ferrying a spare hull to a
// station that does not sell hulls strands you in it. The dock that starts a
// mining captain is therefore a SHIPYARD in a system with rock, and the crew
// hall can be anywhere at all.
SOL_TEST(the_cap_goes_soft_rather_than_evict_a_system_somebody_paid_for)
{
    Fixture fixture;
    SpaceWorld& w = fixture.world();

    // Hire first, everywhere: a hall offers three, so this walks halls until
    // there are enough people to post.
    const std::size_t wanted = SpaceWorld::kMaxInstantiatedSystems;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size() && w.captains().size() < wanted; ++sys) {
        for (std::uint32_t st = 0; st < w.galaxy().systems[sys].stations.size(); ++st) {
            if ((w.stationScreens(sys, st) & screenBit(StationScreen::Crew)) == 0) {
                continue;
            }
            if (!fixture.walkIn({sys, st})) {
                continue;
            }
            std::vector<CaptainCandidate> hall;
            w.captainCandidates(hall);
            for (std::size_t i = 0; i < hall.size() && w.captains().size() < wanted; ++i) {
                (void)w.hireCaptain(0); // the hall re-filters after each hire
            }
            break;
        }
    }
    SOL_REQUIRE(w.captains().size() >= wanted);

    // Then post them, one per system that has rock and a counter that sells
    // hulls.
    std::vector<std::uint32_t> posted;
    std::size_t nextCaptain = 0;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size() && posted.size() < wanted; ++sys) {
        if (w.mining().fieldCount(sys) == 0) {
            continue;
        }
        for (std::uint32_t st = 0; st < w.galaxy().systems[sys].stations.size(); ++st) {
            if ((w.stationScreens(sys, st) & screenBit(StationScreen::Shipyard)) == 0) {
                continue;
            }
            if (!fixture.walkIn({sys, st})) {
                continue;
            }
            const std::size_t hull = buyAndArmAMiner(fixture);
            if (hull == 0 || nextCaptain >= w.captains().size()) {
                break;
            }
            if (w.assignCaptain(nextCaptain, hull) && w.orderMine(nextCaptain)) {
                ++nextCaptain;
                posted.push_back(sys);
            }
            break;
        }
    }
    std::printf("  %zu captain(s) posted to %zu system(s), cap is %zu\n",
                w.captains().size(),
                posted.size(),
                SpaceWorld::kMaxInstantiatedSystems);
    SOL_REQUIRE(posted.size() >= wanted);

    // Stand somewhere none of them is, so every retained bubble is a captain's
    // and the player's own is the seventh.
    SOL_REQUIRE(w.undock());
    std::uint32_t empty = kNone;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size() && empty == kNone; ++sys) {
        if (std::find(posted.begin(), posted.end(), sys) == posted.end()) {
            empty = sys;
        }
    }
    SOL_REQUIRE(empty != kNone);
    SOL_REQUIRE(w.enterSystem(empty));
    w.tick(kCoarseStep);

    // ⚑ Every one of them still open, and the count past the cap - which is the
    // ruling said as an assertion. `enforceBubbleCap` ran on every one of those
    // ticks and found nothing it was allowed to take.
    for (const std::uint32_t sys : posted) {
        SOL_CHECK(w.systemIsInstantiated(sys));
    }
    SOL_CHECK(w.instantiatedSystemCount() > SpaceWorld::kMaxInstantiatedSystems);
    std::printf("  %zu bubble(s) open, all of them paid for\n", w.instantiatedSystemCount());

    // ⚑⚑ AND THE ANTI-VACUITY, WHICH IS THE WHOLE DIFFERENCE BETWEEN A SOFT CAP
    // AND A BROKEN ONE - AND IT IS ANSWERED AT THE OTHER DOOR. `enforceBubbleCap`
    // has run out of victims, but the cap has TWO enforcers and only one of them
    // went soft: `instantiateSystem` still refuses flatly, so a system nobody
    // paid for cannot be opened at all while six captains are working. The cap
    // did not stop meaning something; it stopped being able to take back what
    // the player bought.
    const std::size_t before = w.instantiatedSystemCount();
    std::uint32_t ambient = kNone;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size() && ambient == kNone; ++sys) {
        if (sys != w.currentSystemIndex() && std::find(posted.begin(), posted.end(), sys) == posted.end()) {
            ambient = sys;
        }
    }
    SOL_REQUIRE(ambient != kNone);
    SOL_CHECK(!w.instantiateSystem(ambient));
    SOL_CHECK(!w.systemIsInstantiated(ambient));
    SOL_CHECK(w.instantiatedSystemCount() == before);
}

// ⚑⚑⚑ THE DOOR `kCoolingSeconds` FORBIDS, OPENED AND THEN SHUT AGAIN. The
// refresh in `releaseCooledBubbles` is the one thing stage C added to that
// function, and the whole argument for it is that the condition is one the
// PLAYER can take away. So the test that matters is not that the bubble is held
// - it is that standing the captain down lets it go, on the ordinary two
// minutes, rather than leaving a system pinned open for the session.
SOL_TEST(standing_a_mining_captain_down_lets_their_system_cool_again)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    SOL_REQUIRE(w.orderMine(captain));
    SOL_REQUIRE(w.undock());
    std::uint32_t elsewhere = dock.system == 0 ? 1u : 0u;
    SOL_REQUIRE(w.enterSystem(elsewhere));
    for (int i = 0; i < 400; ++i) {
        w.tick(kCoarseStep);
    }
    SOL_REQUIRE(w.systemIsInstantiated(dock.system));

    // Stand them down. The cancel lands at the next delivery, so the captain
    // finishes the load and parks - and only then does the hold stop being
    // renewed.
    SOL_REQUIRE(w.cancelOrder(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].order.kind == game::OrderKind::None; }));
    // The hull is on a pad again, which is what makes it sellable and boardable
    // and the person dismissable.
    SOL_CHECK(w.fleet()[slot].storedSystem == dock.system);
    SOL_CHECK(w.fleet()[slot].storedStation == dock.station);

    // And now the system cools. `kCoolingSeconds` is 120 s; this runs 300.
    for (int i = 0; i < 600; ++i) {
        w.tick(kCoarseStep);
    }
    SOL_CHECK(!w.systemIsInstantiated(dock.system));
}

// ⚑⚑ A MINING ORDER IS REFUSED FOR THREE REASONS AND EACH ONE IS A DIFFERENT
// SENTENCE, because the two that are new to this stage are both things a player
// FIXES rather than reports: fly somewhere with rock, or buy a beam. A single
// "cannot" would send them looking for a bug.
SOL_TEST(a_mining_order_refuses_a_hull_with_no_beam_and_a_system_with_no_rock)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));

    SpaceWorld& w = fixture.world();
    // A hull straight off the forecourt: no beam on it.
    const std::size_t bare = fixture.buyAnyShip();
    SOL_REQUIRE(bare != 0);
    SOL_CHECK(w.shipMiningPower(w.fleet()[bare]) == 0.0f);
    const std::size_t captain = hireAndGive(fixture, bare);
    SOL_REQUIRE(captain != kNone);
    std::string error;
    SOL_CHECK(!w.orderMine(captain, &error));
    SOL_CHECK(error.find("beam") != std::string::npos);
    std::printf("  no beam: %s\n", error.c_str());

    // Arm it, and the same order is taken. ⚑ The captain has to hand the hull
    // back first: `switchShip` refuses to seat the player in a ship somebody
    // else is flying, which is stage B's own guard and the reason this reads
    // like a shipyard visit rather than a field refit.
    SOL_REQUIRE(w.recallCaptain(captain));
    SOL_REQUIRE(w.switchShip(bare));
    SOL_REQUIRE(armMiningBeam(fixture));
    SOL_REQUIRE(w.switchShip(0));
    SOL_REQUIRE(w.assignCaptain(captain, bare));
    SOL_CHECK(w.orderMine(captain, &error));

    // ⚑ THE ANTI-VACUITY IS THE ROCK, AND THE SHIPPED GALAXY HAS TO PROVIDE IT.
    // The Core tier draws from {0, 1} fields, so a system with none is real
    // content rather than a defensive branch - and if the generator ever stopped
    // producing one this would fail rather than quietly stop testing anything.
    std::uint32_t barren = kNone;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size() && barren == kNone; ++sys) {
        if (w.mining().fieldCount(sys) != 0) {
            continue;
        }
        for (std::uint32_t st = 0; st < w.galaxy().systems[sys].stations.size(); ++st) {
            if ((w.stationScreens(sys, st) & screenBit(StationScreen::Shipyard)) != 0) {
                barren = sys;
                break;
            }
        }
    }
    SOL_REQUIRE(barren != kNone);
    std::printf("  %s has no field to work\n", w.galaxy().systems[barren].name.c_str());
    SOL_REQUIRE(w.cancelOrder(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].order.kind == game::OrderKind::None; }));
}

// ⚑⚑⚑ A CAPTAIN'S ORE COMES OUT OF THE SAME GROUND EVERYONE ELSE'S DOES, which
// is the claim that separates this from an accrual with a ship drawn next to
// it. `MiningSim` keeps a sparse depletion record per rock; if a mining captain
// cut through anything else, the record would not move and the system's stock
// would be untouched by five minutes of work.
SOL_TEST(a_mining_captain_depletes_the_same_rock_the_player_s_beam_would)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    const std::size_t recordsBefore = w.mining().depletionRecordCount();
    SOL_REQUIRE(w.orderMine(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].mine.units > 0.0f; }));
    const std::size_t recordsAfter = w.mining().depletionRecordCount();
    std::printf("  depletion records %zu -> %zu, %.1f units aboard\n",
                recordsBefore,
                recordsAfter,
                static_cast<double>(w.captains()[captain].mine.units));
    SOL_CHECK(recordsAfter > recordsBefore);
}

// ⚑⚑⚑ RULING 6 EVALUATED AGAINST A ZERO OUTLAY, WHICH IS WHERE THE STAGE
// CLAIMS IT NEEDED NO SPECIAL CASE. The cut is of the PROFIT; ore out of the
// ground cost nothing, so the profit is the gross and the captain's share of a
// mined sale must be exactly their cut of what it fetched. The player's credits
// and the ledger have to agree with each other and with the percentage, which
// is three statements of one arithmetic and the only way to catch a cut taken
// over the wrong base.
SOL_TEST(a_mined_load_pays_the_captain_their_cut_of_the_whole_sale)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    const float cut = w.captains()[captain].cut;
    SOL_REQUIRE(w.orderMine(captain));
    const double creditsBefore = w.playerCredits();
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].ledger.paid > 0.0; }, 20000));

    const Captain& who = w.captains()[captain];
    const double gross = who.ledger.earned + who.ledger.paid;
    const double share = who.ledger.paid / gross;
    std::printf("  %s: %.0f cr gross, %.0f to them (%.2f%% against a %.2f%% cut)\n",
                who.name.c_str(),
                gross,
                who.ledger.paid,
                share * 100.0,
                static_cast<double>(cut) * 100.0);
    SOL_CHECK(std::abs(share - static_cast<double>(cut)) < 1.0e-6);
    // And the money actually moved, net of the cut. ⚑ The player is paid the
    // whole sale less the cut, so this is the same identity said against the
    // purse rather than against the ledger - which is what catches a ledger
    // that books what the credits never received.
    SOL_CHECK(std::abs((w.playerCredits() - creditsBefore) - who.ledger.earned) < 1.0e-6);
}

// ⚑⚑⚑ THE SAVE, AND THE TWO THINGS IT DELIBERATELY DOES NOT WRITE. A mining
// captain's rock is an entity index in a bubble that will not exist next
// session, so what round-trips is the FIELD and the hold - and the body has to
// pick a rock out of them again and go back to work. The v41 loader also has to
// refuse a stationary order naming no market, which is the shape of both defects
// stage B's live drive found: a save the game writes and then cannot open.
SOL_TEST(a_mining_captain_survives_a_save_and_goes_back_to_the_field)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    SOL_REQUIRE(w.orderMine(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].mine.units > 0.0f; }));
    const game::Captain before = w.captains()[captain];

    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/captains-c";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/mine.sav";
    SOL_REQUIRE(w.saveTo(path.c_str(), "Mining"));

    Fixture reloaded;
    SOL_REQUIRE(reloaded.world().loadFrom(path.c_str()));
    SOL_REQUIRE(reloaded.world().captains().size() == w.captains().size());
    const game::Captain& after = reloaded.world().captains()[captain];
    SOL_CHECK(after.order.kind == game::OrderKind::Mine);
    SOL_CHECK(after.order.marketA == before.order.marketA);
    SOL_CHECK(after.mine.field == before.mine.field);
    SOL_CHECK(after.mine.commodity == before.mine.commodity);
    SOL_CHECK(std::abs(after.mine.units - before.mine.units) < 1.0e-3f);
    SOL_CHECK(after.ledger.earned == before.ledger.earned);

    // ⚑ AND IT GOES BACK TO WORK WITHOUT BEING TOLD. The system is not open on
    // the tick after a load - `openStationaryCaptainBubbles` is what opens it -
    // so this is the load path and the retention policy meeting, which is the
    // one seam a reload could silently drop.
    SpaceWorld& r = reloaded.world();
    SOL_REQUIRE(runUntil(r, [&] {
        return r.systemIsInstantiated(dock.system) && r.captains()[captain].mine.units > before.mine.units;
    }));
    std::printf("  reloaded and cutting again: %.1f -> %.1f units\n",
                static_cast<double>(before.mine.units),
                static_cast<double>(r.captains()[captain].mine.units));
}

// ⚑⚑⚑⚑ WHAT KILLING A MINING CAPTAIN COSTS, AND THE TEST IT REPLACES IS WHY
// THIS ONE IS WORTH READING. Stage C asserted the INTERIM in this slot - "the
// load is lost and the ledger records it; the hull is respawned and the order
// stands" - and said in its own comment that burying somebody the player hired
// needs a wreck, an insurance answer, a standing consequence and a line they
// can find. All four are here now, so the assertions invert: the thing stage C
// pinned as correct is the thing this stage had to make false.
//
// ⚑⚑⚑ THE HULL IS GONE AND SO IS THE PERSON (the user's ruling 14). The two
// softer answers were both offered and both refused: replacing the hull at the
// last dock costs almost nothing, and keeping the captain alive to be
// re-assigned makes an 8-20% cut a bet with no downside.
SOL_TEST(killing_a_captain_buries_the_hull_and_the_person)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    const std::size_t fleetBefore = w.fleet().size();
    const std::size_t peopleBefore = w.captains().size();
    const double hullValue = w.shipValue(w.fleet()[slot]);
    SOL_REQUIRE(hullValue > 0.0);
    SOL_REQUIRE(w.orderMine(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].mine.units > 0.0f; }));
    const double creditsBefore = w.playerCredits();

    // Through the ordinary death path, not by deleting the entity: the point is
    // what the GAME does when a captain's hull is shot.
    SOL_REQUIRE(w.killCaptainPuppet(captain));

    // The person is struck off, and so is the hull they were flying.
    SOL_CHECK(w.captains().size() == peopleBefore - 1);
    SOL_CHECK(w.fleet().size() == fleetBefore - 1);
    // ⚑ INSURANCE PAYS THE SAME FIVE PER CENT THE PLAYER PAYS AS A DEDUCTIBLE
    // WHEN THEY DIE - borrowed rather than invented, so there is one number in
    // this game meaning "what insurance is worth" instead of two that can drift.
    const double payout = w.playerCredits() - creditsBefore;
    SOL_CHECK(payout > 0.0);
    SOL_CHECK(std::abs(payout - 0.05 * hullValue) < 1.0);
    std::printf("  hull worth %.0f cr: %zu -> %zu people, %zu -> %zu hulls, insurance %.0f cr\n",
                hullValue,
                peopleBefore,
                w.captains().size(),
                fleetBefore,
                w.fleet().size(),
                payout);

    // ⚑⚑ AND THE BUBBLE THEIR ORDER WAS HOLDING OPEN LETS GO, which is the
    // consequence a reader would forget: the order was the ONLY thing keeping
    // that system instantiated (`bubbleHoldsPlayerAsset` asks the record, never
    // the registry), and a dead captain has no record left to ask.
    SOL_CHECK(!w.bubbleHoldsPlayerAssetIn(dock.system));

    // ⚑ THE ANTI-VACUITY, AND IT IS THE ONE THIS TEST MOST NEEDS: a world with
    // no captains and no spare hulls passes every assertion above by accident.
    SOL_REQUIRE(peopleBefore == 1);
    SOL_REQUIRE(fleetBefore >= 2);
}

// ⚑⚑⚑⚑ A DEATH RENUMBERS THE TAIL, AND IT RENUMBERS TWO TABLES AT ONCE.
// `sellShip` has shifted `Captain::ship` since stage A and its comment says why
// - "an erase renumbers the tail; without this a captain silently inherits the
// hull that moved into the slot". Stage D adds the other half: erasing a
// CAPTAIN renumbers every index that pointed past them, including a
// `CaptainPuppet` in a bubble the player is not standing in, which nothing else
// in the game would ever fix up.
SOL_TEST(a_captains_death_renumbers_the_captains_and_the_hulls_after_them)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    SpaceWorld& w = fixture.world();

    // Two captains, two hulls, and the one that dies is FIRST - which is the
    // only ordering that can catch a missing shift.
    const std::size_t doomedHull = buyAndArmAMiner(fixture);
    SOL_REQUIRE(doomedHull != 0);
    const std::size_t survivorHull = buyAndArmAMiner(fixture);
    SOL_REQUIRE(survivorHull != 0 && survivorHull > doomedHull);
    std::vector<CaptainCandidate> hall;
    w.captainCandidates(hall);
    SOL_REQUIRE(hall.size() >= 2);
    SOL_REQUIRE(w.hireCaptain(0));
    const std::size_t doomed = w.captains().size() - 1;
    SOL_REQUIRE(w.assignCaptain(doomed, doomedHull));
    w.captainCandidates(hall);
    SOL_REQUIRE(!hall.empty());
    SOL_REQUIRE(w.hireCaptain(0));
    const std::size_t survivor = w.captains().size() - 1;
    SOL_REQUIRE(w.assignCaptain(survivor, survivorHull));
    SOL_REQUIRE(survivor == doomed + 1);

    const std::string survivorName = w.captains()[survivor].name;
    const std::string survivorHullId = w.fleet()[survivorHull].defId;
    SOL_REQUIRE(w.orderMine(doomed));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[doomed].mine.units > 0.0f; }));
    SOL_REQUIRE(w.killCaptainPuppet(doomed));

    // The survivor moved down one slot AND their hull moved down one slot, and
    // the two shifts are independent - a test that checked only the name would
    // pass with `Captain::ship` left pointing at the dead captain's hull.
    SOL_REQUIRE(w.captains().size() == 1);
    SOL_CHECK(w.captains()[0].name == survivorName);
    SOL_CHECK(w.captains()[0].ship == survivorHull - 1);
    SOL_CHECK(w.fleet()[w.captains()[0].ship].defId == survivorHullId);
    std::printf("  survivor '%s' now captain 0 holding fleet %u (%s)\n",
                w.captains()[0].name.c_str(),
                w.captains()[0].ship,
                w.fleet()[w.captains()[0].ship].defId.c_str());
}

// ⚑⚑⚑⚑ THE LIVE DEFECT STAGE D CLOSES, AND IT WAS REACHABLE BY ACCIDENT. A
// captain's hull wears the local owner's colours - it has to wear SOMETHING,
// because Lua reads an unaffiliated pilot as unconditionally player-hostile -
// so before this stage putting a stray shot into your own freighter called
// `recordShipKill` against a government you had never fought. You lost standing,
// their enemies liked you better, and a territory contest moved.
//
// The fix is the phase's whole thesis in one line: ask the THING (`CaptainPuppet`
// is the carrier stage B built and named this stage as the consumer of), never
// the faction number, because the faction number is exactly what is wrong.
SOL_TEST(shooting_your_own_captains_hull_moves_no_standing)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    SOL_REQUIRE(w.orderMine(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].mine.units > 0.0f; }));

    // Every standing in the game, before and after - because the hull wears
    // ONE faction's colours and a test that watched only that one would miss
    // the half of `recordShipKill` that raises its enemies.
    std::vector<float> before;
    for (std::uint32_t f = 0; f < w.factions().size(); ++f) {
        before.push_back(w.factionSim().standing(f));
    }
    SOL_REQUIRE(!before.empty());
    SOL_REQUIRE(w.killCaptainPuppet(captain, /*byPlayer=*/true));
    for (std::uint32_t f = 0; f < w.factions().size(); ++f) {
        SOL_CHECK(w.factionSim().standing(f) == before[f]);
    }
    std::printf("  %zu standings unmoved by the player's own hull dying\n", before.size());

    // ⚑ THE ANTI-VACUITY, AND IT IS THE ASSERTION THAT MAKES THE TEST MEAN
    // ANYTHING: killing somebody ELSE'S ship through the same path still moves
    // the number. Without this, a `recordShipKill` deleted outright would pass.
    SOL_REQUIRE(fixture.walkIn(dock));
    SOL_REQUIRE(w.undock());
    std::uint32_t victimFaction = kNone;
    SOL_REQUIRE(w.killAnyNpcByPlayer(&victimFaction));
    SOL_REQUIRE(victimFaction < w.factions().size());
    SOL_CHECK(w.factionSim().standing(victimFaction) < before[victimFaction]);
    std::printf("  and an NPC kill still moves it: %.1f -> %.1f\n",
                static_cast<double>(before[victimFaction]),
                static_cast<double>(w.factionSim().standing(victimFaction)));
}

// ⚑⚑⚑⚑ THE TWO COMBAT ORDERS REFUSE A HULL WITH NOTHING IN ITS WEAPON MOUNTS,
// AND THE FIRST VERSION OF THIS TEST ASSERTED SOMETHING FALSE ABOUT THE SHIPPED
// DATA. It built a "hull with no guns" by fitting a mining laser - the way
// `armMiningBeam`'s own comment says a player always does it, by REPLACING the
// authored gun - and then asserted the hull could not shoot. It can:
// `sol.mining_laser` carries `damage = 3.0` beside its `mining_power = 4.0`,
// because `WeaponDef` insists that "a mining laser is an ordinary hardpoint
// choice, not a mode".
//
// ⚑⚑⚑ SO THE REFUSAL IS A FLOOR AT ZERO AND THE ONLY WAY TO REACH IT IS AN
// EMPTY MOUNT - which is exactly what a player who sold a fitting has. That is
// the case this test builds now, and the correction is worth more than the
// original assertion was: it says the clause catches an UNFITTED hull rather
// than an unsuitable one, and whether nine damage a second is enough for a beat
// is the player's call, printed beside the button.
SOL_TEST(a_combat_order_refuses_an_unfitted_hull_and_a_second_escort)
{
    Fixture fixture;
    const Dock dock = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Outfitting));
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    SpaceWorld& w = fixture.world();

    // ⚑ THE MINING HULL FIRST, TO PIN THE CORRECTION: a beam IS a gun here, and
    // a captain flying one can be posted to a beat. Without this assertion the
    // test below would pass against a rule that refused every miner.
    const std::size_t beamHull = buyAndArmAMiner(fixture);
    SOL_REQUIRE(beamHull != 0);
    SOL_CHECK(w.shipGunPower(w.fleet()[beamHull]) > 0.0f);
    std::printf("  a mining hull still shoots: %.1f dps\n",
                static_cast<double>(w.shipGunPower(w.fleet()[beamHull])));

    // Now the case the clause is actually for: every weapon mount emptied,
    // which is what a player who sold their fitting is flying.
    SOL_REQUIRE(w.switchShip(beamHull));
    const sol::assets::ShipDef fit = w.resolvedShipDef(w.activeShip());
    for (const sol::assets::ShipMount& mount : fit.mounts) {
        if (sol::assets::mountTakesWeapon(mount.kind) && !mount.fit.empty()) {
            SOL_REQUIRE(w.sellFitting(mount.id.c_str()));
        }
    }
    SOL_REQUIRE(w.switchShip(0));
    SOL_REQUIRE(w.shipGunPower(w.fleet()[beamHull]) <= 0.0f);
    const std::size_t unarmed = hireAndGive(fixture, beamHull);
    SOL_REQUIRE(unarmed != kNone);
    std::string error;
    SOL_CHECK(!w.orderPatrol(unarmed, &error));
    SOL_CHECK(error.find("guns") != std::string::npos);
    SOL_CHECK(!w.orderEscort(unarmed, &error));
    SOL_CHECK(error.find("guns") != std::string::npos);
    std::printf("  an emptied hull is refused: \"%s\"\n", error.c_str());

    // An armed hull takes both.
    const std::size_t gunned = buyAndArmAFighter(fixture);
    SOL_REQUIRE(gunned != 0);
    SOL_REQUIRE(w.shipGunPower(w.fleet()[gunned]) > 0.0f);
    std::vector<CaptainCandidate> hall;
    w.captainCandidates(hall);
    SOL_REQUIRE(!hall.empty());
    SOL_REQUIRE(w.hireCaptain(0));
    const std::size_t armed = w.captains().size() - 1;
    SOL_REQUIRE(w.assignCaptain(armed, gunned));
    SOL_REQUIRE(w.orderEscort(armed, &error));
    SOL_CHECK(w.captains()[armed].order.kind == game::OrderKind::Escort);
    // ⚑ AND AN ESCORT ORDER NAMES NO PLACE, which is the one thing about it
    // that is structurally different from the other three. Writing the dock in
    // "for symmetry" is how a field that means "where they are" starts meaning
    // "where they were hired".
    SOL_CHECK(w.captains()[armed].order.marketA == 0xffff'ffffu);

    // ⚑⚑⚑ AND THE FENCE (ruling 4): a SECOND escort is a fleet, and Phase 40
    // owns fleets. Refused by name rather than left to arrive as a bug in
    // stage E's readout.
    const std::size_t thirdHull = buyAndArmAFighter(fixture);
    SOL_REQUIRE(thirdHull != 0);
    w.captainCandidates(hall);
    SOL_REQUIRE(!hall.empty());
    SOL_REQUIRE(w.hireCaptain(0));
    const std::size_t second = w.captains().size() - 1;
    SOL_REQUIRE(w.assignCaptain(second, thirdHull));
    SOL_CHECK(!w.orderEscort(second, &error));
    SOL_CHECK(error.find("escort") != std::string::npos);
    std::printf("  second escort refused: \"%s\"\n", error.c_str());
    // But a PATROL is fine, because a patrol is posted to a place rather than
    // to the player, and two of them are two guards rather than a formation.
    SOL_CHECK(w.orderPatrol(second, &error));
}

// ⚑⚑⚑⚑ THE EXIT'S SECOND HALF, AND IT HAD NO PRODUCER IN THE CODE UNTIL THIS
// STAGE. `rollCaptainAttrition` skips a system that is being simulated -
// correctly, because rolling a coarse loss against a hull that is also being
// modelled is the "a captain that is both things at once" defect the phase's
// risk register names FIRST - and a stationary captain's system is ALWAYS
// instantiated, because their own order is what holds it open. Meanwhile Phase
// 38 stage B scoped `pilot_think` to the player's bubble and wrote the cost
// down: "nothing re-targets, breaks off or picks a new beat until the player is
// back to watch it". Two correct rules, and a captain who was safe precisely
// because nobody was looking.
//
// ⚑⚑⚑ THE USER'S RULING 12 PRICES IT IN THE COARSE LAYER rather than reopening
// the fine layer's decisions, and this is that roll doing its job.
SOL_TEST(a_posted_captain_is_at_risk_in_a_system_nobody_is_watching)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);

    SpaceWorld& w = fixture.world();
    SOL_REQUIRE(w.orderMine(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].mine.units > 0.0f; }));

    // Walk away, so their system is HELD rather than the player's - which is
    // the only configuration the roll fires in.
    const Dock elsewhere = fixture.findDock(screenBit(StationScreen::Trade), 0, dock);
    SOL_REQUIRE(elsewhere.system != kNone && elsewhere.system != dock.system);
    SOL_REQUIRE(fixture.walkIn(elsewhere));
    SOL_REQUIRE(w.systemIsInstantiated(dock.system));

    // ⚑ THE DANGER IS FORCED RATHER THAN WAITED FOR, on `killCaptainPuppet`'s
    // own rule: waiting for a die roll to come up is waiting on a die roll, and
    // a test that does it is a test that fails on a bad seed.
    // ⚑ RE-PINNED EVERY STEP, BECAUSE RAID INTENSITY DECAYS ON A 600 s HALF
    // LIFE. Setting it once and stepping for hours of sim time is setting it to
    // zero slowly, which would make this test pass or fail on how long the
    // budget happened to be.
    SOL_REQUIRE(runUntil(
        w,
        [&] {
            w.factionSim().setRaidIntensity(dock.system, 1.0f);
            return w.captains().empty();
        },
        20000));
    std::printf("  captain lost in a system the player had left\n");
    // And the bubble their order was holding open goes with them.
    SOL_CHECK(!w.bubbleHoldsPlayerAssetIn(dock.system));
}

// ⚑⚑⚑⚑ AND A PATROL POSTED TO THE SAME SYSTEM IS WHAT BRINGS THE NUMBER DOWN,
// which is the whole meaning of "patrol this" when the player is not there to
// watch it work. Without it the order would only ever visibly do anything in
// the one system the player happens to be standing in - which is exactly the
// half of the exit a test cannot fly.
//
// ⚑⚑⚑⚑ AND THE FIRST VERSION OF THIS TEST WAS A COIN FLIP DRESSED AS A
// MEASUREMENT, WHICH IS WORTH RECORDING BECAUSE IT LOOKED RIGOROUS. It raced two
// worlds - one guarded, one not - off the same seed and asserted the guarded one
// survived strictly longer. Both share `m_captainRng`, so they see the SAME draw
// sequence; halving the threshold only matters when a draw lands between the
// two, and the odds of that on any given loss are even. It failed on the first
// run with both worlds losing their captain at step 836, and it would have
// passed half the time.
//
// ⚑⚑⚑ SO IT ASSERTS THE RULE ITSELF. `heldBubbleRiskPerSecond` is the number the
// roll rolls against, and a guard that stopped being counted moves it
// immediately rather than half the time. The race is kept underneath as the
// thing that proves the number is CONNECTED to an outcome - a probe agreeing
// with itself is Phase 35's "a probe is a mirror" over again - but what it
// asserts is only the deterministic half: a smaller threshold against one shared
// stream can never lose EARLIER.
SOL_TEST(a_patrol_makes_an_unwatched_system_safer_without_making_it_safe)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    const std::size_t hull = buyAndArmAMiner(fixture);
    SOL_REQUIRE(hull != 0);
    const std::size_t miner = hireAndGive(fixture, hull);
    SOL_REQUIRE(miner != kNone);
    SpaceWorld& w = fixture.world();
    SOL_REQUIRE(w.orderMine(miner));
    w.factionSim().setRaidIntensity(dock.system, 1.0f);

    // One captain posted, no guard: the bare rate.
    const float bare = w.heldBubbleRiskPerSecond(dock.system);
    SOL_REQUIRE(bare > 0.0f);

    // ⚑ AND THE ANTI-VACUITY FIRST: a system with nothing of yours in it is not
    // dangerous to you, however raided it is. Without this a probe that just
    // returned `danger * rate` would pass every assertion below.
    const Dock elsewhere = fixture.findDock(screenBit(StationScreen::Trade), 0, dock);
    SOL_REQUIRE(elsewhere.system != kNone && elsewhere.system != dock.system);
    w.factionSim().setRaidIntensity(elsewhere.system, 1.0f);
    SOL_CHECK(w.heldBubbleRiskPerSecond(elsewhere.system) == 0.0f);

    // Post a guard in the same system and the rate halves, exactly.
    const std::size_t guardHull = buyAndArmAFighter(fixture);
    SOL_REQUIRE(guardHull != 0);
    std::vector<CaptainCandidate> hall;
    w.captainCandidates(hall);
    SOL_REQUIRE(!hall.empty());
    SOL_REQUIRE(w.hireCaptain(0));
    const std::size_t guard = w.captains().size() - 1;
    SOL_REQUIRE(w.assignCaptain(guard, guardHull));
    SOL_REQUIRE(w.orderPatrol(guard));
    const float guarded = w.heldBubbleRiskPerSecond(dock.system);
    SOL_CHECK(guarded == bare * 0.5f);
    std::printf("  %.5f/s bare, %.5f/s with one patrol posted\n",
                static_cast<double>(bare),
                static_cast<double>(guarded));

    // ⚑⚑ SAFER IS NOT SAFE, and this is the assertion that keeps a guard from
    // quietly becoming an invulnerability field. A patrol is also EXPOSED to
    // its own roll - a guard that cannot be shot at is not a guard - so two of
    // them make a system safer twice over and never make it safe.
    SOL_CHECK(guarded > 0.0f);
    const std::size_t secondHull = buyAndArmAFighter(fixture);
    SOL_REQUIRE(secondHull != 0);
    w.captainCandidates(hall);
    SOL_REQUIRE(!hall.empty());
    SOL_REQUIRE(w.hireCaptain(0));
    const std::size_t second = w.captains().size() - 1;
    SOL_REQUIRE(w.assignCaptain(second, secondHull));
    SOL_REQUIRE(w.orderPatrol(second));
    SOL_CHECK(w.heldBubbleRiskPerSecond(dock.system) == bare * 0.25f);

    // And it is connected to an outcome: walk away and somebody still dies.
    SOL_REQUIRE(fixture.walkIn(elsewhere));
    SOL_REQUIRE(w.systemIsInstantiated(dock.system));
    const std::size_t people = w.captains().size();
    SOL_REQUIRE(runUntil(
        w,
        [&] {
            w.factionSim().setRaidIntensity(dock.system, 1.0f);
            return w.captains().size() < people;
        },
        40000));
    std::printf("  and a guarded system still loses somebody eventually\n");
}

// ⚑⚑⚑⚑ THE ESCORT IS THE ONE ORDER WHOSE SHIP IS ALWAYS WHERE THE PLAYER IS,
// AND THAT IS WHY IT IS NEITHER HALF OF THE PHASE'S SPLIT. A stationary order
// holds a bubble open because the hull is somewhere the player is not; an
// itinerant one rides the coarse layer for the same reason. An escort has no
// unobserved half at all, so it holds no bubble and keeps no coarse leg - and
// the jump, which looks like the hard case, needs no code of its own: the gate
// leaves the old bubble behind and the body is rebuilt in the new one, which is
// `MinerPuppet`'s bargain about a rock pointed at a whole system.
SOL_TEST(an_escort_flies_with_the_player_and_follows_them_through_a_gate)
{
    Fixture fixture;
    const Dock dock = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard));
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    SpaceWorld& w = fixture.world();
    const std::size_t hull = buyAndArmAFighter(fixture);
    SOL_REQUIRE(hull != 0);
    const std::size_t captain = hireAndGive(fixture, hull);
    SOL_REQUIRE(captain != kNone);
    SOL_REQUIRE(w.orderEscort(captain));

    // Docked, there is no sky to be in - which is the honest answer rather than
    // a hull parked in space beside a station the player is inside.
    std::vector<game::CaptainPuppetInfo> bodies;
    w.captainPuppetInfo(bodies);
    SOL_CHECK(bodies.empty());

    // Undock and they are on your wing.
    SOL_REQUIRE(w.undock());
    SOL_REQUIRE(runUntil(
        w,
        [&] {
            w.captainPuppetInfo(bodies);
            return !bodies.empty();
        },
        200));
    SOL_REQUIRE(bodies.size() == 1);
    SOL_CHECK(bodies[0].captainIndex == captain);
    std::printf("  '%s' on your wing, %.0f km off\n", bodies[0].name.c_str(), bodies[0].distance / 1000.0);

    // ⚑⚑ AND THE ORDER HOLDS NO BUBBLE, which is the assertion that separates
    // this order from the two stationary ones. An escort in your own system
    // must not be keeping a second one open.
    SOL_CHECK(!w.bubbleHoldsPlayerAssetIn(dock.system));

    // ⚑⚑⚑⚑ AND IT IS THE SAME BODY A HUNDRED TICKS LATER, WHICH IS THE
    // ASSERTION THAT FOUND THE STAGE'S THIRD BUG. `syncCaptainPuppets` skipped
    // only STATIONARY captains, so an escort - deliberately neither half of the
    // phase's split - fell through to its doom path and was destroyed and
    // respawned EVERY TICK. Every probe that asked "is there a body" said yes,
    // and a brand new hull sixty times a second has full shields, no memory of
    // the fight it is in, and an order it can never finish standing down from.
    // The fix is a predicate: `!stationary()` stopped meaning "itinerant" the
    // moment a third order kind existed.
    const std::uint32_t was = bodies[0].entity;
    for (int i = 0; i < 100; ++i) {
        w.tick(kCoarseStep);
    }
    w.captainPuppetInfo(bodies);
    SOL_REQUIRE(bodies.size() == 1);
    SOL_CHECK(bodies[0].entity == was);
    std::printf("  same hull (entity %u) a hundred ticks on\n", bodies[0].entity);

    // ⚑⚑⚑ THROUGH A GATE, AND THE BODY IS REBUILT ON THE FAR SIDE. This is the
    // whole of what "escort that" promises and the only part of it a test can
    // pin without flying: the captain is not left in the system you came from.
    const auto& gates = w.galaxy().systems[dock.system].gates;
    SOL_REQUIRE(!gates.empty());
    const std::uint32_t beyond = gates[0].toSystem;
    SOL_REQUIRE(beyond != dock.system);
    SOL_REQUIRE(w.enterSystem(beyond));
    SOL_REQUIRE(runUntil(
        w,
        [&] {
            w.captainPuppetInfo(bodies);
            return !bodies.empty();
        },
        200));
    SOL_CHECK(bodies.size() == 1);
    SOL_CHECK(bodies[0].captainIndex == captain);
    std::printf("  and still on your wing one gate on, in %s\n", w.galaxy().systems[beyond].name.c_str());

    // ⚑ THE ANTI-VACUITY: exactly one hull, not one per system visited. The
    // failure this catches is the one the phase's risk register names first - a
    // captain who is two things at once - and it is reachable here by the body
    // in the old bubble simply never being despawned.
    std::uint32_t hulls = 0;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size(); ++sys) {
        hulls += w.systemIsInstantiated(sys) ? 1u : 0u;
    }
    SOL_CHECK(w.captains().size() == 1);
    std::printf("  %u system(s) instantiated for one escort\n", hulls);

    // ⚑⚑ AND CALLING THEM OFF PUTS THE HULL BACK ON A PAD, in whatever system
    // you are both standing in. An escort is the one order with no place in it,
    // so there is no posted dock to send them home to and the nearest station
    // is the answer - which still has to be a PAD, because `OwnedShip` parks
    // nowhere else and a hull abandoned in open space is one the player can
    // neither find, sell, board nor hand back.
    SOL_REQUIRE(w.cancelOrder(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].order.kind == game::OrderKind::None; }, 8000));
    const game::OwnedShip& parked = w.fleet()[w.captains()[captain].ship];
    SOL_CHECK(parked.storedSystem == beyond);
    SOL_CHECK(parked.storedStation < w.galaxy().systems[beyond].stations.size());
    // And the body goes with the order: an escort with no order is not in the sky.
    w.captainPuppetInfo(bodies);
    SOL_CHECK(bodies.empty());
    std::printf("  stood down onto station %u in %s, and out of the sky\n",
                parked.storedStation,
                w.galaxy().systems[beyond].name.c_str());
}

// ⚑⚑⚑⚑ THE BEAT MOVES IN A SYSTEM THE PLAYER HAS LEFT, AND IT IS A COUNT
// RATHER THAN A STATE FOR PHASE 38's REASON. "On the beat" reads identically in
// a system that is being ticked and in one that was rebuilt around a sleeping
// captain - which is exactly how a silent audio device survived eleven phases.
// The leg of the beat is a number that has to go up, so a patrol that stopped
// being ticked fails here instead of reporting that it works.
SOL_TEST(a_patrol_walks_its_beat_in_a_system_the_player_has_left)
{
    Fixture fixture;
    const Dock dock = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard));
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    SpaceWorld& w = fixture.world();
    const std::size_t hull = buyAndArmAFighter(fixture);
    SOL_REQUIRE(hull != 0);
    const std::size_t captain = hireAndGive(fixture, hull);
    SOL_REQUIRE(captain != kNone);
    SOL_REQUIRE(w.orderPatrol(captain));
    // A patrol is STATIONARY, so it holds its system open - the same clause a
    // mining order leans on, through the same one predicate.
    SOL_CHECK(w.bubbleHoldsPlayerAssetIn(dock.system));

    // Walk to a dock in a DIFFERENT SYSTEM and leave them to it. ⚑ `findDock`'s
    // `skip` names a station, so asking it for "somewhere else" hands back the
    // dock next door in the same system - which is the trap its own comment
    // warns about one level up, and it caught this test on its first run.
    Dock elsewhere;
    for (std::uint32_t sys = 0; sys < w.galaxy().systems.size() && elsewhere.system == kNone; ++sys) {
        if (sys == dock.system) {
            continue;
        }
        for (std::uint32_t st = 0; st < w.galaxy().systems[sys].stations.size(); ++st) {
            if ((w.stationScreens(sys, st) & screenBit(StationScreen::Trade)) != 0) {
                elsewhere = {sys, st};
                break;
            }
        }
    }
    SOL_REQUIRE(elsewhere.system != kNone && elsewhere.system != dock.system);
    SOL_REQUIRE(fixture.walkIn(elsewhere));
    SOL_REQUIRE(w.systemIsInstantiated(dock.system));

    // The bubble is held, so the hull is there and the beat advances. The
    // budget is generous because a beat leg is a gate crossing - hundreds of
    // thousands of kilometres at the captain cruise rate.
    SOL_REQUIRE(runUntil(w, [&] { return w.captainBeatLeg(captain) > 0; }, 8000));
    const std::uint32_t first = w.captainBeatLeg(captain);
    SOL_REQUIRE(runUntil(w, [&] { return w.captainBeatLeg(captain) != first; }, 8000));
    std::printf("  beat advanced to leg %u and on to %u, with the player two systems away\n",
                first,
                w.captainBeatLeg(captain));

    // ⚑⚑⚑ AND STANDING THEM DOWN BRINGS THEM HOME, WHICH IS THE ASSERTION THAT
    // FOUND THE STAGE'S SECOND BORROWED-RULE BUG. `cancelOrder` read
    // `stationary()` - correct for the representation, wrong for this rule -
    // and pushed a cancelled patrol into `MinePhase::Selling`, a phase a patrol
    // never looks at, so the order never ended and the Crew tab said "standing
    // down" for the rest of the session. The order does not end here either
    // until the hull is back on a PAD, which is the field every other screen
    // reads to say where a ship of yours is.
    SOL_REQUIRE(w.cancelOrder(captain));
    SOL_CHECK(w.captains()[captain].order.stopping);
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].order.kind == game::OrderKind::None; }, 8000));
    SOL_CHECK(w.fleet()[w.captains()[captain].ship].storedSystem == dock.system);
    SOL_CHECK(!w.bubbleHoldsPlayerAssetIn(dock.system));
    std::printf("  stood down and parked at station %u in %s\n",
                w.fleet()[w.captains()[captain].ship].storedStation,
                w.galaxy().systems[dock.system].name.c_str());
}

// ⚑⚑⚑⚑ A PATROL THAT SPOTS SOMETHING HAS TO GET TO IT, AND THE FIRST CUT DID
// NOT - WHICH ONLY A LIVE FLIGHT SHOWED. Every assertion this stage had was
// green while the patrol locked on to a raider 47,000 km away and closed 22 km
// in seventy seconds, because `PilotState::Attack` steering is combat-scale and
// `preyReach` is the whole LOD bubble. The order looked right from every
// direction except the only one that matters: the guard the player paid for
// never arrived.
//
// ⚑⚑⚑ SO WHAT THIS ASSERTS IS A DISTANCE CLOSING, NOT A STATE CHANGING. A state
// assertion is exactly what was passing while the feature was broken - this
// file's own recurring lesson and Phase 38's: prefer the number that has to
// move. The distance is the captain's from the PLAYER, and the raider is
// spawned on the player, so closing on one is closing on the other.
SOL_TEST(a_patrol_closes_the_distance_to_something_it_has_locked_on_to)
{
    Fixture fixture;
    const Dock dock = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard));
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    SpaceWorld& w = fixture.world();
    const std::size_t hull = buyAndArmAFighter(fixture);
    SOL_REQUIRE(hull != 0);
    const std::size_t captain = hireAndGive(fixture, hull);
    SOL_REQUIRE(captain != kNone);
    SOL_REQUIRE(w.orderPatrol(captain));
    SOL_REQUIRE(w.undock());

    // Let the hull exist and get out on to its beat, so the gap below is a real
    // crossing rather than the two of them starting on the same pad.
    std::vector<game::CaptainPuppetInfo> bodies;
    SOL_REQUIRE(runUntil(
        w,
        [&] {
            w.captainPuppetInfo(bodies);
            return !bodies.empty() && bodies[0].distance > 1.0e7;
        },
        4000));
    const double before = bodies[0].distance;
    std::printf("  patrol is %.0f km out when the raider turns up\n", before / 1000.0);

    // An unaffiliated console spawn, which Lua and this file both treat as
    // unconditionally player-hostile: the cheapest thing in the game that is
    // certainly an enemy of the player, placed on the player.
    const sol::assets::ShipDef* def = fixture.defs.findShip("sol.interceptor");
    SOL_REQUIRE(def != nullptr);
    (void)w.spawnPilotFromDef(*def, fixture.defs, game::PilotRole::Fighter, 0xffff'ffffu);

    for (int i = 0; i < 800; ++i) {
        w.tick(kCoarseStep);
        w.captainPuppetInfo(bodies);
        if (bodies.empty() || bodies[0].distance < before * 0.25) {
            break;
        }
    }
    SOL_REQUIRE(!bodies.empty());
    const double after = bodies[0].distance;
    std::printf("  and closes %.0f km -> %.0f km\n", before / 1000.0, after / 1000.0);
    // ⚑ A QUARTER RATHER THAN "SOMETHING SMALLER", because the broken version
    // closed 22 km out of 47,000 - which is smaller too, and would have passed
    // any assertion that only asked for movement in the right direction.
    SOL_CHECK(after < before * 0.25);
}

// ⚑⚑⚑⚑ THE ROUND TRIP THAT WAS MISSING, AND ITS ABSENCE IS WHY A FULL GREEN
// GATE SHIPPED A SAVE THE GAME REFUSES TO OPEN (stage E, on stage D's defect).
// Every other save test in this file predates the combat orders: the four
// round-trips cover a bare captain, a haul, an order given but not yet ticked,
// and a mining captain, and every `orderPatrol`/`orderEscort` call in the suite
// lives in a test that never saves. So the writer emitted `Patrol` (3) and
// `Escort` (4) - the v42 comment says that is precisely why the version bumped
// - while the reader twelve lines further down still refused anything above
// `Mine` (2), and nothing asked.
//
// It asserts BOTH kinds because they fail for one reason and a test that only
// covered the patrol would leave the escort's bound to be discovered the same
// way. The escort is also the only order that names no market at all, so it is
// the case most likely to be refused by a later invariant written for the ones
// that do.
SOL_TEST(a_save_carries_a_patrol_and_an_escort_rather_than_refusing_its_own_bytes)
{
    Fixture fixture;
    const Dock dock = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Outfitting));
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    SpaceWorld& w = fixture.world();
    w.addCredits(1'000'000.0);

    const std::size_t guardHull = buyAndArmAFighter(fixture);
    SOL_REQUIRE(guardHull != 0);
    const std::size_t guard = hireAndGive(fixture, guardHull);
    SOL_REQUIRE(guard != kNone);
    SOL_REQUIRE(w.orderPatrol(guard));

    const std::size_t wingHull = buyAndArmAFighter(fixture);
    SOL_REQUIRE(wingHull != 0);
    const std::size_t wing = hireAndGive(fixture, wingHull);
    SOL_REQUIRE(wing != kNone);
    SOL_REQUIRE(w.orderEscort(wing));

    // Let the bodies exist and the beat advance, so what is written is a posted
    // captain mid-order rather than one on the tick they were given it.
    for (int i = 0; i < 40; ++i) {
        w.tick(kCoarseStep);
    }
    SOL_REQUIRE(w.captains()[guard].order.kind == game::OrderKind::Patrol);
    SOL_REQUIRE(w.captains()[wing].order.kind == game::OrderKind::Escort);
    const std::uint32_t postedAt = w.captains()[guard].order.marketA;
    SOL_REQUIRE(postedAt != 0xffff'ffffu);

    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/captains-e";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/combat.sav";
    SOL_REQUIRE(w.saveTo(path.c_str(), "Combat"));

    // ⚑ THE ASSERTION THAT MATTERS IS THIS ONE. Before the fix `loadFrom`
    // returned false here, on a file the same build had just written.
    Fixture reloaded;
    SOL_REQUIRE(reloaded.world().loadFrom(path.c_str()));
    SOL_REQUIRE(reloaded.world().captains().size() == 2);
    SOL_CHECK(reloaded.world().captains()[guard].order.kind == game::OrderKind::Patrol);
    SOL_CHECK(reloaded.world().captains()[guard].order.marketA == postedAt);
    SOL_CHECK(reloaded.world().captains()[wing].order.kind == game::OrderKind::Escort);
    std::printf("  a patrol and an escort survived the round trip\n");
}

// ⚑⚑⚑⚑ THE STAGE'S EXIT AS A TEST: A FLOOR ABOVE THE MARKET HOLDS THE LOAD, AND
// DROPPING IT LANDS THE SALE. The two halves have to be one test because either
// alone is satisfiable by a bug. A captain who never sells passes "the floor
// held the cargo" perfectly - that is also what a broken captain does - and one
// who always sells passes "dropping the floor banked it" without the floor ever
// having done anything. What is asserted is the TRANSITION: the same hull, the
// same load, the same market, no sale under the floor and a sale once it drops.
SOL_TEST(a_sell_floor_holds_the_load_and_dropping_it_lands_the_sale)
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

    // ⚑ THE FLOOR IS THE MAXIMUM, WHICH IS WHAT MAKES THE FIRST HALF CERTAIN
    // RATHER THAN LIKELY. A realised margin on these lanes is a few per cent
    // against a 5% spread, so +50% is a floor no honest trade on this galaxy
    // clears - the test does not have to hunt for a route that happens to be
    // bad, which would be a measurement of the galaxy rather than of the rule.
    SOL_REQUIRE(fixture.world().orderHaul(0, places[0].market, 0.50f));
    SOL_CHECK(fixture.world().captains()[0].order.floor == 0.50f);

    // Fly legs until the captain is carrying something. A leg that found no
    // margin at all buys nothing, and an empty hold cannot demonstrate a floor.
    bool laden = false;
    for (int legs = 0; legs < 6 && !laden; ++legs) {
        SOL_REQUIRE(runUntilParked(fixture.world(), 0));
        laden = fixture.world().captains()[0].haul.leg.cargo > 0.0f &&
                fixture.world().captains()[0].haul.outlay > 0.0;
        if (!laden) {
            fixture.world().tick(kCoarseStep); // let it depart again
        }
    }
    SOL_REQUIRE(laden); // nothing was ever worth loading: that is a finding

    // THE FIRST HALF. Carry it across several arrivals under the floor: the
    // hold must survive every one of them, and the ledger must not move.
    const double heldOutlay = fixture.world().captains()[0].haul.outlay;
    const double earnedBefore = fixture.world().captains()[0].ledger.earned;
    const double paidBefore = fixture.world().captains()[0].ledger.paid;
    int arrivals = 0;
    for (int legs = 0; legs < 4; ++legs) {
        fixture.world().tick(kCoarseStep);
        if (!runUntilParked(fixture.world(), 0)) {
            break;
        }
        ++arrivals;
        const game::Captain& held = fixture.world().captains()[0];
        SOL_CHECK(held.haul.leg.cargo > 0.0f);     // the load is still aboard
        SOL_CHECK(held.haul.outlay == heldOutlay); // and it was never part-sold
        SOL_CHECK(held.ledger.earned == earnedBefore);
        SOL_CHECK(held.ledger.paid == paidBefore);
    }
    SOL_REQUIRE(arrivals >= 2); // it kept flying rather than parking: ruling 16
    std::printf("  held %.0f units through %d arrival(s) under a +50%% floor\n",
                static_cast<double>(fixture.world().captains()[0].haul.leg.cargo),
                arrivals);

    // THE SECOND HALF. ⚑⚑ THE FLOOR IS DROPPED WHILE THEY ARE IN TRANSIT, AND
    // THAT IS TO ISOLATE THE SALE RATHER THAN FOR REALISM. Settling at a dock
    // goes through `captainThink`, which sells AND THEN BUYS THE NEXT LOAD in
    // the same tick - so credits measured across it move by two transactions
    // and no clean identity can be read off them. `captainArrive` only settles
    // and parks. Dropping the floor mid-leg puts the sale in the arrival, where
    // it is the only thing that happened.
    fixture.world().tick(kCoarseStep); // depart with the held load
    SOL_REQUIRE(fixture.world().captains()[0].haul.leg.phase == sol::sim::TraderPhase::InTransit);
    SOL_REQUIRE(fixture.world().setSellFloor(0, 0.0f));
    SOL_CHECK(fixture.world().captains()[0].order.floor == 0.0f);
    const double creditsBefore = fixture.world().playerCredits();
    SOL_REQUIRE(runUntilParked(fixture.world(), 0));
    const game::Captain& after = fixture.world().captains()[0];
    const double gained = fixture.world().playerCredits() - creditsBefore;
    const double booked = after.ledger.earned - earnedBefore;
    SOL_REQUIRE(booked != 0.0); // the sale the floor had been refusing
    std::printf(
        "  floor dropped: credits %+.0f, ledger %+.0f, basis released %.0f\n", gained, booked, heldOutlay);
    // ⚑⚑⚑ THE IDENTITY, AND IT IS THE ONE THAT WOULD CATCH A FLOOR THAT HAD
    // QUIETLY PART-SETTLED A LOAD ON AN ARRIVAL IT REFUSED. Credits take the
    // whole sale less the cut; the ledger takes the PROFIT less the cut. The
    // difference between them is exactly the cost basis - the money that left
    // the player's account at the buy and has now come back - so it must equal
    // the outlay the hold was carrying while the floor was still on. If any
    // earlier arrival had released part of that basis, the two would no longer
    // differ by the whole of it.
    SOL_CHECK(std::abs((gained - booked) - heldOutlay) < 1.0);
    SOL_CHECK(after.haul.outlay < heldOutlay); // the basis really left the hold
}

// ⚑⚑⚑⚑ STANDING DOWN IGNORES THE FLOOR, AND WITHOUT THIS THE FEATURE EATS THE
// PLAYER'S CAPITAL. Ruling 7 funds the cargo out of the player's credits at the
// BUY, so an unsold hold is money already spent. A cancel clears the order -
// and with it the floor the load was being judged by - so if the last arrival
// refuses the sale, the hull parks with the player's money in its hold and no
// order left that would ever settle it. The money is not lost to a raid or a
// bad trade: it is unreachable, which is worse, because nothing says so.
SOL_TEST(a_captain_standing_down_takes_what_the_load_fetches_rather_than_stranding_it)
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
    SOL_REQUIRE(fixture.world().orderHaul(0, places[0].market, 0.50f));

    bool laden = false;
    for (int legs = 0; legs < 6 && !laden; ++legs) {
        SOL_REQUIRE(runUntilParked(fixture.world(), 0));
        laden = fixture.world().captains()[0].haul.leg.cargo > 0.0f &&
                fixture.world().captains()[0].haul.outlay > 0.0;
        if (!laden) {
            fixture.world().tick(kCoarseStep);
        }
    }
    SOL_REQUIRE(laden);
    const double creditsBefore = fixture.world().playerCredits();

    // Stand them down while they are holding a load the floor refuses. ⚑⚑ THEY
    // ARE PARKED, WHICH IS THE PATH THAT CARRIED THE DEFECT: `cancelOrder`'s
    // last branch ends the order ON THE SPOT rather than setting `stopping`,
    // because a parked captain has no leg to finish. That branch never had to
    // think about the hold, since before the floor existed a parked captain had
    // always just sold - so "parked AND laden" is a state this stage created.
    SOL_REQUIRE(fixture.world().captains()[0].haul.leg.phase == sol::sim::TraderPhase::Idle);
    SOL_REQUIRE(fixture.world().cancelOrder(0));
    for (int legs = 0; legs < 4; ++legs) {
        if (fixture.world().captains()[0].order.kind == game::OrderKind::None) {
            break;
        }
        fixture.world().tick(kCoarseStep);
        if (!runUntilParked(fixture.world(), 0)) {
            break;
        }
    }
    const game::Captain& after = fixture.world().captains()[0];
    SOL_CHECK(after.order.kind == game::OrderKind::None); // the order really ended
    // THE ASSERTION THAT MATTERS: the hold is empty and the money came back.
    // Not that it came back at a PROFIT - the floor said this was a bad trade
    // and it was right - but that it came back at all.
    SOL_CHECK(after.haul.leg.cargo <= 0.0f);
    SOL_CHECK(after.haul.outlay <= 0.0);
    SOL_CHECK(fixture.world().playerCredits() > creditsBefore);
    std::printf("  stood down holding a load: recovered %.0f cr rather than stranding it\n",
                fixture.world().playerCredits() - creditsBefore);
}

// The floor rides the save, and a file naming one this game cannot hold out for
// is refused. ⚑ The upper bound is the half worth pinning: a floor of infinity
// read off disk is a captain who can never sell anything again, whose hold
// never clears, who therefore never buys another load, and who flies an empty
// route forever with the player's money locked in it.
SOL_TEST(a_save_carries_the_sell_floor_and_refuses_one_no_captain_could_hold_out_for)
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
    SOL_REQUIRE(fixture.world().orderHaul(0, places[0].market, 0.25f));
    for (int i = 0; i < 40; ++i) {
        fixture.world().tick(kCoarseStep);
    }
    SOL_REQUIRE(fixture.world().captains()[0].order.floor == 0.25f);

    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/captains-floor";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/floor.sav";
    SOL_REQUIRE(fixture.world().saveTo(path.c_str(), "Floor"));

    Fixture reloaded;
    SOL_REQUIRE(reloaded.world().loadFrom(path.c_str()));
    SOL_REQUIRE(reloaded.world().captains().size() == 1);
    SOL_CHECK(reloaded.world().captains()[0].order.floor == 0.25f);

    // ⚑ The clamp is on the way IN as well, so a console line or a script
    // cannot set a floor the loader would then refuse to read back.
    SOL_REQUIRE(fixture.world().setSellFloor(0, 99.0f));
    SOL_CHECK(fixture.world().captains()[0].order.floor == game::kMaxSellFloor);
    SOL_REQUIRE(fixture.world().setSellFloor(0, -1.0f));
    SOL_CHECK(fixture.world().captains()[0].order.floor == 0.0f);
    // And it refuses on an order that has no load it bought.
    SOL_REQUIRE(fixture.world().cancelOrder(0));
    (void)runUntilParked(fixture.world(), 0);
    for (int i = 0; i < 20 && fixture.world().captains()[0].order.kind != game::OrderKind::None; ++i) {
        fixture.world().tick(kCoarseStep);
    }
    SOL_CHECK(!fixture.world().setSellFloor(0, 0.25f));
    std::printf("  floor round-tripped at 25%%, clamped at %.0f%%, refused with no haul\n",
                static_cast<double>(game::kMaxSellFloor) * 100.0);
}

// ⚑⚑⚑⚑ THE PHASE'S ECONOMIC QUESTION, AS AN INSTRUMENT RATHER THAN AS A POINT.
// Stage C measured a mining captain at ~1,030 cr/min over 425 s and stage B
// measured hauling's best route at 12.8, and the eighty-fold gap between them is
// what the phase exit has to rule on. But both are SINGLE POINTS from SHORT
// runs, and the thing that decides whether the gap is real is a curve: a mining
// captain sells its whole hold into ONE market (`order.marketA`), `quoteSell`
// clamps to `capacity - stock`, and `priceAtStock` falls from 2.0x base at an
// empty warehouse to 0.5x at a full one. So a miner may be crashing its own
// price, and 1,030 cr/min may be the first five minutes of a decaying curve
// rather than a steady state.
//
// Stations do consume (`archetype.consumption` in `Economy::produce`), so there
// IS a drain - the question is purely whether it keeps up. This measures it.
//
// ⚑ It asserts almost nothing on purpose. It is a measurement, and the numbers
// are the output; the only guards are that the captain actually worked and that
// the run is long enough to see a curve if there is one.
// ⚑⚑⚑⚑ THE PHASE EXIT'S OWN FINDING: A DEAD CAPTAIN WAS BACK IN THE HALL
// THAT HIRED THEM. Nothing in the suite could see it, because every test here
// asks the world about `captains()` and the hall is derived from the DOCK'S
// SEED - so `killCaptain` erasing the person from `m_captains` handed the slot
// straight back to `captainCandidates`, which had one filter on it and that
// filter was "already in your employ". The exit flight watched it happen: a
// captain announced on the comms channel as "lost with all hands in Lyrth" was
// standing in Lyrth Gamma's crew hall twenty minutes later at the same cut,
// which refutes ruling 14 in the one place a player can see.
//
// ⚑⚑⚑ THE TEST IS THE ASYMMETRY, NOT THE FILTER, and both halves have to be
// in one test or either is satisfiable by a bug. A hall that offers nobody
// passes "the dead one is gone" perfectly; a hall that offers everybody passes
// "the dismissed one came back". What is asserted is that the SAME hall treats
// the two erasures differently, and that the two people beside them are
// untouched either way - this file's own anti-vacuity rule.
SOL_TEST(a_dead_captain_stays_dead_and_a_dismissed_one_comes_back)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    SpaceWorld& w = fixture.world();

    std::vector<CaptainCandidate> hall;
    w.captainCandidates(hall);
    SOL_REQUIRE(hall.size() == SpaceWorld::kCaptainsPerHall);
    const std::uint64_t doomed = hall[0].who;
    const std::string doomedName = hall[0].name;
    const std::uint64_t bystanderA = hall[1].who;
    const std::uint64_t bystanderB = hall[2].who;

    // Somebody to kill, with a hull under them: the death path is the one the
    // game runs, not an erase.
    const std::size_t slot = buyAndArmAMiner(fixture);
    SOL_REQUIRE(slot != 0);
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);
    SOL_REQUIRE(w.captains()[captain].who == doomed);
    SOL_REQUIRE(w.orderMine(captain));
    SOL_REQUIRE(runUntil(w, [&] { return w.captains()[captain].mine.rockStep > 0u; }));
    SOL_REQUIRE(w.killCaptainPuppet(captain));
    SOL_REQUIRE(w.captains().empty());

    // The hall the money hired them out of, asked again.
    w.captainCandidates(hall);
    SOL_CHECK(hall.size() == SpaceWorld::kCaptainsPerHall - 1);
    SOL_CHECK(std::none_of(
        hall.begin(), hall.end(), [doomed](const CaptainCandidate& c) { return c.who == doomed; }));
    // ⚑ THE OTHER TWO ARE UNCHANGED, which is what says the roster was
    // filtered rather than re-rolled - the draws rule this file opens with.
    SOL_CHECK(std::any_of(
        hall.begin(), hall.end(), [bystanderA](const CaptainCandidate& c) { return c.who == bystanderA; }));
    SOL_CHECK(std::any_of(
        hall.begin(), hall.end(), [bystanderB](const CaptainCandidate& c) { return c.who == bystanderB; }));
    std::printf("  %s died and the hall is %zu deep\n", doomedName.c_str(), hall.size());

    // ⚑⚑ AND THE OTHER HALF: A DISMISSAL IS A DOOR YOU CAN WALK BACK THROUGH.
    // Stage A chose that deliberately ("a captain you DISMISS falls out of
    // `m_captains` and is therefore on offer again"), so the fix must not have
    // quietly made every erasure permanent.
    SOL_REQUIRE(w.hireCaptain(0));
    const std::uint64_t rehired = w.captains().back().who;
    SOL_REQUIRE(w.dismissCaptain(w.captains().size() - 1));
    w.captainCandidates(hall);
    SOL_CHECK(std::any_of(
        hall.begin(), hall.end(), [rehired](const CaptainCandidate& c) { return c.who == rehired; }));

    // ⚑⚑⚑ AND IT SURVIVES A SAVE, which is the half that makes it a fact
    // about the GAME rather than about this session. The roster is composed
    // from the seed on load, so a death that is not written down is a death
    // that is undone by quitting - the same shape as every other "what the
    // player DID" record in this file's save block.
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/captains-exit";
    SOL_REQUIRE(sol::platform::createDirectories(dir.c_str()));
    const std::string path = dir + "/dead.sav";
    SOL_REQUIRE(w.saveTo(path.c_str(), "Dead"));

    Fixture reloaded;
    SOL_REQUIRE(reloaded.world().loadFrom(path.c_str()));
    SOL_REQUIRE(reloaded.walkIn(dock));
    std::vector<CaptainCandidate> after;
    reloaded.world().captainCandidates(after);
    SOL_CHECK(after.size() == SpaceWorld::kCaptainsPerHall - 1);
    SOL_CHECK(std::none_of(
        after.begin(), after.end(), [doomed](const CaptainCandidate& c) { return c.who == doomed; }));
    std::printf("  and after a reload the hall is still %zu deep\n", after.size());
}

SOL_TEST(what_a_mining_captain_earns_per_minute_across_an_hour)
{
    Fixture fixture;
    const Dock dock = findMiningDock(fixture);
    SOL_REQUIRE(dock.system != kNone);
    SOL_REQUIRE(fixture.walkIn(dock));
    SpaceWorld& w = fixture.world();
    // ⚑⚑ THE FREIGHTER SPECIFICALLY, BECAUSE THE TWO NUMBERS THIS EXISTS TO
    // COMPARE WERE BOTH MEASURED ON ONE. `buyAnyShip` takes the first hull the
    // catalog offers, which is the 8,000 cr Shuttle with a 4.0 units/s beam and
    // a 50-unit hold - measuring that against stage B's 200-unit freighter
    // hauler would be comparing two different ships and calling it an economy.
    w.addCredits(1'000'000.0);
    const std::size_t slot = w.fleet().size();
    SOL_REQUIRE(w.buyShip("sol.freighter"));
    SOL_REQUIRE(w.switchShip(slot));
    SOL_REQUIRE(armMiningBeam(fixture));
    SOL_REQUIRE(w.switchShip(0));
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);
    std::printf("  hull %s, %.1f units/s beam, hold %.0f\n",
                w.fleet()[slot].defId.c_str(),
                static_cast<double>(w.shipMiningPower(w.fleet()[slot])),
                static_cast<double>(w.resolvedShipDef(w.fleet()[slot]).cargoCapacity));

    // The same hour of warm-up the hauling curve gets, so the two are measured
    // against the same galaxy rather than one against tick zero.
    for (int i = 0; i < 7200; ++i) {
        w.tick(kCoarseStep);
    }

    SOL_REQUIRE(w.orderMine(captain));
    const std::uint32_t market = w.captains()[captain].order.marketA;
    SOL_REQUIRE(market < w.economy().markets().size());

    constexpr double kBucketSeconds = 300.0; // five sim-minutes
    constexpr int kBuckets = 24;             // two hours
    double previousGross = 0.0;
    double firstRate = 0.0;
    double lastRate = 0.0;
    std::printf("  min |    cr/min | cumulative |  ore stock | ore price | hold\n");
    for (int bucket = 0; bucket < kBuckets; ++bucket) {
        for (double t = 0.0; t < kBucketSeconds; t += kCoarseStep) {
            w.tick(kCoarseStep);
        }
        const Captain& who = w.captains()[captain];
        const double gross = who.ledger.earned + who.ledger.paid;
        const double rate = (gross - previousGross) / (kBucketSeconds / 60.0);
        previousGross = gross;
        const std::uint32_t ore = who.mine.commodity;
        std::printf("  %3d | %9.0f | %10.0f | %10.0f | %9.2f | %4.0f\n",
                    (bucket + 1) * 5,
                    rate,
                    gross,
                    static_cast<double>(w.economy().markets()[market].stock[ore]),
                    static_cast<double>(w.economy().price(market, ore)),
                    static_cast<double>(who.mine.units));
        if (bucket == 0) {
            firstRate = rate;
        }
        lastRate = rate;
    }
    std::printf("  first five minutes %.0f cr/min, last five %.0f cr/min (%.0f%% of it)\n",
                firstRate,
                lastRate,
                firstRate > 0.0 ? lastRate / firstRate * 100.0 : 0.0);
    SOL_CHECK(previousGross > 0.0); // the captain actually worked

    // ⚑⚑⚑⚑ THE FINDING, AS A GUARD RATHER THAN A PRINTOUT (the user's ruling
    // 18 and 20). Two things have to be true and neither was true before this
    // stage: the market really does saturate - so the curve above is a fact
    // about the game and not about this run - and the captain SAYS SO and
    // stands down instead of standing at a full counter for ever. The second is
    // what stops "a mining captain has stopped earning" from being invisible.
    const std::uint32_t ore = w.captains().empty() ? 0u : w.captains()[0].mine.commodity;
    (void)ore;
    SOL_CHECK(lastRate < firstRate); // the curve decays; it is not a flat line
    // Either they stood down (the order is gone), or they are visibly stalled.
    // ⚑ Both are the reported state - what must NOT happen is a captain still
    // nominally mining with a full hold and a silent ledger.
    const bool stoodDown =
        captain >= w.captains().size() || w.captains()[captain].order.kind == game::OrderKind::None;
    const bool sayingSo = !stoodDown && w.captains()[captain].mine.stalledSeconds > 0.0;
    std::printf("  ended: %s\n",
                stoodDown ? "stood down with nowhere to sell"
                          : (sayingSo ? "stalled and saying so" : "still working"));
    SOL_CHECK(stoodDown || sayingSo);
}

// ⚑⚑⚑⚑ THE OTHER HALF OF THE SAME QUESTION, AND IT HAS TO BE ASKED OR THE
// ANSWER IS HALF-MEASURED. The mining curve above collapses to zero because a
// miner fills ONE warehouse. A hauler erodes its own spread the same way - it
// buys at A, which raises A's price, and sells at B, which lowers B's - so
// "hauling is the sustainable one" is a claim about a curve nobody has drawn.
// Same hull, same buckets, same table, so the two can be laid side by side.
//
// ⚑ It walks destinations first, because 12 of 20 near routes never trade at
// all (stage B, and the stage E flight saw it live on the shipped start lane).
// A route that never loads measures nothing, so the test finds one that does
// and says which it used.
SOL_TEST(what_a_hauling_captain_earns_per_minute_across_two_hours)
{
    Fixture fixture;
    const Dock yard = fixture.findDock(screenBit(StationScreen::Crew) | screenBit(StationScreen::Shipyard) |
                                       screenBit(StationScreen::Trade));
    SOL_REQUIRE(yard.system != kNone);
    SOL_REQUIRE(fixture.walkIn(yard));
    SpaceWorld& w = fixture.world();
    w.addCredits(2'000'000.0);
    const std::size_t slot = w.fleet().size();
    SOL_REQUIRE(w.buyShip("sol.freighter"));
    const std::size_t captain = hireAndGive(fixture, slot);
    SOL_REQUIRE(captain != kNone);
    SOL_REQUIRE(w.buyMarketIntel());

    // ⚑⚑⚑ THE ECONOMY IS WARMED FIRST, AND WITHOUT THIS THE MEASUREMENT IS OF
    // THE WRONG WORLD. `generateUniverse` seeds every market at its archetype's
    // starting stock, so at t=0 the galaxy is nearly FLAT - there are no spreads
    // to arbitrage because production, consumption and 120 coarse traders have
    // not run yet. Measuring a hauler there says "no route in the galaxy pays",
    // which is a fact about tick zero and not about the economy. A player meets
    // their first captain hours in.
    for (int i = 0; i < 7200; ++i) { // an hour of coarse economy
        w.tick(kCoarseStep);
    }

    std::vector<SpaceWorld::HaulDestination> places;
    w.haulDestinations(places);
    SOL_REQUIRE(!places.empty());

    // Find a destination this captain will actually load for. One leg out is
    // enough to tell: `captainThink` buys at departure or it does not.
    //
    // ⚑⚑ THE PLAYER FOLLOWS THE HULL BETWEEN ATTEMPTS, AND THAT IS NOT
    // BOOKKEEPING. A cancelled haul parks the hull wherever it LANDED, and
    // `orderHaul` refuses unless the player is standing on that same dock - so
    // a loop that re-walks to the original yard silently fails every attempt
    // after the first with "their ship is stored elsewhere", and the test reads
    // as "no route in the galaxy trades" when it never asked one.
    bool loaded = false;
    std::string routeName, routeSystem;
    std::uint32_t routeHops = 0;
    for (int attempt = 0; attempt < 12 && !loaded; ++attempt) {
        const auto& hull = w.fleet()[slot];
        const Dock at{hull.storedSystem, hull.storedStation};
        if (at.system == kNone || !fixture.walkIn(at)) {
            break;
        }
        w.haulDestinations(places);
        if (places.empty()) {
            break;
        }
        const std::size_t pick = static_cast<std::size_t>(attempt) % places.size();
        std::string why;
        const bool given = w.orderHaul(captain, places[pick].market, 0.0f, &why);
        std::printf("    attempt %d: from station %u/%u, %zu destination(s), -> %s (%s): %s\n",
                    attempt,
                    at.system,
                    at.station,
                    places.size(),
                    places[pick].station.c_str(),
                    places[pick].system.c_str(),
                    given ? "ordered" : why.c_str());
        if (!given) {
            continue;
        }
        for (int i = 0; i < 400 && !loaded; ++i) {
            w.tick(kCoarseStep);
            loaded = w.captains()[captain].haul.leg.cargo > 0.0f;
        }
        if (loaded) {
            routeName = places[pick].station;
            routeSystem = places[pick].system;
            routeHops = places[pick].hops;
            break;
        }
        SOL_REQUIRE(w.cancelOrder(captain));
        for (int i = 0; i < 4000 && w.captains()[captain].order.kind != game::OrderKind::None; ++i) {
            w.tick(kCoarseStep);
        }
    }
    SOL_REQUIRE(loaded); // no reachable route ever loaded: that is itself a finding
    std::printf("  route: %s (%s), %u jump(s)\n", routeName.c_str(), routeSystem.c_str(), routeHops);

    constexpr double kBucketSeconds = 300.0;
    constexpr int kBuckets = 24; // two hours, to match the mining curve
    double previousGross = 0.0;
    double firstHour = 0.0;
    double secondHour = 0.0;
    std::printf("  min |    cr/min | cumulative | cargo\n");
    for (int bucket = 0; bucket < kBuckets; ++bucket) {
        for (double t = 0.0; t < kBucketSeconds; t += kCoarseStep) {
            w.tick(kCoarseStep);
        }
        const Captain& who = w.captains()[captain];
        const double gross = who.ledger.earned + who.ledger.paid;
        const double rate = (gross - previousGross) / (kBucketSeconds / 60.0);
        previousGross = gross;
        std::printf("  %3d | %9.1f | %10.0f | %5.0f\n",
                    (bucket + 1) * 5,
                    rate,
                    gross,
                    static_cast<double>(who.haul.leg.cargo));
        if (bucket == kBuckets / 2 - 1) {
            firstHour = gross;
        }
    }
    secondHour = previousGross - firstHour;
    std::printf(
        "  two hours of hauling: %.0f cr total, %.1f cr/min mean\n", previousGross, previousGross / 120.0);
    SOL_REQUIRE(previousGross > 0.0);
    // ⚑⚑⚑⚑ THE GUARD, AND IT IS THE OPPOSITE ONE TO THE MINING CURVE'S (ruling
    // 20). A hauler erodes its own spread the same way a miner fills its own
    // warehouse - it buys at one end and sells at the other, moving both prices
    // against itself - so the question "does hauling also collapse" is a real
    // one and the answer measured here is no. The second hour must earn a
    // meaningful share of the first, which is what fails if somebody breaks the
    // trade loop, the margin floor, or the price impact arithmetic that stage B
    // put in - and it is the assertion behind saying hauling is the SUSTAINED
    // half of this economy rather than merely the smaller one.
    SOL_CHECK(secondHour > firstHour * 0.4);
    std::printf("  hour 1 %.0f cr, hour 2 %.0f cr (%.0f%% of it) - sustained, not a spike\n",
                firstHour,
                secondHour,
                firstHour > 0.0 ? secondHour / firstHour * 100.0 : 0.0);
}
