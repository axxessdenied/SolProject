// A ship's MOUNTS as things that can be shot off (engine plan Phase 31 stage F,
// decisions/014's "each mount carries hit points", gdd.md §5's standing
// systems-damage promise).
//
// ⚑ Why this is not in `armament_tests`. That suite owns what a fit is FOR —
// how many guns a mount list becomes, where each shoots from, which of them the
// capacitor can pay for. This one owns what happens to the PLACES afterwards,
// and its subject is deliberately wider than guns: an empty mount and an
// internal subsystem bay are both under test here and neither is armament.
//
// ⚑⚑ EVERY CHECK BELOW RUNS THE REAL DAMAGE PATH. Nothing writes a condition
// field by hand — a shot is fired or a hull is rammed, `applyDamage` layers it,
// and `noteDamage` resolves it against a bearing. That is the point: the whole
// claim of the stage is that a hit arriving somewhere costs the thing that is
// there, and a fixture that set hit points directly would be measuring
// arithmetic nobody performs.

#include "space_world.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/platform/file_io.hpp>
#include <sol/test/test.hpp>

using sol::assets::DefDatabase;
using sol::platform::createDirectories;

namespace {

// ⚑ A HITSCAN GUN, BECAUSE A HIT HAS TO LAND ON THE TICK IT IS FIRED. A bolt
// would spend several ticks in flight and every count below would become a
// guess about how many ticks to run; a beam resolves inside the firing pass.
// `mining_power` is deliberately absent — a beam that cuts rock sweeps
// asteroids first, and a field drifting into the line would turn a damage
// assertion into a mining one.
//
// 25 damage against a `small` mount's 60 hit points is three shots to take one
// off, so "how many shots did that cost" has a visible answer rather than a
// boundary.
constexpr const char* kDefs = R"(
[[commodity]]
id = "sol.food"
name = "Foodstuffs"
base_price = 8.0

[[model]]
id = "pod_body"
mesh = "cube"
texture = "hull"
radius = 1.0

[[weapon]]
id = "sol.beam"
name = "Test Beam"
kind = "hitscan"
mount = "fixed"
size = "small"
damage = 25.0
rate_of_fire = 60.0
range = 4000.0
energy_cost = 0.0
price = 500.0
# ⚑ NAMED SO THAT A DESTROYED *GUN* MOUNT IS COUNTABLE ON SCREEN. Stage E draws
# a gun and a component through two different loops, so a fitting that vanishes
# has to be asserted twice or one of the two loops is untested.
model = "pod_body"

[[component]]
id = "sol.hold"
name = "Hold Pod"
mount = "utility"
size = "small"
model = "pod_body"
price = 300.0
mass = 100.0

# ⚑ SOMETHING TO RAM THAT HAS NO MOUNTS OF ITS OWN, so that when the player's
# gun stops being drawn the count that proves it has nothing else in it.
[[ship]]
id = "sol.hulk"
name = "Hulk"
model = "ship"
scale = 1.0
max_speed = 100.0
cargo = 10.0
power_output = 1.0
hull = 9000.0
armor = 0.0
shield_strength = 0.0

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

// ⚑ THE SHOOTER, AND ITS GUN IS ON THE CENTRELINE ON PURPOSE. A muzzle offset
// to one side would put the beam on a chord through the target's collision
// sphere rather than through its axis, and every bearing measured below would
// be wrong by however far the gun sits off the keel.
//
// It is also the hull that does the RAMMING at the bottom of this file, which
// is what `forward_accel` and `max_speed` are for.
constexpr const char* kShooter = R"(
[[ship]]
id = "sol.shuttle"
name = "Gunboat"
model = "ship"
scale = 1.0
max_speed = 400.0
forward_accel = 200.0
shield_strength = 0.0
armor = 0.0
hull = 9000.0
cargo = 50.0
power_output = 6.0
weapon_capacitor = 100.0
weapon_recharge = 100.0

