#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <sol/assets/data_defs.hpp>
#include <sol/assets/loadout.hpp>
#include <sol/test/test.hpp>

using sol::assets::ComponentDef;
using sol::assets::CrewDef;
using sol::assets::DefDatabase;
using sol::assets::FittedMount;
using sol::assets::MountKind;
using sol::assets::MountSize;
using sol::assets::ShipDef;
using sol::assets::WeaponDef;

namespace {

bool merge(DefDatabase& db, const char* toml, const char* source, std::string* outError = nullptr)
{
    return db.mergeToml(toml, std::strlen(toml), source, outError);
}

bool nearlyEqual(float a, float b)
{
    return std::fabs(a - b) < 1.0e-4f;
}

// The testbed reproduces what the shuttle's four `slots_*` counts used to say,
// as mounts: one shield, one engine, two utility (the old cargo + utility
// merge), and one gun. Written out rather than fixtured from `game/data` on
// purpose - these tests are about the RULES, and the shipped hulls are
// asserted separately in data_defs_tests where a content change should be
// noticed.
constexpr const char* kOutfittingDefs = R"(
[[ship]]
id = "sol.testbed"
name = "Testbed"
forward_accel = 100.0
max_speed = 200.0
shield_strength = 100.0
cargo = 50.0
price = 8000.0
mass = 10000.0
power_output = 6.0
crew_berths = 1

  [[ship.mount]]
  id = "gun"
  kind = "fixed"
  size = "small"
  at = [0.0, 0.0, -2.0]

  [[ship.mount]]
  id = "shield_core"
  kind = "shield"
  size = "small"

  [[ship.mount]]
  id = "drive"
  kind = "engine"
  size = "small"
  at = [0.0, 0.0, 2.0]

  [[ship.mount]]
  id = "bay_port"
  kind = "utility"
  size = "medium"
  at = [-1.0, 0.0, 0.0]

  [[ship.mount]]
  id = "bay_starboard"
  kind = "utility"
  size = "small"
  at = [1.0, 0.0, 0.0]

[[weapon]]
id = "sol.popgun"
name = "Popgun"
kind = "projectile"
mount = "fixed"
size = "small"
damage = 5.0
price = 400.0

[[component]]
id = "sol.shield_mk1"
name = "Shield Booster Mk1"
mount = "shield"
size = "small"
price = 500.0
mass = 500.0
power_draw = 2.0
shield_strength_mul = 1.5
shield_strength_add = 20.0

[[component]]
id = "sol.cargo_pod"
name = "Cargo Pod"
mount = "utility"
size = "small"
price = 300.0
mass = 1000.0
power_draw = 0.0
cargo_add = 25.0

[[component]]
id = "sol.big_pod"
name = "Bulk Pod"
mount = "utility"
size = "medium"
price = 900.0
mass = 1500.0
cargo_add = 80.0

[[component]]
id = "sol.hungry_reactor_sink"
name = "Hungry Sink"
mount = "utility"
size = "small"
power_draw = 100.0

[[crew]]
id = "sol.engineer_kim"
name = "Kim"
role = "Engineer"
price = 400.0
shield_regen_mul = 1.1
)";

// A fit, spelled the way the game spells one: a mount id plus whichever def
// the mount's kind says to look up.
FittedMount at(const char* mountId, const ComponentDef* component)
{
    return FittedMount{.mountId = mountId, .component = component};
}

FittedMount at(const char* mountId, const WeaponDef* weapon)
{
    return FittedMount{.mountId = mountId, .weapon = weapon};
}

} // namespace

