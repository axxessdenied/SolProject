// A ship's GUNS as the sim holds and fires them (engine plan Phase 31 stage
// C1). Until C1 a hull had one `ShipWeapon` and the only interesting question
// about it was whether the loadout resolved; the interesting questions now are
// all about plurality — how many guns a hull's mount list becomes, where each
// one shoots from, and which of them go off when the capacitor cannot pay for
// all of them at once.
//
// ⚑ Why this is not in `ship_fit_tests`. That suite owns the PLUMBING of a fit
// — a hull's `fit` becoming a starter fleet, a save round-tripping the pairing.
// This one owns what the fit is FOR, and needs the world stepped rather than
// merely built: three of the checks below are only visible after a tick with
// the trigger down.

#include "ship_camera.hpp"
#include "space_world.hpp"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;

namespace {

// ⚑ TWO GUNS ON TWO WING MOUNTS, AT A SCALE THAT IS NOT ONE. Both halves are
// load-bearing. Two mounts is the whole subject; `scale = 2.0` is what makes
// the muzzle test discriminating, because a firing pass that used a mount's
// `at` raw rather than scaling it by the hull would still put the two bolts in
// two different places and only be wrong about how far apart.
//
// The guns differ in damage and rate of fire so that "which gun is index 0" is
// answerable, and both are `projectile` so both leave a bolt to look at.
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
base_price = 8.0

# ⚑⚑ THREE DRAWABLES THAT ONLY PHASE 31 STAGE E NEEDS, and they have to be
# real `[[model]]` rows rather than names: a fitting whose model does not
# RESOLVE is not drawn, so a fixture that named meshes it never declared would
# assert the stage does nothing and pass. The mesh stems are the unit cube's
# because nothing here renders - what is under test is which model id an
# instance carries and where it is put, not what it looks like.
[[model]]
id = "gun_body"
mesh = "cube"
texture = "hull"
radius = 1.0

[[model]]
id = "beam_body"
mesh = "cube"
texture = "hull"
radius = 1.0

# The shape a BROKEN name falls back to. Deliberately a third model rather
# than one of the two above, so "fell back" and "resolved correctly" are
# distinguishable answers.
[[model]]
id = "fallback_body"
mesh = "cube"
texture = "hull"
radius = 1.0

# What a BOLT is drawn as, which nothing in this suite declared until Phase 31
# stage E needed to prove that a gun's mesh and its shot's mesh are two
# different answers. See `a_bolt_is_drawn_as_its_own_model_and_not_as_its_gun`.
[[model]]
id = "bolt_body"
mesh = "cube"
texture = "hull"
radius = 1.0

[[role]]
id = "fitting"
model = "fallback_body"

[[role]]
id = "bolt"
model = "bolt_body"

[[weapon]]
id = "sol.left_gun"
name = "Left Gun"
kind = "projectile"
mount = "fixed"
size = "small"
damage = 5.0
rate_of_fire = 2.0
range = 2000.0
projectile_speed = 600.0
energy_cost = 10.0
price = 400.0
model = "gun_body"

[[weapon]]
id = "sol.right_gun"
name = "Right Gun"
kind = "projectile"
mount = "fixed"
size = "small"
damage = 7.0
rate_of_fire = 3.0
range = 2500.0
projectile_speed = 800.0
energy_cost = 10.0
price = 500.0
model = "gun_body"

[[weapon]]
id = "sol.beam"
name = "Cutting Beam"
kind = "hitscan"
mount = "fixed"
size = "small"
damage = 2.0
rate_of_fire = 10.0
range = 800.0
energy_cost = 1.0
mining_power = 4.0
price = 600.0
model = "beam_body"

# A beam that does NOT cut rock (Phase 31 stage C2). The shield-facing test
# below needs a hitscan shot that certainly lands on a hull, and a beam with
# `mining_power` sweeps rocks first - so an asteroid drifting into the line
# would turn a damage assertion into a mining one.
[[weapon]]
id = "sol.pulse_beam"
name = "Pulse Beam"
kind = "hitscan"
mount = "fixed"
size = "small"
damage = 6.0
rate_of_fire = 4.0
range = 2000.0
energy_cost = 2.0
price = 700.0

[[weapon]]
id = "sol.short_gun"
name = "Short Gun"
kind = "projectile"
mount = "fixed"
size = "small"
damage = 4.0
rate_of_fire = 2.0
range = 500.0
projectile_speed = 600.0
energy_cost = 10.0
price = 300.0

# ⚑⚑ TWO GUNS FOR THE TWO WAYS A FITTING CAN FAIL TO BE DRAWN (Phase 31
# stage E), and they are separate weapons rather than edits to the ones above
# because the difference between them is the whole rule. A weapon that names
# NO model is not drawn - a bare hardpoint, which is what every gun in this
# game looked like before stage E and what a mod's unarted gun still looks
# like. A weapon that names a model which does not EXIST is an author's
# mistake, and falls back to the `fitting` role so the mistake is visible.
[[weapon]]
id = "sol.unseen_gun"
name = "Unseen Gun"
kind = "projectile"
mount = "fixed"
size = "small"
damage = 3.0
rate_of_fire = 2.0
range = 2000.0
projectile_speed = 600.0
energy_cost = 10.0
price = 400.0

[[weapon]]
id = "sol.typo_gun"
name = "Typo Gun"
kind = "projectile"
mount = "fixed"
size = "small"
damage = 3.0
rate_of_fire = 2.0
range = 2000.0
projectile_speed = 600.0
energy_cost = 10.0
price = 400.0
model = "no_such_mesh"

# Something to shoot AT (Phase 31 stage C2): a hull with no mounts at all, so
# it never fires back and never adds a bolt to the frame being differenced.
[[ship]]
id = "sol.hulk"
name = "Hulk"
model = "ship"
max_speed = 100.0
cargo = 10.0
power_output = 1.0

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

// ⚑ THE HULL IS COMPOSED RATHER THAN LAYERED, because a `[[ship]]` row in a
// later layer REPLACES the def it shadows and `name` is required on every one
// of them — so "override just the capacitor" is not a thing the def format
// offers, and pretending otherwise here would be testing against a merge
// behaviour the game does not have.
//
// `scale = 2.0` is not decoration: it is what makes the muzzle test
// discriminating, because a firing pass that used a mount's `at` raw rather
// than scaling it by the hull would still put two bolts in two places and only
// be wrong about how far apart.
[[nodiscard]] std::string
hull(const std::string& mounts, const char* capacitor = "100.0", const char* recharge = "15.0")
{
    return std::string(R"(
[[ship]]
id = "sol.shuttle"
name = "Gunboat"
model = "ship"
scale = 2.0
max_speed = 220.0
cargo = 50.0
power_output = 6.0
weapon_capacitor = )") +
           capacitor + "\nweapon_recharge = " + recharge + "\n" + mounts;
}

// One gun mount, as a `[[ship.mount]]` stanza.
[[nodiscard]] std::string gunMount(const char* id, double x, const char* fit)
{
    std::string stanza = "\n  [[ship.mount]]\n  id = \"";
    stanza += id;
    stanza += "\"\n  kind = \"fixed\"\n  size = \"small\"\n  at = [";
    stanza += std::to_string(x);
    stanza += ", 0.0, -5.0]\n";
    if (fit != nullptr) {
        stanza += "  fit = \"";
        stanza += fit;
        stanza += "\"\n";
    }
    return stanza;
}

// One RING, as a `[[ship.mount]]` stanza (Phase 31 stage C2). `aim` and `arc`
// are the two keys the stage exists to read, so both are arguments; `at` is
// deliberately small and on the centreline, because where the muzzle sits is
// C1's subject and a big offset here would only blur the direction a bolt
// leaves in.
[[nodiscard]] std::string turretMount(const char* id, double x, const char* aim, double arc, const char* fit)
{
    std::string stanza = "\n  [[ship.mount]]\n  id = \"";
    stanza += id;
    stanza += "\"\n  kind = \"turret\"\n  size = \"small\"\n  at = [";
    stanza += std::to_string(x);
    stanza += ", 0.0, 0.0]\n  aim = ";
    stanza += aim;
    stanza += "\n  arc = ";
    stanza += std::to_string(arc);
    stanza += "\n";
    if (fit != nullptr) {
        stanza += "  fit = \"";
        stanza += fit;
        stanza += "\"\n";
    }
    return stanza;
}

// A bolted nose gun to port and a dorsal ring to starboard - the pair every
// traverse test below flies, because the whole subject is the DIFFERENCE
// between a gun the pilot aims and a gun that aims itself.
//
// The ring's `aim` is straight up with a 270-degree arc, which is the shipped
// freighter's dorsal turret exactly: 135 degrees either side of up reaches
// ahead, astern and to both beams, and is blind only through the hull.
[[nodiscard]] std::string boltedAndRing(const char* ringFit = "sol.left_gun", double arc = 270.0)
{
    return hull(gunMount("gun_port", -3.0, "sol.left_gun") +
                turretMount("turret_dorsal", 3.0, "[0.0, 1.0, 0.0]", arc, ringFit));
}

// The two-wing-gun hull every test that does not say otherwise flies.
[[nodiscard]] std::string twoGuns()
{
    return hull(gunMount("gun_port", -3.0, "sol.left_gun") + gunMount("gun_starboard", 3.0, "sol.right_gun"));
}

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;