  [[ship.mount]]
  id = "gun_nose"
  kind = "fixed"
  size = "small"
  at = [0.0, 0.0, -5.0]
  fit = "sol.beam"
)";

// ⚑⚑ THE TARGET, AND ITS FOUR MOUNTS ARE THE WHOLE EXPERIMENT.
//
//   `tail`  external, dead astern       — what a shot from behind should find
//   `nose`  external, dead ahead        — 180 degrees from `tail`
//   `beam`  external, dead to starboard — 90 degrees from both of them
//   `core`  INTERNAL (no `at`)          — never aimed at, whatever the bearing
//
// `tail` is a `medium` where the rest are `small`, which makes "a bigger mount
// takes more to knock out" answerable on one hull rather than by comparing two.
// It is also the only one carrying a fitting, so "a destroyed mount stops being
// drawn" has exactly one instance to count.
//
// The defence numbers are arguments because two of them are subjects in their
// own right: a shield that eats the shot, and an armour layer that does not.
[[nodiscard]] std::string target(const char* shield = "0.0", const char* armor = "0.0")
{
    return std::string(R"(
[[ship]]
id = "sol.target"
name = "Target"
model = "ship"
scale = 1.0
max_speed = 100.0
cargo = 10.0
power_output = 1.0
hull = 9000.0
shield_regen = 0.0
shield_strength = )") +
           shield + "\narmor = " + armor + R"(

  [[ship.mount]]
  id = "tail"
  kind = "utility"
  size = "medium"
  at = [0.0, 0.0, 5.0]
  aim = [0.0, 0.0, 1.0]
  fit = "sol.hold"

  [[ship.mount]]
  id = "nose"
  kind = "utility"
  size = "small"
  at = [0.0, 0.0, -5.0]
  aim = [0.0, 0.0, -1.0]

  [[ship.mount]]
  id = "beam"
  kind = "utility"
  size = "small"
  at = [5.0, 0.0, 0.0]
  aim = [1.0, 0.0, 0.0]

  [[ship.mount]]
  id = "core"
  kind = "subsystem"
  size = "small"
)";
}

