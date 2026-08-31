// Which hull a wing flies, and what job its pilot is given (Phase 32 stage D).
//
// ⚑ `chooseWingHull` is a pure function of numbers, exactly like the three
// security curves it sits beside and like `chooseTraderHull` in `sim` - so most
// of this suite is arithmetic with no world at all. The two at the bottom need
// one, because "a core system's traffic differs from a fringe system's" is a
// claim about the shipped galaxy and nothing smaller can make it.

#include "space_world.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::HullClass;
using sol::assets::RosterCell;

namespace {

constexpr std::uint32_t kNoClass = static_cast<std::uint32_t>(sol::assets::kHullClassCount);

[[nodiscard]] std::uint32_t klass(HullClass c)
{
    return static_cast<std::uint32_t>(c);
}

// How many slots of a wing of `count` fly the roster entry at `index`.
[[nodiscard]] std::uint32_t slotsFlying(const std::vector<std::uint32_t>& classes,
                                        float security,
                                        std::uint32_t count,
                                        std::uint32_t index)
{
    std::uint32_t flown = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (game::chooseWingHull(classes, security, i, count) == index) {
            ++flown;
        }
    }
    return flown;
}

bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

} // namespace

// ⚑⚑⚑ THE RULE, ON THE SHIPPED TRADER ROSTER'S OWN SHAPE: a light hull and a
// heavy one, which is `sol.shuttle` and `sol.freighter` exactly. Strongly-held
// space flies mostly the heavy end, the margins mostly the light end, and the
// wing is SPREAD rather than being N copies of one hull - so the readout is a
// proportion a player can see part of and still read correctly.
SOL_TEST(a_wing_flies_the_heavy_end_where_the_place_is_firmly_held)
{
    const std::vector<std::uint32_t> roster = {klass(HullClass::Light), klass(HullClass::Heavy)};

    // A core system: four civilians at security 0.85.
    SOL_CHECK(slotsFlying(roster, 0.85f, 4, 1) == 3); // heavy
    SOL_CHECK(slotsFlying(roster, 0.85f, 4, 0) == 1); // light

    // A frontier system: three at 0.49.
    SOL_CHECK(slotsFlying(roster, 0.49f, 3, 1) == 1);
    SOL_CHECK(slotsFlying(roster, 0.49f, 3, 0) == 2);

    // A fringe system: one at 0.20, and it is the light hull.
    SOL_CHECK(slotsFlying(roster, 0.20f, 1, 1) == 0);
    SOL_CHECK(slotsFlying(roster, 0.20f, 1, 0) == 1);
}

// ⚑⚑ IT IS THE MAGNITUDE, NOT THE SIGN, AND THAT IS ONE RULE RATHER THAN TWO.
// `baselineSecurity` is negative in clan space, where `raidersFor` already
// reads its magnitude to decide how many. A clan's home fields its heaviest
// raiders for the same reason a core system fields its heaviest patrols: the
// question is how firmly the place is held, not by whom.
SOL_TEST(clan_space_is_ranked_by_how_firmly_it_is_held_not_by_its_sign)
{
    const std::vector<std::uint32_t> roster = {klass(HullClass::Light), klass(HullClass::Heavy)};
    for (std::uint32_t count = 1; count <= 5; ++count) {
        for (std::uint32_t i = 0; i < count; ++i) {
            SOL_CHECK(game::chooseWingHull(roster, -0.85f, i, count) ==
                      game::chooseWingHull(roster, 0.85f, i, count));
            SOL_CHECK(game::chooseWingHull(roster, -0.20f, i, count) ==
                      game::chooseWingHull(roster, 0.20f, i, count));
        }
    }
}

