// The fit model as the GAME sees it (engine plan Phase 31 stage B): a fitting
// is named by the mount it occupies, the resolved def carries the player's fit
// rather than the hull's defaults, and a save round-trips the pairing.
//
// ⚑ Why this is a separate suite from `assets.unit`'s loadout tests. Those own
// the RULES - what a mount accepts, what the math does. These own the
// PLUMBING: a hull's `fit` becoming a starter fleet, the saved pairing
// surviving a file, and a def that changed under a save. Only game code can
// see any of that, and the loadout tests cannot link it.

#include "space_world.hpp"

#include <cstring>
#include <string>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::platform::createDirectories;
using sol::platform::deleteFile;

namespace {

// The shuttle is `kPlayerShipDefId`, so the starter fleet is built from this
// def's mounts. Two utility mounts of different sizes, because the size rule
// is only observable when the hull has somewhere a fitting does NOT go.
//
// ⚑ `bay_hold` (medium) is authored with the SMALL pod in it while `bay_port`
// (small) is left empty, and that is not an oversight - it is what makes the
// round-trip test discriminating. A save that stored a flat list of def ids
// and re-placed them on load would put that pod in `bay_port`, because
// `bay_port` is the first mount that accepts it. Rule 3's "fitting small kit
// to a big mount wastes the mount, and the waste is the player's trade" is
// exactly the state such a save would quietly undo.
//
// The commodities are load-bearing for the same reason `save_format_tests`
// says they are: with none, saveTo writes economy bytes loadFrom never reads.
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
base_price = 8.0

[[commodity]]
id = "sol.ore"
name = "Raw Ore"
base_price = 12.0
ore_weight_core = 1.0
ore_weight_frontier = 1.0
ore_weight_fringe = 1.0

[[weapon]]
id = "sol.popgun"
name = "Popgun"
kind = "projectile"
mount = "fixed"
size = "small"
damage = 5.0
price = 400.0

[[weapon]]
id = "sol.bigshot"
name = "Bigshot"
kind = "projectile"
mount = "fixed"
size = "medium"
damage = 30.0
price = 4000.0

[[component]]
id = "sol.pod"
name = "Cargo Pod"
mount = "utility"
size = "small"
price = 600.0
cargo_add = 25.0

[[component]]
id = "sol.bulk_pod"
name = "Bulk Pod"
mount = "utility"
size = "medium"
price = 1800.0
cargo_add = 60.0

[[ship]]
id = "sol.shuttle"
name = "Shuttle"
model = "ship"
max_speed = 220.0
cargo = 50.0
power_output = 6.0

  [[ship.mount]]
  id = "gun_nose"
  kind = "fixed"
  size = "small"
  at = [0.0, 0.0, -2.0]
  fit = "sol.popgun"

  [[ship.mount]]
  id = "bay_port"
  kind = "utility"
  size = "small"
  at = [-1.0, 0.0, 0.0]

  [[ship.mount]]
  id = "bay_hold"
  kind = "utility"
  size = "medium"
  at = [0.0, -1.0, 0.0]
  fit = "sol.pod"

[[faction]]
id = "sol.navy"
name = "Solar Navy"
color = [0.25, 0.45, 1.0]
kind = "major"

[[station]]
id = "sol.station_agri"
name = "Agricultural Station"
weight_core = 1.0
weight_frontier = 1.5
weight_fringe = 0.5
)";

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    explicit Fixture(std::uint64_t seed = 1701)
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "fit_defs.toml", &error));
        world.spawn(seed);
        // ⚑ applyDefs BEFORE generateUniverse, which is the order content.cpp
        // uses and is not cosmetic: `resetFleetToStarter` runs inside the
        // generate and reads the hull's mounts for its `fit` keys, so a world
        // that learns its defs afterwards starts with an EMPTY ship.
        world.applyDefs(defs);
        SOL_CHECK(world.generateUniverse(defs));
    }
};

std::string scratchPath(const char* leaf)
{
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/fits";
    SOL_CHECK(createDirectories(dir.c_str()));
    return dir + "/" + leaf;
}

} // namespace

// ⚑ WHERE `ShipDef::weaponId` WENT. The starter ship is armed because its
// hull's gun mount names a `fit`, and nothing in the game knows what a
// "default weapon" is any more.
SOL_TEST(a_new_fleet_is_fitted_from_the_hulls_own_mounts)
{
    Fixture fixture;
    const game::OwnedShip& ship = fixture.world.activeShip();
    SOL_REQUIRE(ship.fittings.size() == 2);
    SOL_REQUIRE(ship.fittingAt("gun_nose") != nullptr);
    SOL_CHECK(ship.fittingAt("gun_nose")->defId == "sol.popgun");
    SOL_REQUIRE(ship.fittingAt("bay_hold") != nullptr);
    SOL_CHECK(ship.fittingAt("bay_hold")->defId == "sol.pod");
    // A bare mount is NOT a fitting: it is absent from the list rather than
    // present with an empty id, so the count is what is aboard.
    SOL_CHECK(ship.fittingAt("bay_port") == nullptr);
}