    explicit Fixture(const std::string& shipToml, std::uint64_t seed = 1701)
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "gun_defs.toml", &error));
        const std::string full = shipToml;
        if (!defs.mergeToml(full.c_str(), full.size(), "gun_hull.toml", &error)) {
            std::printf("  hull did not parse: %s\n", error.c_str());
            SOL_CHECK(false);
        }
        world.spawn(seed);
        world.applyDefs(defs);
        SOL_CHECK(world.generateUniverse(defs));
    }

    // ⚑ SOMETHING TO SHOOT AT, PUT WHERE THE NOSE IS NOT (Phase 31 stage C2).
    // A hulk spawns ahead of the player, and then the player is warped a
    // kilometre PAST it - so it ends up directly astern with the nose still
    // pointing the way it was. Astern is the discriminating bearing: it is 90
    // degrees from a dorsal ring's `aim` (inside a 270 arc) and 180 degrees
    // from the nose, so a gun that lays itself and a gun that does not fire in
    // visibly opposite directions.
    //
    // The warp is what makes this exact rather than approximate: it parks the
    // ship on a point, faces the nose the way it came, and zeroes the velocity,
    // so nothing drifts between the placement and the shot.
    //
    // ⚑ THE HULK IS FLOWN BY AN UNAFFILIATED PILOT, and that is load-bearing
    // rather than decoration: a ring only lays on a target the player is at
    // war with, and an unaffiliated console spawn is unconditionally
    // player-hostile (the pre-8b rule `threatTier` still honours). Spawn it
    // with `spawnShipFromDef` instead - no pilot, no threat - and it is tier 2,
    // which is what `a_ring_ignores_a_selection_that_is_not_hostile` asserts.
    bool hulkAstern(double range, bool hostile = true)
    {
        const sol::assets::ShipDef* hulk = defs.findShip("sol.hulk");
        if (hulk == nullptr) {
            return false;
        }
        if (hostile) {
            (void)world.spawnPilotFromDef(*hulk, defs, game::PilotRole::Fighter);
        } else {
            (void)world.spawnShipFromDef(*hulk, defs);
        }
        if (!world.targetShipByName("Hulk")) {
            return false;
        }
        const sol::core::DVec3 at = world.currentTargetInfo().nav.position;
        const sol::core::DVec3 nose =
            toDVec3(rotate(world.shipState().orientation, sol::core::Vec3{0.0f, 0.0f, -1.0f}));
        const bool warped = world.warpTo(at + nose * range, 0.0);
        // Selecting is a SEPARATE step: half these tests want the hulk sitting
        // there unselected, because "a turret with no target follows the nose"
        // is only a claim if the target it is not following is really present.
        world.selectTarget(0);
        return warped;
    }

    bool selectHulk() { return world.targetShipByName("Hulk"); }

    // One tick with the trigger held, and everything it drew that was not
    // drawn before it: a bolt is identified by DIFFERENCE rather than by its
    // model or its scale, which keeps the test from encoding what a bolt
    // happens to look like this month.
    std::vector<game::RenderInstance> fireOnceDrawn(double dt = 1.0 / 60.0)
    {
        std::vector<game::RenderInstance> before;
        world.buildRenderInstances(1.0f, true, before);
        sol::sim::FlightInput input;
        input.trigger = true;
        world.setShipInput(input);
        world.tick(dt);
        std::vector<game::RenderInstance> after;
        world.buildRenderInstances(1.0f, true, after);

        std::vector<game::RenderInstance> fresh;
        for (const game::RenderInstance& instance : after) {
            bool seen = false;
            for (const game::RenderInstance& old : before) {
                if (old.key == instance.key) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                fresh.push_back(instance);
            }
        }
        return fresh;
    }

    // ⚑⚑ EVERYTHING THE RENDERER WOULD DRAW WITH NO ENTITY BEHIND IT, which
    // since Phase 31 stage E is exactly the fittings. The cockpit is the only
    // other keyless instance in the game and `main.cpp` pushes it, not the
    // world - so inside this suite "no key" and "a fitting" are the same set.
    //
    // Read back through `buildRenderInstances` rather than off the armament
    // deliberately: what is asserted is what would be on screen, including the
    // interpolation and the hull's own pose, and not what a component happens
    // to hold.
    std::vector<game::RenderInstance> fittingsDrawn(bool includeShip = true)
    {
        std::vector<game::RenderInstance> all;
        world.buildRenderInstances(1.0f, includeShip, all);
        std::vector<game::RenderInstance> fittings;
        for (const game::RenderInstance& instance : all) {
            if (instance.key == game::kNoInstanceKey) {
                fittings.push_back(instance);
            }
        }
        return fittings;
    }

    // Turns the hull to face `direction` and parks it, both ends of the tick
    // written - which is what `warpTo` does and what keeps the nlerp from
    // swinging the ship through the whole turn on the frame under test.
    bool faceAlong(const sol::core::DVec3& direction)
    {
        return world.warpTo(world.shipState().position + direction, 0.0);
    }

    // The same, reduced to where each bolt appeared.
    std::vector<sol::core::DVec3> fireOnce(double dt = 1.0 / 60.0)
    {
        std::vector<sol::core::DVec3> positions;
        for (const game::RenderInstance& instance : fireOnceDrawn(dt)) {
            positions.push_back(instance.position);
        }
        return positions;
    }
};

[[nodiscard]] double distance(const sol::core::DVec3& a, const sol::core::DVec3& b)
{
    return length(a - b);
}

} // namespace

// ⚑ THE COUNTERFACTUAL THIS TEST EXISTS FOR is stage B's own shipped
// behaviour: `applyShipDef` used to `break` after the first weapon-taking
// mount, so a hull with two guns flew with one and said so only in a comment.
// Restore the break and this turns red on the first line.
SOL_TEST(every_fitted_gun_mount_becomes_a_gun_in_mount_order)
{
    Fixture fixture(twoGuns());
    const std::span<const game::ShipWeapon> guns = fixture.world.playerGuns();
    SOL_REQUIRE(guns.size() == 2);
    // Authored order, not sorted by anything: `gun_port` is first in the file.
    SOL_CHECK(guns[0].damage == 5.0f);
    SOL_CHECK(guns[1].damage == 7.0f);
    SOL_CHECK(guns[0].at[0] == -3.0f);
    SOL_CHECK(guns[1].at[0] == 3.0f);

    // ⚑ AND THE LEAD MARKER FOLLOWS THE FIRST GUN, NOT THE BEST ONE. The port
    // gun is the SLOWER of the two on purpose: a summary that took the fastest
    // projectile - the obvious alternative, and arguably the kinder one - would
    // read 800 here and be indistinguishable from this rule on any hull whose
    // guns happen to be listed fastest-first.
    SOL_CHECK(fixture.world.playerArmament().leadSpeed == 600.0f);
}

// ⚑ A COOLDOWN IS A CLOCK, NOT A TRIGGER-HELD TIMER, and the difference only
// shows on the frame after a pause. The tick runs the decrement BEFORE it
// looks at the trigger; move it after and a gun that fired, then rested with
// the trigger up, comes back still owing the whole cycle - so the first shot
// of every burst after the first is late by one cooldown, on every gun and
// every NPC in the game.
SOL_TEST(a_cooldown_runs_down_while_the_trigger_is_up)
{
    Fixture fixture(twoGuns());
    SOL_REQUIRE(fixture.fireOnce().size() == 2);
    SOL_REQUIRE(fixture.world.playerGuns()[0].cooldown > 0.0f);

    // Trigger up, and a full second of it - longer than either gun's cycle.
    fixture.world.setShipInput(sol::sim::FlightInput{});
    for (int i = 0; i < 60; ++i) {
        fixture.world.tick(1.0 / 60.0);
    }
    for (const game::ShipWeapon& gun : fixture.world.playerGuns()) {
        SOL_CHECK(gun.cooldown <= 0.0f);
    }
    // And the proof that a run-down cooldown is a gun that fires: both go off
    // on the very next frame the trigger is down.
    SOL_CHECK(fixture.fireOnce().size() == 2);
}

// ⚑ WHERE A SHOT COMES FROM. Two claims in one test because either alone is
// satisfied by the wrong implementation: that the two bolts appear in DIFFERENT
// places (the old code put every shot on one invented point on the boresight),
// and that they are the hull's scale apart rather than the mount's raw
// separation (six metres of authored offset on a `scale = 2.0` hull is twelve
// metres of muzzle separation).
SOL_TEST(each_gun_fires_from_its_own_mount_scaled_by_the_hull)
{
    Fixture fixture(twoGuns());
    const sol::sim::ShipState ship = fixture.world.shipState();
    const std::vector<sol::core::DVec3> bolts = fixture.fireOnce();
    SOL_REQUIRE(bolts.size() == 2);

    // The world starts the player at identity orientation, so the hull frame
    // and the sim frame agree and the expected muzzles are arithmetic.
    const sol::core::DVec3 port = ship.position + sol::core::DVec3{-6.0, 0.0, -10.0};
    const sol::core::DVec3 starboard = ship.position + sol::core::DVec3{6.0, 0.0, -10.0};
    const double portError = std::min(distance(bolts[0], port), distance(bolts[1], port));
    const double starboardError = std::min(distance(bolts[0], starboard), distance(bolts[1], starboard));
    if (portError > 1.0 || starboardError > 1.0) {
        std::printf("  muzzles off by %.3f m and %.3f m\n", portError, starboardError);
    }
    SOL_CHECK(portError < 1.0);
    SOL_CHECK(starboardError < 1.0);
    // And they really are two places: a run that put both on one point would
    // pass the two checks above by matching each expectation with the same bolt.
    SOL_CHECK(distance(bolts[0], bolts[1]) > 10.0);
}