// Mount order on `sol.target`, which is `def.mounts` order and therefore the
// order `ShipMounts` is indexed in. Named rather than counted at each use: that
// indexing IS the contract between this component and the two that point into
// it, so bare 0..3 would be silent about what it means.
enum Mount : std::size_t
{
    kTail = 0,
    kNose = 1,
    kBeam = 2,
    kCore = 3,
};

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;
    sol::ecs::Entity mark{};

    explicit Fixture(const std::string& targetToml = target(), std::uint64_t seed = 1701)
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "mount_defs.toml", &error));
        const std::string hulls = std::string(kShooter) + targetToml;
        if (!defs.mergeToml(hulls.c_str(), hulls.size(), "mount_hulls.toml", &error)) {
            std::printf("  hulls did not parse: %s\n", error.c_str());
            SOL_CHECK(false);
        }
        world.spawn(seed);
        world.applyDefs(defs);
        SOL_CHECK(world.generateUniverse(defs));
    }

    bool spawnTarget() { return spawn("sol.target"); }

    // The mountless hull, for the checks that need the player's own kit to be
    // the only thing on screen.
    bool spawnHulk() { return spawn("sol.hulk"); }

    bool spawn(const char* defId)
    {
        const sol::assets::ShipDef* def = defs.findShip(defId);
        if (def == nullptr) {
            return false;
        }
        mark = world.spawnShipFromDef(*def, defs);
        return true;
    }

    // Puts the target in the water and leaves the player looking at its TAIL
    // from `range` metres.
    //
    // ⚑ `spawnShipFromDef` and not `spawnPilotFromDef`: a pilot would
    // manoeuvre, and every bearing measured below is measured against a hull
    // that is exactly where it was put. It also never shoots back, which keeps
    // the shooter's own mounts out of an experiment about the target's.
    bool placeTargetAstern(double range = 300.0)
    {
        // `spawnShipFromDef` gives the target the player's own attitude and
        // puts it down the player's nose, so a player who does not move is
        // looking at the back of it.
        return spawnTarget() && world.warpTo(targetPosition(), range);
    }

    // ⚑⚑ THE SAME PLACEMENT SEEN FROM THE OTHER END, and the pair is the whole
    // geometry argument. Hopping past the target and warping back leaves the
    // player on the far side looking at its NOSE — two bearings, one hull, and
    // nothing different between the two runs except where the shooter stands,
    // which is the only thing the rule under test is allowed to depend on.
    bool placeTargetAhead(double range = 300.0)
    {
        if (!spawnTarget()) {
            return false;
        }
        const sol::core::DVec3 at = targetPosition();
        const sol::core::DVec3 nose =
            toDVec3(rotate(world.shipState().orientation, sol::core::Vec3{0.0f, 0.0f, -1.0f}));
        return world.warpTo(at + nose * 2'000.0, 0.0) && world.warpTo(at, range);
    }

    // The one spawned ship's position. Slot 0 because these fixtures spawn
    // exactly one, and `contactInfo` is the public read of it.
    [[nodiscard]] sol::core::DVec3 targetPosition() const { return world.contactInfo(0).nav.position; }

    [[nodiscard]] static std::vector<game::MountCondition> copy(std::span<const game::MountCondition> live)
    {
        return {live.begin(), live.end()};
    }

    [[nodiscard]] std::vector<game::MountCondition> targetMounts() const
    {
        return copy(world.shipMounts(mark.index));
    }

    [[nodiscard]] std::vector<game::MountCondition> playerMounts() const
    {
        return copy(world.playerMounts());
    }

    // `shots` ticks with the trigger held. The beam fires every tick at 60
    // rounds a second, so shots and ticks are the same number and a caller can
    // say how many shots it meant.
    void fire(std::uint32_t shots)
    {
        sol::sim::FlightInput input;
        input.trigger = true;
        for (std::uint32_t i = 0; i < shots; ++i) {
            world.setShipInput(input);
            world.tick(1.0 / 60.0);
        }
    }

    // Everything the renderer would draw with no entity behind it, which since
    // Phase 31 stage E is exactly the fittings — read back through
    // `buildRenderInstances` rather than off a component, so what is asserted
    // is what would be on the screen.
    [[nodiscard]] std::size_t fittingsDrawn()
    {
        std::vector<game::RenderInstance> all;
        world.buildRenderInstances(1.0f, true, all);
        std::size_t count = 0;
        for (const game::RenderInstance& instance : all) {
            if (instance.key == game::kNoInstanceKey) {
                ++count;
            }
        }
        return count;
    }

    // ⚑ WHETHER THE GUN WENT OFF, MEASURED ON THE THING IT WAS POINTED AT. The
    // obvious version - diff the render instances and count the new bolts -
    // measures nothing here, because the gun under test is a HITSCAN beam and a
    // beam leaves no entity behind it. What a beam leaves is a hole in the
    // target, so that is what is counted.
    [[nodiscard]] double damageDealtInOneTick()
    {
        const double before = world.shipHullFraction(mark);
        fire(1);
        return before - world.shipHullFraction(mark);
    }

    // ⚑⚑ FLIES THE SHOOTER INTO THE TARGET, WHICH IS THE ONE FULLY
    // DETERMINISTIC WAY THIS GAME LETS THE PLAYER'S OWN HULL BE HIT. A bolt
    // cannot hit its own shooter, and an NPC that shoots back has to be flown
    // by a pilot that manoeuvres; a ram is arranged entirely by the test. Both
    // ships sit on one axis, so the contact normal is that axis and the impact
    // lands on the player's NOSE bearing — which is where their gun is.
    //
    // Impact damage is k*v^2 with k = 0.01, so a couple of hundred metres a
    // second is several hundred points: far past a small mount's 60, and far
    // short of the 9000 hull that keeps the shooter flying to be asked about.
    bool ramTarget()
    {
        sol::sim::FlightInput input;
        input.linear = {0.0f, 0.0f, -1.0f};
        for (int tick = 0; tick < 600; ++tick) {
            world.setShipInput(input);
            world.tick(1.0 / 60.0);
            const std::vector<game::MountCondition> mounts = playerMounts();
            if (!mounts.empty() && mounts[0].destroyed()) {
                return true;
            }
        }
        return false;
    }
};

