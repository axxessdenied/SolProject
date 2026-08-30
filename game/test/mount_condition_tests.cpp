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

#include "ship_ui.hpp"
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
// It is also the hull that does the RAMMING at the bottom of this file, which is
// what the two accelerations and `max_speed` are for - forward into a target to
// take its own nose gun off, and backward into one to take its drive off.
//
// ⚑ Both arguments have exactly one caller each. `extraMounts` is appended
// verbatim, for the hull that carries TWO drives - the only way to ask what a
// PARTIAL drive does, because one drive can only answer "none left". `hull` is
// for the check that a DESTROYED ship wakes up whole: nine thousand points is
// what keeps every other ram in this file survivable, which is the opposite of
// what that one needs.
[[nodiscard]] std::string shooter(const char* extraMounts = "", const char* hull = "9000.0")
{
    return std::string(R"(
[[ship]]
id = "sol.shuttle"
name = "Gunboat"
model = "ship"
scale = 1.0
max_speed = 400.0
forward_accel = 200.0
reverse_accel = 200.0
shield_strength = 0.0
armor = 0.0
cargo = 50.0
power_output = 6.0
weapon_capacitor = 100.0
weapon_recharge = 100.0
hull = )") +
           hull + R"(

  [[ship.mount]]
  id = "gun_nose"
  kind = "fixed"
  size = "small"
  at = [0.0, 0.0, -5.0]
  fit = "sol.beam"

  # ⚑ DEAD ASTERN AND BARE, WHICH IS BOTH HALVES OF THE POINT (Phase 31 stage
  # F2). Astern, so that backing into something takes it off and nothing
  # arriving at the nose can; and bare, because no `engine` mount in the
  # shipped game carries a `fit` either - a drive bell is part of the hull, and
  # this stage's exit criterion is about shooting one off a hull nobody has
  # outfitted.
  [[ship.mount]]
  id = "drive_main"
  kind = "engine"
  size = "small"
  at = [0.0, 0.0, 5.0]
  aim = [0.0, 0.0, 1.0]
)" + extraMounts;
}

// ⚑ A SECOND DRIVE, HUNG WHERE A STERN RAM CANNOT REACH IT. Ninety degrees off
// `drive_main`'s bearing is well outside the hit cone, so backing into
// something takes one of the two and leaves the other — which is a half drive,
// and the only reading that can tell "scaled by the share left" from "switched
// off when the last one goes".
constexpr const char* kVentralDrive = R"(
  [[ship.mount]]
  id = "drive_ventral"
  kind = "engine"
  size = "small"
  at = [0.0, -5.0, 0.0]
  aim = [0.0, -1.0, 0.0]
)";

// ⚑⚑ THE TARGET, AND ITS FIVE MOUNTS ARE THE WHOLE EXPERIMENT.
//
//   `tail`  external, dead astern       — what a shot from behind should find
//   `nose`  external, dead ahead        — 180 degrees from `tail`
//   `beam`  external, dead to starboard — 90 degrees from both of them
//   `core`  INTERNAL (no `at`)          — never aimed at, whatever the bearing
//   `guts`  INTERNAL, and a `shield`     — a second one, so that damage which
//                                          reaches the inside can be seen to
//                                          SPREAD rather than land twice in
//                                          the same place
//
// `tail` is a `medium` where the rest are `small`, which makes "a bigger mount
// takes more to knock out" answerable on one hull rather than by comparing two.
// It is also the only one carrying a fitting, so "a destroyed mount stops being
// drawn" has exactly one instance to count.
//
// The defence numbers are arguments because three of them are subjects in their
// own right: a shield that eats the shot, an armour layer that does not, and a
// regeneration rate that stops when the generator that drives it is shot off.
[[nodiscard]] std::string
target(const char* shield = "0.0", const char* armor = "0.0", const char* regen = "0.0")
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
shield_regen_delay = 0.0
shield_strength = )") +
           shield + "\narmor = " + armor + "\nshield_regen = " + regen + R"(

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

  [[ship.mount]]
  id = "guts"
  kind = "shield"
  size = "small"
)";
}