SOL_TEST(loadout_parse_components_and_crew)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kOutfittingDefs, "outfit.toml", &error));
    SOL_CHECK(db.components().size() == 4);
    SOL_CHECK(db.crew().size() == 1);

    const ComponentDef* shield = db.findComponent("sol.shield_mk1");
    SOL_CHECK(shield != nullptr);
    if (shield == nullptr) {
        return;
    }
    SOL_CHECK(shield->mount == MountKind::Shield);
    SOL_CHECK(shield->size == MountSize::Small);
    SOL_CHECK(shield->price == 500.0f);
    const auto statIdx = static_cast<std::size_t>(sol::assets::FitStat::ShieldStrength);
    SOL_CHECK(shield->modifiers.mul[statIdx] == 1.5f);
    SOL_CHECK(shield->modifiers.add[statIdx] == 20.0f);

    // A weapon carries the same two keys, and `kind` still means how the shot
    // travels rather than where the gun sits - the collision the def format
    // has to keep straight.
    const WeaponDef* gun = db.findWeapon("sol.popgun");
    SOL_REQUIRE(gun != nullptr);
    SOL_CHECK(gun->kind == "projectile");
    SOL_CHECK(gun->mount == MountKind::Fixed);
    SOL_CHECK(gun->size == MountSize::Small);

    const CrewDef* kim = db.findCrew("sol.engineer_kim");
    SOL_CHECK(kim != nullptr);
    if (kim == nullptr) {
        return;
    }
    SOL_CHECK(kim->role == "Engineer");
    const auto regenIdx = static_cast<std::size_t>(sol::assets::FitStat::ShieldRegen);
    SOL_CHECK(nearlyEqual(kim->modifiers.mul[regenIdx], 1.1f));

    const ShipDef* ship = db.findShip("sol.testbed");
    SOL_CHECK(ship != nullptr);
    if (ship == nullptr) {
        return;
    }
    SOL_CHECK(ship->price == 8000.0f);
    SOL_CHECK(ship->mounts.size() == 5);
    SOL_CHECK(ship->crewBerths == 1);
}

SOL_TEST(loadout_rejects_bad_mount_and_unknown_modifier)
{
    DefDatabase db;
    std::string error;
    const char* badMount = R"(
[[component]]
id = "sol.bad"
name = "Bad"
mount = "hyperdrive"
size = "small"
)";
    SOL_CHECK(!merge(db, badMount, "bad.toml", &error));
    SOL_CHECK(error.find("mount") != std::string::npos);

    const char* badSize = R"(
[[component]]
id = "sol.bad_size"
name = "Bad Size"
mount = "utility"
size = "enormous"
)";
    SOL_CHECK(!merge(db, badSize, "bad_size.toml", &error));
    SOL_CHECK(error.find("size") != std::string::npos);

    const char* badKey = R"(
[[component]]
id = "sol.bad2"
name = "Bad2"
mount = "utility"
size = "small"
warp_factor_mul = 2.0
)";
    SOL_CHECK(!merge(db, badKey, "bad2.toml", &error));
    SOL_CHECK(error.find("warp_factor_mul") != std::string::npos);
}

// ⚑ The mount kind is what tells the fit model WHICH TABLE a def id lives in,
// so a def on the wrong side of that line is an id nothing could ever resolve.
// Both directions, because catching one and not the other leaves the other's
// failure looking like a missing def at load time in somebody else's file.
SOL_TEST(loadout_refuses_a_component_in_a_gun_mount_and_a_weapon_out_of_one)
{
    DefDatabase db;
    std::string error;
    const char* componentInTurret = R"(
[[component]]
id = "sol.confused"
name = "Confused"
mount = "turret"
size = "small"
)";
    SOL_CHECK(!merge(db, componentInTurret, "confused.toml", &error));
    SOL_CHECK(error.find("weapon") != std::string::npos);

    const char* weaponInShield = R"(
[[weapon]]
id = "sol.confused_gun"
name = "Confused Gun"
kind = "hitscan"
mount = "shield"
size = "small"
)";
    SOL_CHECK(!merge(db, weaponInShield, "confused_gun.toml", &error));
    SOL_CHECK(error.find("component") != std::string::npos);
}

// The size rule, at the enum level: a mount takes its own size or smaller, and
// the ordering is the declaration order of `MountSize`.
SOL_TEST(loadout_mount_accepts_its_size_or_smaller)
{
    SOL_CHECK(sol::assets::mountAccepts(MountSize::Medium, MountSize::Small));
    SOL_CHECK(sol::assets::mountAccepts(MountSize::Medium, MountSize::Medium));
    SOL_CHECK(!sol::assets::mountAccepts(MountSize::Medium, MountSize::Large));
    SOL_CHECK(!sol::assets::mountAccepts(MountSize::Small, MountSize::Medium));
    SOL_CHECK(sol::assets::mountAccepts(MountSize::XLarge, MountSize::Small));
}