// ⚑ PER-MOUNT CAPACITOR DRAW, WHICH IS ONLY OBSERVABLE WHEN THE CHARGE RUNS
// OUT. With a capacitor that can pay for one shot of the two, the gun the
// AUTHOR LISTED FIRST fires and the second holds — and the second's cooldown
// stays at zero, which is the discriminating half: a gun that had been charged
// the cost and then refused a shot would have spent a cycle it never fired.
SOL_TEST(mount_order_is_firing_priority_when_the_capacitor_runs_short)
{
    // 15 units of capacitor against two 10-unit guns, and no recharge at all,
    // so the second gun cannot be paid for later in the same tick either.
    Fixture fixture(
        hull(gunMount("gun_port", -3.0, "sol.left_gun") + gunMount("gun_starboard", 3.0, "sol.right_gun"),
             "15.0",
             "0.0"));
    SOL_REQUIRE(fixture.world.playerGuns().size() == 2);

    const std::vector<sol::core::DVec3> bolts = fixture.fireOnce();
    SOL_CHECK(bolts.size() == 1);
    const std::span<const game::ShipWeapon> guns = fixture.world.playerGuns();
    SOL_REQUIRE(guns.size() == 2);
    SOL_CHECK(guns[0].cooldown > 0.0f);  // fired, and is paying for it
    SOL_CHECK(guns[1].cooldown == 0.0f); // held, and owes nothing
    // The charge left is what the first gun did not spend, not half of it
    // split between two guns that both went hungry.
    SOL_CHECK(fixture.world.playerPower().weaponCharge == 5.0f);
}

// ⚑ THE SUMMARY'S FIELDS ARE ABOUT DIFFERENT GUNS ON PURPOSE, and this is the
// case that proves the summary does not conflate them: a hull carrying a long
// cannon and a short mining beam can shoot at 2500 m and cut at 800 m. A
// `miningRange` taken as a max over all guns — the obvious wrong version —
// would say the rock 2 km away is cuttable.
SOL_TEST(the_mining_reach_is_the_beams_own_and_not_the_longest_guns)
{
    Fixture fixture(
        hull(gunMount("gun_port", -3.0, "sol.right_gun") + gunMount("gun_starboard", 3.0, "sol.beam")));
    const game::ArmamentSummary summary = fixture.world.playerArmament();
    SOL_CHECK(summary.armed);
    SOL_CHECK(summary.maxRange == 2500.0f); // the cannon's
    SOL_CHECK(summary.canMine);
    SOL_CHECK(summary.miningRange == 800.0f); // the beam's
    // The lead marker follows the first PROJECTILE gun; a hitscan beam has no
    // flight time and contributes none.
    SOL_CHECK(summary.leadSpeed == 800.0f);
}

// The other half of the lead rule: an all-hitscan fit shows no lead at all,
// which is the meaning a zero `projectile_speed` had when there was one gun.
SOL_TEST(an_all_hitscan_fit_offers_no_lead_solution)
{
    Fixture fixture(
        hull(gunMount("gun_port", -3.0, "sol.beam") + gunMount("gun_starboard", 3.0, "sol.beam")));
    const game::ArmamentSummary summary = fixture.world.playerArmament();
    SOL_CHECK(summary.armed);
    SOL_CHECK(summary.leadSpeed == 0.0f);
    SOL_CHECK(summary.canMine);
}

// An unarmed hull is a real state — the player can strip the guns off — and
// every field of the summary has to survive it, because the pilot brain reads
// `maxRange` before it reads `armed`.
SOL_TEST(a_hull_with_no_guns_summarises_to_nothing_rather_than_to_defaults)
{
    Fixture fixture(hull(gunMount("gun_port", -3.0, nullptr)));
    SOL_CHECK(fixture.world.playerGuns().empty());
    const game::ArmamentSummary summary = fixture.world.playerArmament();
    SOL_CHECK(!summary.armed);
    SOL_CHECK(summary.maxRange == 0.0f);
    SOL_CHECK(!summary.canMine);
    SOL_CHECK(fixture.fireOnce().empty());
}

// ⚑ THE CEILING, MEASURED RATHER THAN ASSUMED. `kMaxShipWeapons` is a fact
// about the save format (the ECS snapshot stores components by memcpy), so a
// hull that overruns it must fit what it can and drop the rest rather than
// write past the array. Seventeen mounts, sixteen guns, and the sixteenth is
// still the one the author listed sixteenth.
SOL_TEST(a_hull_with_more_gun_mounts_than_the_ceiling_fits_the_first_sixteen)
{
    std::string mounts;
    for (std::uint32_t i = 0; i < game::kMaxShipWeapons + 1; ++i) {
        const std::string id = "gun_" + std::to_string(i);
        // The seventeenth is the BEAM, so "which one fell off" is answerable
        // by kind rather than by counting.
        mounts += gunMount(
            id.c_str(), static_cast<double>(i), i == game::kMaxShipWeapons ? "sol.beam" : "sol.left_gun");
    }
    Fixture fixture(hull(mounts));
    const std::span<const game::ShipWeapon> guns = fixture.world.playerGuns();
    SOL_REQUIRE(guns.size() == game::kMaxShipWeapons);
    // The one that fell off the end is the LAST authored, not the first: the
    // hull keeps the guns its author put at the top of the list.
    for (const game::ShipWeapon& gun : guns) {
        SOL_CHECK(gun.kind == game::WeaponKind::Projectile);
    }
}

// ⚑ THE REGRESSION THAT ONLY THIS LAYER CAN SEE, and the reason stage C1 moved
// the shipped weapon mounts. `at` became a muzzle in C1; the seat is at
// `kCockpitOffset` and does NOT scale with the hull. Stage A2 authored the
// shuttle's nose gun at z = -2.4, which is two and a half metres BEHIND the
// pilot's eye — so every bolt would have spawned out of view behind the
// player's head, on the ship they start the game in.
//
// The rule asserted is the general one rather than the instance: a
// FORWARD-firing gun mount must sit ahead of the seat on the hull it is
// authored for. A turret is exempt because a dorsal ring firing over the
// canopy is exactly what a turret is for, which is why the check is on `fixed`
// mounts alone.
SOL_TEST(a_shipped_fixed_gun_mount_sits_ahead_of_the_pilots_seat)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    SOL_REQUIRE(!defs.ships().empty());

    std::uint32_t checked = 0;
    for (const sol::assets::ShipDef& def : defs.ships()) {
        for (const sol::assets::ShipMount& mount : def.mounts) {
            if (mount.kind != sol::assets::MountKind::Fixed) {
                continue;
            }
            ++checked;
            // -Z is forward, so "ahead of the seat" is a SMALLER z. The seat
            // is a constant in ship space; the mount is authored at scale 1.
            const float muzzleZ = mount.at[2] * def.scale;
            if (muzzleZ >= game::ShipCamera::kCockpitOffset.z) {
                std::printf("  %s: fixed mount '%s' fires from z %.2f, behind the seat at %.2f\n",
                            def.id.c_str(),
                            mount.id.c_str(),
                            static_cast<double>(muzzleZ),
                            static_cast<double>(game::ShipCamera::kCockpitOffset.z));
            }
            SOL_CHECK(muzzleZ < game::ShipCamera::kCockpitOffset.z);
        }
    }
    // Two shipped hulls carry a fixed gun; a parse that produced no mounts
    // would satisfy the loop above by never entering it.
    SOL_CHECK(checked == 2);
}

// --- Traverse (Phase 31 stage C2) -------------------------------------------
//
// Most tests below fly `boltedAndRing()`: a bolted nose gun to port and a
// dorsal ring to starboard, with a hulk a kilometre directly astern. The port
// gun therefore fires FORWARD in every one of them, and the whole subject is
// what the starboard one does.

namespace {

// ⚑ WHICH WAY A BOLT WENT, READ OFF THE BOLT. A shot's position is its
// MUZZLE - the projectile pass has already run by the time the weapon pass
// spawns it - so where a bolt appears says where the gun is, not where it is
// pointing. Its drawn rotation is the bearing: the mesh is long on +Z and
// `facingRotation` puts that axis on the line of flight.
//
// The two are only the same claim because stage C2 made them so, which is why
// `a_bolt_is_drawn_along_the_way_it_was_fired` below checks the drawn axis
// against a second tick of actual travel rather than trusting this.
[[nodiscard]] sol::core::DVec3 bearingOf(const game::RenderInstance& bolt)
{
    const sol::core::Vec3 axis = rotate(bolt.rotation, sol::core::Vec3{0.0f, 0.0f, 1.0f});
    return {static_cast<double>(axis.x), static_cast<double>(axis.y), static_cast<double>(axis.z)};
}

[[nodiscard]] bool firedForward(const game::RenderInstance& bolt)
{
    return bearingOf(bolt).z < -0.9;
}

[[nodiscard]] bool firedAstern(const game::RenderInstance& bolt)
{
    return bearingOf(bolt).z > 0.9;
}

} // namespace

// ⚑ THE STAGE IN ONE TEST. Both guns fire on the same held trigger and they
// fire in OPPOSITE DIRECTIONS: the bolted gun down the nose because that is
// where it is bolted, the ring aft because that is where the target is.
//
// The counterfactual that turns it red is the pre-C2 firing pass - every gun
// down the hull's boresight - and so is its opposite, a pass that lets a gun
// with no ring seek the target too: `arc = 0` refuses any bearing but its own,
// so the nose gun would hold its fire and only one bolt would leave.
SOL_TEST(a_ring_lays_itself_on_the_target_and_a_bolted_gun_does_not)
{
    Fixture fixture(boltedAndRing());
    SOL_REQUIRE(fixture.hulkAstern(1000.0));
    SOL_REQUIRE(fixture.selectHulk());

    const std::vector<game::RenderInstance> bolts = fixture.fireOnceDrawn();
    SOL_REQUIRE(bolts.size() == 2);

    int forward = 0;
    int astern = 0;
    for (const game::RenderInstance& bolt : bolts) {
        forward += firedForward(bolt) ? 1 : 0;
        astern += firedAstern(bolt) ? 1 : 0;
    }
    if (forward != 1 || astern != 1) {
        std::printf("  bearings: %.3f and %.3f on z\n", bearingOf(bolts[0]).z, bearingOf(bolts[1]).z);
    }
    SOL_CHECK(forward == 1);
    SOL_CHECK(astern == 1);
}