// Mount order on `sol.target`, which is `def.mounts` order and therefore the
// order `ShipMounts` is indexed in. Named rather than counted at each use: that
// indexing IS the contract between this component and the two that point into
// it, so bare 0..4 would be silent about what it means.
enum Mount : std::size_t
{
    kTail = 0,
    kNose = 1,
    kBeam = 2,
    kCore = 3,
    kGuts = 4,
    kMountCount = 5,
};

struct Fixture
{
    DefDatabase defs;
    game::SpaceWorld world;
    sol::ecs::Entity mark{};

    explicit Fixture(const std::string& targetToml = target(),
                     const std::string& shooterToml = shooter(),
                     std::uint64_t seed = 1701)
    {
        std::string error;
        SOL_CHECK(defs.mergeToml(kDefs, std::strlen(kDefs), "mount_defs.toml", &error));
        const std::string hulls = shooterToml + targetToml;
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

    // ⚑⚑ THE SAME TRICK RUN BACKWARDS, WHICH IS HOW THIS SUITE SHOOTS A DRIVE
    // OFF. The player's drive is dead astern, so a contact has to arrive there
    // - which means turning the ship around and holding full reverse. Warping
    // to a point on the far side of where the ship already is leaves the nose
    // pointing away from the target and the target squarely behind it.
    bool ramAstern()
    {
        // ⚑ THE STANDOFF IS SET HERE RATHER THAN INHERITED, because a caller
        // may have flown the ship somewhere first - the drive test holds full
        // thrust for a second before it shoots anything off, and a run-up
        // measured from wherever that left the ship is a run-up of unknown
        // length. Park at 500 m facing the target, then flip: the ship ends up
        // 1000 m out with the target dead astern.
        if (!world.warpTo(targetPosition(), 500.0)) {
            return false;
        }
        const sol::core::DVec3 here = world.shipState().position;
        if (!world.warpTo(here + (here - targetPosition()), 0.0)) {
            return false;
        }
        sol::sim::FlightInput input;
        input.linear = {0.0f, 0.0f, 1.0f}; // full reverse
        for (int tick = 0; tick < 900; ++tick) {
            world.setShipInput(input);
            world.tick(1.0 / 60.0);
            const std::vector<game::MountCondition> mounts = playerMounts();
            if (mounts.size() > 1 && mounts[1].destroyed()) {
                return true;
            }
        }
        return false;
    }

    // Holds an input for `ticks` and answers how fast the ship ended up going.
    // Speed rather than distance because a ship that has been rammed is already
    // moving, and the question is whether the drive can change that.
    double flyFor(int ticks, const sol::core::Vec3& linear, const sol::core::Vec3& angular = {})
    {
        sol::sim::FlightInput input;
        input.linear = linear;
        input.angular = angular;
        for (int tick = 0; tick < ticks; ++tick) {
            world.setShipInput(input);
            world.tick(1.0 / 60.0);
        }
        return length(world.shipState().velocity);
    }

    // Parks the ship with its velocity zeroed, so an acceleration test starts
    // from a standstill rather than from whatever the ram left behind.
    //
    // ⚑⚑ FIFTY KILOMETRES, AND THE NUMBER IS LOAD-BEARING. `warpTo` points the
    // nose AT what it parks off, so a test that then holds full forward thrust
    // is flying straight at the thing - and at 400 m/s, fifteen seconds of it
    // covers six kilometres. At the four kilometres this used to say, the
    // measurement ended in a collision and the speed being read was whatever
    // the impact had left, which is not a fact about the drive at all.
    bool comeToRest() { return world.warpTo(targetPosition(), 50'000.0); }

    [[nodiscard]] float targetShieldFore() const
    {
        const game::ShipDefense* defense = world.shipDefense(mark);
        return defense != nullptr ? defense->state.shieldFore : -1.0f;
    }

    [[nodiscard]] float targetShieldAft() const
    {
        const game::ShipDefense* defense = world.shipDefense(mark);
        return defense != nullptr ? defense->state.shieldAft : -1.0f;
    }

    [[nodiscard]] double playerHullFraction() const { return world.playerDefense().state.hull; }
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
    SOL_REQUIRE(mounts.size() == kMountCount);
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
    SOL_REQUIRE(mounts.size() == kMountCount);
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
    SOL_REQUIRE(mounts.size() == kMountCount);
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
    // ⚑ ARMOURED, so the shot stops at the plating and the INTERNAL mounts stay
    // out of it. What is under test here is which EXTERNAL mount a bearing
    // picks; a bare-hulled target would have the hull-breach pass below
    // spending the same shot on the inside as well, and a check that "nothing
    // else paid" would be measuring two rules at once.
    Fixture fixture(target("0.0", "4000.0"));
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(2); // 50 points, into a 150-point medium mount
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == kMountCount);
    SOL_CHECK(mounts[kTail].hp == mounts[kTail].maxHp - 50.0f);
    SOL_CHECK(mounts[kNose].hp == mounts[kNose].maxHp);
    SOL_CHECK(mounts[kBeam].hp == mounts[kBeam].maxHp);
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
    SOL_REQUIRE(mounts.size() == kMountCount);
    SOL_CHECK(mounts[kNose].hp == mounts[kNose].maxHp - 50.0f);
    SOL_CHECK(mounts[kTail].hp == mounts[kTail].maxHp);
    SOL_CHECK(mounts[kBeam].hp == mounts[kBeam].maxHp);
}

// ⚑⚑ AN INTERNAL MOUNT IS NEVER AIMED AT, WHATEVER THE BEARING AND HOWEVER
// LONG THE SHOOTING GOES ON — `decisions/014` rule 2. This hull's armour is
// what holds it true: 4000 points of it, so a thousand rounds of beam never
// reach the hull, and everything that lands is spent on the plating and on
// whatever external mount was in the way.
//
// The counterfactual is `damageMounts`' second pass keying on anything other
// than `hullDamage` — sum in `armorAbsorbed` there, as the external pass
// legitimately does, and the inside of an armoured ship starts failing while
// the armour is still on it.
SOL_TEST(an_internal_mount_is_untouched_while_the_armour_holds)
{
    Fixture fixture(target("0.0", "4000.0"));
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(40); // 1000 points, none of which reaches the hull
    SOL_REQUIRE(fixture.world.shipHullFraction(fixture.mark) == 1.0);
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == kMountCount);
    SOL_CHECK(mounts[kCore].hp == mounts[kCore].maxHp);
    SOL_CHECK(mounts[kGuts].hp == mounts[kGuts].maxHp);
}

// ⚑⚑⚑ AND THE OTHER HALF OF THE SAME RULE, WHICH IS THE ONE STAGE F2 ADDS: an
// internal mount IS reached once the armour is gone. `hullDamage` is non-zero
// only after `applyDamage` has spent the shield facing and then the armour, so
// "reachable only once armour and hull are compromised" needed no condition of
// its own — it is already the name of a field on the result.
//
// ⚑ THE DAMAGE SPREADS RATHER THAN PILING ONTO ONE PLACE. There is no geometry
// to tell one internal mount from another, so the pick is arbitrary either way;
// sharing it out is what leaves a ship degrading instead of losing whole
// subsystems while others sit untouched. Take "the first internal mount" instead
// and the second line here turns red.
SOL_TEST(an_internal_mount_is_reached_once_the_armour_is_gone)
{
    Fixture fixture; // no armour at all: every point lands on the hull
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(4); // 100 points of hull damage across two internal mounts
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == kMountCount);
    SOL_CHECK(mounts[kCore].hp == mounts[kCore].maxHp - 50.0f);
    SOL_CHECK(mounts[kGuts].hp == mounts[kGuts].maxHp - 50.0f);
}