std::string scratchPath(const char* leaf)
{
    const std::string dir = std::string(SOL_GAME_TEST_SCRATCH_DIR) + "/saves";
    SOL_CHECK(createDirectories(dir.c_str()));
    return dir + "/" + leaf;
}

} // namespace

// ⚑ THE COUNTERFACTUAL: filter this walk the way the two walks beside it in
// `applyShipDef` are filtered — to mounts that hold something, or to external
// ones — and this turns red on the count. Condition belongs to the MOUNT, and
// three of these four hold nothing a fit would notice.
SOL_TEST(every_mount_a_hull_declares_gets_a_condition_even_when_nothing_is_in_it)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAstern());
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == 4);
    for (const game::MountCondition& mount : mounts) {
        SOL_CHECK(mount.maxHp > 0.0f);
        SOL_CHECK(mount.hp == mount.maxHp);
        SOL_CHECK(!mount.destroyed());
    }
    // And `at` came across, which is what the hit test resolves against.
    SOL_CHECK(mounts[kTail].external && mounts[kTail].at[2] == 5.0f);
    SOL_CHECK(mounts[kNose].external && mounts[kNose].at[2] == -5.0f);
    SOL_CHECK(mounts[kBeam].external && mounts[kBeam].at[0] == 5.0f);
    // decisions/014 rule 2: no `at`, so internal.
    SOL_CHECK(!mounts[kCore].external);
}

// ⚑ THE COUNTERFACTUAL: return one constant from `mountHitPoints` and this is
// the test that says so. Size is the only thing a mount declares about how much
// kit it holds, and it is what stands in for an authored `hp` key.
SOL_TEST(a_bigger_mount_takes_more_to_knock_out)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAstern());
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == 4);
    SOL_CHECK(mounts[kTail].maxHp == sol::assets::mountHitPoints(sol::assets::MountSize::Medium));
    SOL_CHECK(mounts[kNose].maxHp == sol::assets::mountHitPoints(sol::assets::MountSize::Small));
    SOL_CHECK(mounts[kTail].maxHp > mounts[kNose].maxHp);
}

// ⚑⚑ THE RULE THAT DECIDES WHEN A MOUNT IS REACHABLE AT ALL, and the reason it
// is `armorAbsorbed + hullDamage` rather than the whole hit: a shield is a
// bubble around the entire hull, so a facing that ate the shot ate it on behalf
// of everything bolted underneath. Spend `shieldAbsorbed` here too and this
// turns red — 200 points of shield would have taken a 150-point mount off
// without the shield ever going down.
SOL_TEST(a_shot_the_shield_ate_costs_no_mount_anything)
{
    Fixture fixture(target("400.0"));
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(8); // 200 points, every one of them into a 400-point facing
    for (const game::MountCondition& mount : fixture.targetMounts()) {
        SOL_CHECK(mount.hp == mount.maxHp);
    }
}