// ⚑ AND WITH NOTHING SELECTED THE RING FOLLOWS THE PILOT. The hulk is sitting
// in exactly the same place; the only difference is that nobody laid the gun
// on it. Both guns fire forward, which is what every gun in the game did
// before this stage and what a trigger held in empty space still has to do.
SOL_TEST(a_ring_with_no_target_follows_the_pilots_nose)
{
    Fixture fixture(boltedAndRing());
    SOL_REQUIRE(fixture.hulkAstern(1000.0));
    // The half of the setup that makes this test a claim rather than an
    // accident: the hulk is there, and it is not what is selected.
    SOL_REQUIRE(!fixture.world.currentTargetInfo().isShip);

    const std::vector<game::RenderInstance> bolts = fixture.fireOnceDrawn();
    SOL_REQUIRE(bolts.size() == 2);
    SOL_CHECK(firedForward(bolts[0]));
    SOL_CHECK(firedForward(bolts[1]));
}

// ⚑⚑ A RING OPENS ONLY ON SOMEONE THE PLAYER IS ALREADY AT WAR WITH, which is
// a RULED decision rather than the obvious one. Laying on the bare selection
// was simpler and made a trap the game had never had: hail a patrol, forget to
// change the selection, hold the trigger to cut a rock, and a dorsal ring puts
// a bolt into the police while your nose is on the asteroid.
//
// The hulk here is spawned with NO pilot, which is what makes it tier 2 -
// nobody is flying it, so it threatens nothing. Everything else about this
// test is `a_ring_lays_itself_on_the_target_and_a_bolted_gun_does_not`: same
// hull, same ring, same 1 km astern, same selection. The ring looks down the
// nose instead, and both bolts go forward.
SOL_TEST(a_ring_ignores_a_selection_that_is_not_hostile)
{
    Fixture fixture(boltedAndRing());
    SOL_REQUIRE(fixture.hulkAstern(1000.0, /*hostile=*/false));
    SOL_REQUIRE(fixture.selectHulk());
    SOL_REQUIRE(fixture.world.currentTargetInfo().isShip);

    const std::vector<game::RenderInstance> bolts = fixture.fireOnceDrawn();
    SOL_REQUIRE(bolts.size() == 2);
    SOL_CHECK(firedForward(bolts[0]));
    SOL_CHECK(firedForward(bolts[1]));
}

// ⚑ A GUN THAT CANNOT BEAR HOLDS, AND HOLDS FOR FREE. The ring here aims
// forward with a 90-degree arc, so it reaches 45 degrees either side of the
// nose and the hulk astern is well outside it. What makes this more than a
// shot count is the second half: the held gun's cooldown is still zero and the
// capacitor is down by exactly ONE gun's cost. A pass that fired into its own
// stop, or that charged for a shot it then refused, is red here and green on
// the bolt count alone.
SOL_TEST(a_ring_that_cannot_reach_the_target_holds_its_fire_and_pays_nothing)
{
    Fixture fixture(hull(gunMount("gun_port", -3.0, "sol.left_gun") +
                         turretMount("turret_nose", 3.0, "[0.0, 0.0, -1.0]", 90.0, "sol.left_gun")));
    SOL_REQUIRE(fixture.hulkAstern(1000.0));
    SOL_REQUIRE(fixture.selectHulk());

    const float charged = fixture.world.playerPower().weaponCharge;
    const std::vector<game::RenderInstance> bolts = fixture.fireOnceDrawn();
    SOL_REQUIRE(bolts.size() == 1);
    SOL_CHECK(firedForward(bolts[0]));

    const std::span<const game::ShipWeapon> guns = fixture.world.playerGuns();
    SOL_REQUIRE(guns.size() == 2);
    SOL_CHECK(guns[0].cooldown > 0.0f);  // the bolted gun fired
    SOL_CHECK(guns[1].cooldown == 0.0f); // the ring never came up
    SOL_CHECK(charged - fixture.world.playerPower().weaponCharge == 10.0f);
}

// ⚑ A TARGET IT COULD NOT HIT ANYWAY IS NOT A TARGET. Two rings, same arc,
// same hull, and the only difference is reach: the 2000 m gun lays itself on
// the hulk 1000 m astern and the 500 m gun keeps looking down the nose.
//
// This is the rule that keeps the shipped freighter minable. Its dorsal ring
// carries the mining laser, and without the reach gate a fighter selected
// three kilometres away would swing the beam off the rock in front of it.
SOL_TEST(a_ring_ignores_a_target_beyond_its_own_reach)
{
    Fixture fixture(hull(turretMount("turret_long", -3.0, "[0.0, 1.0, 0.0]", 270.0, "sol.left_gun") +
                         turretMount("turret_short", 3.0, "[0.0, 1.0, 0.0]", 270.0, "sol.short_gun")));
    SOL_REQUIRE(fixture.hulkAstern(1000.0));
    SOL_REQUIRE(fixture.selectHulk());

    const std::vector<game::RenderInstance> bolts = fixture.fireOnceDrawn();
    SOL_REQUIRE(bolts.size() == 2);
    int forward = 0;
    int astern = 0;
    for (const game::RenderInstance& bolt : bolts) {
        forward += firedForward(bolt) ? 1 : 0;
        astern += firedAstern(bolt) ? 1 : 0;
    }
    SOL_CHECK(forward == 1); // the short gun, still looking where the pilot is
    SOL_CHECK(astern == 1);  // the long one, laid on the hulk
}

// ⚑ A BOLT IS DRAWN THE WAY IT WAS FIRED, and this is the one test that does
// not take the drawn axis on trust: it fires, steps one more tick with the
// trigger up, and compares where each bolt actually TRAVELLED against the
// direction it is drawn along.
//
// A bolt is a long thin box, so while every gun shot down the boresight the
// hull's own orientation was indistinguishable from the right answer - the
// first shot to leave a ring at an angle is the first one that could be drawn
// sideways to its own flight.
SOL_TEST(a_bolt_is_drawn_along_the_way_it_was_fired)
{
    Fixture fixture(boltedAndRing());
    SOL_REQUIRE(fixture.hulkAstern(1000.0));
    SOL_REQUIRE(fixture.selectHulk());

    const std::vector<game::RenderInstance> bolts = fixture.fireOnceDrawn();
    SOL_REQUIRE(bolts.size() == 2);

    fixture.world.setShipInput(sol::sim::FlightInput{});
    fixture.world.tick(1.0 / 60.0);
    std::vector<game::RenderInstance> after;
    fixture.world.buildRenderInstances(1.0f, true, after);

    std::uint32_t matched = 0;
    for (const game::RenderInstance& bolt : bolts) {
        for (const game::RenderInstance& moved : after) {
            if (moved.key != bolt.key) {
                continue;
            }
            ++matched;
            const sol::core::DVec3 travelled = moved.position - bolt.position;
            SOL_CHECK(length(travelled) > 1.0); // it really went somewhere
            SOL_CHECK(dot(normalize(travelled), bearingOf(bolt)) > 0.999);
        }
    }
    SOL_CHECK(matched == 2);
}

// ⚑ THE LEAD MARKER FOLLOWS A GUN THE PILOT HAS TO AIM. The ring is listed
// FIRST and is the faster of the two, so a summary that still took "the first
// projectile gun" - stage C1's rule, and the one this narrows - would read 800
// here. The marker's whole job is to say where to point the nose, and a ring
// does not care where the nose points.
SOL_TEST(the_lead_marker_skips_a_gun_that_lays_itself)
{
    Fixture fixture(hull(turretMount("turret_dorsal", -3.0, "[0.0, 1.0, 0.0]", 270.0, "sol.right_gun") +
                         gunMount("gun_nose", 3.0, "sol.left_gun")));
    const game::ArmamentSummary summary = fixture.world.playerArmament();
    SOL_CHECK(summary.armed);
    SOL_CHECK(summary.maxRange == 2500.0f); // still the longest gun, ring or not
    SOL_CHECK(summary.leadSpeed == 600.0f); // the bolted gun's, not the ring's 800

    // And a hull whose every gun lays itself offers no marker at all, which is
    // the honest answer rather than a gap: there is nothing the pilot could do
    // with one.
    Fixture rings(hull(turretMount("turret_dorsal", -3.0, "[0.0, 1.0, 0.0]", 270.0, "sol.right_gun") +
                       turretMount("turret_ventral", 3.0, "[0.0, -1.0, 0.0]", 270.0, "sol.left_gun")));
    SOL_CHECK(rings.world.playerArmament().armed);
    SOL_CHECK(rings.world.playerArmament().leadSpeed == 0.0f);
}

// ⚑ THE RING LEADS, AND IT LEADS WITH ITS OWN SPEED. Driven through `layGun`
// directly rather than through a world, because a moving target is one line
// here and a whole flight rig there.
//
// A bolt gun is laid AHEAD of a crossing target and a beam is laid straight at
// it - the same split `computeInterceptDirection` has always made for the
// pilot brain, made per gun rather than per ship. A ring taking the summary's
// `leadSpeed` would hand the beam the bolt gun's lead and miss with something
// that cannot miss.
SOL_TEST(a_ring_leads_a_crossing_target_with_its_own_projectile_speed)
{
    game::GunneryFrame frame;
    frame.forward = {0.0, 0.0, -1.0};
    frame.hasTarget = true;
    frame.targetPosition = {0.0, 0.0, -1000.0};
    frame.targetVelocity = {200.0, 0.0, 0.0};

    game::ShipWeapon ring;
    ring.kind = game::WeaponKind::Projectile;
    ring.projectileSpeed = 600.0f;
    ring.range = 2000.0f;
    ring.arc = 360.0f;

    sol::core::DVec3 muzzle;
    sol::core::DVec3 bearing;
    SOL_CHECK(game::layGun(frame, ring, muzzle, bearing));
    SOL_CHECK(bearing.x > 0.1); // out into the target's path

    game::ShipWeapon beam = ring;
    beam.kind = game::WeaponKind::Hitscan;
    beam.projectileSpeed = 0.0f;
    sol::core::DVec3 beamBearing;
    SOL_CHECK(game::layGun(frame, beam, muzzle, beamBearing));
    SOL_CHECK(std::abs(beamBearing.x) < 1e-6); // straight at it: nothing to lead
}