// ⚑ AND THE TWO PASSES ARE INDEPENDENT: ONE SHOT CAN COST BOTH. They are
// different mechanisms — an external mount is hit because it is physically in
// the way, an internal one because the plating over it has failed — so a hull
// breach that spared the sensor suite because a cargo pod happened to be on the
// same bearing would be geometry deciding something it knows nothing about.
SOL_TEST(one_shot_can_cost_an_external_mount_and_an_internal_one)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.placeTargetAstern());
    fixture.fire(2); // 50 points
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == kMountCount);
    SOL_CHECK(mounts[kTail].hp == mounts[kTail].maxHp - 50.0f); // the bearing
    SOL_CHECK(mounts[kCore].hp < mounts[kCore].maxHp);          // and the breach
}

// ⚑ A SHOT THAT ARRIVES WHERE NOTHING IS BOLTED COSTS NOTHING, which is what
// `kMountHitCosine` is for. `beam` sits 90 degrees off both of the bearings
// this fixture can shoot from, so it is the mount that proves a cone exists at
// all — remove the threshold, take the nearest mount unconditionally, and
// `beam` starts absorbing shots fired at the far end of the ship.
SOL_TEST(a_mount_ninety_degrees_off_the_shot_never_pays_for_it)
{
    Fixture astern(target("0.0", "4000.0"));
    SOL_REQUIRE(astern.placeTargetAstern());
    astern.fire(20);
    const std::vector<game::MountCondition> fromAstern = astern.targetMounts();
    SOL_REQUIRE(fromAstern.size() == kMountCount);
    SOL_CHECK(fromAstern[kBeam].hp == fromAstern[kBeam].maxHp);

    Fixture ahead(target("0.0", "4000.0"));
    SOL_REQUIRE(ahead.placeTargetAhead());
    ahead.fire(20);
    const std::vector<game::MountCondition> fromAhead = ahead.targetMounts();
    SOL_REQUIRE(fromAhead.size() == kMountCount);
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
    SOL_REQUIRE(spent.size() == kMountCount);
    SOL_REQUIRE(spent[kNose].destroyed());
    SOL_CHECK(spent[kNose].hp == 0.0f);
    fixture.fire(20); // and 500 more into the same bearing
    const std::vector<game::MountCondition> after = fixture.targetMounts();
    SOL_REQUIRE(after.size() == kMountCount);
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
    SOL_REQUIRE(mounts.size() == kMountCount);
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
    SOL_REQUIRE(mounts.size() == kMountCount);
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
    SOL_REQUIRE(fixture.playerMounts().size() == 2);
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
    SOL_REQUIRE(fixture.playerMounts().size() == 2);
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
    SOL_REQUIRE(fixture.playerMounts().size() == 2);
    SOL_REQUIRE(fixture.playerMounts()[0].destroyed());

    const std::string path = scratchPath("mount_condition.sav");
    SOL_REQUIRE(fixture.world.saveTo(path.c_str(), "Rammed"));
    SOL_REQUIRE(fixture.world.loadFrom(path.c_str()));
    SOL_REQUIRE(fixture.playerMounts().size() == 2);
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
    SOL_REQUIRE(fixture.playerMounts().size() == 2);
    SOL_REQUIRE(fixture.playerMounts()[0].destroyed());
    fixture.world.applyDefs(fixture.defs);
    const std::vector<game::MountCondition> mounts = fixture.playerMounts();
    SOL_REQUIRE(mounts.size() == 2);
    SOL_CHECK(!mounts[0].destroyed());
    SOL_CHECK(mounts[0].hp == mounts[0].maxHp);
}