// ⚑ THE ONE ASYMMETRY, PINNED IN BOTH DIRECTIONS. A turret is a ring with a
// motor, so it holds a fixed gun and the gun traverses; a bare hardpoint
// cannot hold what needs the ring. A test that only checked the permissive
// half would pass on a rule that accepted everything.
SOL_TEST(loadout_a_turret_takes_a_fixed_gun_and_not_the_reverse)
{
    SOL_CHECK(sol::assets::mountAcceptsKind(MountKind::Turret, MountKind::Fixed));
    SOL_CHECK(!sol::assets::mountAcceptsKind(MountKind::Fixed, MountKind::Turret));
    SOL_CHECK(sol::assets::mountAcceptsKind(MountKind::Turret, MountKind::Turret));
    // Nothing else is relaxed - launcher and bay stay strict, and a shield
    // mount takes a shield and nothing else.
    SOL_CHECK(!sol::assets::mountAcceptsKind(MountKind::Bay, MountKind::Launcher));
    SOL_CHECK(!sol::assets::mountAcceptsKind(MountKind::Launcher, MountKind::Bay));
    SOL_CHECK(!sol::assets::mountAcceptsKind(MountKind::Utility, MountKind::Subsystem));
    SOL_CHECK(sol::assets::mountAcceptsKind(MountKind::Shield, MountKind::Shield));
    // And which side of the component/weapon line each kind sits on.
    SOL_CHECK(sol::assets::mountTakesWeapon(MountKind::Turret));
    SOL_CHECK(sol::assets::mountTakesWeapon(MountKind::Bay));
    SOL_CHECK(!sol::assets::mountTakesWeapon(MountKind::Utility));
    SOL_CHECK(!sol::assets::mountTakesWeapon(MountKind::Subsystem));
}

SOL_TEST(loadout_resolve_adds_then_muls)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kOutfittingDefs, "outfit.toml", &error));
    const ShipDef* base = db.findShip("sol.testbed");
    const ComponentDef* shield = db.findComponent("sol.shield_mk1");
    SOL_CHECK(base != nullptr && shield != nullptr);
    if (base == nullptr || shield == nullptr) {
        return;
    }

    const FittedMount fit[] = {at("shield_core", shield)};
    const ShipDef effective = sol::assets::resolveLoadout(*base, fit, {});
    // (100 + 20) * 1.5 — adds before muls.
    SOL_CHECK(nearlyEqual(effective.defense.shieldStrength, 180.0f));
    // Mass penalty: 10000 / 10500 on accelerations only.
    SOL_CHECK(nearlyEqual(effective.flight.forwardAccel, 100.0f * 10'000.0f / 10'500.0f));
    SOL_CHECK(effective.flight.maxSpeed == 200.0f); // untouched stat
    SOL_CHECK(effective.price == base->price);      // identity preserved
}

// ⚑ The resolved def IS the ship as flown: its mounts carry what is actually
// in them, which is what lets `applyShipDef` serve an NPC hull and the
// player's ship through one path. A resolve that returned the hull's authored
// defaults instead would arm the player's ship with whatever the def file
// said, forever.
SOL_TEST(loadout_resolve_writes_the_fit_onto_the_mounts)
{
    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kOutfittingDefs, "outfit.toml", &error));
    const ShipDef* base = db.findShip("sol.testbed");
    const WeaponDef* gun = db.findWeapon("sol.popgun");
    const ComponentDef* pod = db.findComponent("sol.cargo_pod");
    SOL_REQUIRE(base != nullptr && gun != nullptr && pod != nullptr);

    const FittedMount fit[] = {at("gun", gun), at("bay_port", pod)};
    const ShipDef effective = sol::assets::resolveLoadout(*base, fit, {});
    SOL_REQUIRE(effective.findMount("gun") != nullptr);
    SOL_CHECK(effective.findMount("gun")->fit == "sol.popgun");
    SOL_CHECK(effective.findMount("bay_port")->fit == "sol.cargo_pod");
    // And the mounts nobody filled are EMPTY rather than left at the def's
    // default, which is the half a "the fit is written on" check misses.
    SOL_CHECK(effective.findMount("shield_core")->fit.empty());
    SOL_CHECK(effective.findMount("bay_starboard")->fit.empty());
}