// ⚑⚑ AND THE OTHER HALF OF THE SAME RULE: ARMOUR IS NOT A SHIELD.
// `decisions/014` puts armour and hull together on the far side of the line —
// external mounts are reached once the SHIELD is down, internal ones once the
// armour is — so a hit soaked entirely by armour still costs the mount it
// landed on. Drop `armorAbsorbed` from the sum and this is the test that
// catches it, while the hull fraction staying at 1 proves the shot really did
// stop at the armour.
SOL_TEST(armour_does_not_shelter_a_mount_the_way_a_shield_does)
{
    Fixture fixture(target("0.0", "4000.0"));
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(4); // 100 points, all of it ablated
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == 4);
    SOL_CHECK(fixture.world.shipHullFraction(fixture.mark) == 1.0);
    SOL_CHECK(mounts[kTail].hp < mounts[kTail].maxHp);
}

// ⚑⚑⚑ THE STAGE'S CENTRAL CLAIM: A HIT COSTS THE PLACE IT ARRIVED OVER.
//
// The counterfactual is the obvious cheap implementation — spread the damage
// over every mount, or take the FIRST external one — and either turns this red
// on the second line, because `tail` is index 0 and `nose` and `beam` are
// exactly the mounts that must not have paid.
SOL_TEST(a_shot_lands_on_the_mount_it_arrived_over)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(2); // 50 points, into a 150-point medium mount
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == 4);
    SOL_CHECK(mounts[kTail].hp == mounts[kTail].maxHp - 50.0f);
    SOL_CHECK(mounts[kNose].hp == mounts[kNose].maxHp);
    SOL_CHECK(mounts[kBeam].hp == mounts[kBeam].maxHp);
    SOL_CHECK(mounts[kCore].hp == mounts[kCore].maxHp);
}

// ⚑⚑ THE SAME HULL, THE SAME GUN, THE OTHER END — and the mounts that pay swap
// over. This is the test that makes the one above a claim about GEOMETRY rather
// than about mount order: nothing differs between the two runs except where the
// shooter is standing.
SOL_TEST(the_same_hull_shot_from_ahead_loses_the_mount_at_the_other_end)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAhead());
    fixture.fire(2);
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == 4);
    SOL_CHECK(mounts[kNose].hp == mounts[kNose].maxHp - 50.0f);
    SOL_CHECK(mounts[kTail].hp == mounts[kTail].maxHp);
    SOL_CHECK(mounts[kBeam].hp == mounts[kBeam].maxHp);
}

// ⚑⚑ AN INTERNAL MOUNT IS NEVER THE ONE A HIT LANDS ON, whatever the bearing
// and however long the shooting goes on — `decisions/014` rule 2, and the half
// of it this stage implements. Reaching one is a different rule, about which of
// several a hull hides behind its armour; this hull has one so that the absence
// is measurable rather than assumed.
//
// The counterfactual: drop the `external` check in `damageMounts` and `core`
// starts paying, because an internal mount's `at` is the origin and a bearing
// read off it is whatever `normalize` does with a zero vector.
SOL_TEST(an_internal_mount_is_never_the_one_a_hit_lands_on)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(40); // 1000 points, several times what the whole mount list holds
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == 4);
    SOL_CHECK(mounts[kCore].hp == mounts[kCore].maxHp);
}

// ⚑ A SHOT THAT ARRIVES WHERE NOTHING IS BOLTED COSTS NOTHING, which is what
// `kMountHitCosine` is for. `beam` sits 90 degrees off both of the bearings
// this fixture can shoot from, so it is the mount that proves a cone exists at
// all — remove the threshold, take the nearest mount unconditionally, and
// `beam` starts absorbing shots fired at the far end of the ship.
SOL_TEST(a_mount_ninety_degrees_off_the_shot_never_pays_for_it)
{
    Fixture astern;
    SOL_REQUIRE(astern.placeTargetAstern());
    astern.fire(20);
    const std::vector<game::MountCondition> fromAstern = astern.targetMounts();
    SOL_REQUIRE(fromAstern.size() == 4);
    SOL_CHECK(fromAstern[kBeam].hp == fromAstern[kBeam].maxHp);

    Fixture ahead;
    SOL_REQUIRE(ahead.placeTargetAhead());
    ahead.fire(20);
    const std::vector<game::MountCondition> fromAhead = ahead.targetMounts();
    SOL_REQUIRE(fromAhead.size() == 4);
    SOL_CHECK(fromAhead[kBeam].hp == fromAhead[kBeam].maxHp);
}