// ---------------------------------------------------------------------------
// Phase 31 stage F2 — what a destroyed mount STOPS DOING.
// ---------------------------------------------------------------------------

// ⚑⚑⚑ PHASE 31'S OWN EXIT CRITERION, HALF OF IT: "shoot a freighter's drive off
// and watch it stop, STILL ALIVE". The shooter's drive is its only one, so
// taking it off leaves nothing to push with — and its hull is nowhere near
// gone, which is the "still alive" the criterion insists on.
//
// The control is the first check: the same hull, the same input, the same
// number of ticks, before anything has been shot off.
SOL_TEST(a_hull_whose_only_drive_is_shot_off_cannot_accelerate)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.spawnHulk());
    SOL_REQUIRE(fixture.comeToRest());
    SOL_REQUIRE(fixture.flyFor(60, {0.0f, 0.0f, -1.0f}) > 100.0); // one second of drive

    SOL_REQUIRE(fixture.ramAstern());
    const std::vector<game::MountCondition> mounts = fixture.playerMounts();
    SOL_REQUIRE(mounts.size() == 2);
    SOL_REQUIRE(mounts[1].destroyed()); // `drive_main`
    SOL_REQUIRE(fixture.playerHullFraction() > 0.0);

    SOL_REQUIRE(fixture.comeToRest()); // and zero the velocity the ram left
    SOL_CHECK(fixture.flyFor(60, {0.0f, 0.0f, -1.0f}) == 0.0);
}