// ⚑ A MOUNT'S `aim` IS IN THE HULL FRAME, AND THE GAP THIS CLOSES IS THAT
// EVERY OTHER TEST IN THIS FILE FLIES AT IDENTITY. The world starts the player
// unrotated and the warp that places the hulk faces the nose the way it
// already pointed, so a firing pass that treated an authored `aim` as a WORLD
// direction - never rotating it at all - would be green everywhere above.
//
// The hull here is yawed 90 degrees, which puts its own starboard (+X) along
// world -Z and its own nose along world -X. A gun bolted to starboard
// therefore shoots along world -Z, and its muzzle five metres up the hull's
// nose sits five metres along world -X.
SOL_TEST(a_guns_rest_direction_and_muzzle_turn_with_the_hull)
{
    game::GunneryFrame frame;
    frame.orientation = sol::core::fromAxisAngle({0.0f, 1.0f, 0.0f}, sol::core::kHalfPi);
    frame.forward = toDVec3(rotate(frame.orientation, sol::core::Vec3{0.0f, 0.0f, -1.0f}));

    game::ShipWeapon gun;
    gun.kind = game::WeaponKind::Projectile;
    gun.aim[0] = 1.0f; // bolted to starboard
    gun.aim[1] = 0.0f;
    gun.aim[2] = 0.0f;
    gun.at[2] = -5.0f; // five metres up the nose

    sol::core::DVec3 muzzle;
    sol::core::DVec3 bearing;
    SOL_CHECK(game::layGun(frame, gun, muzzle, bearing));
    SOL_CHECK(length(bearing - sol::core::DVec3{0.0, 0.0, -1.0}) < 1e-6);
    SOL_CHECK(length(muzzle - sol::core::DVec3{-5.0, 0.0, 0.0}) < 1e-5);
    // And the nose really did move, so the check above is about the ring
    // rather than about an orientation that happened to be identity.
    SOL_CHECK(frame.forward.x < -0.99);
}

// ⚑ THE FREIGHTER'S TWO RINGS LEAVE NO BLIND BEARING, asserted against the
// SHIPPED file rather than a fixture. That pair is the reason `turret_ventral`
// was authored in stage C1 - a dorsal ring alone is blind through its own hull
// - and it is a claim about the HULL rather than about how it happens to be
// fitted: the ventral mount ships bare, and what this says is that a player
// who arms it can shoot in any direction at all.
SOL_TEST(the_shipped_freighter_can_be_armed_to_cover_every_bearing)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));
    const sol::assets::ShipDef* freighter = defs.findShip("sol.freighter");
    SOL_REQUIRE(freighter != nullptr);

    // A spiral of directions over the whole sphere - dense enough that a gap
    // the size of the one a single 270 ring leaves cannot slip between them.
    constexpr int kSamples = 2000;
    int covered = 0;
    for (int i = 0; i < kSamples; ++i) {
        const double z = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / kSamples;
        const double radius = std::sqrt(z * z < 1.0 ? 1.0 - z * z : 0.0);
        const double theta = 2.399963229728653 * static_cast<double>(i); // golden angle
        const sol::core::DVec3 direction{radius * std::cos(theta), radius * std::sin(theta), z};

        bool reached = false;
        for (const sol::assets::ShipMount& mount : freighter->mounts) {
            if (!sol::assets::mountTakesWeapon(mount.kind)) {
                continue;
            }
            sol::core::DVec3 bearing;
            if (sol::sim::layWithinArc({mount.aim[0], mount.aim[1], mount.aim[2]},
                                       direction,
                                       static_cast<double>(mount.arc),
                                       bearing)) {
                reached = true;
                break;
            }
        }
        covered += reached ? 1 : 0;
    }
    if (covered != kSamples) {
        std::printf("  %d of %d bearings are blind\n", kSamples - covered, kSamples);
    }
    SOL_CHECK(covered == kSamples);
}

// ⚑ A HIT ARRIVES FROM WHERE THE SHOT CAME FROM, NOT FROM WHERE THE SHOOTER'S
// NOSE HAPPENS TO POINT. `facingForHit` decides which of the target's shields
// eats a beam, and while every gun fired down the boresight those two were the
// same vector - so this is a claim only a ring can be wrong about.
//
// The geometry: the hulk is spawned facing the way the player was, then the
// player warps a kilometre PAST it, so the two ships are nose to tail and the
// player is squarely in front of the hulk. A dorsal ring firing astern
// therefore lands on the hulk's FORE shield. Take the facing off the shooter's
// nose instead - the pre-C2 line - and it lands on the aft one.
SOL_TEST(a_beam_lands_on_the_shield_facing_the_beam)
{
    Fixture fixture(hull(turretMount("turret_dorsal", 0.0, "[0.0, 1.0, 0.0]", 270.0, "sol.pulse_beam")));
    SOL_REQUIRE(fixture.hulkAstern(500.0));
    SOL_REQUIRE(fixture.selectHulk());
    SOL_REQUIRE(fixture.world.currentTargetInfo().isShip);
    SOL_REQUIRE(fixture.world.currentTargetInfo().shieldFore == 1.0f);

    (void)fixture.fireOnceDrawn(); // a beam leaves no bolt; the damage is the record
    const game::TargetInfo hit = fixture.world.currentTargetInfo();
    if (hit.shieldFore >= 1.0f || hit.shieldAft < 1.0f) {
        std::printf("  hulk shields: fore %.3f aft %.3f\n",
                    static_cast<double>(hit.shieldFore),
                    static_cast<double>(hit.shieldAft));
    }
    SOL_CHECK(hit.shieldFore < 1.0f); // the side the shot came from
    SOL_CHECK(hit.shieldAft == 1.0f); // and not the side the shooter's nose is on
}

// A gun carries its mount's facing and traverse for the same reason it carries
// its mount's position: the component is flattened and keeps no def id, so
// nothing downstream can go back and ask.
SOL_TEST(a_gun_carries_the_facing_and_traverse_of_the_mount_it_sits_in)
{
    Fixture fixture(boltedAndRing());
    const std::span<const game::ShipWeapon> guns = fixture.world.playerGuns();
    SOL_REQUIRE(guns.size() == 2);
    SOL_CHECK(guns[0].arc == 0.0f); // the bolted gun, which authored neither key
    SOL_CHECK(guns[0].aim[2] == -1.0f);
    SOL_CHECK(guns[1].arc == 270.0f);
    SOL_CHECK(guns[1].aim[1] == 1.0f);
}

// --- Fire groups (Phase 31 stage C3) ----------------------------------------
//
// ⚑ THE ONE FACT EVERY TEST BELOW LEANS ON: the port gun sits at x = -3 and
// the starboard gun at x = +3 on a `scale = 2.0` hull, so a single bolt's sign
// says WHICH gun fired it. Counting bolts alone would not - "one bolt" is what
// both "only the port gun fired" and "only the starboard gun fired" look like.

// The default, and the reason nothing else in this suite had to change: every
// gun on every hull comes out of `applyShipDef` in group 1 and the trigger is
// on group 1, so a ship nobody has regrouped fires exactly as it did before
// fire groups existed.
SOL_TEST(every_gun_starts_in_group_one_with_the_trigger_on_it)
{
    Fixture fixture(twoGuns());
    const std::span<const game::ShipWeapon> guns = fixture.world.playerGuns();
    SOL_REQUIRE(guns.size() == 2);
    SOL_CHECK(guns[0].group == 1);
    SOL_CHECK(guns[1].group == 1);
    SOL_CHECK(fixture.world.playerFireGroup() == 1);
    SOL_CHECK(fixture.world.playerFireGroupsInUse() == 0x1u);
    SOL_CHECK(fixture.fireOnce().size() == 2);
}

// ⚑⚑ THE PHASE'S OWN EXIT CRITERION: two different guns on two mounts of one
// hull, fired INDEPENDENTLY. Both halves are checked by the x sign, because a
// bolt count cannot tell "the port gun fired" from "the starboard gun fired".
SOL_TEST(two_guns_in_two_groups_fire_one_at_a_time)
{
    Fixture fixture(twoGuns());
    SOL_REQUIRE(fixture.world.setFireGroup("gun_starboard", 2, nullptr));
    SOL_CHECK(fixture.world.playerFireGroupsInUse() == 0x3u);

    const sol::core::DVec3 shipAt = fixture.world.shipState().position;
    const std::vector<sol::core::DVec3> first = fixture.fireOnce();
    SOL_REQUIRE(first.size() == 1);
    SOL_CHECK(first[0].x < shipAt.x); // the port gun, alone

    SOL_CHECK(fixture.world.cycleFireGroup() == 2);
    const std::vector<sol::core::DVec3> second = fixture.fireOnce();
    SOL_REQUIRE(second.size() == 1);
    SOL_CHECK(second[0].x > shipAt.x); // and now the starboard one, alone
}

// ⚑ THE COUNTERFACTUAL: move the group check one line UP, above the cooldown
// decrement, and this turns red. A clock that only runs while its group is
// selected hands every group a free first shot on the frame you switch to it,
// so a hull with its guns split fires faster than the same hull with them
// together - a rate of fire no def names, bought by pressing one key.
SOL_TEST(a_gun_in_an_unselected_group_still_runs_its_cooldown)
{
    Fixture fixture(twoGuns());
    // Both on one trigger to start with, so BOTH guns owe a cooldown - the
    // starboard one has to have a clock to run before the test can ask
    // whether it runs.
    SOL_REQUIRE(fixture.fireOnce().size() == 2);
    SOL_REQUIRE(fixture.world.playerGuns()[1].cooldown > 0.0f);
    SOL_REQUIRE(fixture.world.setFireGroup("gun_starboard", 2, nullptr));

    // Trigger up for three quarters of a second, comfortably past the
    // starboard gun's 0.33 s cycle. Exactly its cycle would fail on the float
    // residue, which says nothing about the rule under test.
    fixture.world.setShipInput(sol::sim::FlightInput{});
    for (int i = 0; i < 45; ++i) {
        fixture.world.tick(1.0 / 60.0);
    }
    SOL_CHECK(fixture.world.playerGuns()[1].cooldown <= 0.0f);

    // And the consequence that makes it matter: switching to a group hands you
    // guns that are READY, not guns that start owing a cycle from the moment
    // you selected them.
    SOL_CHECK(fixture.world.cycleFireGroup() == 2);
    SOL_CHECK(fixture.fireOnce().size() == 1);
}