// ⚑⚑⚑ IT RANKS BY CLASS AND NOT BY ROSTER POSITION, which is the whole
// difference from the round-robin it replaced - and the only way to see it is
// to author the roster in the WRONG order. Heavy first, light second: a pick
// keyed on the index would hand a core system entry 0 either way, so this is
// the test that would still pass on the old code if the classes were ignored.
SOL_TEST(the_pick_follows_the_class_rather_than_the_authored_order)
{
    const std::vector<std::uint32_t> ascending = {klass(HullClass::Light), klass(HullClass::Heavy)};
    const std::vector<std::uint32_t> descending = {klass(HullClass::Heavy), klass(HullClass::Light)};

    // The same wing, the same security, the roster reversed: the heavy hull is
    // flown by the same number of slots, at the other index.
    SOL_CHECK(slotsFlying(ascending, 0.85f, 4, 1) == slotsFlying(descending, 0.85f, 4, 0));
    SOL_CHECK(slotsFlying(ascending, 0.20f, 4, 0) == slotsFlying(descending, 0.20f, 4, 1));

    // And three classes rank in class order however they are listed.
    const std::vector<std::uint32_t> jumbled = {
        klass(HullClass::Heavy), klass(HullClass::Skiff), klass(HullClass::Medium)};
    SOL_CHECK(game::chooseWingHull(jumbled, 1.0f, 0, 1) == 0); // the heavy
    SOL_CHECK(game::chooseWingHull(jumbled, 0.0f, 0, 1) == 1); // the skiff
}

// ⚑⚑⚑ AND IT FALLS BACK TO THE ROUND-ROBIN IT REPLACED WHENEVER THE CONTENT HAS
// NOT SPOKEN. `class` is optional with no default - stage A refused to let the
// parser invent one - so a cell where any hull declares none cannot be ranked,
// and neither can one where every hull declares the SAME class. Both answer
// `slot % size`, which is byte-for-byte what shipped before this stage.
SOL_TEST(a_roster_that_declares_no_classes_keeps_the_round_robin)
{
    const std::vector<std::uint32_t> silent = {kNoClass, kNoClass};
    const std::vector<std::uint32_t> partial = {klass(HullClass::Light), kNoClass};
    const std::vector<std::uint32_t> uniform = {klass(HullClass::Light), klass(HullClass::Light)};
    for (std::uint32_t i = 0; i < 6; ++i) {
        SOL_CHECK(game::chooseWingHull(silent, 0.85f, i, 6) == i % 2);
        // ⚑ PARTIAL COUNTS AS SILENT. Ranking two hulls when one has no class
        // means deciding where the silent one goes, which is the parser
        // inventing content by another route.
        SOL_CHECK(game::chooseWingHull(partial, 0.85f, i, 6) == i % 2);
        SOL_CHECK(game::chooseWingHull(uniform, 0.85f, i, 6) == i % 2);
    }
    // A single-hull cell - which is what `ships_patrol` and `ships_raider` are
    // in every shipped faction - is unchanged at any security.
    const std::vector<std::uint32_t> one = {klass(HullClass::Light)};
    SOL_CHECK(game::chooseWingHull(one, 0.85f, 3, 4) == 0);
    SOL_CHECK(game::chooseWingHull(one, 0.05f, 3, 4) == 0);
}

// ⚑⚑⚑ WHAT A SPAWNED PILOT IS DOING COMES FROM THE CELL IT WAS DRAWN OUT OF,
// and this table is what seven call sites each passed by hand before stage D.
// The parameter carried no information and could only ever disagree with the
// roster beside it.
SOL_TEST(the_job_comes_from_the_roster_cell)
{
    SOL_CHECK(game::pilotRoleFor(RosterCell::Patrol) == game::PilotRole::Patrol);
    SOL_CHECK(game::pilotRoleFor(RosterCell::Raider) == game::PilotRole::Fighter);
    SOL_CHECK(game::pilotRoleFor(RosterCell::Trader) == game::PilotRole::Trader);
    // Every cell answers, so no site can reach a default that means nothing.
    for (std::size_t i = 0; i < sol::assets::kRosterCellCount; ++i) {
        const game::PilotRole role = game::pilotRoleFor(static_cast<RosterCell>(i));
        SOL_CHECK(role == game::PilotRole::Patrol || role == game::PilotRole::Fighter ||
                  role == game::PilotRole::Trader);
    }
}