// ⚑⚑ AND IT CAN STILL TURN, WHICH IS DELIBERATE AND IS WHY `MountKind` HAS BOTH
// `engine` AND `thruster`. Engines push and thrusters turn — gdd.md §11.5 lists
// them separately — so a hull with its drive shot off is dead in the water and
// still able to point itself, which is what lets a crippled freighter keep a
// turret on you. Scale the angular envelope by the drive too and this is the
// test that says so.
SOL_TEST(a_ship_with_its_drive_shot_off_can_still_turn)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.spawnHulk());
    SOL_REQUIRE(fixture.ramAstern());
    SOL_REQUIRE(fixture.comeToRest());
    const sol::core::Quat before = fixture.world.shipState().orientation;
    (void)fixture.flyFor(60, {}, {0.0f, 1.0f, 0.0f}); // a second of yaw
    const sol::core::Quat after = fixture.world.shipState().orientation;
    SOL_CHECK(std::abs(dot(before, after)) < 0.999f);
}

// ⚑ A DRIVE THAT IS GONE CANNOT CRUISE AWAY EITHER, and that is not a second
// rule — cruise is a multiple of the same speed cap the drive scales. A
// disabled ship that could still engage cruise is not a disabled ship, and it
// is the whole reason `maxSpeed` scales beside the accelerations rather than
// the accelerations alone.
SOL_TEST(a_ship_with_its_drive_shot_off_cannot_cruise_away)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.spawnHulk());
    SOL_REQUIRE(fixture.ramAstern());
    SOL_REQUIRE(fixture.comeToRest());
    sol::sim::FlightInput input;
    input.linear = {0.0f, 0.0f, -1.0f};
    input.cruise = true;
    for (int tick = 0; tick < 120; ++tick) {
        fixture.world.setShipInput(input);
        fixture.world.tick(1.0 / 60.0);
    }
    SOL_CHECK(length(fixture.world.shipState().velocity) == 0.0);
}

// ⚑⚑ HALF A DRIVE IS HALF THE TOP SPEED, AND NOT MERELY A SLOWER CLIMB TO THE
// SAME ONE. This is the check that makes `tuning.maxSpeed *= drive` a rule
// rather than a line nobody would miss: with the accelerations scaled and the
// cap left alone, a half-drive ship takes twice as long to get going and then
// runs exactly as fast as it ever did — which is not a ship anybody would call
// crippled, and which fifteen seconds of held thrust cannot tell from a whole
// one.
//
// It needs a hull with TWO drives, because one drive can only answer "none
// left". The second is hung on the belly, ninety degrees off the bearing a
// stern ram arrives on, so the ram takes exactly one of the two.
SOL_TEST(half_a_drive_is_half_the_top_speed)
{
    Fixture fixture(target(), shooter(kVentralDrive));
    SOL_REQUIRE(fixture.spawnHulk());
    SOL_REQUIRE(fixture.comeToRest());
    const double whole = fixture.flyFor(900, {0.0f, 0.0f, -1.0f}); // fifteen seconds: terminal
    SOL_REQUIRE(whole > 100.0);

    SOL_REQUIRE(fixture.ramAstern());
    const std::vector<game::MountCondition> mounts = fixture.playerMounts();
    SOL_REQUIRE(mounts.size() == 3);
    SOL_REQUIRE(mounts[1].destroyed());  // `drive_main`, on the bearing
    SOL_REQUIRE(!mounts[2].destroyed()); // `drive_ventral`, ninety degrees off it

    SOL_REQUIRE(fixture.comeToRest());
    const double halved = fixture.flyFor(900, {0.0f, 0.0f, -1.0f});
    SOL_CHECK(halved > whole * 0.4);
    SOL_CHECK(halved < whole * 0.6);
}