// ⚑ A DESTROYED MOUNT STOPS AT ZERO AND DOES NOT KEEP FALLING, and once it is
// gone the shooting that follows finds nothing else on that bearing to spend
// itself on. Leave the clamp out and `hp` goes negative, which reads as a
// health bar with a rounding error; leave the `destroyed()` skip out of the
// search and the shots keep arriving at a mount that is not there.
SOL_TEST(a_mount_stops_at_zero_when_it_is_shot_off)
{
    // ⚑⚑ SHOT AT THE *SMALL* MOUNT AND NOT THE MEDIUM ONE, WHICH IS THE
    // DIFFERENCE BETWEEN THIS TEST BITING AND AGREEING WITH BOTH ANSWERS. 25
    // divides 150 exactly, so shooting the medium `tail` lands the subtraction
    // on precisely zero and an unclamped mount is indistinguishable from a
    // clamped one. Three shots is 75 into `nose`'s 60, which overshoots — and
    // overshooting is the only condition under which a clamp does anything.
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAhead());
    fixture.fire(3); // 75 points into a 60-point small mount
    const std::vector<game::MountCondition> spent = fixture.targetMounts();
    SOL_REQUIRE(spent.size() == 4);
    SOL_REQUIRE(spent[kNose].destroyed());
    SOL_CHECK(spent[kNose].hp == 0.0f);
    fixture.fire(20); // and 500 more into the same bearing
    const std::vector<game::MountCondition> after = fixture.targetMounts();
    SOL_REQUIRE(after.size() == 4);
    SOL_CHECK(after[kNose].hp == 0.0f);
    SOL_CHECK(after[kTail].hp == after[kTail].maxHp);
}

// ⚑⚑ THE HULL PAYS THE WHOLE HIT AS WELL, and that double spend is deliberate:
// a mount is a separate pool bolted to the outside of the ship, which is the
// entire difference between disabling a hull and killing it. Take the mount's
// share off the hull and a ship you shot the drive off would be measurably
// healthier than one you shot in the flank for the same number of rounds.
SOL_TEST(the_hull_pays_the_hit_as_well_as_the_mount)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(4); // 100 points
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == 4);
    SOL_CHECK(mounts[kTail].hp == mounts[kTail].maxHp - 100.0f);
    // 9000 hull, no armour, so the fraction is the arithmetic said out loud.
    SOL_CHECK(std::abs(fixture.world.shipHullFraction(fixture.mark) - (8'900.0 / 9'000.0)) < 1.0e-6);
}

// ⚑⚑ AND THE ABSENCE IS THE FEEDBACK. A mount that has been shot off draws
// nothing, so a hull you have worked over reads as one missing its kit from
// across the fight with no icon to consult. `tail` is the only mount on this
// hull carrying a fitting, so the count is one either way and the difference is
// unambiguous.
SOL_TEST(a_mount_that_has_been_shot_off_stops_being_drawn)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAstern());
    // Two: the target's hold pod, and the player's own gun standing in its
    // mount. Only the first of them is under test here.
    SOL_REQUIRE(fixture.fittingsDrawn() == 2);
    fixture.fire(6);
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == 4);
    SOL_REQUIRE(mounts[kTail].destroyed());
    SOL_CHECK(fixture.fittingsDrawn() == 1);
}