// ⚑⚑ THE SHIPPED CONTENT IS WHY THIS STAGE IS OBSERVABLE AT ALL. The spec
// warned that C and D "will look like very little until art lands", and for the
// patrol and raider cells that is exactly right - one hull each, so the pick
// cannot do anything. The TRADER cell is the exception: `sol.freighter` and
// `sol.shuttle` are two classes apart and 36 metres apart drawn, which is what
// makes "a core system's traffic differs from a fringe system's" a thing a
// player can see without a single new mesh.
SOL_TEST(the_shipped_trader_cell_is_the_one_the_pick_can_act_on)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    const sol::assets::FactionDef* navy = defs.findFaction("sol.navy");
    SOL_REQUIRE(navy != nullptr);
    SOL_REQUIRE(navy->shipsTrader.size() == 2);

    std::vector<std::uint32_t> classes;
    for (const std::string& id : navy->shipsTrader) {
        const sol::assets::ShipDef* ship = defs.findShip(id.c_str());
        SOL_REQUIRE(ship != nullptr);
        SOL_CHECK(ship->hasHullClass);
        classes.push_back(static_cast<std::uint32_t>(ship->hullClass));
    }
    SOL_CHECK(classes[0] != classes[1]);

    // The patrol and raider cells hold one hull, so they are the unchanged
    // half - and saying so is what stops a later roster edit silently making
    // this test the only thing that noticed.
    SOL_CHECK(navy->shipsPatrol.size() == 1);
    SOL_CHECK(navy->shipsRaider.size() == 1);
}

// ⚑⚑⚑⚑ THE EXIT, IN THE SHIPPED GALAXY: THE SAME FACTION FIELDS DIFFERENT
// HULLS IN A CORE SYSTEM AND IN THE FRINGE. Nothing smaller can assert this -
// it needs the generator's own security spread, the faction that owns both
// places, and the wings that actually spawn when a system loads.
SOL_TEST(one_faction_fields_different_hulls_in_the_core_and_in_the_fringe)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    // The most and least firmly held systems one faction owns, so the two
    // readings differ by SECURITY and not by owner.
    const sol::sim::Galaxy& galaxy = world.galaxy();
    auto core = static_cast<std::uint32_t>(galaxy.systems.size());
    auto fringe = static_cast<std::uint32_t>(galaxy.systems.size());
    float best = 0.0f;
    float worst = 2.0f;
    const std::uint32_t owner = world.systemOwnerFaction(world.currentSystemIndex());
    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        if (world.systemOwnerFaction(i) != owner) {
            continue;
        }
        const float baseline = world.systemSecurityBaseline(i);
        if (baseline <= 0.0f) {
            continue;
        }
        if (baseline > best) {
            best = baseline;
            core = i;
        }
        if (baseline < worst) {
            worst = baseline;
            fringe = i;
        }
    }
    SOL_REQUIRE(core < galaxy.systems.size() && fringe < galaxy.systems.size() && core != fringe);

    // ⚑ COUNTED OFF THE CONTACT LIST, WHICH IS THE ONLY VIEW OF THE SKY A TEST
    // AND A PLAYER BOTH HAVE. All three shipped hulls draw ONE mesh, so a model
    // id cannot tell a freighter from a shuttle; the contact NAME can, and it
    // is what the HUD prints. The patrol wing is Interceptors either way, so
    // counting Freighters against Shuttles is counting the civilian traffic.
    const auto haulersNamed = [&world](const char* prefix) {
        std::vector<std::size_t> order;
        world.contactOrder(order);
        std::uint32_t flown = 0;
        for (const std::size_t slot : order) {
            const game::TargetInfo contact = world.contactInfo(slot);
            if (contact.isShip && contact.nav.name.rfind(prefix, 0) == 0) {
                ++flown;
            }
        }
        return flown;
    };

    SOL_REQUIRE(world.enterSystem(core));
    const std::uint32_t coreHeavy = haulersNamed("Freighter");
    const std::uint32_t coreLight = haulersNamed("Shuttle");
    SOL_REQUIRE(world.enterSystem(fringe));
    const std::uint32_t fringeHeavy = haulersNamed("Freighter");
    const std::uint32_t fringeLight = haulersNamed("Shuttle");

    std::printf("  core %.2f: %u freighter(s), %u shuttle(s) | fringe %.2f: %u, %u\n",
                static_cast<double>(best),
                coreHeavy,
                coreLight,
                static_cast<double>(worst),
                fringeHeavy,
                fringeLight);
    // ⚑ The claim is about the MIX, not the count: a core system spawns more
    // civilians than a fringe one anyway (`civiliansFor`), so "more freighters"
    // alone would be true of the old round-robin too. What is new is that the
    // core's traffic leans heavy and the fringe's leans light.
    SOL_CHECK(coreHeavy > coreLight);
    SOL_CHECK(fringeLight > fringeHeavy);
}
