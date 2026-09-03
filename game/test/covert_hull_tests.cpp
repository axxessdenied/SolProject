// Covert hulls and the pilots who fly them (engine plan Phase 37 stage D):
// `HullRole`'s first reader, three phases after the vocabulary was authored.
//
// ⚑⚑⚑⚑ THE POINT OF THIS FILE IS THAT A WORD IN A DEF FILE NOW CHANGES WHAT A
// SHIP DOES. `HullRole` shipped in Phase 32 and ruling 11 left it deliberately
// unread; Phase 36 said so again in writing when it shipped covert COMPONENTS
// instead of covert hulls. Anything that has been unread for three phases is
// one edit away from being quietly wrong, so the assertions here are about the
// CONNECTION - change `role = "covert"` to anything else and a clan's raider
// goes back to fighting to the death - rather than about the hull's stat line.

#include "content.hpp"
#include "space_world.hpp"
#include "station_ui.hpp"

#include <cstdio>
#include <deque>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/sim/universe.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::assets::HullRole;

namespace {

// Named rather than found by scanning for `role == Covert`, which is Phase 36
// stage B's rule and this phase's third repeat of it: a test that collects its
// own subject by reading back the thing under test cannot see the subject
// removed. Demote this hull's role and these tests fail loudly.
constexpr const char* kCovertHull = "sol.ghostline";

[[nodiscard]] bool loadShippedDefs(DefDatabase& defs)
{
    std::string error;
    if (!defs.mergeDirectory(SOL_DEF_DATA_DIR, &error)) {
        std::printf("  cannot load %s: %s\n", SOL_DEF_DATA_DIR, error.c_str());
        return false;
    }
    return true;
}

} // namespace

// ⚑⚑⚑⚑ THE HULL IS QUIET BEFORE A CREDIT IS SPENT ON KIT, WHICH IS WHAT
// `ShipDef::signature` WAS AUTHORED FOR AND WHAT NOTHING HAD EVER USED. Its own
// comment in `data_defs.hpp` says "no shipped hull uses that, deliberately...
// the key is what makes the eventual family a data pass". This is the test that
// the data pass happened.
SOL_TEST(the_covert_hull_is_the_first_shipped_ship_to_spend_its_signature)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    const sol::assets::ShipDef* covert = defs.findShip(kCovertHull);
    SOL_REQUIRE(covert != nullptr);
    SOL_CHECK(covert->role == HullRole::Covert);
    SOL_CHECK(covert->signature < 1.0f);
    SOL_CHECK(covert->signature > 0.0f);

    // ⚑⚑ AND IT IS THE ONLY ONE, WHICH IS THE HALF THAT MAKES IT A STATEMENT.
    // Every other hull sits at the ordinary 1.0, so "quiet" is a property of
    // this hull rather than a number that drifted across the whole file.
    std::uint32_t quiet = 0;
    std::uint32_t ordinary = 0;
    for (const sol::assets::ShipDef& def : defs.ships()) {
        if (def.signature < 1.0f) {
            ++quiet;
            std::printf("  %s: signature %.2f, role '%s'\n",
                        def.id.c_str(),
                        static_cast<double>(def.signature),
                        sol::assets::hullRoleName(def.role));
            SOL_CHECK(def.role == HullRole::Covert);
        } else {
            ++ordinary;
            SOL_CHECK(def.role != HullRole::Covert);
        }
    }
    std::printf("  %u quiet hull(s) against %u ordinary\n", quiet, ordinary);
    SOL_CHECK(quiet == 1);
    SOL_REQUIRE(ordinary > 0);

    // ⚑ Two subsystem mounts, which is one more than any other hull has and is
    // the reason to fly this rather than a fitted shuttle. Phase 36 stage E made
    // the shuttle's single `covert_bay` an exclusive choice - a dampener OR a
    // scanner; this is where the covert family stops being a trade and becomes
    // a loadout, and the count is what says so.
    std::uint32_t bays = 0;
    for (const sol::assets::ShipMount& mount : covert->mounts) {
        bays += mount.kind == sol::assets::MountKind::Subsystem ? 1u : 0u;
    }
    std::printf("  %s carries %u subsystem mount(s)\n", covert->id.c_str(), bays);
    SOL_CHECK(bays == 2);

    // And it gives something up for it, or the phase shipped a free upgrade.
    const sol::assets::ShipDef* freighter = defs.findShip("sol.freighter");
    const sol::assets::ShipDef* interceptor = defs.findShip("sol.interceptor");
    SOL_REQUIRE(freighter != nullptr && interceptor != nullptr);
    SOL_CHECK(covert->cargoCapacity < freighter->cargoCapacity);
    SOL_CHECK(covert->flight.maxSpeed < interceptor->flight.maxSpeed);
    SOL_CHECK(covert->defense.hull < freighter->defense.hull);
    SOL_CHECK(covert->price > interceptor->price);
}