// A cycle that stepped through all four positions would spend three of them on
// a trigger wired to nothing, which is a key the player learns not to press.
SOL_TEST(the_cycle_visits_only_groups_that_have_a_gun_in_them)
{
    Fixture fixture(twoGuns());
    SOL_REQUIRE(fixture.world.setFireGroup("gun_starboard", 4, nullptr));
    SOL_CHECK(fixture.world.playerFireGroupsInUse() == 0x9u); // 1 and 4
    SOL_CHECK(fixture.world.cycleFireGroup() == 4);
    SOL_CHECK(fixture.world.cycleFireGroup() == 1);
}

// And the degenerate case that is every ship in the shipped game: one group,
// so the key correctly does nothing rather than walking three empty positions.
SOL_TEST(a_ship_with_one_group_has_a_cycle_of_length_one)
{
    Fixture fixture(twoGuns());
    SOL_CHECK(fixture.world.cycleFireGroup() == 1);
    SOL_CHECK(fixture.world.cycleFireGroup() == 1);
}

// ⚑ A SELECTION IS NEVER LEFT POINTING AT AN EMPTY GROUP. Moving the only gun
// in the selected group out of it is the ordinary way to reach that state -
// one click on the ship readout - and a trigger wired to nothing reads exactly
// like a broken gun. Delete the `normalizeFireGroup` call in `setFireGroup`
// and this fires zero bolts.
SOL_TEST(a_selection_follows_the_last_gun_out_of_a_group)
{
    Fixture fixture(hull(gunMount("gun_port", -3.0, "sol.left_gun")));
    SOL_REQUIRE(fixture.world.setFireGroup("gun_port", 3, nullptr));
    SOL_CHECK(fixture.world.playerFireGroup() == 3);
    SOL_CHECK(fixture.fireOnce().size() == 1);
}

// ⚑ THE SUMMARY DESCRIBES WHAT THE TRIGGER WILL DO, NOT WHAT IS BOLTED ON.
// Every field it carries is read to PREDICT a shot: drop the group test from
// `armamentSummary` and the HUD draws a lead marker for a bolt that is not
// coming and lights the mining prompt for a beam the trigger is not wired to.
//
// The beam is deliberately the one with `mining_power`, so both halves of the
// claim - reach, and whether a rock can be cut at all - move together.
SOL_TEST(the_armament_summary_describes_the_selected_group)
{
    Fixture fixture(
        hull(gunMount("gun_port", -3.0, "sol.right_gun") + gunMount("gun_starboard", 3.0, "sol.beam")));
    const game::ArmamentSummary both = fixture.world.playerArmament();
    SOL_CHECK(both.maxRange == 2500.0f); // the cannon's, and both are in group 1
    SOL_CHECK(both.canMine);

    SOL_REQUIRE(fixture.world.setFireGroup("gun_starboard", 2, nullptr));
    const game::ArmamentSummary cannon = fixture.world.playerArmament();
    SOL_CHECK(cannon.maxRange == 2500.0f);
    SOL_CHECK(cannon.leadSpeed == 800.0f);
    SOL_CHECK(!cannon.canMine); // the beam is not on this trigger

    SOL_CHECK(fixture.world.cycleFireGroup() == 2);
    const game::ArmamentSummary beam = fixture.world.playerArmament();
    SOL_CHECK(beam.maxRange == 800.0f);
    SOL_CHECK(beam.leadSpeed == 0.0f); // hitscan: instant
    SOL_CHECK(beam.canMine);
    SOL_CHECK(beam.miningRange == 800.0f);
}

// ⚑ A GROUP IS THE PILOT'S AND IT HAS TO SURVIVE THE HULL BEING RE-READ. A
// def hot-reload runs `applyShipDef`, which rebuilds every gun at group 1 from
// the mount list; without `applyPilotFireGroups` behind it, editing a TOML
// file - or buying a cargo pod, which takes the same path - silently puts
// every gun back on one trigger.
SOL_TEST(a_fire_group_survives_the_hull_being_reapplied)
{
    Fixture fixture(twoGuns());
    SOL_REQUIRE(fixture.world.setFireGroup("gun_starboard", 2, nullptr));
    fixture.world.applyDefs(fixture.defs);
    const std::span<const game::ShipWeapon> guns = fixture.world.playerGuns();
    SOL_REQUIRE(guns.size() == 2);
    SOL_CHECK(guns[0].group == 1);
    SOL_CHECK(guns[1].group == 2);
    SOL_CHECK(fixture.world.playerFireGroupsInUse() == 0x3u);
}

// ⚑⚑ AN NPC HAS NO CONSOLE, SO IT FIRES EVERYTHING IT CARRIES. This is why
// `applyPilotFireGroups` is called from `applyActiveLoadout` and not from
// `applyShipDef`: route it through the def instead - the tidier-looking
// version - and the player's own regrouping reaches every hull of that type
// in the system, whose pilots would then fly with half their guns switched
// off and no way to switch them back.
SOL_TEST(an_npc_fires_every_gun_whatever_the_player_has_regrouped)
{
    Fixture fixture(twoGuns());
    SOL_REQUIRE(fixture.world.setFireGroup("gun_starboard", 2, nullptr));
    SOL_CHECK(fixture.world.playerArmament().maxRange == 2000.0f); // the port gun alone

    const sol::assets::ShipDef* gunboat = fixture.defs.findShip("sol.shuttle");
    SOL_REQUIRE(gunboat != nullptr);
    const sol::ecs::Entity npc =
        fixture.world.spawnPilotFromDef(*gunboat, fixture.defs, game::PilotRole::Fighter);
    // The longer gun is the starboard one the PLAYER moved to group 2; an NPC
    // that had inherited the regroup would read 2000 here too.
    SOL_CHECK(fixture.world.armamentSummary(npc.index).maxRange == 2500.0f);
}

// Which mount a gun came out of is recorded on the gun, because that loop is
// the only walk that knows - and it is what lets one group change reach one
// gun without rebuilding the armament (which would refill the shields with it).
SOL_TEST(a_gun_remembers_which_mount_it_came_out_of)
{
    Fixture fixture(twoGuns());
    const std::span<const game::ShipWeapon> guns = fixture.world.playerGuns();
    SOL_REQUIRE(guns.size() == 2);
    SOL_CHECK(guns[0].mount == 0);
    SOL_CHECK(guns[1].mount == 1);

    std::string error;
    SOL_CHECK(!fixture.world.setFireGroup("no_such_mount", 2, &error));
    SOL_CHECK(!error.empty());
    SOL_CHECK(!fixture.world.setFireGroup("gun_port", 0, &error));
    SOL_CHECK(!fixture.world.setFireGroup("gun_port", game::kFireGroupCount + 1, &error));
}

// ===========================================================================
// Phase 31 stage E - what is bolted to a hull, drawn where it is bolted.
//
// ⚑ These read the fittings back through `buildRenderInstances`, which is the
// renderer's own entry point, so what they assert is what would be on screen:
// the hull's pose, its scale, the mount's `at` and the gunner's answer all
// composed the way the frame composes them. Asserting off `ShipArmament`
// instead would test that a field was copied.
// ===========================================================================

namespace {

// The model's own axes, once its drawn rotation is applied: -Z is the barrel
// and +Y is the top of the gun. Both are the contract `fittingRotation`
// promises and both are load-bearing - a gun is not symmetric about its
// barrel, so the roll is as much a claim as the bearing.
[[nodiscard]] sol::core::Vec3 barrelOf(const game::RenderInstance& fitting)
{
    return rotate(fitting.rotation, sol::core::Vec3{0.0f, 0.0f, -1.0f});
}

[[nodiscard]] sol::core::Vec3 topOf(const game::RenderInstance& fitting)
{
    return rotate(fitting.rotation, sol::core::Vec3{0.0f, 1.0f, 0.0f});
}

// One fitting hull: a bolted gun to port with a mesh and a second mount whose
// gun is whatever the caller wants to see fail to be drawn.
[[nodiscard]] std::string drawnAnd(const char* otherFit)
{
    return hull(gunMount("gun_port", -3.0, "sol.left_gun") + gunMount("gun_starboard", 3.0, otherFit));
}

} // namespace