SOL_TEST(loadout_resolve_is_order_independent)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kOutfittingDefs, "outfit.toml", &error));
    const ShipDef* base = db.findShip("sol.testbed");
    const ComponentDef* shield = db.findComponent("sol.shield_mk1");
    const ComponentDef* pod = db.findComponent("sol.cargo_pod");
    SOL_CHECK(base != nullptr && shield != nullptr && pod != nullptr);
    if (base == nullptr || shield == nullptr || pod == nullptr) {
        return;
    }

    const FittedMount ab[] = {at("shield_core", shield), at("bay_port", pod)};
    const FittedMount ba[] = {at("bay_port", pod), at("shield_core", shield)};
    const ShipDef first = sol::assets::resolveLoadout(*base, ab, {});
    const ShipDef second = sol::assets::resolveLoadout(*base, ba, {});
    SOL_CHECK(first.defense.shieldStrength == second.defense.shieldStrength);
    SOL_CHECK(first.cargoCapacity == second.cargoCapacity);
    SOL_CHECK(first.flight.forwardAccel == second.flight.forwardAccel);
    SOL_CHECK(nearlyEqual(first.cargoCapacity, 75.0f));
}

SOL_TEST(loadout_validate_mounts_and_budgets)
{
    DefDatabase db;
    std::string error;
    SOL_CHECK(merge(db, kOutfittingDefs, "outfit.toml", &error));
    const ShipDef* ship = db.findShip("sol.testbed");
    const WeaponDef* gun = db.findWeapon("sol.popgun");
    const ComponentDef* shield = db.findComponent("sol.shield_mk1");
    const ComponentDef* pod = db.findComponent("sol.cargo_pod");
    const ComponentDef* bigPod = db.findComponent("sol.big_pod");
    const ComponentDef* sink = db.findComponent("sol.hungry_reactor_sink");
    const CrewDef* kim = db.findCrew("sol.engineer_kim");
    SOL_REQUIRE(ship != nullptr && gun != nullptr && shield != nullptr && pod != nullptr &&
                bigPod != nullptr && sink != nullptr && kim != nullptr);

    // A legal fit: gun, shield, two pods, 2.0/6.0 power, 1 crew.
    const FittedMount legal[] = {
        at("gun", gun), at("shield_core", shield), at("bay_port", pod), at("bay_starboard", pod)};
    const CrewDef* crew[] = {kim};
    SOL_CHECK(sol::assets::validateLoadout(*ship, legal, crew, &error));

    // A mount this hull does not have.
    const FittedMount nowhere[] = {at("turret_dorsal", shield)};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, nowhere, {}, &error));
    SOL_CHECK(error.find("turret_dorsal") != std::string::npos);

    // ⚑ Two fittings in ONE mount, which is exactly what the four slot counts
    // could never say and is the whole reason a fitting is named by its place.
    //
    // ⚑⚑ BOTH ARE THE SAME POD, AND THAT IS THE POINT - measured, not assumed.
    // The first wording put a shield booster in the second slot, and removing
    // the duplicate check left the refusal GREEN because the KIND check caught
    // it instead: a utility mount takes no shield. A check satisfied by the
    // alternative it was not testing is a check that proves nothing, so the
    // two fittings here are indistinguishable except for being two.
    const FittedMount doubled[] = {at("bay_port", pod), at("bay_port", pod)};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, doubled, {}, &error));
    SOL_CHECK(error.find("already fitted") != std::string::npos);

    // Wrong kind: a shield booster does not go in the engine mount.
    const FittedMount wrongKind[] = {at("drive", shield)};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, wrongKind, {}, &error));
    SOL_CHECK(error.find("drive") != std::string::npos);

    // Wrong size: the medium bulk pod fits `bay_port` (medium) and not
    // `bay_starboard` (small). Both halves, because a size rule that refused
    // everything would pass the second check alone.
    const FittedMount fitsMedium[] = {at("bay_port", bigPod)};
    SOL_CHECK(sol::assets::validateLoadout(*ship, fitsMedium, {}, &error));
    const FittedMount tooBig[] = {at("bay_starboard", bigPod)};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, tooBig, {}, &error));
    SOL_CHECK(error.find("bay_starboard") != std::string::npos);

    // A fitting whose def has gone is refused by name rather than skipped: the
    // mount is occupied and a fit that will not reload must not validate.
    const FittedMount missing[] = {sol::assets::FittedMount{.mountId = "bay_port"}};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, missing, {}, &error));
    SOL_CHECK(error.find("missing") != std::string::npos);

    // Power overflow.
    const FittedMount tooHungry[] = {at("bay_port", sink)};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, tooHungry, {}, &error));
    SOL_CHECK(error.find("power") != std::string::npos);

    // Berth overflow.
    const CrewDef* tooManyCrew[] = {kim, kim};
    SOL_CHECK(!sol::assets::validateLoadout(*ship, {}, tooManyCrew, &error));
    SOL_CHECK(error.find("berth") != std::string::npos);
}