// ⚑ THE COUNTERFACTUAL THIS TEST EXISTS FOR: a `resolvedShipDef` that returned
// the hull's authored mounts unchanged would leave the player flying whatever
// the def file said, forever - a refit would apply its stat modifiers and
// change no gun. `applyShipDef` reads the gun off the RESOLVED def, so this is
// the seam that makes one code path serve an NPC and the player alike.
SOL_TEST(the_resolved_def_carries_the_players_fit_not_the_hulls_default)
{
    Fixture fixture;
    const game::SpaceWorld& world = fixture.world;

    sol::assets::ShipDef resolved = world.resolvedShipDef(world.activeShip());
    SOL_REQUIRE(resolved.findMount("gun_nose") != nullptr);
    SOL_CHECK(resolved.findMount("gun_nose")->fit == "sol.popgun");
    SOL_CHECK(resolved.findMount("bay_port")->fit.empty());

    // Strip the gun and move the pod - no station needed, which is what keeps
    // this a test of resolution rather than of the shop.
    game::OwnedShip refitted = world.activeShip();
    refitted.fittings.clear();
    refitted.fittings.push_back({.mountId = "bay_hold", .defId = "sol.bulk_pod"});
    resolved = world.resolvedShipDef(refitted);
    SOL_CHECK(resolved.findMount("gun_nose")->fit.empty()); // disarmed, as asked
    SOL_CHECK(resolved.findMount("bay_hold")->fit == "sol.bulk_pod");
    // And the stat math still ran: the bigger pod's add landed on the base.
    SOL_CHECK(resolved.cargoCapacity > world.resolvedShipDef(world.activeShip()).cargoCapacity);
}

// The auto-placement a catalog "Fit" button means. Authored order decides, and
// the size rule is what makes the answer non-obvious.
SOL_TEST(auto_placement_takes_the_first_empty_mount_that_accepts_it)
{
    Fixture fixture;
    const game::SpaceWorld& world = fixture.world;

    // `bay_port` is the only free utility mount, and it takes the small pod.
    SOL_CHECK(world.firstFreeMountFor("sol.pod") == "bay_port");
    // The medium pod does not fit it, and the medium mount is occupied - so
    // there is nowhere for it at all. "No free mount" rather than "swap".
    SOL_CHECK(world.firstFreeMountFor("sol.bulk_pod").empty());
    // The gun mount is occupied by the hull's own fit, so a second gun has
    // nowhere to go either.
    SOL_CHECK(world.firstFreeMountFor("sol.popgun").empty());
    // And a medium gun would not fit the small nose mount even if it were
    // free, which is the size rule biting on a weapon rather than a component.
    SOL_CHECK(world.firstFreeMountFor("sol.bigshot").empty());
    // A def nobody ships resolves to nowhere rather than to the first mount.
    SOL_CHECK(world.firstFreeMountFor("sol.nonesuch").empty());
}

// ⚑ THE SAVE FORMAT'S OWN PROMISE (v18, decisions/014 rule 1). A save stores
// mount id and def id as a PAIR, so a fitting comes back in the place the
// player put it - not in the first place that would take it.
SOL_TEST(a_save_round_trips_which_mount_holds_which_fitting)
{
    const std::string path = scratchPath("fit_round_trip.sav");
    (void)deleteFile(path.c_str());

    Fixture writer;
    SOL_REQUIRE(writer.world.saveTo(path.c_str(), "fits"));

    Fixture reader;
    SOL_REQUIRE(reader.world.loadFrom(path.c_str()));
    const game::OwnedShip& loaded = reader.world.activeShip();
    SOL_REQUIRE(loaded.fittings.size() == 2);
    SOL_REQUIRE(loaded.fittingAt("gun_nose") != nullptr);
    SOL_CHECK(loaded.fittingAt("gun_nose")->defId == "sol.popgun");

    // ⚑ THE DISCRIMINATING PAIR. The pod is in the MEDIUM mount, wasting it,
    // and `bay_port` - the small mount that would have taken it first - is
    // still empty. Both halves, because either alone is satisfied by a save
    // that re-placed the pod from a flat id list.
    SOL_REQUIRE(loaded.fittingAt("bay_hold") != nullptr);
    SOL_CHECK(loaded.fittingAt("bay_hold")->defId == "sol.pod");
    SOL_CHECK(loaded.fittingAt("bay_port") == nullptr);

    (void)deleteFile(path.c_str());
}

// ⚑ A HULL THAT LOST A MOUNT UNDER A SAVE. A mod is uninstalled, or an author
// renames a mount: the fitting has nowhere to go. It is DROPPED rather than
// carried, because a carried one fails validation forever and leaves the ship
// unrefittable - a save that loads into a state the player cannot get out of.
SOL_TEST(a_fitting_whose_mount_is_gone_is_dropped_rather_than_carried)
{
    Fixture fixture;
    const game::SpaceWorld& world = fixture.world;

    game::OwnedShip stale = world.activeShip();
    stale.fittings.clear();
    stale.fittings.push_back({.mountId = "gun_nose", .defId = "sol.popgun"});
    stale.fittings.push_back({.mountId = "wing_pylon_left", .defId = "sol.pod"});

    const sol::assets::ShipDef resolved = world.resolvedShipDef(stale);
    SOL_CHECK(resolved.findMount("gun_nose")->fit == "sol.popgun");
    SOL_CHECK(resolved.findMount("wing_pylon_left") == nullptr);
    // The hull is still flyable and still armed, which is the whole point:
    // losing a mount costs the player what was in it and nothing else.
    SOL_CHECK(resolved.mounts.size() == 3);
}
