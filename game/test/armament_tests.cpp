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

    // One tick with the trigger held, and the bolts it produced. A bolt is
    // identified by DIFFERENCE rather than by its model or its scale: whatever
    // is drawn after the tick that was not drawn before it came out of a
    // muzzle, which keeps the test from encoding what a bolt happens to look
    // like this month.
    std::vector<sol::core::DVec3> fireOnce(double dt = 1.0 / 60.0)
    {
        std::vector<game::RenderInstance> before;
        world.buildRenderInstances(1.0f, true, before);
        sol::sim::FlightInput input;
        input.trigger = true;
        world.setShipInput(input);
        world.tick(dt);
        std::vector<game::RenderInstance> after;
        world.buildRenderInstances(1.0f, true, after);

        std::vector<sol::core::DVec3> fresh;
        for (const game::RenderInstance& instance : after) {
            bool seen = false;
            for (const game::RenderInstance& old : before) {
                if (old.key == instance.key) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                fresh.push_back(instance.position);
            }
        }
        return fresh;
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