// ⚑ HALF THE DRIVES IS HALF THE PUSH, and the readings either side of it are
// what make this a proportion rather than a switch.
//
// ⚑⚑ THIS IS THE ONE CHECK IN THE FILE THAT BUILDS THE COMPONENT BY HAND, and
// it is allowed to because `driveFraction` is a PURE FUNCTION OF IT. Everything
// else here is a claim about how a mount came to be damaged, which only the real
// damage path can make; this is a claim about arithmetic over a struct, and
// routing it through a ram would only make it slower to read.
SOL_TEST(a_hull_flies_on_the_share_of_its_drives_that_is_left)
{
    game::ShipMounts mounts;
    mounts.count = 3;
    mounts.mounts[0].kind = sol::assets::MountKind::Engine;
    mounts.mounts[1].kind = sol::assets::MountKind::Engine;
    mounts.mounts[2].kind = sol::assets::MountKind::Turret; // not a drive: never counted
    for (std::uint32_t m = 0; m < mounts.count; ++m) {
        mounts.mounts[m].maxHp = 60.0f;
        mounts.mounts[m].hp = 60.0f;
    }
    SOL_CHECK(game::driveFraction(mounts) == 1.0f);
    mounts.mounts[0].hp = 0.0f;
    SOL_CHECK(game::driveFraction(mounts) == 0.5f);
    mounts.mounts[1].hp = 0.0f;
    SOL_CHECK(game::driveFraction(mounts) == 0.0f);
    // ⚑ And shooting the turret off changes nothing about the drive, which is
    // what keeps this from quietly becoming "how much of this ship is left".
    mounts.mounts[2].hp = 0.0f;
    SOL_CHECK(game::driveFraction(mounts) == 0.0f);
}

// ⚑⚑ A HULL THAT DECLARES NO ENGINE MOUNT FLIES EXACTLY AS IT ALWAYS DID, and
// that is not a fallback so much as the rule read carefully: a drive you cannot
// shoot off is a drive that cannot be missing. It matters because it is most of
// the game — every test hull in `armament_tests`, every station, every rock, and
// every ship built before this stage existed.
SOL_TEST(a_hull_with_no_engine_mount_at_all_flies_as_it_always_did)
{
    game::ShipMounts none;
    SOL_CHECK(game::driveFraction(none) == 1.0f);
    SOL_CHECK(game::shieldsArePowered(none));

    game::ShipMounts unarmed;
    unarmed.count = 1;
    unarmed.mounts[0].kind = sol::assets::MountKind::Utility;
    unarmed.mounts[0].maxHp = 60.0f;
    unarmed.mounts[0].hp = 0.0f; // shot off, and still not a drive
    SOL_CHECK(game::driveFraction(unarmed) == 1.0f);
    SOL_CHECK(game::shieldsArePowered(unarmed));
}

// ⚑⚑⚑ A SHIELD GENERATOR THAT HAS BEEN SHOT OFF STOPS THE FACINGS COMING BACK.
// `guts` is a `shield` mount and it is INTERNAL, which is how all three shipped
// hulls author theirs — so this is also the end-to-end proof that stage F2's two
// halves meet: the shot has to get through the shield and then the armour to
// reach the generator that is the reason the shield was there at all.
SOL_TEST(a_shot_off_shield_generator_stops_the_facings_coming_back)
{
    // 200 per facing, no armour, and a regen fast enough to see inside a second.
    Fixture fixture(target("200.0", "0.0", "50.0"));
    SOL_REQUIRE(fixture.placeTargetAstern());
    // The control first: dent the aft facing and watch it recover.
    fixture.fire(4); // 100 points into a 200-point facing
    const float dented = fixture.targetShieldAft();
    SOL_REQUIRE(dented < 200.0f);
    (void)fixture.flyFor(60, {});
    SOL_REQUIRE(fixture.targetShieldAft() > dented);

    // Now through the facing and into the hull until the generator itself goes.
    fixture.fire(60);
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == kMountCount);
    SOL_REQUIRE(mounts[kGuts].destroyed());
    const float stranded = fixture.targetShieldAft();
    (void)fixture.flyFor(120, {});
    SOL_CHECK(fixture.targetShieldAft() == stranded);
}