// ⚑⚑ THE STAGE, IN ONE TEST. Two fitted mounts become two drawn fittings,
// each standing at its own mount's `at` - scaled by the hull, which is the
// half that a fitting drawn straight off the authored numbers would get wrong
// and only be wrong about by a factor. `scale = 2.0` is what makes that
// measurable and is why this fixture has carried it since C1.
SOL_TEST(a_fitted_gun_is_drawn_standing_in_its_mount)
{
    Fixture fixture(twoGuns());
    const std::vector<game::RenderInstance> fittings = fixture.fittingsDrawn();
    SOL_REQUIRE(fittings.size() == 2);

    const sol::core::DVec3 hull = fixture.world.shipState().position;
    const std::uint32_t body = fixture.defs.modelIndex("gun_body");
    SOL_REQUIRE(body != DefDatabase::kNoModel);

    // Authored at (-3, 0, -5) and (3, 0, -5) on a hull drawn at scale 2, and
    // this suite flies at identity orientation - so the offsets are the
    // authored numbers doubled, in world axes.
    const sol::core::DVec3 port = fittings[0].position - hull;
    const sol::core::DVec3 starboard = fittings[1].position - hull;
    SOL_CHECK(distance(port, {-6.0, 0.0, -10.0}) < 1e-4);
    SOL_CHECK(distance(starboard, {6.0, 0.0, -10.0}) < 1e-4);

    for (const game::RenderInstance& fitting : fittings) {
        SOL_CHECK(game::modelIndex(fitting.model) == body);
        // ⚑ A FITTING IS DRAWN AT THE HULL'S SCALE, not at the mount's `size`
        // and not at 1. Its mesh is authored at real size, exactly as `at` is,
        // so one turret reads as a light ring on a shuttle and a heavy one on
        // a freighter without a second asset or a size-to-metres table.
        SOL_CHECK(std::abs(fitting.scale.x - 2.0f) < 1e-5f);
        SOL_CHECK(std::abs(fitting.scale.y - 2.0f) < 1e-5f);
        SOL_CHECK(std::abs(fitting.scale.z - 2.0f) < 1e-5f);
    }
}

// ⚑⚑ AN EMPTY MODEL IS A BARE HARDPOINT, NOT A PLACEHOLDER BOX, and this is
// the rule that keeps the stage from changing somebody else's ship. Every
// weapon in this game named no mesh until stage E filled `weapons.toml` in,
// and a mod's gun written before it still names none - so a fallback here
// would put a grey crate on their hull, chosen by our default rather than by
// their file. The gun still fits and still fires; there is simply nothing to
// see.
SOL_TEST(a_gun_whose_weapon_names_no_mesh_leaves_the_hardpoint_bare)
{
    Fixture fixture(drawnAnd("sol.unseen_gun"));
    SOL_REQUIRE(fixture.world.playerGuns().size() == 2);

    const std::vector<game::RenderInstance> fittings = fixture.fittingsDrawn();
    SOL_CHECK(fittings.size() == 1);

    // And the one that IS drawn is the port gun, so this is about the model
    // and not about the second mount having quietly stopped existing.
    SOL_REQUIRE(fittings.size() == 1);
    SOL_CHECK((fittings[0].position - fixture.world.shipState().position).x < 0.0);
}

// ⚑⚑ AND THE OTHER FAILURE IS NOT THE SAME FAILURE. A name that does not
// resolve is an author's mistake rather than an author's choice, so it falls
// back to the `fitting` role and is visible - the same warn-and-draw-something
// treatment a ship def's broken model already gets. Silence would leave a
// typo in a weapon def indistinguishable from a deliberately unarted gun.
SOL_TEST(a_gun_whose_model_name_is_broken_falls_back_rather_than_vanishing)
{
    Fixture fixture(drawnAnd("sol.typo_gun"));

    const std::vector<game::RenderInstance> fittings = fixture.fittingsDrawn();
    SOL_REQUIRE(fittings.size() == 2);

    const std::uint32_t body = fixture.defs.modelIndex("gun_body");
    const std::uint32_t fallback = fixture.defs.modelIndex("fallback_body");
    SOL_REQUIRE(fallback != DefDatabase::kNoModel);
    SOL_CHECK(body != fallback); // the fixture's two answers are distinguishable
    SOL_CHECK(game::modelIndex(fittings[0].model) == body);
    SOL_CHECK(game::modelIndex(fittings[1].model) == fallback);
}

// ⚑⚑⚑ THE PAYOFF OF THE WHOLE STAGE: A RING IS DRAWN WHERE THE GUNNER IS
// LOOKING, AND A BOLTED GUN IS NOT. Same hull, same frame, same target - the
// nose gun stares down the hull's own axis while the dorsal ring has come
// round onto a hulk sitting astern. That difference has existed in the sim
// since C2 and has been invisible from outside the ship until now.
//
// ⚑ It is read WITHOUT firing. A turret is laid by a gunner whether or not the
// trigger is down - the firing pass merely happens to be the only thing that
// asked before this stage - so a ring that only came round while shooting
// would be a ring that snapped to its target the instant you opened fire.
SOL_TEST(a_drawn_ring_follows_the_gun_it_lays_and_a_bolted_one_does_not)
{
    Fixture fixture(boltedAndRing());
    SOL_REQUIRE(fixture.hulkAstern(1000.0));
    SOL_REQUIRE(fixture.selectHulk());

    const std::vector<game::RenderInstance> fittings = fixture.fittingsDrawn();
    SOL_REQUIRE(fittings.size() == 2);

    // `gun_port` is authored first, so it is first in mount order and first
    // out of the draw - the same order every other assertion in this file
    // relies on.
    SOL_CHECK(barrelOf(fittings[0]).z < -0.99f); // bolted: down the nose
    SOL_CHECK(barrelOf(fittings[1]).z > 0.99f);  // the ring: astern, onto the hulk
}

// ⚑⚑ A GUN THAT CANNOT BEAR IS STILL DRAWN, AT ITS STOP - which is the one
// picture a pilot needs when their shots are not going off. `layGun` sets the
// bearing on a refusal too, and this asserts the draw uses it rather than
// falling back to the rest direction or dropping the instance.
//
// A 30-degree ring aimed straight up reaches 15 degrees either side of up, and
// the hulk is dead astern: 90 degrees away, so the gun is hard against its
// stop and firing is refused.
SOL_TEST(a_ring_that_cannot_bear_is_drawn_straining_at_its_stop)
{
    Fixture fixture(boltedAndRing("sol.left_gun", 30.0));
    SOL_REQUIRE(fixture.hulkAstern(1000.0));
    SOL_REQUIRE(fixture.selectHulk());

    const std::vector<game::RenderInstance> fittings = fixture.fittingsDrawn();
    SOL_REQUIRE(fittings.size() == 2);
    const sol::core::Vec3 barrel = barrelOf(fittings[1]);

    // Not on the target - it cannot reach - and not at rest either: it has
    // come the fifteen degrees it has, toward the hulk rather than away.
    SOL_CHECK(barrel.z < 0.99f);
    SOL_CHECK(barrel.y > 0.9f);  // still essentially up, which is where the stop is
    SOL_CHECK(barrel.z > 0.01f); // and leaning astern, not forward

    // The stop really is 15 degrees off the aim, half of the full cone.
    const float offAim = std::acos(sol::core::clamp(barrel.y, -1.0f, 1.0f));
    SOL_CHECK(std::abs(offAim - sol::core::radians(15.0f)) < 1e-3f);
}

// ⚑⚑ A VENTRAL RING HANGS FROM THE HULL RATHER THAN STANDING ON ITS HEAD,
// and this is the whole reason `fittingRotation` takes two vectors instead of
// one. A shortest-arc rotation onto the bearing leaves the ROLL free; a gun is
// not symmetric about its own barrel, so free means arbitrary. The mount's
// `aim` is what settles it - which for an external mount points out of the
// plating, so the base plate ends up flat against the hull either way up.
//
// The shipped freighter is exactly this case: `turret_dorsal` aims +Y and
// `turret_ventral` aims -Y, and before this rule both would have been drawn
// the same way up.
SOL_TEST(a_ventral_ring_hangs_under_the_hull_and_a_dorsal_one_stands_on_it)
{
    Fixture fixture(hull(turretMount("ring_dorsal", -3.0, "[0.0, 1.0, 0.0]", 270.0, "sol.left_gun") +
                         turretMount("ring_ventral", 3.0, "[0.0, -1.0, 0.0]", 270.0, "sol.left_gun")));

    const std::vector<game::RenderInstance> fittings = fixture.fittingsDrawn();
    SOL_REQUIRE(fittings.size() == 2);

    // Nothing selected, so both rings follow the nose and the BEARINGS are
    // identical - which is precisely what makes this a test about the roll.
    SOL_CHECK(barrelOf(fittings[0]).z < -0.99f);
    SOL_CHECK(barrelOf(fittings[1]).z < -0.99f);

    SOL_CHECK(topOf(fittings[0]).y > 0.99f);  // dorsal: upright
    SOL_CHECK(topOf(fittings[1]).y < -0.99f); // ventral: inverted, base still on the hull
}

// ⚑⚑ IT IS BOLTED TO THE HULL, NOT PARKED BESIDE IT. Every world-level test
// in this suite flies at identity orientation, which is exactly the pose that
// cannot tell a hull-frame offset from a world-frame one - so this one turns
// the ship ninety degrees first. A fitting placed in world axes stays where it
// was; a fitting bolted to the hull swings round with it.
//
// ⚑ The BEARING is checked with it. `layGun` answers in world space off the
// sim pose, and the draw brings that answer back into the hull's frame before
// hanging it off the render pose - so a rotation composed the other way round
// would put the barrel through the hull here and nowhere else.
SOL_TEST(a_fitting_turns_with_the_hull_it_is_bolted_to)
{
    Fixture fixture(twoGuns());
    SOL_REQUIRE(fixture.faceAlong({1000.0, 0.0, 0.0})); // nose onto +X

    const std::vector<game::RenderInstance> fittings = fixture.fittingsDrawn();
    SOL_REQUIRE(fittings.size() == 2);
    const sol::core::DVec3 hull = fixture.world.shipState().position;

    // The port mount is (-3, 0, -5) at scale 2, so (-6, 0, -10) in the hull's
    // frame. Yawing the nose from -Z onto +X takes hull -Z to world +X and
    // hull +X to world +Z, so both guns end up 10 m along +X - ahead of the
    // ship, which is where a nose gun belongs whichever way the ship is
    // pointing - and the port/starboard split lands on world Z.
    SOL_CHECK(distance(fittings[0].position - hull, {10.0, 0.0, -6.0}) < 1e-3);
    SOL_CHECK(distance(fittings[1].position - hull, {10.0, 0.0, 6.0}) < 1e-3);

    // And both barrels went with it: bolted guns aiming down a nose that is
    // now on +X.
    SOL_CHECK(barrelOf(fittings[0]).x > 0.99f);
    SOL_CHECK(barrelOf(fittings[1]).x > 0.99f);
}