// ⚑⚑⚑⚑ THE FIRST READER, ASSERTED AS A CONNECTION RATHER THAN AS A FIELD.
// `spawnWing` hands a pilot the role of the CELL it was sent as - its own
// comment says "the job is the cell's, not a caller's opinion of it" - and
// Phase 37 stage D makes exactly one exception: a covert HULL overrides it.
// This is the test that the exception is real, and it is written as a
// comparison between two hulls in the same roster so that a change to the rule
// cannot hide behind a change to the content.
SOL_TEST(a_covert_hull_makes_its_pilot_covert_whatever_cell_sent_it)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));

    // The shipped clans field both, in the SAME cell, which is what makes this
    // a statement about the hull and not about the roster.
    const sol::assets::FactionDef* clan = defs.findFaction("sol.corsairs");
    SOL_REQUIRE(clan != nullptr);
    bool hasCovert = false;
    bool hasLine = false;
    for (const std::string& id : clan->shipsRaider) {
        const sol::assets::ShipDef* def = defs.findShip(id.c_str());
        SOL_REQUIRE(def != nullptr);
        hasCovert = hasCovert || def->role == HullRole::Covert;
        hasLine = hasLine || def->role == HullRole::Line;
    }
    std::printf(
        "  %s raider cell: covert %d, line %d\n", clan->id.c_str(), hasCovert ? 1 : 0, hasLine ? 1 : 0);
    SOL_REQUIRE(hasCovert);
    SOL_REQUIRE(hasLine); // or the comparison below has nothing to compare

    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    // ⚑⚑ READ THROUGH `collectDuePilotThinks`, WHICH IS THE EXACT STRING THE
    // LUA HOOK BRANCHES ON. A test that read the enum would agree with itself
    // while `pilotRoleName` handed Lua something else, and Lua's `pilot_think`
    // falls through EVERY branch on an unknown role - a covert pilot that idled
    // forever would look like a calm ship rather than a broken one. So the
    // assertion is on the word that actually reaches the AI.
    std::uint32_t covertPilots = 0;
    std::uint32_t otherPilots = 0;
    std::vector<game::SpaceWorld::PilotThink> thinks;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        if (!world.enterSystem(s)) {
            continue;
        }
        // Ambient wings spawn on entry; one think interval brings them due.
        thinks.clear();
        world.collectDuePilotThinks(1.0, thinks);
        for (const game::SpaceWorld::PilotThink& think : thinks) {
            if (std::string(think.role) == "covert") {
                ++covertPilots;
            } else {
                ++otherPilots;
            }
        }
        if (covertPilots > 0 && otherPilots > 0) {
            break;
        }
    }
    std::printf("  %u covert pilot(s) and %u other(s) in the shipped sky\n", covertPilots, otherPilots);
    SOL_REQUIRE(otherPilots > 0);
    SOL_CHECK(covertPilots > 0);
}

// ⚑⚑⚑⚑ EVERY ROLE THE ENGINE CAN NAME HAS A BRANCH IN THE SCRIPT THAT READS
// IT, AND THIS IS THE ONE COUPLING IN THE STAGE THAT WOULD HAVE FAILED IN
// SILENCE. `pilot_think` is an `if/elseif` chain on the role STRING with no
// else: a role the script has never heard of falls through every branch and the
// pilot does nothing at all, forever. That does not crash, does not warn, and
// does not even look wrong - an idle ship reads as a calm one.
//
// ⚑⚑⚑ SO THE ASSERTION IS ON THE SHIPPED SCRIPT AS TEXT, which is ugly and is
// the only thing that can see this. The alternative - flying a covert pilot and
// checking it acts - cannot distinguish "no branch" from "nothing to do right
// now", because the covert branch's own answer to an empty sky is to do
// nothing. ⚑ It reads `pilotRoleName` for the vocabulary rather than listing
// the words, so adding a fifth role fails here until the script learns it.
SOL_TEST(every_pilot_role_the_engine_can_name_has_a_branch_in_the_shipped_script)
{
    const std::string path = std::string(SOL_DEF_DATA_DIR) + "/scripts/init.lua";
    std::ifstream file(path);
    SOL_REQUIRE(file.good());
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string script = buffer.str();
    SOL_REQUIRE(script.find("function pilot_think") != std::string::npos);

    std::uint32_t checked = 0;
    for (std::uint32_t r = 0; r <= static_cast<std::uint32_t>(game::PilotRole::Covert); ++r) {
        const char* name = game::pilotRoleName(static_cast<game::PilotRole>(r));
        const std::string branch = std::string("role == \"") + name + "\"";
        if (script.find(branch) == std::string::npos) {
            std::printf("  pilot_think has no branch for role '%s'\n", name);
        }
        SOL_CHECK(script.find(branch) != std::string::npos);
        ++checked;
    }
    std::printf("  %u role(s), every one of them handled\n", checked);
    SOL_CHECK(checked == 4);
}