// ⚑⚑⚑ "A DESTROYED TURRET THAT STOPS WORKING" — the roadmap's own words for
// half of this stage, and the one check here that is about the SHOOTER rather
// than the target. Ramming is how the test arranges it: a bolt cannot hit its
// own shooter, so the only fully deterministic way to damage the player's own
// hull is to fly it into something.
//
// The counterfactual is the whole of the change in the firing pass — drop the
// `destroyed()` check and the gun keeps shooting out of a ring that is no
// longer there.
SOL_TEST(a_gun_whose_mount_has_been_shot_off_does_not_fire)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAstern());
    SOL_REQUIRE(fixture.damageDealtInOneTick() > 0.0); // it worked before the ram
    SOL_REQUIRE(fixture.ramTarget());
    SOL_REQUIRE(fixture.playerMounts().size() == 1);
    SOL_REQUIRE(fixture.playerMounts()[0].destroyed());
    // Back off and line up again: the ram left the two hulls in contact, and a
    // shot fired from inside a collision would be measured against damage the
    // ram was still dealing.
    SOL_REQUIRE(fixture.world.warpTo(fixture.targetPosition(), 300.0));
    SOL_CHECK(fixture.damageDealtInOneTick() == 0.0);
}

// ⚑⚑ AND THE SAME FOR A GUN, WHICH IS A SECOND LOOP AND THEREFORE A SECOND
// CLAIM. `appendFittingInstances` draws a component and a gun through two
// separate walks — one still, one laid every frame by `layGun` — so a guard
// added to one of them is not a guard on the other, and the pod test above
// cannot see this half at all.
//
// The hulk is what makes the count unambiguous: it carries no mounts, so the
// player's own gun is the only fitting on the screen.
SOL_TEST(a_gun_whose_mount_has_been_shot_off_stops_being_drawn)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.spawnHulk());
    SOL_REQUIRE(fixture.fittingsDrawn() == 1);
    SOL_REQUIRE(fixture.ramTarget());
    SOL_REQUIRE(fixture.playerMounts().size() == 1);
    SOL_REQUIRE(fixture.playerMounts()[0].destroyed());
    SOL_CHECK(fixture.fittingsDrawn() == 0);
}

// ⚑ CONDITION IS SAVED STATE, NOT FEEDBACK. `kSaveVersion` moved for this, and
// a ship whose drive you shot off before docking has to still be missing it
// when the campaign is reloaded — otherwise every reload is a free repair.
SOL_TEST(a_shot_off_mount_survives_a_save_and_a_load)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.spawnTarget());
    SOL_REQUIRE(fixture.ramTarget());
    SOL_REQUIRE(fixture.playerMounts().size() == 1);
    SOL_REQUIRE(fixture.playerMounts()[0].destroyed());

    const std::string path = scratchPath("mount_condition.sav");
    SOL_REQUIRE(fixture.world.saveTo(path.c_str(), "Rammed"));
    SOL_REQUIRE(fixture.world.loadFrom(path.c_str()));
    SOL_REQUIRE(fixture.playerMounts().size() == 1);
    SOL_CHECK(fixture.playerMounts()[0].destroyed());
}

// ⚑ AND A REFIT MAKES A SHIP WHOLE AGAIN, which is a rule this stage arrives at
// rather than invents: `applyShipDef` already reset the DEFENCES to full every
// time it ran, and mount condition is filled by the same call. A player who has
// just paid a shipyard has been to a shipyard. `applyDefs` is that path — the
// def hot-reload — and it is what the outfitting screen runs underneath.
SOL_TEST(a_refit_makes_a_shot_off_mount_whole_again)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.spawnTarget());
    SOL_REQUIRE(fixture.ramTarget());
    SOL_REQUIRE(fixture.playerMounts().size() == 1);
    SOL_REQUIRE(fixture.playerMounts()[0].destroyed());
    fixture.world.applyDefs(fixture.defs);
    const std::vector<game::MountCondition> mounts = fixture.playerMounts();
    SOL_REQUIRE(mounts.size() == 1);
    SOL_CHECK(!mounts[0].destroyed());
    SOL_CHECK(mounts[0].hp == mounts[0].maxHp);
}