SOL_TEST(loadout_scanner_components_move_scan_stats)
{
    constexpr const char* kScannerDefs = R"(
[[ship]]
id = "sol.surveyor"
name = "Surveyor"
scan_range = 6.0e7
scan_speed = 1.0
mass = 10000.0
power_output = 6.0

  [[ship.mount]]
  id = "bay_a"
  kind = "utility"
  size = "small"

  [[ship.mount]]
  id = "bay_b"
  kind = "utility"
  size = "small"

[[component]]
id = "sol.scanner_mk1"
name = "Scanner Mk1"
mount = "utility"
size = "small"
price = 1500.0
mass = 0.0
power_draw = 1.5
scan_range_mul = 1.6
scan_speed_mul = 1.25

[[component]]
id = "sol.scanner_booster"
name = "Scanner Booster"
mount = "utility"
size = "small"
price = 500.0
mass = 0.0
scan_range_add = 1.0e7
)";

    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kScannerDefs, "scanners.toml", &error));

    const ShipDef* ship = db.findShip("sol.surveyor");
    const ComponentDef* scanner = db.findComponent("sol.scanner_mk1");
    const ComponentDef* booster = db.findComponent("sol.scanner_booster");
    SOL_REQUIRE(ship != nullptr && scanner != nullptr && booster != nullptr);
    SOL_CHECK(nearlyEqual(ship->scanRange, 6.0e7f));
    SOL_CHECK(nearlyEqual(ship->scanSpeed, 1.0f));

    // Adds land before muls, the Phase 8a ordering rule, so the booster's
    // 10,000 km rides the scanner's multiplier: (6e7 + 1e7) * 1.6.
    const FittedMount fit[] = {at("bay_a", scanner), at("bay_b", booster)};
    const ShipDef resolved = sol::assets::resolveLoadout(*ship, fit, {});
    SOL_CHECK(std::fabs(resolved.scanRange - 1.12e8f) < 1.0e3f);
    SOL_CHECK(nearlyEqual(resolved.scanSpeed, 1.25f));

    // Order-independence: the same fit the other way round resolves equal.
    const FittedMount swapped[] = {at("bay_b", booster), at("bay_a", scanner)};
    const ShipDef other = sol::assets::resolveLoadout(*ship, swapped, {});
    SOL_CHECK(other.scanRange == resolved.scanRange);
    SOL_CHECK(other.scanSpeed == resolved.scanSpeed);
}

SOL_TEST(loadout_collector_rigs_move_collector_range)
{
    constexpr const char* kMinerDefs = R"(
[[ship]]
id = "sol.prospector"
name = "Prospector"
collector_range = 250.0
mass = 10000.0
power_output = 6.0

  [[ship.mount]]
  id = "rig"
  kind = "utility"
  size = "small"

[[component]]
id = "sol.collector_mk1"
name = "Collector Rig Mk1"
mount = "utility"
size = "small"
price = 1100.0
mass = 0.0
power_draw = 1.5
collector_range_mul = 3.0
)";

    DefDatabase db;
    std::string error;
    SOL_REQUIRE(merge(db, kMinerDefs, "miners.toml", &error));

    const ShipDef* ship = db.findShip("sol.prospector");
    const ComponentDef* collector = db.findComponent("sol.collector_mk1");
    SOL_REQUIRE(ship != nullptr && collector != nullptr);
    SOL_CHECK(nearlyEqual(ship->collectorRange, 250.0f));

    // A collector rig is an ordinary utility component: it moves collector_range
    // exactly the way a scanner moves scan_range.
    const FittedMount fit[] = {at("rig", collector)};
    const ShipDef resolved = sol::assets::resolveLoadout(*ship, fit, {});
    SOL_CHECK(nearlyEqual(resolved.collectorRange, 750.0f));
}