// ⚑⚑⚑ THE HULL IS ON THE FENCE'S COUNTER AT EVERY BACK ROOM, AND ON NO
// LAWFUL SHELF ANYWHERE. This is the measurement that chose the design: exactly
// ONE of the eight fence docks in the shipped galaxy rolled a Shipyard module,
// so a covert hull left to the ordinary Shipyard tab would have been for sale at
// one dock in 125 - and at a DIFFERENT one on any other seed. The fence already
// has a counter; the hull is sold over it.
SOL_TEST(the_covert_hull_is_sold_at_every_back_room_and_no_lawful_shipyard)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    game::GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    const sol::assets::ShipDef* covert = content.defs().findShip(kCovertHull);
    SOL_REQUIRE(covert != nullptr);
    SOL_REQUIRE(covert->gate.factions.size() == 1);
    SOL_CHECK(covert->gate.factions[0] == "sol.ninth_shift");

    std::uint32_t fences = 0;
    std::uint32_t fencesSelling = 0;
    std::uint32_t lawfulSelling = 0;
    std::uint32_t yards = 0;
    const auto shipyardBit = 1u << static_cast<std::uint32_t>(sol::assets::StationScreen::Shipyard);
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            const bool yard = (world.stationScreens(s, t) & shipyardBit) != 0u;
            yards += yard ? 1u : 0u;
            const bool fence = world.stationShadowOwner(s, t) != sol::sim::kNoFaction;
            if (!fence && !yard) {
                continue; // nowhere a hull could be sold either way
            }
            if (!world.enterSystem(s) || !world.warpToStationOffset(t, {150.0, 0.0, 0.0}) ||
                !world.tryDockNearestStation(2000.0)) {
                continue;
            }
            fences += fence ? 1u : 0u;
            if (world.stationFenceCarries(covert->gate)) {
                fencesSelling += 1u;
                SOL_CHECK(fence); // only a back room carries it
            } else if (yard) {
                // A lawful yard: it must refuse, and refuse even to somebody the
                // local authority thinks well of.
                const std::uint32_t owner = world.systemOwnerFaction(s);
                if (owner < world.factions().size()) {
                    world.factionSim().setStanding(owner, 100.0f);
                }
                if (world.stationSells(covert->gate)) {
                    std::printf("  %s sells the covert hull over a lawful counter\n",
                                system.stations[t].name.c_str());
                    ++lawfulSelling;
                }
                SOL_CHECK(!world.stationSells(covert->gate));
            }
            (void)world.undock();
        }
    }
    std::printf("  %u back room(s) reached, %u sell it; %u shipyard(s), %u sell it\n",
                fences,
                fencesSelling,
                yards,
                lawfulSelling);
    SOL_REQUIRE(fences > 0);
    SOL_REQUIRE(yards > 0);             // or the negative half is vacuous
    SOL_CHECK(fencesSelling == fences); // every back room, not just the lucky one
    SOL_CHECK(lawfulSelling == 0);
}