// ⚑ AND WHAT IS ALREADY IN THE ENVELOPE DOES NOT EVAPORATE WITH IT. Collapsing
// both facings when the generator goes would make one lucky shot a larger swing
// than anything else in the damage model can do in a single hit — larger than
// destroying the hull's own armour — so a destroyed generator stops the supply
// and takes nothing back.
SOL_TEST(shields_already_up_do_not_evaporate_when_the_generator_goes)
{
    Fixture fixture(target("200.0", "0.0", "0.0"));
    SOL_REQUIRE(fixture.placeTargetAstern());
    // Every shot lands aft, so the FORE facing is untouched and still full at
    // the moment the generator underneath it is destroyed.
    fixture.fire(60);
    const std::vector<game::MountCondition> mounts = fixture.targetMounts();
    SOL_REQUIRE(mounts.size() == kMountCount);
    SOL_REQUIRE(mounts[kGuts].destroyed());
    SOL_CHECK(fixture.targetShieldFore() == 200.0f);
}

// ⚑⚑⚑ A DESTROYED SHIP WAKES UP WHOLE, AND THIS TEST EXISTS BECAUSE A DRIVE
// FOUND THAT IT DID NOT. `decisions/007` is that death costs the cargo and an
// insurance deductible and puts the player back in THE SAME SHIP AND FIT — and
// a fit with a hole shot in it is not that fit. The respawn block resets the
// transform, the body, the defences and the capacitor, which was everything the
// damage model could leave broken until this phase; mount condition was simply
// not on a list nobody had needed to revisit. Nothing in it was wrong, so
// nothing looked wrong.
//
// Flying a hostile freighter into the player's face in the real game showed
// `gun_nose 0/60 DESTROYED` and `shield_core 0/60 DESTROYED` STILL THERE two
// respawns later — a player who had paid the deductible twice, flying a hull
// with no gun and no shield generator, and no repair anywhere in the game.
//
// A ram does it deterministically here: one contact that takes the nose mount
// off and takes the hull with it, against a shooter authored thin enough to die
// of it.
SOL_TEST(a_destroyed_ship_wakes_up_with_its_mounts_whole)
{
    Fixture fixture(target(), shooter("", "150.0"));
    SOL_REQUIRE(fixture.spawnHulk());
    const double before = fixture.world.playerCredits();

    // Straight at it until something gives. The ram cannot be waited on by
    // watching for a destroyed mount the way the others are — the repair is the
    // thing under test, and it happens in the same tick as the destruction.
    sol::sim::FlightInput input;
    input.linear = {0.0f, 0.0f, -1.0f};
    for (int tick = 0; tick < 600 && fixture.world.playerCredits() == before; ++tick) {
        fixture.world.setShipInput(input);
        fixture.world.tick(1.0 / 60.0);
    }
    // Credits fell, which on this hull can only be the insurance deductible.
    SOL_REQUIRE(fixture.world.playerCredits() < before);

    const std::vector<game::MountCondition> mounts = fixture.playerMounts();
    SOL_REQUIRE(mounts.size() == 2);
    for (const game::MountCondition& mount : mounts) {
        SOL_CHECK(!mount.destroyed());
        SOL_CHECK(mount.hp == mount.maxHp);
    }
}

// ⚑ AND THE PLAYER CAN SEE IT. The ship readout is where a fit is read, so a
// mount that has been shot off has to say so there or the only way to find out
// is to notice the ship handling differently. `shipInfoReport` is the same text
// the screen draws its rows from, which is how this is verified without reading
// pixels.
SOL_TEST(the_ship_readout_says_which_mount_has_been_shot_off)
{
    Fixture fixture;
    SOL_REQUIRE(fixture.spawnHulk());
    SOL_REQUIRE(game::shipInfoReport(fixture.world, fixture.defs).find("DESTROYED") == std::string::npos);
    SOL_REQUIRE(fixture.ramAstern());
    const std::string report = game::shipInfoReport(fixture.world, fixture.defs);
    SOL_CHECK(report.find("drive_main") != std::string::npos);
    SOL_CHECK(report.find("DESTROYED") != std::string::npos);
}