// ⚑⚑⚑ A FITTING IS DRAWN AT THE POSE ITS HULL IS DRAWN AT, WHICH IS NOT
// THE POSE THE GUNNER ANSWERED IN. The hull is nlerped between the last two
// ticks; `layGun` works off the sim transform, because that is the pose the
// gunnery question is about. Taking `layGun`'s muzzle as the instance position
// - which is right there, already computed, and correct at the tick boundary -
// would slide every fitting off its own ship by whatever the hull travelled
// that tick, and it would do it ONLY between ticks, which is the hardest kind
// of wrong to see and the easiest to blame on the mount.
//
// ⚑ It is measured against the HULL INSTANCE from the same list rather than
// against `shipState()`, and that is the whole method: `shipState` is the sim
// pose, so comparing to it would agree with the bug. Two instances drawn in
// one frame have to agree with EACH OTHER.
SOL_TEST(a_fitting_is_drawn_at_the_same_interpolated_pose_as_its_hull)
{
    Fixture fixture(twoGuns());

    // Get the ship moving, so the two ends of the tick are different places.
    sol::sim::FlightInput input;
    input.linear = {0.0f, 0.0f, -1.0f}; // full forward
    fixture.world.setShipInput(input);
    for (int i = 0; i < 30; ++i) {
        fixture.world.tick(1.0 / 60.0);
    }

    // The hull is the keyed instance beside the fittings; the fittings are the
    // keyless ones. One list, one alpha, two readings.
    struct Frame
    {
        bool found = false;
        sol::core::DVec3 hull;
        std::vector<sol::core::DVec3> fittings;
    };

    const auto sample = [&fixture](float alpha) -> Frame {
        std::vector<game::RenderInstance> all;
        fixture.world.buildRenderInstances(alpha, true, all);
        Frame frame;
        for (const game::RenderInstance& instance : all) {
            if (instance.key == game::kNoInstanceKey) {
                frame.fittings.push_back(instance.position);
            } else if (std::abs(instance.scale.x - 2.0f) < 1e-5f) {
                // The gunboat: nothing else in this system is drawn at scale 2.
                frame.hull = instance.position;
                frame.found = true;
            }
        }
        return frame;
    };

    const Frame start = sample(0.0f);
    const Frame end = sample(1.0f);
    SOL_REQUIRE(start.found && end.found);
    SOL_REQUIRE(start.fittings.size() == 2 && end.fittings.size() == 2);

    // The tick really did move the ship, so the two alphas are two places -
    // without this the whole test would agree with a fitting nailed to the sim
    // pose, which is exactly the bug it is here to catch.
    SOL_REQUIRE(length(end.hull - start.hull) > 0.01);

    // And at BOTH ends the guns sit exactly where the mounts put them,
    // measured from the hull as it is drawn at that same alpha.
    for (std::size_t i = 0; i < 2; ++i) {
        const sol::core::DVec3 expected{i == 0 ? -6.0 : 6.0, 0.0, -10.0};
        SOL_CHECK(distance(start.fittings[i] - start.hull, expected) < 1e-4);
        SOL_CHECK(distance(end.fittings[i] - end.hull, expected) < 1e-4);
    }
}

// ⚑ A fitting follows its hull in and out of the frame. The hull is hidden
// from the seat because the eye sits inside it; guns left behind would be two
// turrets flying in formation with nothing between them, which on the shipped
// freighter - rings above and below the canopy - is exactly what you would
// see.
SOL_TEST(a_fitting_is_hidden_from_the_seat_with_the_hull_it_is_bolted_to)
{
    Fixture fixture(twoGuns());
    SOL_CHECK(fixture.fittingsDrawn(true).size() == 2);
    SOL_CHECK(fixture.fittingsDrawn(false).empty());
}

// ⚑⚑ THE ROLL RULE ON ITS OWN, because the world-level tests above cannot
// reach its degenerate end: a gun laid straight out along its own mount's aim
// has no roll left to choose, and there is nothing wrong with such a gun. The
// shortest arc stands rather than a refusal or a NaN out of a normalize.
SOL_TEST(a_fitting_laid_along_its_own_aim_still_has_a_rotation)
{
    // Aim and bearing the same axis: the ring is pointing straight up out of
    // itself, which a 360 mount really can do.
    const sol::core::Quat straightUp = game::fittingRotation({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const sol::core::Vec3 barrel = rotate(straightUp, sol::core::Vec3{0.0f, 0.0f, -1.0f});
    SOL_CHECK(barrel.y > 0.99f);
    SOL_CHECK(std::abs(length(barrel) - 1.0f) < 1e-4f);

    // Directly opposed is the other end of the same case and is just as legal:
    // a 360 ring laid straight down through its own mounting.
    const sol::core::Quat straightDown = game::fittingRotation({0.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    SOL_CHECK(rotate(straightDown, sol::core::Vec3{0.0f, 0.0f, -1.0f}).y < -0.99f);

    // And a zero bearing - which nothing produces, but which a mount whose
    // `aim` and `arc` conspired could - is identity rather than a NaN.
    SOL_CHECK(game::fittingRotation({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}) == sol::core::Quat::identity());
}

// ⚑⚑⚑ THE SHIPPED HULLS, ASSERTED AGAINST THE COMMITTED FILES. The tests
// above fly a fixture; this one flies what the player flies, and it is the
// only thing standing between the stage and a `weapons.toml` edit that leaves
// the game with nothing drawn on any hardpoint in the galaxy.
SOL_TEST(the_shipped_shuttle_draws_the_gun_it_starts_the_game_with)
{
    DefDatabase defs;
    std::string error;
    SOL_REQUIRE(defs.mergeDirectory(SOL_DEF_DATA_DIR, &error));

    game::SpaceWorld world;
    world.spawn(1701);
    world.applyDefs(defs);
    SOL_REQUIRE(world.generateUniverse(defs));

    std::vector<game::RenderInstance> all;
    world.buildRenderInstances(1.0f, true, all);
    std::vector<game::RenderInstance> fittings;
    for (const game::RenderInstance& instance : all) {
        if (instance.key == game::kNoInstanceKey) {
            fittings.push_back(instance);
        }
    }

    // ⚑⚑ NOT THE ONLY ONE IN THE FRAME, WHICH IS ITSELF THE POINT. The
    // starting system has NPC wings in it and every armed hull among them
    // draws its guns too - so the player's own is picked out by proximity
    // rather than by being the only thing on the list. A stage that drew only
    // the player's fittings would pass a count of one and be wrong about the
    // whole galaxy.
    // Eight of them at this seed and one of those is ours. The number is not
    // asserted - it is a fact about where the generator put a wing - but that
    // there is MORE THAN ONE is the claim, and it is the difference between a
    // stage that drew the player's guns and a stage that drew everybody's.
    SOL_CHECK(fittings.size() > 1);
    SOL_REQUIRE(!fittings.empty());
    const game::RenderInstance* ours = nullptr;
    std::uint32_t onOurHull = 0;
    for (const game::RenderInstance& fitting : fittings) {
        // The shuttle is an 8 m hull; nothing else is within twenty of it.
        if (length(fitting.position - world.shipState().position) < 20.0) {
            ours = &fitting;
            ++onOurHull;
        }
    }
    // One gun on the starter hull: `gun_nose`, holding a Pulse Cannon.
    SOL_REQUIRE(onOurHull == 1);
    SOL_REQUIRE(ours != nullptr);
    SOL_CHECK(game::modelIndex(ours->model) == defs.modelIndex("cannon"));

    // ⚑ AT THE TIP OF THE WEDGE, WHICH IS THE NUMBER STAGE C1 MOVED AND THE
    // ONE STAGE E MAKES VISIBLE. `gun_nose` is at (0, -0.1, -6.6) on a hull
    // drawn at scale 1, which is 1.6 m AHEAD of the seat rather than the
    // 2.5 m behind it that the stage-A2 position would have put it.
    const sol::core::DVec3 offset = ours->position - world.shipState().position;
    SOL_CHECK(distance(offset, {0.0, -0.1, -6.6}) < 1e-4);
}

// ⚑⚑⚑ A GUN'S MESH AND ITS SHOT'S MESH ARE TWO ANSWERS, AND UNTIL THIS
// TEST NOTHING IN THE PROJECT SAID SO. Phase 31 stage E split `WeaponDef`'s one
// `model` key into `model` (the gun on the hull) and `bolt_model` (the shot) -
// and the counterfactual that points the BOLT at the gun's key came back green
// against every suite. That is the shape of bug the split invites: it compiles,
// it resolves, it draws a turret where each bolt should be, and no assertion
// anywhere had an opinion.
//
// ⚑ The fixture's guns name a mesh and leave `bolt_model` unset, which is
// exactly what all four shipped weapons do - so what this pins is the FALLBACK
// path: an unset bolt override resolves to the `bolt` role, and never to the
// gun standing beside it.
SOL_TEST(a_bolt_is_drawn_as_its_own_model_and_not_as_its_gun)
{
    Fixture fixture(twoGuns());

    const std::vector<game::RenderInstance> bolts = fixture.fireOnceDrawn();
    SOL_REQUIRE(bolts.size() == 2);

    const std::uint32_t bolt = fixture.defs.modelIndex("bolt_body");
    const std::uint32_t gun = fixture.defs.modelIndex("gun_body");
    SOL_REQUIRE(bolt != DefDatabase::kNoModel);
    SOL_REQUIRE(gun != DefDatabase::kNoModel);
    SOL_CHECK(bolt != gun);

    for (const game::RenderInstance& shot : bolts) {
        SOL_CHECK(game::modelIndex(shot.model) == bolt);
        SOL_CHECK(game::modelIndex(shot.model) != gun);
    }
}