// ⚑⚑⚑⚑ AND IT IS ON THE FENCE'S SHELF IN THE FILL, NOT MERELY SELLABLE IN THE
// WORLD. This test exists because a mutation walked through the suite: routing
// the covert hull to `shipRows` instead of `blackMarketShipRows` - which would
// hide it from the Black Market tab entirely and leak it into a Shipyard tab -
// left 366 of 366 green.
//
// ⚑⚑⚑ THAT IS THE SECOND TIME IN THIS PHASE, AND IT IS THE SAME SHAPE BOTH
// TIMES: every other guard here asks the WORLD whether a counter would sell the
// thing, and none of them asks the FILL which shelf it put the row on. Phase 37
// stage C paid for this once with the contraband rows and had to count vertices;
// here the fill hands back spans, so the row can be named directly. *A stage
// whose point is what the player sees needs at least one guard that looks at
// what the fill produced.*
SOL_TEST(the_fill_puts_the_covert_hull_on_the_back_rooms_shelf_and_not_the_shipyards)
{
    DefDatabase defs;
    SOL_REQUIRE(loadShippedDefs(defs));
    game::SpaceWorld world;
    world.spawn(game::kDefaultUniverseSeed);
    game::GameContent content;
    SOL_REQUIRE(content.initialize(SOL_DEF_DATA_DIR, {}, &world));
    SOL_REQUIRE(world.generateUniverse(content.defs()));

    bool docked = false;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size() && !docked; ++s) {
        const sol::sim::SystemSpec& system = world.galaxy().systems[s];
        for (std::uint32_t t = 0; t < system.stations.size(); ++t) {
            if (world.stationShadowOwner(s, t) == sol::sim::kNoFaction) {
                continue;
            }
            SOL_REQUIRE(world.enterSystem(s));
            SOL_REQUIRE(world.warpToStationOffset(t, {150.0, 0.0, 0.0}));
            SOL_REQUIRE(world.tryDockNearestStation(2000.0));
            docked = true;
            break;
        }
    }
    SOL_REQUIRE(docked);

    std::deque<std::string> text;
    sol::ui::StationPanel panel;
    std::vector<sol::ui::MountRow> mounts;
    std::vector<sol::ui::OutfitRow> components;
    std::vector<sol::ui::OutfitRow> blackMarket;
    std::vector<sol::ui::OutfitRow> blackMarketShips;
    std::vector<sol::ui::OutfitRow> weapons;
    std::vector<sol::ui::OutfitRow> crewCatalog;
    std::vector<sol::ui::OutfitRow> crewAboard;
    std::vector<sol::ui::OutfitRow> ships;
    std::vector<sol::ui::FleetRow> fleet;
    std::vector<sol::ui::CaptainRow> captains;
    std::vector<sol::ui::CaptainRow> captainHires;
    std::vector<sol::ui::FactionRow> factions;
    game::fillStationOutfitting(world,
                                content.defs(),
                                text,
                                panel,
                                mounts,
                                components,
                                blackMarket,
                                blackMarketShips,
                                weapons,
                                crewCatalog,
                                crewAboard,
                                ships,
                                fleet,
                                captains,
                                captainHires,
                                factions);

    const auto names = [](const std::span<const sol::ui::OutfitRow>& rows) {
        std::string joined;
        for (const sol::ui::OutfitRow& row : rows) {
            joined += joined.empty() ? "" : ", ";
            joined += row.id;
        }
        return joined.empty() ? std::string("(none)") : joined;
    };
    std::printf(
        "  %s  back room hulls: %s\n", world.dockedStationName(), names(panel.blackMarketShips).c_str());
    std::printf("  %s  shipyard hulls:  %s\n", world.dockedStationName(), names(panel.shipCatalog).c_str());

    bool onFenceShelf = false;
    for (const sol::ui::OutfitRow& row : panel.blackMarketShips) {
        onFenceShelf = onFenceShelf || std::string(row.id) == kCovertHull;
    }
    SOL_CHECK(onFenceShelf);
    for (const sol::ui::OutfitRow& row : panel.shipCatalog) {
        SOL_CHECK(std::string(row.id) != kCovertHull);
    }
    // ⚑ And the fence's shelf carries the covert hull and nothing else, so a
    // fill that simply moved every hull behind the curtain would fail here.
    SOL_CHECK(panel.blackMarketShips.size() == 1);

    // ⚑⚑⚑ THE `owned` COLUMN MOVES WHEN YOU BUY ONE, WHICH IS HERE BECAUSE IT
    // DID NOT. The ship fill hardcoded `.fitted = 0` from Phase 8a, and nothing
    // was wrong with that for six phases: the Shipyard tab never drew the field.
    // The back room's Hulls shelf does, so the row read "owned 0" in the frame
    // AFTER the purchase - caught by flying it, not by any test. *A field
    // nobody displays is a field nobody checks, and the day it gets a reader it
    // has been wrong the whole time.*
    SOL_CHECK(panel.blackMarketShips[0].fitted == 0);
    world.addCredits(100'000.0);
    std::string error;
    SOL_REQUIRE(world.buyShip(kCovertHull, &error));
    game::fillStationOutfitting(world,
                                content.defs(),
                                text,
                                panel,
                                mounts,
                                components,
                                blackMarket,
                                blackMarketShips,
                                weapons,
                                crewCatalog,
                                crewAboard,
                                ships,
                                fleet,
                                captains,
                                captainHires,
                                factions);
    SOL_REQUIRE(panel.blackMarketShips.size() == 1);
    std::printf("  after buying one: owned %d\n", panel.blackMarketShips[0].fitted);
    SOL_CHECK(panel.blackMarketShips[0].fitted == 1);
}
