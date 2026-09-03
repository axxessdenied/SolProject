#pragma once

#include "combat_effects.hpp"
#include "save_catalog.hpp"
#include "scene_renderer.hpp"
#include "thruster_particles.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/core/math/math.hpp"
#include "sol/core/random.hpp"
#include "sol/ecs/ecs.hpp"
#include "sol/sim/collision.hpp"
#include "sol/sim/damage.hpp"
#include "sol/sim/docking.hpp"
#include "sol/sim/economy.hpp"
#include "sol/sim/faction_sim.hpp"
#include "sol/sim/flight.hpp"
#include "sol/sim/jump_transition.hpp"
#include "sol/sim/mining.hpp"
#include "sol/sim/missions.hpp"
#include "sol/sim/pilot_tips.hpp"
#include "sol/sim/power.hpp"
#include "sol/sim/predation.hpp"
#include "sol/sim/steering.hpp"
#include "sol/sim/survey.hpp"
#include "sol/sim/universe.hpp"
#include "sol/ui/screens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// The live game world (Phase 7): a seeded procedural galaxy of which exactly
// one system — the player's — is instantiated at full fidelity (sim-LOD
// bubble); jumping through a gate demotes the old system to specs and
// promotes the destination. Positions are meters in the current system's
// barycenter frame (large-world rule: DVec3 in sim, camera-relative floats
// only at render time).

struct Transform
{
    sol::core::DVec3 position;
    sol::core::DVec3 previousPosition;
    sol::core::Quat orientation = sol::core::Quat::identity();
    sol::core::Quat previousOrientation = sol::core::Quat::identity();
};

// Ship dynamic state beyond the transform (the flight model's ShipState is
// assembled from Transform + this each tick).
struct FlightBody
{
    sol::core::DVec3 velocity;       // m/s, sim space
    sol::core::Vec3 angularVelocity; // rad/s, body space
};

// Marks the player's ship; exactly one alive at a time. Serialized so player
// identity survives save/load now that NPCs fly too (Phase 6).
struct PlayerShip
{
    std::uint8_t reserved = 0; // no state yet; explicit byte keeps the POD honest
};

// Per-ship flight tuning and commanded input. The player's input is latched
// from the input mapper each tick; NPC input comes from pilots (Phase 6 AI).
struct ShipControl
{
    sol::sim::ShipTuning tuning;
    sol::sim::FlightInput input;
};

// Per-ship power tuning + state (decisions/003: Elite-style pips). ENG
// allocation scales the flight envelope each tick; WEP feeds the capacitor.
struct ShipPower
{
    sol::sim::PowerTuning tuning;
    sol::sim::PowerState state;
};

// Per-ship defenses (decisions/002: directional shields) fed from the def.
struct ShipDefense
{
    sol::sim::DefenseTuning tuning;
    sol::sim::DefenseState state;
    // Counts down from kAssistSeconds each time the player lands damage here
    // (Phase 8l). Positive at the moment of death means the player was in the
    // fight, so a bounty counts the kill even when someone else finished it.
    // Transient by design: spawned ships are repopulated rather than saved.
    double playerAssist = 0.0;
};

enum class WeaponKind : std::uint32_t
{
    None = 0,
    Projectile,
    Hitscan,
};

// ONE gun as fitted to ONE mount, flattened from its def (POD for the
// snapshot). Until Phase 31 stage C1 this WAS the ship's armament - singular,
// because a hull had a single `weaponId`. It is now an element of
// `ShipArmament` below, and the only field the change added is where it sits.
struct ShipWeapon
{
    WeaponKind kind = WeaponKind::None;
    float damage = 0.0f;
    float rateOfFire = 1.0f;      // shots/s
    float range = 1'000.0f;       // meters
    float projectileSpeed = 0.0f; // m/s (projectile kind)
    float energyCost = 0.0f;      // capacitor draw per shot
    float cooldown = 0.0f;        // seconds until the next shot
    float miningPower = 0.0f;     // Phase 8f: yield units cut per second
    // The muzzle: its mount's `at`, in the HULL FRAME AT SCALE 1, exactly as
    // authored (Phase 31 stage C1). The firing pass multiplies by the hull's
    // scale and rotates it, so a `scale = 4.0` freighter's turret sits four
    // times as far off its centreline - the same rule every other authored
    // length on a hull obeys.
    //
    // ⚑ It is stored here rather than looked up because this component is
    // FLATTENED and keeps no def id - the same reason `boltModel` is here,
    // and the same reason an NPC's guns survive a save with no def in sight.
    float at[3] = {0.0f, 0.0f, 0.0f};
    // Where the mount points with nothing laid on it, and how far round it
    // will go (Phase 31 stage C2). Hull frame, and copied from the mount for
    // the same reason `at` is: this component is flattened and keeps no def id.
    //
    // ⚑ THE DEFAULT IS THE SHIP'S OWN NOSE, which is why every gun in the game
    // before C2 kept firing exactly where it did. `arc` is the FULL cone angle
    // centred on `aim` (see `sim::layWithinArc`), so zero is a gun bolted down
    // and a hull that authors neither key has a gun that is bolted down
    // pointing forward - the pre-C2 rule, arrived at rather than special-cased.
    float aim[3] = {0.0f, 0.0f, -1.0f};
    float arc = 0.0f; // degrees of traverse; 0 = bolted down
    // ⚑ WHICH TRIGGER THIS GUN ANSWERS TO (Phase 31 stage C3). 1-based, and
    // ONE is the default for every gun on every hull, which is why a ship
    // nobody has regrouped fires exactly as it did before C3 - including
    // every NPC in the galaxy, which never touches its selection.
    //
    // ⚑⚑ IT IS NOT AN AUTHORED KEY AND `ships.toml` CANNOT SET IT. A fire
    // group is the PILOT'S, not the hull's: a hull that shipped a gun in
    // group 2 would be a hull whose gun an NPC never fires, because an NPC
    // has no console to change the selection with. `applyShipDef` therefore
    // gives every gun group 1 and `applyPilotFireGroups` - player only -
    // overwrites from the saved fit.
    std::uint32_t group = 1;
    // Which of the hull's mounts this gun came out of: an index into
    // `ShipDef::mounts`. It is here so that "which mount is this gun in" is a
    // fact the gun carries rather than a walk two callers reproduce - the
    // drift that a second copy of `applyShipDef`'s skip conditions would be.
    // Stage C3 needs it to point a group change at one gun; stages E and F
    // need it to draw a fitting and to damage one.
    std::uint32_t mount = 0;
    // What its bolt draws as (Phase 19), resolved once when the loadout is
    // applied. It lives here rather than being looked up at the muzzle
    // because this component is FLATTENED from its def and keeps no def id -
    // which is exactly why the bolt was a hardcoded "cube" until now.
    ModelId boltModel = kNoModel;
    // ⚑⚑ WHAT THE GUN ITSELF DRAWS AS, STANDING IN ITS MOUNT (Phase 31 stage
    // E). `kNoModel` means it is not drawn, which is what every gun in the
    // game did before this stage - so a weapon def naming no mesh still fits
    // and still fires and simply leaves the hardpoint bare.
    //
    // ⚑ It is beside `boltModel` for the same reason `at` and `aim` are here:
    // the component is flattened and keeps no def id, so anything the draw
    // needs has to be resolved at `applyShipDef` and carried. That is also why
    // this is the field that made `ShipArmament` wider again, and why
    // `kSaveVersion` moved with it.
    ModelId fittingModel = kNoModel;
};

// ⚑⚑ A HARD CEILING ON GUNS PER HULL, AND IT BUYS SOMETHING REAL. The ECS
// snapshot stores components by memcpy (`snapshot.hpp` static_asserts it), so
// a ship's guns are either a bounded array here or a set of separate entities
// - and the entity shape is a trap this world is already shaped to spring:
// `despawnSystem` destroys everything in the TRANSFORM pool, a gun entity has
// no Transform, and `Registry::destroy` recycles indices. An orphaned gun
// would therefore survive a jump and then re-attach itself to whatever spawned
// into its owner's old slot. An array on the ship entity cannot outlive the
// ship, which makes that whole class inexpressible rather than merely absent.
//
// Sixteen covers gdd.md 11.1's mount BUDGET through class 4 (14-22 mounts
// total, guns a subset of that), which is the whole of Phase 32's hull spine.
// A hull that declares more is not silently truncated - `applyShipDef` says
// so, names the hull, and fits the first sixteen.
inline constexpr std::uint32_t kMaxShipWeapons = 16;

// ⚑ HOW MANY TRIGGERS A SHIP HAS (Phase 31 stage C3). Four rather than one
// per gun: a selection the player steps through with one key is only usable
// while the cycle is short, and sixteen positions to walk past is a menu
// pretending to be a control. Four is what gdd.md 11.1's mount budget through
// class 4 can meaningfully split - a main battery, a secondary, ordnance and
// the tools - and it is the same count as the power pips beside it on the
// panel, which is not a coincidence a player has to be told about.
inline constexpr std::uint32_t kFireGroupCount = 4;

// What a ship has fitted, in the hull's own MOUNT ORDER (Phase 31 stage C1).
//
// ⚑ MOUNT ORDER IS FIRING PRIORITY, and that is the whole of "per-mount
// capacitor draw". Each gun pays its own `energyCost` as it fires, so a ship
// whose capacitor cannot cover a full salvo fires the guns the author listed
// first and the rest simply do not go off that tick. The rule is authored,
// visible in the def file, and needs no new key: the alternative - splitting
// the charge evenly, or refusing the whole salvo - would make a second gun
// make the first one WORSE, which is not what bolting a gun to a hull does.
struct ShipArmament
{
    std::uint32_t count = 0;
    // ⚑ WHICH GROUP THE TRIGGER IS ON (Phase 31 stage C3), 1-based. A gun
    // fires when its own `group` matches this and holds otherwise - it still
    // ticks its cooldown, for the same reason a gun ticks it with the trigger
    // up: a clock that only runs while a gun is selected would hand every
    // group a free first shot on the frame you switched to it.
    //
    // ⚑ IT IS NEVER LEFT POINTING AT AN EMPTY GROUP. `normalizeFireGroup`
    // moves it to the lowest group that has a gun in it, which is what stops a
    // refit or a regroup from leaving the player holding a trigger that is
    // wired to nothing and saying nothing about why.
    std::uint32_t selectedGroup = 1;
    ShipWeapon weapons[kMaxShipWeapons];
};

// ⚑⚑ WHAT ELSE IS BOLTED TO THE OUTSIDE OF THIS HULL (Phase 31 stage E2):
// every EXTERNAL mount holding a `[[component]]` that names a mesh, flattened
// the way `ShipArmament` flattens the guns and for the same three reasons - the
// snapshot stores components by memcpy, a fitting must not be able to outlive
// its ship, and the draw path has no route from an entity back to a def.
//
// ⚑⚑ IT IS A SEPARATE COMPONENT FROM `ShipArmament` BECAUSE A GUN AND A POD
// ARE DIFFERENT THINGS TO DRAW, not because they are different kinds of kit. A
// gun is LAID: `layGun` answers where it points, every frame, and the answer
// depends on a target. Nothing here moves at all - a hold pod bolted to the
// belly is at its mount's `at` facing its mount's `aim` for the life of the
// fit. Folding the two together would mean asking the gunnery question about a
// cargo pod, once per pod per frame, to be told it points where it always did.
//
// ⚑ INTERNAL MOUNTS ARE NOT IN HERE. `decisions/014` rule 2: absent `at`
// means internal, which means never drawn. Such a mount still exists and is
// still destructible - that is stage F's business, and stage F will want a list
// that includes them. This one is what the RENDERER needs, so it holds exactly
// what is drawn.
struct FittedPart
{
    // Hull frame at scale 1, exactly as authored - the muzzle rule from C1,
    // applied to something that does not shoot.
    float at[3] = {0.0f, 0.0f, 0.0f};
    // Which way it faces, which for a fitting that does not traverse is the
    // whole of its orientation. See `mountRotation`.
    float aim[3] = {0.0f, 0.0f, -1.0f};
    // Which of the hull's mounts this came out of, for the same reason
    // `ShipWeapon::mount` carries it: here is the only walk that knows.
    std::uint32_t mount = 0;
    ModelId model = kNoModel;
};

// ⚑ Sixteen, matching `kMaxShipWeapons` and for the same reason - gdd.md
// §11.1's mount budget through class 4 - and a hull that declares more is
// warned by name rather than truncated in silence.
inline constexpr std::uint32_t kMaxDrawnFittings = 16;

struct ShipFittings
{
    std::uint32_t count = 0;
    FittedPart parts[kMaxDrawnFittings];
};

// ⚑⚑ EVERY PLACE ON THIS HULL AND HOW MUCH OF IT IS LEFT (Phase 31 stage F).
// Thirty-two rather than sixteen because this one is EVERY mount, not the guns
// or the drawn kit: gdd.md §11.1 budgets 14-22 through class 4 and a hull is
// free to spend that on subsystems nobody draws and no trigger reaches.
inline constexpr std::uint32_t kMaxShipMounts = 32;

// One place on the hull, and what it would take to knock it out.
//
// ⚑⚑ CONDITION BELONGS TO THE MOUNT AND NOT TO WHAT IS IN IT, and the shipped
// content is what settles that rather than taste. Not one `[[ship.mount]]` of
// kind `engine` in this game carries a `fit` - the shuttle's `drive_main`, the
// interceptor's pair and the freighter's are all bare mounts - so a condition
// that lived on the FITTING would leave Phase 31's own exit criterion ("shoot
// a freighter's drive off and watch it stop") unreachable in shipped content.
// It is also what `decisions/014` says in as many words: *each mount carries
// hit points*. A drive bell is part of the hull, and an empty turret ring is
// still a ring somebody can shoot away.
struct MountCondition
{
    // Hull frame at scale 1, exactly as authored - the same copy `ShipWeapon`
    // and `FittedPart` make, and for the same reason: this component is
    // FLATTENED and keeps no def id, so a hit arriving in world space has
    // nothing else to resolve itself against.
    //
    // ⚑ Only the BEARING of this is ever read, never its length, which is why
    // the hull's scale never enters the hit test: a mount is four times
    // further out on a `scale = 4.0` freighter and sits on exactly the same
    // line from the hull's centre.
    float at[3] = {0.0f, 0.0f, 0.0f};
    float hp = 0.0f;
    float maxHp = 0.0f;
    // ⚑⚑ WHAT KIND OF PLACE THIS IS (Phase 31 stage F2), and the reason it is
    // carried rather than looked up is the reason everything else here is: the
    // component is FLATTENED and keeps no def id. Stage F1 did not need it
    // because a destroyed mount only had to stop being drawn and stop firing,
    // and the gun already knew which mount it was in. "A destroyed DRIVE stops
    // the ship" is the first question that has to be asked of the mount list
    // itself, with no fitting to ask on its behalf - and it has to be asked
    // every tick, on the flight path, of every ship in the system.
    sol::assets::MountKind kind = sol::assets::MountKind::Utility;
    // `decisions/014` rule 2, carried rather than inferred from `at`. A mount
    // authored at the hull's own centre is a legal external mount and an
    // internal one is not merely a mount at the origin - one is aimed at and
    // the other is reached through the armour.
    bool external = false;

    [[nodiscard]] bool destroyed() const { return maxHp > 0.0f && hp <= 0.0f; }
};

// ⚑⚑ INDEXED THE SAME WAY `def.mounts` IS, WHICH IS THE WHOLE POINT. Both
// `ShipWeapon::mount` and `FittedPart::mount` are indices into the hull's mount
// list, so a gun or a drawn pod reaches its own condition without a search and
// without a second copy of `applyShipDef`'s skip conditions. A hull declaring
// more than `kMaxShipMounts` is warned by name and its overflow mounts are
// indestructible rather than silently reindexed - anything else would point a
// gun at somebody else's condition.
struct ShipMounts
{
    std::uint32_t count = 0;
    MountCondition mounts[kMaxShipMounts];
};

// ⚑⚑ HOW MUCH OF A HULL'S MAIN DRIVE IS STILL THERE (Phase 31 stage F2): the
// share of its `engine` mounts that have not been shot off, and the whole of
// "a destroyed drive that stops working".
//
// ⚑ A HULL THAT DECLARES NO ENGINE MOUNT FLIES EXACTLY AS IT ALWAYS DID, which
// is what the 1.0 is for and is not a fallback so much as the rule read
// carefully: a drive you cannot shoot off is a drive that cannot be missing.
// Half this project's test hulls declare no engine mount, and neither does a
// station, a rock or a wreck.
[[nodiscard]] float driveFraction(const ShipMounts& mounts);

// And whether its shield generators are still there. False ONLY for a hull
// that declares `shield` mounts and has had every one of them destroyed - on
// the same reading, and for the same reason, as `driveFraction`'s 1.0.
[[nodiscard]] bool shieldsArePowered(const ShipMounts& mounts);

// Puts every mount back to full without touching the list itself. The mounts a
// hull has do not change - only how much of each is left - so this is what a
// repair IS, and it is deliberately separate from `applyShipDef`'s rebuild:
// that one is a refit and resets the defences and the guns with it.
void repairMounts(ShipMounts& mounts);

// ⚑ HOW WIDE A HIT HAS TO LAND TO COUNT AS HITTING A MOUNT (Phase 31 stage F),
// as the cosine of the half-angle between where the shot arrived and where the
// mount sits, both measured as bearings from the hull's centre.
//
// A bearing rather than a distance because a hit position is a point on the
// ship's COLLISION SPHERE, which is a good deal bigger than the mesh inside it
// (the shuttle's sphere is 8 m and its hull is a 12 m wedge) - so how far the
// impact is from a mount is mostly a fact about the sphere, while which way it
// came in is the question a player is actually asking when they line up on a
// freighter's tail.
//
// Thirty degrees is measured against the shipped freighter, whose mounts are
// the closest together in the game: its aftmost cargo pod and its drive are 25
// degrees apart, so a cone this wide has them overlapping and the NEAREST
// bearing wins - which is the behaviour worth having, because a shot into the
// tail should take the drive rather than nothing.
inline constexpr float kMountHitCosine = 0.866f; // cos(30 degrees)

// The groups this ship's guns actually occupy, as a bit per group: bit (n-1)
// is set when at least one gun is in group n. Zero means nothing is fitted.
// One bit set means there is no choice to make, which is what the HUD reads to
// decide whether the fire-group row is a control or clutter.
[[nodiscard]] std::uint32_t fireGroupsInUse(const ShipArmament& armament);

// Points `selectedGroup` at a group that has a gun in it, leaving it alone
// when it already does. Called after anything that can change which groups
// exist: a refit, a regroup, a ship swap.
void normalizeFireGroup(ShipArmament& armament);

// What the rest of the game needs to know about a ship's guns without walking
// them (Phase 31 stage C1). Every field answers exactly one question, and they
// are deliberately not all about the same gun: the reach that decides where a
// fight starts and the beam that decides whether a rock can be cut are
// different facts, and a summary that conflated them would be the "check that
// sums two alternatives" bug this project has now found twice.
//
// ⚑⚑ SINCE STAGE C3 IT DESCRIBES THE SELECTED FIRE GROUP, NOT EVERY GUN
// ABOARD, because every one of these fields is read to predict WHAT HAPPENS
// WHEN THE TRIGGER GOES DOWN. A lead marker drawn for a cannon sitting in an
// unselected group points at where a bolt that is not coming would have gone,
// and a mining prompt lit by a beam the trigger is not wired to says the rock
// can be cut when holding the trigger cuts nothing. Nothing changes for an
// NPC: every gun it carries is in group 1 and group 1 is what it has selected.
struct ArmamentSummary
{
    bool armed = false;    // anything at all fitted that can fire
    float maxRange = 0.0f; // the longest gun's reach: where fighting becomes possible
    // The first PROJECTILE gun's speed in mount order, for the lead solution.
    // Zero when every fitted gun is hitscan, which the caller reads as
    // "instant" - the same meaning the singular weapon gave it.
    float leadSpeed = 0.0f;
    bool canMine = false;     // any fitted beam with mining_power
    float miningRange = 0.0f; // the furthest of THOSE, not of all guns
};

// Everything a gun needs to know about the SHIP carrying it, read once
// (Phase 31 stage C2). The firing pass builds one of these per ship and then
// walks that ship's guns against it, which is the nesting the capacitor forced
// in C1 and which C2 leans on again: where the hull is, which way it points
// and what it has laid its guns on are facts about the ship, and only `aim`,
// `arc` and reach differ gun to gun.
struct GunneryFrame
{
    sol::core::DVec3 position;
    sol::core::Quat orientation;
    sol::core::DVec3 velocity;
    // The pilot's nose. A turret with nothing to shoot at follows it, so a
    // ship with no target fires exactly where it did before C2.
    sol::core::DVec3 forward{0.0, 0.0, -1.0};
    double hullScale = 1.0;

    // ⚑ THE ONE THING A GUN CAN BE LAID ON IS A SHIP. A station, a planet, a
    // gate and an ore field are all things this game lets you SELECT, and none
    // of them is a gunnery target - so a turret ignores them and keeps looking
    // down the nose. That is not a nicety: the only turret in the shipped game
    // carries the mining laser, and a ring that swung to the station in the
    // distance would take the beam off the rock the pilot is pointing at.
    bool hasTarget = false;
    sol::core::DVec3 targetPosition;
    sol::core::DVec3 targetVelocity;
};

// Where one gun points, and whether it can point there (Phase 31 stage C2).
// The ONE definition of a gun's bearing: the firing pass fires along it, the
// console probe reports it, and stage E will draw the turret down it.
//
// `outMuzzle` is always set. `outBearing` is set even on a refusal, where it
// is the gun swung as far round as its ring goes - see `sim::layWithinArc`.
// The return is whether the gun BEARS, and a gun that does not bear holds its
// fire rather than spending a shot it cannot land.
[[nodiscard]] bool layGun(const GunneryFrame& frame,
                          const ShipWeapon& weapon,
                          sol::core::DVec3& outMuzzle,
                          sol::core::DVec3& outBearing);

// ⚑⚑ HOW A FITTING STANDS IN ITS MOUNT (Phase 31 stage E). Both arguments
// are in the HULL's frame and so is the answer, which is what keeps a drawn
// gun rigidly attached to the hull: the traverse comes off the sim tick and
// the hull's own pose comes off the interpolated render transform, and mixing
// the two frames is how a turret ends up swimming across its own ship at low
// tick rates.
//
// TWO vectors rather than one, because a shortest-arc rotation onto the
// bearing leaves the ROLL free and a gun is not rotationally symmetric about
// its barrel:
//
//   -Z of the model goes onto `bearing`, wherever the gunner has laid it;
//   +Y of the model goes as near `mountAim` as a right angle allows.
//
// `mountAim` is the mount's own `aim` - which for an external mount points out
// of the hull - so the gun's base plate sits flat against the plating and a
// VENTRAL ring hangs upside down from it rather than standing on its head.
// That case is not hypothetical: it is the shipped freighter's `turret_ventral`
// exactly, and it is the whole reason the roll is not left free.
//
// ⚑ A bearing parallel to the aim leaves no roll to choose and any answer is
// as good as another - a gun laid straight up out of its own ring. The
// shortest arc is returned rather than a refusal, because there is nothing
// wrong with the gun.
[[nodiscard]] sol::core::Quat fittingRotation(sol::core::Vec3 bearing, sol::core::Vec3 mountAim);

// ⚑⚑⚑ HOW A FITTING THAT DOES NOT TRAVERSE STANDS IN ITS MOUNT (Phase 31
// stage E2). Hull frame in, hull frame out, exactly like `fittingRotation` -
// and it is a SECOND function rather than a second argument to that one because
// the two pin different things:
//
//   a GUN's rotation is pinned by where it SHOOTS. Its -Z must be exactly the
//   bearing `layGun` answered, or the drawn barrel and the bolt disagree, and
//   the roll is whatever is left over.
//
//   a POD's rotation is pinned by where it is BOLTED. Its +Y must be exactly
//   the mount's `aim` - the way out of the plating - or it floats off the hull
//   or sinks into it, and the roll is whatever is left over.
//
// Same construction, opposite constraint, and neither can be expressed as the
// other: swapping them would leave a nozzle's bell pointing up out of the deck
// exactly when `aim` is the one direction (dead astern) an engine actually uses.
//
// ⚑ Every fitting mesh is authored the same way - mounting face at the origin,
// body extending along +Y - so `aim` is the one key an author sets and the mesh
// needs no per-kind convention. `reference` settles the roll: the hull's own
// nose, so a hold pod lies fore-and-aft rather than across the belly.
[[nodiscard]] sol::core::Quat mountRotation(sol::core::Vec3 aim, sol::core::Vec3 reference);

// A live bolt: Transform carries the position, this the rest.
struct Projectile
{
    sol::core::DVec3 velocity; // sim space, m/s
    double lifetime = 0.0;     // seconds remaining
    float damage = 0.0f;
    std::uint32_t shooterIndex = 0; // entity index; never hits its shooter
};

// What the PLAYER's ship has been told to do for itself (engine plan Phase 28).
// Distinct from PilotState below, which is what an NPC's Lua brain has decided:
// these are orders a person gave, and the vocabulary is deliberately one a
// captain could accept unchanged when Phase 39 arrives.
//
// ⚑ Autopilot is the only TERMINATING member — it arrives and disengages. Every
// other member is STANDING: it holds until it is ended, its subject is lost, or
// the ship docks. See the engage/clear block on SpaceWorld for why the two
// kinds answer a stick nudge differently.
enum class CommandMode : std::uint32_t
{
    None = 0,
    Autopilot,    // fly to the target and stop at the arrival range
    Orbit,        // circle the target at orbitRange()
    MatchSpeed,   // hold the current offset from the target, velocity-matched
    KeepDistance, // settle at keepDistanceRange() from the target
    Hold,         // stop, and stay where you are
    Follow,       // sit off the target's shoulder and stay there
};

// Whether a mode is one the ship holds until told otherwise. The single place
// this question is answered, because three separate guards ask it.
[[nodiscard]] constexpr bool isStandingCommand(CommandMode mode)
{
    return mode != CommandMode::None && mode != CommandMode::Autopilot;
}

// Whether a mode is meaningless without something to point it at. Hold is the
// one command that needs no target: it is about where the ship IS.
[[nodiscard]] constexpr bool commandNeedsTarget(CommandMode mode)
{
    return mode != CommandMode::None && mode != CommandMode::Hold;
}

// Prose, for logs and menus: "Keep Distance".
[[nodiscard]] const char* commandModeName(CommandMode mode);

// The HUD chip, and a separate function on purpose: the flight panel drops a
// chip whole rather than clipping it when the row runs out of width, so these
// are short and upper-case to match ASSIST / BOOST / CRUISE beside them. ""
// when nothing is engaged, which is what makes the chip vanish.
[[nodiscard]] const char* commandModeChip(CommandMode mode);

// ⚑⚑⚑⚑ WHAT A PILOT CAME OUT TO DO, AND SINCE PHASE 37 STAGE D IT IS NOT
// ALWAYS THE CELL THAT DECIDES. The first three come from the `RosterCell` a
// wing was sent as - `spawnWing`'s own comment says "the job is the cell's, not
// a caller's opinion of it" - and that is right for a hull that could be doing
// any of them. `Covert` is the exception, and it is an exception on purpose:
// a `HullRole::Covert` hull is a statement about the job BEFORE anybody is sent
// anywhere, so the hull overrides the cell. See `pilotRoleFor`.
//
// ⚑⚑ APPENDED, AND IT COSTS NO SAVE BUMP. `ShipPilot::role` is already a
// `std::uint32_t` in the entity snapshot, so a new VALUE rides in a field that
// is already the right width - which is the distinction Phase 36 stage C
// established when it added an inspection state, against a new FIELD which
// would have moved every record.
enum class PilotRole : std::uint32_t
{
    Fighter = 0,
    Trader,
    Patrol,
    Covert,
};

enum class PilotState : std::uint32_t
{
    Idle = 0,
    Patrol, // fly to waypoint
    Attack, // pursue + shoot target
    Flee,   // evade target
    // Long-haul travel to waypoint on the cruise drive (Phase 8x). Patrol's
    // steering is combat-scale — it closes to 50 m and stops — and a trade
    // leg is hundreds of thousands of kilometres, which is a distance only
    // the cruise envelope crosses in a sane time. This is the same
    // steerTravel the player's autopilot flies.
    Travel,
    // Running an inspection on the player (Phase 36 stage C): nose held on
    // them, closing to a standoff, not shooting. A STATE rather than a flag on
    // `ShipPilot` because a flag would be a new FIELD on snapshot component 19
    // and that is a save-format promise; a new enum VALUE in a field that is
    // already a uint32 is not. A pilot loaded back in this state with no hold
    // record behind it is put back on its beat by `tickInspection`.
    Inspect,
};

// An NPC pilot: Lua's pilot_think picks the state (strategy); C++ steering
// flies it every tick (engine plan §Scripting split).
struct ShipPilot
{
    PilotRole role = PilotRole::Fighter;
    PilotState state = PilotState::Idle;
    std::uint32_t targetIndex = 0;
    std::uint32_t hasTarget = 0;
    sol::core::DVec3 waypoint;
    float thinkTimer = 0.0f; // counts down to the next Lua think
    float weavePhase = 0.0f;
    // Who last shot this ship, and how long that is still worth acting on
    // (Phase 8x). Before this a pilot had no idea it was being attacked: it
    // read its own hull and nothing else, so a hauler waited to lose 40% of it
    // before running, and ran from whatever entity 0 happened to be. The
    // window is short on purpose - a threat you have not seen in a few seconds
    // is a threat you have lost, not one you keep hunting forever.
    std::uint32_t threatIndex = 0;
    float threatTimer = 0.0f;
    // Faction table index (Phase 8b), or ~0u for unaffiliated console spawns
    // (which Lua treats as unconditionally player-hostile, the pre-8b rule).
    std::uint32_t factionIndex = 0xffff'ffffu;
    // Seconds left on a dispatched response (Phase 30 stage C). Non-zero is
    // also the MARKER that says this pilot is answering a call rather than
    // flying its own business, which the Travel case needs: a responder that
    // arrives and finds nothing has to go back to its patrol, where a trader
    // puppet arriving at its pad deliberately holds station instead.
    float respondTimer = 0.0f;
};

// One trader body, for the console probe that proves the promotion happened.
struct TraderPuppetInfo
{
    std::uint32_t traderIndex = 0;
    std::string name;
    double distance = 0.0; // from the player, meters
    double speed = 0.0;
    const char* state = "";
};

// A body for one coarse EconomyTrader (Phase 8x). The entity is a view and
// the trader is the record: this holds only the index that ties them, so
// nothing about the haul can drift out of step with the economy that owns it.
// Deliberately not serialised — a puppet is rebuilt from the record on load,
// exactly as ambient wings are.
struct TraderPuppet
{
    std::uint32_t traderIndex = 0;
    // Where this leg ends, in system space. Recomputed when the leg changes
    // rather than every tick, because it only moves when the record does.
    sol::core::DVec3 destination;
    // The record moved it this tick rather than its engines (Phase 8x stage
    // 4). Written by the reconcile, read by prey selection: through the paced
    // middle of a leg the schedule outruns every hull in the game, so a hunter
    // that locked on would chase it forever without ever firing.
    std::uint32_t paced = 0;
};

// A body for an extractor station's ore draw (Phase 8x stage 6). The same
// puppet relationship a trader has, against a different coarse actor: an
// outpost marked produces_from = "field" pulls real rock out of MiningSim
// every economy tick whether or not anyone is watching, and this is that draw
// with a ship on the end of it. The market is the record; the entity is the
// view. Transient like TraderPuppet — rebuilt from the books on load.
struct MinerPuppet
{
    std::uint32_t market = 0;          // economy market index of the outpost it feeds
    std::uint32_t commodity = 0;       // what that outpost digs
    std::uint32_t field = 0;           // asteroid field it is working
    std::uint32_t rock = 0xffff'ffffu; // entity index of the rock, or none
    float rockSeconds = 0.0f;          // time left before it moves to the next
    std::uint32_t rockStep = 0;        // which rock in the field it took last
};

// One miner body, for the console probe. Same job as TraderPuppetInfo: the
// failure mode of a promotion is the record and the sky disagreeing, so this
// reports what is drawn and which outpost's draw it stands for.
struct MinerPuppetInfo
{
    std::uint32_t market = 0;
    std::string name;
    std::string station;
    double distance = 0.0;     // from the player, meters
    double rockDistance = 0.0; // from the rock it is working, meters
    double speed = 0.0;        // moving means it is between rocks, not working one
    bool working = false;      // it has a rock at all
};

// One fighter and whatever it is going for, for the console probe. The
// promotion's failure mode is the record and the sky disagreeing, and
// predation's is a raider that says it is hunting while flying nowhere - so
// this reports every fighter, hunting or not, and what it settled for.
// One hull currently answering a call (Phase 30 stage C), for the console probe
// and the tests. Same shape and same reason as HunterInfo below it: a response
// is a thing you have to be able to ASK about, because the alternative is
// waiting out a flight that may correctly never start.
struct ResponderInfo
{
    std::string name;
    double distanceToIncident = 0.0; // meters, to the point it was sent to
    double secondsLeft = 0.0;        // before it gives up and goes back on beat
    // ⚑ Every responder must be in `Travel`. Carried explicitly rather than
    // left implied because it is the exact thing decisions/019 §3 got wrong:
    // `Patrol` is combat-scale steering and would grind across the system.
    PilotState state = PilotState::Idle;
    sol::core::DVec3 position;
    bool pirate = false; // down the negative band the responders ARE the clan
};

struct HunterInfo
{
    std::string name;
    std::string prey;              // what it is going for, hauler or otherwise
    std::uint32_t traderIndex = 0; // which coarse trader, when prey is a hauler
    double distance = 0.0;         // to its target, meters
    const char* state = "";
    bool hunting = false; // its target is a trader body
};

// Runtime faction identity (Phase 8b): authored majors first, generated
// pirate clans after, aligned with sim::SystemSpec::factionIndex and the
// FactionSim agent order. Clans jitter their template def per clan seed.
struct GameFaction
{
    std::string defId; // majors: own def id; clans: template def id (gates)
    std::string name;
    sol::core::Vec3 color = {1.0f, 1.0f, 1.0f};
    // ⚑⚑⚑⚑ THIS WAS `bool pirate` AND ONE BIT WAS ANSWERING SIX UNRELATED
    // QUESTIONS (Phase 37 stage B). `FactionKind` used to be an asset-layer
    // word that `initializeFactions` read once and threw away, so nothing
    // downstream could ask what KIND a faction was - only whether it was a
    // pirate. Six sites read that bit and meant six different things by it:
    // whether this owner fences past `min_rep`, the player's opening standing,
    // the default cross-kind enmity, raiders-or-patrols, which `RosterCell`
    // its wings fly, and which way the security sign points.
    //
    // ⚑⚑ A SECOND BOOL BESIDE IT WOULD HAVE BEEN THE SAME BUG WITH MORE
    // STATES - `pirate && shadow` is nonsense and nothing but convention would
    // have stopped it. The kind is carried instead and `pirate()` is derived,
    // so every one of those six sites still reads exactly the sentence it read
    // before and the fourth state is unrepresentable.
    //
    // ⚑⚑⚑ AND FIVE OF THE SIX TURNED OUT TO BE UNREACHABLE FOR A SHADOW
    // FACTION WITHOUT A LINE BEING WRITTEN, which is worth saying plainly
    // because the spec budgeted a third arm for all six. Every one of them
    // except the opening standing is keyed off `systemOwnerFaction` - and a
    // faction that claims nothing is never a system's owner. "Claims nothing"
    // is not a property this table has to enforce; it is what makes those five
    // questions unaskable.
    //
    // ⚑ Clans carry their TEMPLATE's kind, exactly as they carry its `defId`
    // and its roster: a clan stamped from a pirate template is a pirate.
    sol::assets::FactionKind kind = sol::assets::FactionKind::Major;

    // "Raids and is raided", which is the behaviour the old bool named. Kept as
    // the spelling every reader already used so the kind's arrival is not a
    // rewrite of six unrelated decisions.
    [[nodiscard]] bool pirate() const { return kind == sol::assets::FactionKind::Pirate; }

    // ⚑ The black market (Phase 37, `decisions/017`): no territory, no
    // stations of its own, present wherever a station composed a shadow
    // module. Its rows sit LAST in `m_factionTable`, after the clans - see
    // `initializeFactions`, where that ordering is load-bearing rather than
    // tidy.
    [[nodiscard]] bool shadow() const { return kind == sol::assets::FactionKind::Shadow; }
    float aggression = 0.5f;
    float forgiveness = 0.5f;
    std::vector<std::string> shipsPatrol;
    std::vector<std::string> shipsRaider;
    std::vector<std::string> shipsTrader; // Phase 8x: hulls its haulers fly
    // Phase 32 stage C: the cells this faction declared it fields nothing for.
    // Carried onto the clan copy as well as the major, because a clan stamped
    // from a pirate template inherits what that template says it does not
    // build - it is the same faction's character, jittered.
    bool buildsNo[sol::assets::kRosterCellCount] = {false, false, false};
};

// ⚑⚑⚑ THE ROSTER A FACTION ACTUALLY FLIES FOR A CELL, AND THE ONE PLACE THE
// SUBSTITUTION RULES LIVE (Phase 32 stage C). Six call sites used to read a
// roster off `GameFaction` directly, four of them with their own hand-written
// `x.empty() ? y : x` - so "what does an empty cell mean" had four answers in
// this file and none of them was written down anywhere a seventh site would
// find it.
//
// ⚑⚑ `fallback` IS PASSED IN RATHER THAN TABULATED, ON PURPOSE. The four
// substitutions genuinely differ - a contested attacker with no raiders flies
// its patrol hulls, a response wing with no patrol flies its raiders, and the
// two AMBIENT sites substitute NOTHING at all. Tabulating one answer per cell
// would silently give ambient spawning a fallback it has never had, which is a
// behaviour change this stage has no mandate for. `RosterCell::Count` means
// "no substitute", and it is the argument the ambient sites pass.
//
// ⚑⚑ WHAT IS NEW IS THE FIRST LINE: A DECLARED CELL IS NEVER SUBSTITUTED FOR.
// That is the whole difference between "unspecified" and "we build none", and
// it is visible in shipped content - the two pirate clans declare `builds_no =
// ["trader"]`, and before this stage emptying that list would have made them
// haul in INTERCEPTORS rather than not haul at all.
[[nodiscard]] std::span<const std::string>
factionRoster(const GameFaction& faction, sol::assets::RosterCell cell, sol::assets::RosterCell fallback);

// ⚑⚑⚑ WHAT A FACTION IS, IN ONE WORD, AND THE ONE PLACE THE THREE WORDS LIVE
// (Phase 37 stage B). Two screens print this - the dock's Factions tab and the
// console's `sol.factions` - and until this stage both carried their own
// `pirate ? "pirate clan" : "major"`. Two copies of a two-word vocabulary was
// survivable; two copies of a THREE-word one where the third word is the game's
// only secret organisation is how a black market gets labelled "major" on one
// screen and named correctly on the other.
//
// ⚑⚑ "syndicate" RATHER THAN "shadow", AND THE DIFFERENCE IS WHO IS READING.
// `shadow` is the def keyword and the module family - author vocabulary. A
// player reading the Factions tab is being told what sort of organisation this
// is, and the answer is a syndicate: no territory, no flag, and a row on the
// same list as the Solar Navy.
[[nodiscard]] const char* factionKindLabel(const GameFaction& faction);

// What Lua calls a pilot's job, and the one place those words live. See the
// definition: `pilot_think` branches on this STRING with no else clause, so a
// role missing from the script is a pilot that never acts.
[[nodiscard]] const char* pilotRoleName(PilotRole role);

// --- Response (Phase 30 stage C) -------------------------------------------
//
// How long a dispatched responder keeps flying at an incident before giving up
// and going back to its own patrol. A response that hunts forever is a tax; one
// that never arrives is scenery.
inline constexpr float kResponseGiveUpSeconds = 180.0f;
// Minimum interval between dispatches for one system, so a burst of hits is one
// call rather than one per projectile.
inline constexpr double kResponseCooldownSeconds = 20.0;
// Below this the place is not policed by anybody in any meaningful sense and
// nobody comes at all - decisions/019's zero band, given a width.
inline constexpr float kResponseSilenceBand = 0.08f;

// Whether a call for help HERE is answered at all.
//
// ⚑⚑⚑ THIS IS A FUNCTION RATHER THAN TWO COMPARISONS BECAUSE STAGE D PUTS THE
// RATING IN FRONT OF A PLAYER, AND THE MAP AND THE DISPATCHER MUST NOT BE ABLE
// TO DISAGREE ABOUT IT. The map's whole promise is that a route planned off it
// was planned off the truth - so a map saying "somebody polices this" about a
// system `respondTo` will silently refuse is worse than a map that says
// nothing, because the player flies into it on purpose. `respondTo` reads this
// and so does the map row, and there is one band, in one place, for both.
[[nodiscard]] inline bool securityAnswers(float liveSecurity)
{
    return std::abs(liveSecurity) >= kResponseSilenceBand;
}

// Why a responder was called, and since Phase 36 stage D the cause is READ
// rather than accepted and ignored.
//
// ⚑⚑⚑ THE SPEC SAID "FIRED ON IS `respondTo` WITH A NEW CAUSE AND NOTHING
// ELSE", AND CHECKING THAT IS WHAT SHAPED THE STAGE, BECAUSE IT IS FALSE.
// `respondTo` dispatches hulls to a PLACE, in `PilotState::Travel`. What makes
// any of them shoot the player is `pilotEngageEnemy`, and that only considers
// the player when `playerHostile` - standing below `hostileThreshold`, -30. So
// a dispatch with no standing behind it sends ships to fly to where you were
// and then go back to their beat. Being fired on is a STANDING consequence
// with a dispatch on top of it.
//
// ⚑⚑ AND THE OBVIOUS COROLLARY IS FALSE, WHICH MUTATION TESTING IS WHAT SAID
// SO. The first version of this note claimed the standing had to be spent
// BEFORE the dispatch or the arriving hulls would find nobody they had a
// quarrel with. `respondTo` never consults standing at all - it reads the
// system owner, the security rating and pilot states, and nothing else - and
// what turns a diverted hull into a shooter is `pilotEngageEnemy` on its next
// think, which is a later frame than either. Swapping the two lines is an
// equivalent mutant, and a test was written that could not tell them apart.
// The claim that survives is the one that matters: spend the standing at ALL,
// because the dispatch on its own is a taxi service.
//
// ⚑⚑ AND THE CAUSE CHANGES WHAT A RESPONSE IS ALLOWED TO DO. Weapons fire is
// somebody dying, so the law tops the answer up from the nearest station when
// the local wing is short. A pilot who declined a paperwork check is not worth
// launching hulls over: that one DIVERTS ONLY. Without the split, running from
// a stop would materialise a wing out of nothing 600,000 km from anywhere,
// which is the tax `017` warns about arriving through a side door.
enum class ResponseCause : std::uint32_t
{
    WeaponsFire = 0, // somebody shot somebody the local law protects
    FledInspection,  // a stop was opened and the ship left instead of holding
};

// --- Ambient presence, as curves on the security baseline (Phase 30 stage B) --
//
// ⚑⚑⚑⚑ THESE READ THE BASELINE AND MUST NEVER READ THE LIVE RATING. That is
// decisions/019's ruling taken rather than discovered: the live number is
// `baseline - danger`, so wiring garrison size to it makes a positive-feedback
// spiral - a raid raises danger, which lowers live security, which THINS THE
// PATROLS, which makes the next raid cheaper. A navy's garrison does not
// evaporate because pirates turned up; if anything it digs in. The baseline is
// how much force the owner keeps here; the live rating is how safe it actually
// is right now, and only response TIME is allowed to read the second one.
//
// ⚑⚑ WHAT THESE REPLACED, AND WHY THE SHIPPED GALAXY BARELY MOVES.
// `kPatrolsPerRegion[3] = {3, 2, 1}` and `kCiviliansPerRegion[3] = {4, 3, 1}`,
// with a comment from Phase 13 that already called the index "region
// security". The point of the curve is NOT to change what ships today - it is
// that the input becomes a NUMBER, so an authored `security =` (stage E) or a
// retune moves the sky with no new code. `patrolsFor` is tuned to reproduce
// {3, 2, 1} exactly across all three shipped bands; `civiliansFor` deliberately
// does not, because civilian traffic is the one of the two that can vary
// inside a band without meaning anything about force.

// Patrol hulls a major keeps over its station. Floored at one: territory its
// owner cannot police at all is territory it does not hold, and the generator
// has already decided that it does.
[[nodiscard]] inline std::uint32_t patrolsFor(float baselineSecurity)
{
    if (baselineSecurity <= 0.0f) {
        return 0; // nobody holds it, so nobody garrisons it
    }
    const auto count = static_cast<std::uint32_t>(std::lround(baselineSecurity * 4.0f));
    return count < 1u ? 1u : count;
}

// Civilian traffic over the same station.
//
// ⚑⚑⚑ EXPRESSED AGAINST THE GARRISON RATHER THAN AS ITS OWN CURVE, AND THAT
// IS DELIBERATE. `kCiviliansPerRegion` was {4, 3, 1}, which is NOT linear in
// security - the step from frontier to fringe is more than twice the step from
// core to frontier - so no single multiplier reproduces it, and every one that
// was tried moved ambient traffic in ~32 frontier systems as a side effect.
// Civilian density is scenery rather than strength, this phase was not asked
// to retune it, and it is exactly the kind of change this project's playtests
// notice (a sky that felt dead was once a whole phase's worth of notes). So:
// one hauler per patrol, plus one more wherever the garrison is more than
// token. That reproduces {4, 3, 1} exactly across all three shipped bands with
// no magic threshold in it, and it still moves when an authored rating moves.
[[nodiscard]] inline std::uint32_t civiliansFor(float baselineSecurity)
{
    const std::uint32_t patrols = patrolsFor(baselineSecurity);
    return patrols + (patrols >= 2u ? 1u : 0u);
}

// Raider hulls a clan keeps in a system it holds, scaling DOWN the negative
// band - so a clan's home is genuinely thick with hostiles where today every
// clan system alike holds a flat two.
[[nodiscard]] inline std::uint32_t raidersFor(float baselineSecurity)
{
    if (baselineSecurity >= 0.0f) {
        return 0;
    }
    const auto count = static_cast<std::uint32_t>(std::lround(-baselineSecurity * 6.0f));
    return count < 1u ? 1u : count;
}

// ⚑⚑⚑⚑ WHICH HULL A WING'S SLOT FLIES, AND IT IS THE FOURTH FUNCTION OF
// `baselineSecurity` IN THIS BLOCK (Phase 32 stage D). The three above decide
// HOW MANY ships a place gets; this decides WHICH, out of the cell's roster
// ranked by the `class` stage A authored. Strongly-held space fields the heavy
// end of what a faction builds and the margins field the light end - a core
// system's civilian traffic is freighters, a fringe system's is shuttles, from
// the same roster and the same faction.
//
// ⚑⚑⚑ IT IS THE MAGNITUDE, NOT THE SIGN, AND THAT IS WHAT MAKES IT ONE RULE
// RATHER THAN TWO. `baselineSecurity` is positive where a major polices and
// negative where a clan holds, and `patrolsFor`/`raidersFor` already split on
// that sign to decide the count. What this asks is *how firmly is this place
// held*, which is the same question either way round: a clan's home system
// fields its heaviest raiders for exactly the reason a core system fields its
// heaviest patrols. The shipped galaxy runs +0.85 down to -0.75, so one scale
// covers both.
//
// ⚑⚑⚑ THE WING IS SPREAD ACROSS THE RANGE RATHER THAN BEING N COPIES OF ONE
// HULL. Slot `i` of a wing of `count` sits at `(i + 0.5) / count` through it,
// and the window is centred on how firmly the place is held - so a core system
// at 0.85 with four civilians gets three freighters and one shuttle, a
// frontier at 0.49 with three gets one and two, and a fringe at 0.20 with one
// gets the shuttle. That proportion IS the readout: "mostly freighters" and
// "mostly shuttles" survive a player only ever seeing part of the sky, where
// a hard switch at some threshold would put half the galaxy on a coin flip.
//
// ⚑⚑ AND IT FALLS BACK TO THE ROUND-ROBIN IT REPLACED WHENEVER THE CONTENT HAS
// NOT SPOKEN. `class` is optional with no default (stage A: "an invented class
// is the parser deciding what a hull IS"), so a cell where any hull declares
// none - or where every hull declares the SAME one - ranks into nothing, and
// the answer is `slot % size`, exactly what shipped before this stage. That is
// also `chooseTraderHull`'s rule for equal capacities and for the same reason:
// a roster of same-sized hulls should be a mixed sky, not one repeated ship.
//
// `classes` is in ROSTER order with `assets::kHullClassCount` for a hull that
// declares none, and the answer is a roster index - the same shape
// `chooseTraderHull` has, so the caller extracts the numbers and this decides
// nothing about defs.
[[nodiscard]] inline std::uint32_t chooseWingHull(std::span<const std::uint32_t> classes,
                                                  float baselineSecurity,
                                                  std::uint32_t slot,
                                                  std::uint32_t count)
{
    const auto size = static_cast<std::uint32_t>(classes.size());
    if (size == 0) {
        return 0;
    }
    bool ranked = false;
    for (std::uint32_t i = 0; i < size; ++i) {
        if (classes[i] >= sol::assets::kHullClassCount) {
            return slot % size; // one hull has not said, so the cell has not
        }
        ranked = ranked || classes[i] != classes[0];
    }
    if (!ranked) {
        return slot % size; // every hull the same class: nothing to rank by
    }

    const float held = std::min(std::abs(baselineSecurity), 1.0f);
    const float through = count > 0 ? (static_cast<float>(slot) + 0.5f) / static_cast<float>(count) : 0.5f;
    const float position = sol::core::clamp(held + through - 0.5f, 0.0f, 1.0f);
    const auto wanted = static_cast<std::uint32_t>(std::lround(position * static_cast<float>(size - 1)));

    // The `wanted`-th lightest hull, ties broken by authored order. Counted
    // rather than sorted because a roster is three hulls, not a fleet, and a
    // sort would need a scratch buffer this has no business owning.
    for (std::uint32_t i = 0; i < size; ++i) {
        std::uint32_t lighter = 0;
        for (std::uint32_t j = 0; j < size; ++j) {
            if (classes[j] < classes[i] || (classes[j] == classes[i] && j < i)) {
                ++lighter;
            }
        }
        if (lighter == wanted) {
            return i;
        }
    }
    return 0;
}

// ⚑⚑⚑ WHAT A SPAWNED PILOT IS DOING COMES FROM THE ROSTER CELL IT WAS DRAWN
// OUT OF (Phase 32 stage D), AND IT WAS A HAND-PASSED ARGUMENT AT SEVEN CALL
// SITES BEFORE THIS. All seven already agreed with this table - raider cells
// spawned Fighters, patrol cells Patrols, the trader cell Traders - so the
// parameter carried no information and could only ever disagree.
//
// ⚑⚑ IT IS THE CELL *ASKED FOR*, NOT THE ONE DELIVERED, which matters exactly
// where stage C's substitutions fire: a contested attacker with no raider
// roster flies its patrol HULLS on a raid, and it is still raiding.
//
// ⚑⚑ AND §11.2's SIX ROLE FAMILIES ARE DELIBERATELY NOT READ HERE, WHICH IS A
// DEPARTURE FROM THE PHASE SPEC AND THE REASON IS THAT THEY ANSWER A DIFFERENT
// QUESTION. `HullRole` says what a hull is FOR; the cell says what this faction
// is fielding it AS, and a faction is entitled to put a `logistics` hull on
// patrol - a militia freighter is a real thing and not a content error. Joining
// them would make the def decide the job, which is the one thing the roster is
// for. `role` stays vocabulary, and Phase 34's covert pilots are what will read
// it.
[[nodiscard]] inline PilotRole pilotRoleFor(sol::assets::RosterCell cell)
{
    switch (cell) {
    case sol::assets::RosterCell::Raider:
        return PilotRole::Fighter;
    case sol::assets::RosterCell::Trader:
        return PilotRole::Trader;
    case sol::assets::RosterCell::Patrol:
    case sol::assets::RosterCell::Count:
        break;
    }
    return PilotRole::Patrol;
}

struct RenderShape
{
    sol::core::Vec3 scale = {1.0f, 1.0f, 1.0f};
    ModelId model = kNoModel;
};

// Non-entity scenery: rendered as impostors, referenced as nav targets.
struct CelestialBody
{
    std::string name;
    sol::core::DVec3 position;
    double radius = 0.0; // meters
};

// A jump gate in the current system (decisions/004: gates are the baseline
// inter-system travel).
struct GateInstance
{
    std::string name; // "Gate: <destination>"
    std::uint32_t toSystem = 0;
    // Which way the gate faces (unit), and therefore the normal of the plane
    // you have to cross to use it (Phase 8w). DERIVED, never stored: generation
    // puts every gate at hub + bearing * gateDistance, so this is
    // normalize(position - hub) — the lane the gate serves. Rebuilt with the
    // rest of the system's side data, so it is correct on every load path.
    sol::core::DVec3 axis{0.0, 0.0, 1.0};
    sol::core::DVec3 position;
};

// A scannable site instantiated in the current system (Phase 8e). What it is
// comes from SurveySim::signalsFor — a pure function of the system seed — so
// this list is rebuilt, never saved.
struct SignalInstance
{
    std::uint32_t index = 0; // position in the system's signal list
    sol::sim::SignalKind kind = sol::sim::SignalKind::Derelict;
    sol::core::DVec3 position;
    std::uint64_t seed = 0; // the loot roll for this site
};

// A rock the beam can cut (Phase 8f). What it is comes from
// MiningSim::rocksFor — a pure function of the system seed — so the entity is
// rebuilt per system; only how much has been taken out of it is saved.
struct MineableRock
{
    std::uint32_t field = 0;
    std::uint32_t index = 0;     // rock index within the field
    std::uint32_t commodity = 0; // economy commodity index
    float totalUnits = 0.0f;     // yield of an untouched rock
    sol::core::Vec3 tumbleAxis{0.0f, 1.0f, 0.0f};
    float tumbleRate = 0.0f; // rad/s
};

// A wreck instantiated in the current system (Phase 8f). Unlike a rock, the
// thing it stands for is a record in MiningSim, not a seed.
struct WreckMarker
{
    std::uint32_t id = 0; // sim::WreckRecord::id
};

// Ore cut loose and drifting, waiting to be collected (Phase 8f). This is the
// step that makes mining a place you fly around in rather than a transaction:
// the beam only breaks the rock, the ship still has to gather what came off.
struct OreChunk
{
    sol::core::DVec3 velocity; // sim space, m/s
    double lifetime = 0.0;     // seconds remaining before it is lost
    std::uint32_t commodity = 0;
    float units = 0.0f;
};

// A scan result the game reports outward: GameContent forwards it to Lua for
// flavor and loot composition, the same queue shape mission events use.
struct SurveyEvent
{
    enum class Kind : std::uint32_t
    {
        SignalDiscovered = 0, // a pulse found it; still unidentified
        SignalResolved,       // a target scan identified it
        BodyScanned,          // a star or planet resolved
    };
    Kind kind = Kind::SignalDiscovered;
    std::uint32_t system = 0;
    std::uint32_t index = 0; // signal index, or body index (0 = star)
    sol::sim::SignalKind signalKind = sol::sim::SignalKind::Derelict;
    std::uint64_t seed = 0;
    std::string name;
};

// A ship died and left something to cut open (Phase 8f). GameContent forwards
// it to the Lua loot hook, the same queue shape survey events use.
struct WreckEvent
{
    std::uint32_t id = 0;
    std::uint32_t system = 0;
    std::string defId;       // the victim's ship def
    std::string factionName; // empty for unaffiliated spawns
    std::uint64_t seed = 0;
};

// A rock cut to nothing (Phase 8f). Fired once, when the rock breaks up,
// rather than per bite of the beam: a hook at the weapon's rate of fire would
// be noise, and "that rock is finished" is the moment worth flavoring.
struct RockEvent
{
    std::uint32_t commodity = 0;
    float units = 0.0f; // what the rock held when it was whole
};

// What the beam is pointed at, for the HUD's prospecting readout (Phase 8f).
struct ProspectInfo
{
    bool valid = false;
    bool wreck = false; // a hull to cut rather than a rock
    std::string name;   // commodity name, or the wreck's
    float unitsLeft = 0.0f;
    float unitsTotal = 0.0f;
    double distance = 0.0;
    bool inRange = false; // inside the fitted weapon's reach
};

struct NavTarget
{
    std::string name;
    sol::core::DVec3 position;
    double surfaceRadius = 0.0; // 0 for point targets (station)
};

// What a scannable site is called at each stage of being found: "Contact 3"
// until it is identified, then what it turned out to be. Declared here rather
// than left in space_world.cpp's anonymous namespace because the map screen
// names the sites of systems the player is not in (Phase 8q) and has to name
// them the same way - two copies of this is how the local and the remote map
// come to call one site two different things.
[[nodiscard]] std::string
signalTargetName(sol::sim::SignalKind kind, bool resolved, bool emptied, std::size_t ordinal);

// What anything unidentified is called, whatever it turns out to be (Phase 8z).
// A station, a gate and a derelict all read "Contact 4" until something scans
// them, so the label never says what the glyph is also withholding. The ordinal
// is the object's place in the system's fixed [stations, gates, signals] order,
// so a contact's designation does not change when a different one is
// identified. Shared with the map for the same reason signalTargetName is.
[[nodiscard]] std::string anonymousContactName(std::size_t ordinal);

// Snapshot of the selected nav/combat target for HUD and weapons.
struct TargetInfo
{
    NavTarget nav;
    bool isShip = false;
    sol::core::DVec3 velocity; // ships only
    float shieldFore = 0.0f;   // fractions, ships only
    float shieldAft = 0.0f;
    float hull = 0.0f;
    std::string factionName;   // empty for unaffiliated/static targets
    const char* attitude = ""; // player standing vs its faction (HUD tag)
};

// Physical extent of the built structures. Used as the NPC avoidance sphere
// and, since Phase 8j, as the click box: a thing the player can see filling
// the view has to be selectable where they see it, not only within a few
// pixels of the point its centre projects to. The station's drawn hull is
// 100 m and its sphere is a little past that; a gate's frame is 70 m.
inline constexpr double kStationRadiusMeters = 130.0;
inline constexpr double kGateRadiusMeters = 70.0;
// How far past the opening autopilot aims when flying to a gate (Phase 8w), so
// that "arrive" means through rather than on the threshold. Far enough that
// steerTravel is still carrying real speed as it crosses, short enough that the
// ship is not left a long way past the gate if the jump somehow does not fire.
inline constexpr double kGateApproachOvershoot = 400.0;
// Where a trader puppet stops when it reaches the end of its leg (Phase 8x).
// Dictated by the two spheres 8r already measured rather than picked: the
// station's avoidance sphere is 130 m and its berths ring at 200 m, so a
// hauler holding at 250 m is clear of both and reads as waiting its turn.
// A puppet that beats its own coarse clock parks here until the record
// catches up, which is why this is an arrival range and not a stop.
inline constexpr double kTraderArrivalRange = 250.0;
// Spacing between lane slots (Phase 8x). A freighter is a 4x hull, so tens of
// metres; 400 m is far enough that a convoy never touches even after the
// approach compresses it, and near enough that it still reads as traffic
// sharing one lane rather than ships scattered at random.
inline constexpr double kTraderLaneSpacing = 400.0;
// The window at each end of a leg where a trader flies itself instead of
// being paced by the record (Phase 8x). The distance is where steerTravel's
// own profile drops a hauler back into its normal envelope — 5.76 km for the
// shipped freighter, so 6 km — and the time is roughly what that final
// approach takes at those speeds. Together they are the seam between the
// coarse sim and the bubble, which is the whole Simulation-LOD idea.
inline constexpr double kTraderApproachDistance = 6'000.0;
inline constexpr double kTraderApproachSeconds = 35.0;
// How far off a rock's SURFACE a miner holds while it works it (Phase 8x
// stage 6), and how long it stays before moving to the next one. The clearance
// is a standoff and not a stop: PilotState::Travel arrives within
// kTraderArrivalRange of its waypoint, so the ship settles somewhere between
// the surface and this plus that, and the smallest of those has to still be
// daylight. A minute a rock is slow enough to read as work rather than as a
// patrol, and quick enough that a field is not a still life.
inline constexpr double kMinerRockClearance = 600.0;
inline constexpr double kMinerRockSeconds = 60.0;
// ⚑ And how wide a corridor a miner needs to move between two of them. Rocks
// are solid statics and NOTHING in the game avoids them — m_obstacles holds
// stations and planets only — so a hop is refused unless the straight line
// misses every other rock by this much. It is deliberately far smaller than
// the hold clearance above: a 32 m hull needs room to pass, not room to park,
// and a corridor as wide as the standoff would box a miner in and quietly
// turn it into scenery.
inline constexpr double kMinerPathClearance = 200.0;
// How long an outpost's draw stops after its miner is killed (Phase 8x stage
// 6). ⚑ Not picked: traderLegSeconds is the economy's own figure for crossing
// a system, which is exactly what a replacement has to fly. Killing the ship
// therefore costs the station about that much ore — the first time the player
// can reach into a station's production directly, and the reason a miner is
// worth shooting at rather than scenery.
inline constexpr double kMinerReplacementLegs = 1.0;
// How a manually cruising player is warned off something in the way, measured
// in stopping distances rather than in seconds (Phase 8y §D). At 5.5e6 m/s a
// second is 5,500 km, so a fixed grace second would be meaningless at one
// speed and already fatal at another; what stays constant across every hull
// and every speed is how much room the brakes need. The warning goes out
// while there are four of them left and the drive cuts while there are still
// two, so the player is always told before anything is taken away.
inline constexpr double kCruiseLookaheadStops = 4.0;
inline constexpr double kCruiseCutStops = 2.0;
inline constexpr double kCruiseWarningRepeatSeconds = 6.0;
// How long a pilot remembers being shot at (Phase 8x §D). It asks 8l's
// question - was this ship in this fight - but answers it for the victim
// rather than for a bounty, so it is deliberately shorter than that 10 s
// assist window: a bounty should forgive a lull, while a hauler that has not
// been shot at for six seconds has genuinely been left alone and should get
// back on its lane instead of fleeing forever.
inline constexpr double kThreatMemorySeconds = 6.0;

// ⚑⚑⚑⚑ HOW LONG A SYSTEM KEEPS RUNNING AFTER YOU JUMP OUT OF A FIGHT IN IT
// (Phase 38 stage C). This is the cooling bubble's whole duration and the only
// thing that closes one.
//
// ⚑⚑⚑ THE SPEC WROTE THE POLICY AS "STAYS INSTANTIATED WHILE A FIGHT IN IT IS
// LIVE", AND THAT PREDICATE CANNOT BE RE-ASKED ONCE THE PLAYER IS GONE —
// BECAUSE THE PLAYER LEAVING IS WHAT ENDS THE FIGHT. `tickSystem`'s Attack case
// drops a pilot to Idle the moment `pilot.targetIndex` stops resolving, and the
// migrate retires the player out of the registry they were being shot in, so
// every raider that was shooting at you stands down ONE TICK after the jump.
// Nobody re-engages either: the think pass is player-scoped, which is stage B's
// recorded LOD statement. What is left is `threatTimer` — six seconds, above —
// and whatever bolts are still in the air. A per-tick "while live" reading
// therefore releases the bubble about five seconds into the minute the phase's
// own exit spends in the next system.
//
// So the fight is the ENTRY condition, asked once at the moment of departure,
// and this constant is the retention. Two minutes: the exit is "spend a minute
// in the next system and jump back", and a gate-to-gate crossing sits well
// inside it. Counted DOWN only, never refreshed — a refresh is how "a stalemate
// pins a bubble forever" gets back in through the door the ceiling closes.
inline constexpr double kCoolingSeconds = 120.0;

// How the player is looking at the world this frame (Phase 8j). The frame loop
// owns the camera and the UI scale, and pushes both in here once per frame;
// the world never computes them. It is held rather than passed because a click
// is not the only thing that needs it — the console performs the identical
// pick, and both have to be answered against the same view or the verification
// is testing a different game than the one on screen.
// Owned by the frame loop (Phase 8t); the world only posts cues to it.
class GameAudio;

struct ViewFrame
{
    sol::core::DVec3 cameraPosition;
    sol::core::Quat cameraOrientation;
    sol::core::Vec2 screenSize; // VIRTUAL UI pixels, i.e. after the UI scale
    float tanHalfFovY = 1.0f;
    bool valid = false; // false before the first frame, and while a menu is up

    // Where the HUD ended up this frame (Phase 8m). The pick carries it for the
    // same reason Phase 8j moved the disc's geometry into a header: the radar
    // is no longer at a position the pick can recompute from the screen size —
    // in a cockpit it is bolted to the dash — so the click has to be answered
    // against the frame the disc was actually drawn from.
    sol::ui::HudFrame hud;
    // The ship's nose in camera space. Free-look turns the head without turning
    // the ship, so this is what "target what I am pointing at" means; it is
    // (0,0,-1) whenever the head is straight.
    sol::core::Vec3 boresightCamera = {0.0f, 0.0f, -1.0f};
};

// The new-game starter ship def; mods can override it (Phase 5 data pipeline).
inline constexpr const char* kPlayerShipDefId = "sol.shuttle";

// How much larger than the living hull a wreck is drawn (Phase 19 keeps the
// figure it found inline). There is no broken-hull mesh: a dead ship is its
// own model, oversized and adrift, and the oversize IS the effect.
inline constexpr float kWreckOversize = 1.4f;

// One ship the player owns (Phase 8a outfitting): its def, fit, crew, and —
// unless it is the active ship — where it is stored.
// One fitting on one hull (Phase 31 stage B, decisions/014 rule 1). A save
// names a fitting BY THE MOUNT IT OCCUPIES and never by index, so an author
// inserting a mount into a hull cannot silently rearrange every existing
// player's ship - the worst kind of save bug, because it corrupts nothing and
// looks like the player misremembering.
struct ShipFitting
{
    std::string mountId; // a `[[ship.mount]]` id on this ship's def
    std::string defId;   // component or weapon def id; the MOUNT says which
    // Which trigger a GUN in this mount answers to (Phase 31 stage C3),
    // 1-based and meaningless on a mount holding anything else. It is saved
    // here rather than left in the live `ShipArmament` because a refit rebuilds
    // that from the def: without a durable copy, buying a cargo pod would put
    // every gun back in group 1.
    std::uint32_t group = 1;
};

struct OwnedShip
{
    std::string defId;
    std::vector<ShipFitting> fittings;         // what is in which mount
    std::vector<std::string> crewIds;          // hired crew aboard
    std::uint32_t storedSystem = 0xffff'ffffu; // active ship ignores these
    std::uint32_t storedStation = 0xffff'ffffu;

    // What is in a mount, or null for a bare one.
    [[nodiscard]] const ShipFitting* fittingAt(std::string_view mountId) const;
};

// ---------------------------------------------------------------------------
// Captains (Phase 39 stage A).
//
// ⚑⚑⚑⚑ A CAPTAIN IS A PERSON, AND THAT IS THE WHOLE OF WHY THIS IS A NEW TYPE
// RATHER THAN A `CrewDef` WITH A FLAG. `OwnedShip::crewIds` is a list of catalog
// id STRINGS - so two hired Engineers are one string twice, unnameable and
// unaddressable - and `decisions/006` states the model in its own words: "no
// personalities, no management, no leveling, no wages". There is no object in
// this game a standing order could be given to, which is the finding the phase
// spec opens with. `decisions/020` is the conscious replacement 006's own last
// consequence asks for, and passive-bonus crew stand beside this untouched.
//
// ⚑⚑⚑ AND THE SPEC WAS WRONG ABOUT ONE THING IN THE CHEAP DIRECTION: this is
// NOT "the first instance of a person the game has ever had". Phase 35 built
// one - `CastSeat` names somebody in all 62 rooms and `CastMemory{who, visits,
// regard}` is a SAVED relationship ledger keyed by a person-or-chair
// distinction that has its own comment. So a captain reuses that identity SPACE
// (`castKeyForCharacter`'s 63-bit hash) rather than opening a second one, and
// the parallel-table defect Phase 34's risk register names is refused here
// rather than discovered later.
//
// ⚑⚑ THE NAME IS COPIED IN, NOT LOOKED UP, and it dissolves the spec's own
// "a person is a new kind of save-format promise" risk. `CastSeat` already
// copies for this reason - "a regular's name exists in no def at all" - and a
// captain's name exists in no def either: it is drawn from the same syllable
// tables the cast uses. There IS no def to go missing on load.
struct Captain
{
    std::string name;  // copied in; lives in no def
    std::string trade; // what they did before you hired them
    // Identity, in the cast's key space. Stable across a save, and what the
    // crew hall filters its candidates by - so a hired captain stops being on
    // offer without a second list being kept anywhere.
    std::uint64_t who = 0;
    // Fleet index of the hull they fly, or `kNoIndex` for a captain with no
    // ship yet.
    //
    // ⚑⚑⚑ ONE DIRECTION ONLY, AND THE SPEC SAID TWO. Its stage A line reads
    // "`OwnedShip` learns it is assigned"; a field on both sides is two truths
    // that can disagree, and the fleet is small enough that `captainOf()` is a
    // search. What `sellShip` has to do instead is shift these indices the way
    // it already shifts `m_activeShip` - which is a real hazard and has a
    // guard.
    std::uint32_t ship = 0xffff'ffffu;
    // What they take of what the ship earns, as a fraction (ruling 3 of the
    // phase spec). Drawn per candidate, so shopping around is worth doing.
    float cut = 0.0f;
};

// Somebody looking for a berth in the docked station's crew hall.
//
// ⚑⚑ DERIVED FROM THE SEED AND NEVER SAVED, exactly as the cast's seating is:
// who is standing in a hall costs nothing on disk. What IS saved is who you
// hired, and the hall filters against that - so a captain you DISMISS is on
// offer again, which is honest (they went back to looking for work) and costs
// no storage at all.
struct CaptainCandidate
{
    std::string name;
    std::string trade;
    std::uint64_t who = 0;
    float cut = 0.0f;
};

// What a per-def model OVERRIDE resolves to (Phase 19). An empty `name` means
// "whatever fills `role`", which is what every def the base game ships relies
// on; a name that no [[model]] defines warns and falls back to the role too,
// the same warn-rather-than-refuse treatment a ship def's model gets.
//
// ⚑ Declared here rather than left static in the .cpp because it is the rule
// the whole override half of the phase rests on, and "adding these keys
// changed nothing" is a claim that needs a test rather than a comment.
//
// `unitRadius` says the slot is drawn at a scale that means metres, so a model
// of any other radius silently changes both size and hit sphere - see
// `model_roles.hpp`. Role models are pinned by a test against committed data;
// an override is written in somebody else's file, so it can only be warned at.
[[nodiscard]] ModelId modelOverrideOr(const sol::assets::DefDatabase& defs,
                                      const std::string& name,
                                      const char* context,
                                      const char* role,
                                      bool unitRadius);

// The seed the game ships with: `--seed N` overrides it, and nothing else does.
//
// ⚑ Phase 29 promoted this out of main.cpp's anonymous namespace, where the
// only thing that could read it was `parseUniverseSeed`. The galaxy golden
// (`game.unit`) has to photograph the seed the game ACTUALLY launches at, and a
// test that copies the literal instead would keep passing while quietly
// describing a galaxy nobody plays.
inline constexpr std::uint64_t kDefaultUniverseSeed = 1701;

class SpaceWorld
{
public:
    // Creates the player ship only; the universe arrives via
    // generateUniverse once GameContent has loaded the defs (faction count
    // and, later, station archetypes feed the generator params).
    void spawn(std::uint64_t universeSeed);

    // Generates the galaxy from the stored seed + defs and instantiates the
    // starting system (first core system with a station). Called once by
    // GameContent::initialize after defs load.
    //
    // ⚑⚑ FALSE when an authored system's placement rule found nowhere to go
    // (Phase 29 stage B). Each one is logged as an error naming the file, the
    // id and the rule before this returns, so a caller that refuses to boot
    // has nothing to add. ⚑ It is a per-SEED verdict rather than a per-file
    // one - a `jumps_from` ring is a claim about a gate graph - so there is no
    // load-time check that could have caught it earlier.
    [[nodiscard]] bool generateUniverse(const sol::assets::DefDatabase& defs);

    // Begins a jump through the nearest gate within activationRange meters of
    // the player. Since Phase 8v this STARTS a transition rather than arriving:
    // the system change happens partway through, at the tunnel's full stretch.
    // Returns false if no gate is in range, or if a jump is already running.
    [[nodiscard]] bool jumpNearestGate(double activationRange);

    // Runs the jump transition on real frame time and performs the system
    // change at its swap point.
    //
    // Called from the FRAME LOOP rather than from tick(), and that is the whole
    // reason it is its own entry point. loadSystem invalidates the tick's pass
    // scratch (collision slots, pools) — which is why the death respawn defers
    // it to end of tick — and a transition suspends the sim, so there is no
    // tick to defer to. The frame loop calls this where no tick is in flight.
    void advanceJumpTransition(double deltaSeconds);

    [[nodiscard]] const sol::sim::JumpTransition& jumpTransition() const { return m_jump; }

    [[nodiscard]] bool jumpActive() const { return m_jump.active(); }

    // Distance to the nearest gate, or a negative value with no gates.
    [[nodiscard]] double nearestGateDistance() const;

    // The nearest gate itself, or nullptr with no gates — so the HUD can name
    // where it leads rather than only how far off it is.
    [[nodiscard]] const GateInstance* nearestGate() const;

    // Jumps via this system's gate to the named destination system,
    // regardless of distance, and ARRIVES IMMEDIATELY — a dev teleport, not a
    // jump. Phase 8v deliberately left this instant while making sol.jump()
    // play the real transition, so a drive always knows which one it called.
    // Returns false if no gate leads there.
    [[nodiscard]] bool jumpToSystem(const char* destinationName);

    // --- Docking (GDD: request -> approach; Phase 8r made the request real) ---
    // Docks at the nearest station within range without asking anyone: the dev
    // shortcut (sol.dock) and the death respawn both need a way in that no
    // dispatcher can refuse. The player's route is requestDocking() below.
    [[nodiscard]] bool tryDockNearestStation(double range);
    [[nodiscard]] bool undock();

    [[nodiscard]] bool isDocked() const { return m_dockedStation != kNoIndex; }

    [[nodiscard]] const char* dockedStationName() const;

    // Station index in the current system, or ~0u while flying.
    [[nodiscard]] std::uint32_t dockedStationIndex() const { return m_dockedStation; }

    // Distance to the nearest station, or a negative value with none.
    [[nodiscard]] double nearestStationDistance() const;

    // --- Docking clearance (Phase 8r) ---
    //
    // Deliberately DISJOINT from m_dockedStation rather than folded into it. A
    // clearance only exists while the ship is *not* docked, so isDocked() —
    // and the ~65 places that ask it "am I inside the station" — keep their
    // exact meaning. The backlog note that became this item expected the
    // opposite (a boolean widened into a state machine); the code says
    // otherwise, and this comment is here so nobody re-derives it.
    struct DockClearance
    {
        std::uint32_t station = kNoIndex; // station index in the current system
        std::uint32_t berth = kNoIndex;   // which port was assigned
        double secondsLeft = 0.0;         // counts down; 0 = no clearance
    };

    // How long a grant stands, and how far a request carries. The request
    // range is long so calling ahead is a sequence rather than a formality;
    // it never overrides the dock/salvage precedence inside kDockRange, which
    // main.cpp owns (see the ladder there).
    static constexpr double kClearanceSeconds = 180.0;
    static constexpr double kDockRequestRange = 20'000.0;

    // Hails the nearest station within kDockRequestRange. Does NOT decide the
    // answer: it queues the request, and GameContent drains it, asks the
    // dock_request hook, and calls grantDocking/denyDocking below. That is the
    // same "C++ enumerates, Lua composes, C++ validates" shape signal_loot,
    // wreck_loot and mission_board already use. False (with a comms line) when
    // docked, already cleared, or with nothing in range.
    bool requestDocking();
    // Pending hail for GameContent to answer, if any. True once per request.
    // `outRoll` is 0..1 and is the only entropy the dock_request hook gets, so
    // the dispatcher's policy stays a pure function of what it is handed —
    // the same rule mission_board and signal_loot are held to.
    [[nodiscard]] bool takeDockRequest(std::uint32_t& outStation, double& outRoll);
    // The two answers. grantDocking validates the berth index and refuses a
    // station that is no longer the one asked about.
    bool grantDocking(std::uint32_t station, std::uint32_t berth, const std::string& message);
    void denyDocking(std::uint32_t station, const std::string& message);

    [[nodiscard]] const DockClearance& clearance() const { return m_clearance; }

    [[nodiscard]] bool hasClearance() const { return m_clearance.station != kNoIndex; }

    // Where the cleared berth is, in sim space. Only meaningful with a
    // clearance; returns the origin otherwise.
    [[nodiscard]] sol::core::DVec3 clearedBerthPoint() const;
    // Drops any clearance, saying why on the comms line when `reason` is set.
    void clearClearance(const char* reason);

    // --- The transponder (Phase 36 stage A, decisions/017) -----------------
    //
    // What your ship tells everybody about itself, and the only piece of legal
    // state that is a SWITCH rather than a consequence. On is the default and
    // the honest state; off is "running dark".
    //
    // ⚑⚑⚑⚑ DARK COSTS YOU CLEARANCE, AND THAT IS THE PHASE'S FIRST RULING
    // (the user, 2026-09-01). A station will not clear a ship that will not
    // identify itself. It is enforced through the `dock_request` hook rather
    // than hardcoded here - the hook is handed `dark` and decides, exactly as
    // it already decides on `hostile` - so a mod can author a station that does
    // not care, and the scriptless default refuses on the same terms.
    //
    // ⚑⚑ WHY THE SWITCH NEEDS A PRICE AT ALL: without one, dark is strictly
    // dominant. A player flips it once on session one and never touches it
    // again, the mechanic has no idle state, and stage B would be tuning a
    // condition nobody ever leaves. Clearance is the cheapest real price in the
    // tree because `DockClearance` is ALREADY a timed, revocable grant - which
    // is also why going dark while holding one drops it, the first time that
    // revocability has ever been used for anything but a timeout.
    [[nodiscard]] bool transponderOn() const { return m_transponderOn; }

    // True when the ship is running dark. Spelled both ways on purpose: every
    // caller reads better one way or the other, and a `!` in a UI condition is
    // how a lamp ends up lit backwards.
    [[nodiscard]] bool runningDark() const { return !m_transponderOn; }

    // Sets it, and says so on the comms channel. Returns true when the state
    // actually changed, so a caller can tell a toggle from a no-op.
    //
    // ⚑ Switching OFF drops any docking clearance in hand: the grant was made
    // to somebody who was identifying themselves, and they have stopped.
    bool setTransponder(bool on);

    bool toggleTransponder() { return setTransponder(!m_transponderOn); }

    // ⚑⚑⚑ WHAT YOU BROADCAST, AND IT IS DERIVED RATHER THAN STORED. A
    // registration is a fact about the hull and the save it lives in, not a
    // thing the player edits, so storing it would be a save field with one
    // writer and no author. Composed from the active ship's radio name and a
    // registration drawn from the universe seed, which makes it stable for a
    // playthrough, different between playthroughs, and free on disk.
    //
    // ⚑ Stage E's spoofer is what makes this a stored override rather than a
    // pure function, and that is stage E's save bump to owe - not this one's.
    [[nodiscard]] std::string broadcastIdentity() const;

    // What a listener hears: the identity, or the empty string when dark.
    // The one place the switch and the identity are combined, so no caller has
    // to remember to check both.
    [[nodiscard]] std::string broadcastHeard() const
    {
        return m_transponderOn ? broadcastIdentity() : std::string();
    }

    // --- Comms (Phase 8r) ---
    // A short transient log of what has been said to the player. Built for
    // docking clearance, but deliberately not named after it: the pilot-info
    // half of the same playtest note inherits a channel instead of starting
    // from nothing.
    struct CommsMessage
    {
        std::string from;
        std::string text;
        double secondsLeft = 0.0;
    };

    static constexpr std::size_t kCommsLines = 3;
    static constexpr double kCommsMessageSeconds = 8.0;
    void say(const std::string& from, const std::string& text);

    [[nodiscard]] std::span<const CommsMessage> comms() const { return m_comms; }

    // --- Territory (Phase 8u) ---
    //
    // Drains the sim's contest resolutions: forwards each to MissionSim so a
    // Hold objective can settle, announces the ones the player is standing
    // in, and logs the rest. Also speaks once when a contest opens over the
    // player's head. Called from the coarse faction tick.
    void drainContestResolutions();
    // "Fleetcom" - a short callsign, because 8s established that the comms
    // sender column is 116 px and a faction name prints straight through its
    // own message.
    static constexpr const char* kFleetcom = "Fleetcom";

    // --- Attrition (Phase 8x) ---
    //
    // Drains the coarse trader losses the faction sim rolled this tick and
    // logs them. One drain and one log line for every road to the same event,
    // so a hauler shot off the player's bow and a hauler lost forty light
    // years away are reported by the same code.
    void drainTraderLosses();
    // Dev lever (sol.trader_kill): kills a coarse trader the way the running
    // game would - through its body if it has one in ANY instantiated system,
    // so the wreck, the loot and the standing hit are all real.
    //
    // ⚑ "Here" meant the player's sky until Phase 38 stage B, which was the
    // only sky there was. A hauler flying a leg through a bubble the player has
    // left has a body exactly like one flying past their nose, and the lever's
    // own rule - reach only states the sim can reach - is what makes finding it
    // the right answer rather than a convenience.
    bool killCoarseTrader(std::uint32_t traderIndex);
    // Dev lever (sol.miner_kill): the same, one actor over. There is no
    // recordless road here — a miner IS its body — so this only ever destroys a
    // ship that is in the sky, through the death path a raider's shot uses.
    bool killMinerPuppet(std::uint32_t market);

    // How many traders have been lost since this session started. A probe, not
    // sim state: it is never saved, and nothing reads it but the console.
    [[nodiscard]] std::uint32_t traderLossCount() const { return m_traderLossCount; }

    // --- Pilot comms (Phase 8s) ---
    //
    // Talking to another ship, on the channel 8r built general on purpose. The
    // shape is requestDocking()'s, deliberately: the world queues a hail, and
    // GameContent drains it, asks the pilot_hail hook and calls one of the
    // three answers below. A drive that knows one knows the other.
    //
    // Same 20 km a station is hailed from, so the player learns one number.
    static constexpr double kHailRange = kDockRequestRange;

    // Everything a pilot could know about themselves and about the player,
    // enumerated here so the hook composes words rather than deciding facts.
    struct HailRequest
    {
        sol::ecs::Entity pilot;
        std::string name;
        std::string factionName; // empty for an unaffiliated console spawn
        const char* role = "";
        const char* attitude = ""; // "hostile"/"neutral"/"friendly"/"none"
        double standing = 0.0;
        bool hostile = false;
        // Whether this pilot has anything of each kind left to say. The hook
        // picks WHICH KIND of tip to offer; the engine picks which market or
        // which site, because a tip is a claim about the galaxy and a script
        // that could name the position could bookmark interstellar space.
        bool canTipMarket = false;
        bool canTipPlace = false;
        double roll = 0.0; // 0..1, the hook's only entropy
    };

    // Hails the selected ship contact. False (with a comms line saying why)
    // with nothing selected, a non-ship selected, or the ship out of range.
    // A pilot who has already spoken repeats themselves verbatim rather than
    // re-rolling, which is what stops a hail being a slot machine.
    bool hailTarget();
    // Pending hail for GameContent to answer. True once per request.
    [[nodiscard]] bool takeHailRequest(HailRequest& out);

    // The three answers, valid only while a hail is being answered. Each says
    // the words and records them against the pilot; the two tip forms also
    // write what the pilot knew into SurveySim, where the Trade tab and the
    // maps already read it back.
    [[nodiscard]] bool answeringHail() const { return !isNull(m_answeringHail.pilot); }

    bool replyHail(const std::string& message);
    bool tipMarket(const std::string& message);
    bool tipPlace(const std::string& message);
    // Closes the answering window. GameContent calls it after the hook has run
    // and its own scriptless default has had a turn, so a hook that errors
    // halfway cannot leave the builders callable from on_tick.
    void finishHail();

    // How many pilots have been hailed in this system (the console probe).
    [[nodiscard]] std::size_t hailCount() const { return m_hails.size(); }

    // --- Trading (Phase 7 economy; player trades ride the same markets the
    // NPC agents move) ---
    [[nodiscard]] const sol::sim::Economy& economy() const { return m_economy; }

    // Whether a coarse trader currently has a body here (Phase 8x). Answered
    // off the reconcile's own bookkeeping, so it cannot disagree with what is
    // actually in the sky.
    [[nodiscard]] bool traderHasBody(std::uint32_t traderIndex) const
    {
        return traderIndex < m_puppetPresent.size() && m_puppetPresent[traderIndex] != 0;
    }

    // Where that body is, when there is one. The escort marker's whole job
    // (Phase 8x §E), and the reason it can be a moving one.
    [[nodiscard]] bool traderBodyPosition(std::uint32_t traderIndex, sol::core::DVec3* out) const;
    // Every trader body in the system, for the console.
    void traderPuppetInfo(std::vector<TraderPuppetInfo>& out);
    // Every miner body in the system, and the outposts that are drawing
    // without one — because the two disagreeing is what a broken promotion
    // looks like from the outside (Phase 8x stage 6).
    void minerPuppetInfo(std::vector<MinerPuppetInfo>& out);

    // Seconds an outpost's draw is stopped for, or 0. The probe's other half:
    // a mine with no miner and no hold is a bug, and one with a hold is the
    // player's own doing.
    [[nodiscard]] double minerHold(std::uint32_t market) const
    {
        return market < m_minerHold.size() ? m_minerHold[market] : 0.0;
    }

    [[nodiscard]] const std::vector<std::string>& commodityIds() const { return m_commodityIds; }

    // The modules one station is composed of, as indices into `defs.modules()`,
    // or empty when it has no composition (Phase 34 stage B). This is the read
    // path stage C's screens and stage D's holds are built on, and the one the
    // guards use to score the galaxy the game actually generates.
    [[nodiscard]] std::span<const std::uint32_t> stationModules(std::uint32_t system,
                                                                std::uint32_t station) const;

    // How many distinct compositions the galaxy needed. One per station is the
    // worst case and roughly what a galaxy of optional modules produces; the
    // number is worth reporting because it is the size of the table every
    // market indexes into.
    [[nodiscard]] std::size_t compositionCount() const { return m_compositions.size(); }

    // Which dock screens one station offers, as a bit per
    // `assets::StationScreen` (Phase 34 stage C): the UNION over the modules it
    // is composed of, which is what turns gdd.md §12's "a mining outpost with no
    // market floor has no Trade tab" from a sentence into a filter.
    //
    // ⚑ A station with no composition offers EVERY screen, the same fallback
    // `Economy::initialize` takes when it finds no composition - a galaxy built
    // without `[[module]]` content is the galaxy that shipped before this phase
    // and must keep behaving like it.
    //
    // ⚑⚑ RULED 2026-08-31: `Factions` and `Survey` are NOT in this mask's gift.
    // No module offers either, and neither is a facility - standings are a fact
    // about the galaxy and a scan ledger is the player's own, with shipped hint
    // text that says so ("scan data sells anywhere"). They are unconditional in
    // the screen that draws the strip, not here, because this function answers
    // one question only: what is this station EQUIPPED to do.
    [[nodiscard]] std::uint32_t stationScreens(std::uint32_t system, std::uint32_t station) const;

    // The same, for the station the player is docked at. Zero when not docked.
    [[nodiscard]] std::uint32_t dockedStationScreens() const;

    // Whether the station the player is docked at has any hold for this good
    // (Phase 34 stage D). False is a REFUSAL rather than an empty shelf: the
    // market has no capacity for it, so it cannot be bought, sold or delivered
    // there, and the trade board leaves the row off entirely.
    //
    // ⚑⚑ THE FILL HAS TO ASK, BECAUSE THE PRICE CANNOT SAY. `priceAtStock`
    // reads a zero-capacity market as a FULL one - fraction 1.0 - and returns
    // the glut price, so a good a station cannot hold would otherwise list at a
    // flat half-price and read as the bargain of the galaxy. The sim already
    // refuses the trade; this is what stops the board offering it.
    // Whether a station has a hold for a good at all (Phase 34 stage D, opened
    // to any station by 35 stage B). ⚑ Not the same question as "is any in
    // stock": a station with no hold for salvage never gets a trade row for it,
    // and the Bar is where the player finds out why.
    [[nodiscard]] bool
    stationStocks(std::uint32_t system, std::uint32_t station, std::uint32_t commodity) const;
    [[nodiscard]] bool dockedStationStocks(std::uint32_t commodity) const;

    // Who runs the black-market module on this dock (Phase 34 stage E), as a
    // faction table index, or `kNoFaction` where nobody does. A STORED fact
    // about the station: the operator does not change when the border does.
    [[nodiscard]] std::uint32_t stationShadowOwner(std::uint32_t system, std::uint32_t station) const;

    // Whether there is a back room here at all - a counter somebody other than
    // the law is standing behind.
    //
    // ⚑⚑⚑⚑ THIS USED TO BE A COMPARISON AND PHASE 37 STAGE C RETIRED THE
    // OTHER HALF OF IT, WHICH IS WORTH RECORDING RATHER THAN QUIETLY DELETING.
    // Phase 34 stage E wrote it as `owner != systemOwnerFaction(system)` and
    // was right to: the operator was a pirate CLAN, a clan can take the system
    // its own fence sits in, and on the day it did the fence stopped being a
    // shadow presence and became the local boss's shop. That reading had to be
    // derived per call and never stored, because ownership has been dynamic
    // since Phase 8u and a stored answer would have been right at t=0 and wrong
    // for the rest of the session.
    //
    // ⚑⚑ THE RULE DID NOT TURN OUT TO BE WRONG - ITS SUBJECT WENT AWAY. Ruling
    // 1 of Phase 37 is one hand-authored black market, and a faction that claims
    // nothing can never be a system's holder, so the comparison is now true at
    // every station in every galaxy. A condition that cannot be false is a
    // comment pretending to be code, so it is a comment.
    [[nodiscard]] bool stationHasShadowPresence(std::uint32_t system, std::uint32_t station) const;


    // --- The cast (Phase 35 stage C) ---------------------------------------
    //
    // ⚑⚑⚑⚑ EVERY ROOM HAS SOMEBODY, AND ONLY SOME OF THEM WERE WRITTEN BY A
    // PERSON. That split is the stage, and it is the phase's own risk register
    // discharged: *"a character with one line is worse than no character"*. The
    // ENGINE half seats a regular in all 62 rooms - a name built from syllables
    // the way a system's name has been since Phase 6, and a trade read off the
    // station's own composition - which costs no writing at all. The AUTHORED
    // half is `characters.toml` plus `scripts/cast.lua`, and one of those rows
    // takes the seat a regular would otherwise have had.
    //
    // ⚑⚑ DERIVED FROM THE SEED AND NEVER SAVED - who sits where costs nothing
    // on disk, exactly like the composition and the shadow operator. What IS
    // saved is what the player did: see `castMemory` below.
    struct CastSeat
    {
        // The `[[character]]` this seat was given to, or `kNoCharacter` for a
        // regular the generator named. An index into `DefDatabase::characters`.
        std::uint32_t character = kNoCharacter;
        // Copied in rather than looked up, for the reason every other figure a
        // composed station carries is: the readers of this run with no def
        // database in reach, and a regular's name exists in no def at all.
        std::string name;
        std::string trade;
    };

    // Who is in the room on this dock, or nullptr where the station has no
    // recreation module. A station with a room ALWAYS has somebody in it.
    [[nodiscard]] const CastSeat* stationCast(std::uint32_t system, std::uint32_t station) const;

    // What the save remembers about one person: whether you have met, how many
    // times you have been in, and how they feel about you.
    //
    // ⚑⚑ `regard` IS WRITTEN NOW AND SPENT IN STAGE D, and saying so here is
    // the alternative to bumping `kSaveVersion` twice for one feature. Stage D
    // makes a bar a second posting facility and has to keep an informal lead
    // distinguishable from a board contract; a relationship is the thing that
    // does that, and it has to have been accumulating before it can gate
    // anything. Nothing reads it yet, and that is recorded rather than hidden.
    struct CastMemory
    {
        // ⚑⚑⚑ A UNIQUE IS A PERSON AND A REGULAR IS A CHAIR, AND THE KEY SAYS
        // WHICH. An authored character is keyed by a hash of their `[[character]]`
        // id, so they carry their memory even if a later build's recipes move
        // them to a different dock - they are somebody, and somebody who moves
        // is still them. A regular is keyed by the SEAT, `kSeatKey | system |
        // station`, because a regular IS the room: there is nobody there to
        // follow, and a generated name that changed under a save would be a
        // different person wearing the same memory.
        std::uint64_t who = 0;
        std::uint32_t visits = 0;
        std::int32_t regard = 0;
    };

    static constexpr std::uint32_t kNoCharacter = 0xFFFF'FFFFu;
    // Top bit set: a seat rather than a person. `fnv1a` of an id is masked to
    // 63 bits so the two spaces cannot collide.
    static constexpr std::uint64_t kSeatKey = 1ull << 63;

    [[nodiscard]] static std::uint64_t castKeyForSeat(std::uint32_t system, std::uint32_t station)
    {
        return kSeatKey | (static_cast<std::uint64_t>(system) << 20u) | station;
    }

    [[nodiscard]] static std::uint64_t castKeyForCharacter(const char* id);

    // The key of whoever is in the room on this dock, or 0 where nobody is.
    [[nodiscard]] std::uint64_t castKeyAt(std::uint32_t system, std::uint32_t station) const;

    // What is remembered about `who`, or nullptr if the player has never
    // spoken to them. SPARSE on purpose: a galaxy has 62 rooms and a save
    // should carry the ones the player actually walked into, not a row per
    // chair in case they ever do.
    [[nodiscard]] const CastMemory* castMemory(std::uint64_t who) const;

    // One visit, from the player walking into the room. Creates the row on the
    // first call - which is what makes `visits == 1` mean "we have just met".
    void noteCastVisit(std::uint64_t who);

    // Move a relationship. Nothing calls this in stage C; stage D does.
    void adjustCastRegard(std::uint64_t who, std::int32_t delta);

    [[nodiscard]] std::span<const CastMemory> castMemories() const { return m_castMemory; }

    // Everybody in a room within `MissionParams::candidateReach` of `from`, for
    // the bar's fifth source (Phase 35 stage C). Rooms in `from`'s OWN system
    // are left out: `chooseCastTalk` refuses them anyway, and a tip about a dock
    // you can reach without jumping is a window rather than a tip.
    //
    // ⚑ Game-side because the cast is, and it shares `MissionSim::jumpDepths`
    // rather than walking the gate graph again - the reach is the board's
    // number and this only asks what it currently means.
    void castCandidates(std::uint32_t from, std::vector<sol::sim::CastCandidate>& out) const;

    // The seating rule itself, for the reason `shadowOperatorFor` is public:
    // the shipped galaxy does not exercise all of it. Returns true when the
    // seat at (`founder`, `region`, `archetype`, `room`, `hasShadow`) satisfies
    // every anchor `entry` declares.
    struct CastEntry
    {
        std::uint32_t faction = sol::sim::kNoFaction; // founding claim to match
        std::uint32_t archetype = kNoIndex;
        std::uint32_t room = kNoIndex;   // a `[[module]]` index
        std::uint32_t region = kNoIndex; // Region, as an index
        bool lawless = false;
        bool shadow = false;
    };

    struct CastSeatFacts
    {
        std::uint32_t founder = sol::sim::kNoFaction;
        std::uint32_t archetype = kNoIndex;
        std::uint32_t room = kNoIndex;
        std::uint32_t region = kNoIndex;
        bool hasShadow = false;
    };

    [[nodiscard]] static bool
    castSeatSuits(const CastEntry& entry, const CastSeatFacts& seat, std::uint32_t clanBase);

    // What goods class a commodity is, for the readouts that say so.
    [[nodiscard]] sol::assets::GoodsClass commodityClass(std::uint32_t commodity) const
    {
        return commodity < m_commodityClass.size() ? m_commodityClass[commodity]
                                                   : sol::assets::GoodsClass::Bulk;
    }

    [[nodiscard]] std::uint32_t commodityIndex(const char* id) const;

    [[nodiscard]] double playerCredits() const { return m_playerCredits; }

    // Dev-console cheat (sol.warp): teleports the flying player to a
    // station-relative offset in the current system. False while docked or
    // with a bad station index.
    bool warpToStationOffset(std::uint32_t station, const sol::core::DVec3& offset);

    // Dev-console cheat (sol.add_credits); clamps at zero.
    void addCredits(double amount)
    {
        m_playerCredits = m_playerCredits + amount > 0.0 ? m_playerCredits + amount : 0.0;
    }

    // Dev-console cheat and test lever (sol.add_cargo), clamped to the hold's
    // free space; returns what actually went in. Negative takes units out.
    //
    // ⚑ IT EXISTS BECAUSE PHASE 36 STAGE D CANNOT BE REACHED WITHOUT IT.
    // Every other road into `m_playerCargo` is a trade at a market, a mined
    // rock or a salvaged wreck - so putting one crate of `sol.salvage` in the
    // hold and flying it into Hegemony space, which is this phase's own exit
    // criterion, is an hour of play or a lever. Stage A's sharpest lesson was
    // that the live drive finds defects no test can, and that only holds if
    // the drive can reach the thing it is meant to look at.
    float addPlayerCargo(std::uint32_t commodity, float units);

    [[nodiscard]] float playerCargo(std::uint32_t commodity) const
    {
        return commodity < m_playerCargo.size() ? m_playerCargo[commodity] : 0.0f;
    }

    [[nodiscard]] float playerCargoTotal() const;

    [[nodiscard]] float playerCargoCapacity() const { return m_playerCargoCapacity; }

    // Market of the docked station, or an invalid index while flying.
    [[nodiscard]] std::uint32_t dockedMarket() const;
    // Buy/sell at the docked station, clamped to stock, cargo space, and
    // credits; returns what actually moved.
    sol::sim::TradeResult playerBuy(std::uint32_t commodity, float units);
    sol::sim::TradeResult playerSell(std::uint32_t commodity, float units);

    // --- Outfitting & fleet (Phase 8a; decisions/006 crew, /007 death) ---
    // Every mutation below requires being docked and returns false with a
    // logged reason (and outError, if given) when refused.
    static constexpr double kInsuranceRate = 0.05; // deductible, of ship value
    static constexpr double kResaleRate = 0.5;     // sell-back, of list price

    [[nodiscard]] const std::vector<OwnedShip>& fleet() const { return m_fleet; }

    [[nodiscard]] std::size_t activeShipIndex() const { return m_activeShip; }

    [[nodiscard]] const OwnedShip& activeShip() const { return m_fleet[m_activeShip]; }

    // Hull + everything in its mounts at list price (crew hires excluded).
    [[nodiscard]] double shipValue(const OwnedShip& ship) const;

    [[nodiscard]] double insuranceDeductible() const { return kInsuranceRate * shipValue(activeShip()); }

    // Buy a component or weapon and put it in a mount (Phase 31 stage B).
    // `mountId` empty or null picks the first EMPTY mount on the hull that
    // accepts it, which is what a catalog "Buy" button means; a named mount
    // that is already occupied swaps, selling the old fitting back at
    // `kResaleRate` in the same transaction - the behaviour the single weapon
    // mount used to have, generalised to every place on the ship.
    bool buyFitting(const char* defId, const char* mountId = nullptr, std::string* outError = nullptr);

    // Remove what is in a mount and refund it at `kResaleRate`.
    bool sellFitting(const char* mountId, std::string* outError = nullptr);

    // Which mount `buyFitting` would choose, or empty if the hull has no free
    // place for it. Public because the outfitting screen has to grey a Buy
    // button that would be refused, and duplicating the rule there is how the
    // button and the transaction drift apart.
    [[nodiscard]] std::string firstFreeMountFor(const char* defId) const;

    // The ship's base def with its fit and crew resolved (Phase 8a); falls
    // back to the base def when ids have gone missing from the data. Since
    // Phase 31 stage B the returned def's MOUNTS carry what is actually in
    // them, which is what makes it "the ship as flown" rather than "the hull
    // with better numbers".
    //
    // ⚑ Public rather than private, and it is the same argument as
    // `shipValue`'s beside it: this is a query about an OwnedShip anyone
    // holding one can ask, and the answer is what every other screen's numbers
    // are derived from. It is also the only way to ask what a fit resolves to
    // WITHOUT being docked, which a test has to be able to do.
    [[nodiscard]] sol::assets::ShipDef resolvedShipDef(const OwnedShip& ship) const;

    bool buyShip(const char* shipDefId, std::string* outError = nullptr);
    bool sellShip(std::size_t fleetIndex, std::string* outError = nullptr);
    bool switchShip(std::size_t fleetIndex, std::string* outError = nullptr);
    bool hireCrew(const char* crewId, std::string* outError = nullptr);
    bool fireCrew(const char* crewId, std::string* outError = nullptr);

    // --- Captains (Phase 39 stage A) ---------------------------------------
    //
    // ⚑⚑⚑ EVERY ONE OF THESE REQUIRES BEING DOCKED, and the assignment pair
    // requires being docked AT THE STATION THE HULL IS PARKED AT - the same
    // rule `sellShip` and `switchShip` already enforce, and for a better
    // reason than symmetry: a captain has to physically take the ship, so both
    // of you have to be standing there.
    //
    // ⚑ HIRING IS FREE, AND THAT IS RULING 3 HELD RATHER THAN SOFTENED. The
    // ruling took "a cut of what the ship earns" OVER a one-time fee; charging
    // both would be re-litigating it. The scarce thing is hulls, not people.
    static constexpr std::uint32_t kCaptainsPerHall = 3;
    static constexpr float kCaptainCutMin = 0.08f;
    static constexpr float kCaptainCutMax = 0.20f;

    [[nodiscard]] const std::vector<Captain>& captains() const { return m_captains; }

    // Who flies this hull, or nullptr for one nobody has been given.
    [[nodiscard]] const Captain* captainOf(std::size_t fleetIndex) const;

    // Everybody looking for a berth in the docked station's crew hall. Empty
    // while flying and at a station with no `crew` screen. Rebuilt on every
    // call from the seed, so an index into it is good for exactly as long as
    // the caller holds it - which is why the mutators below take one.
    void captainCandidates(std::vector<CaptainCandidate>& out) const;

    bool hireCaptain(std::size_t candidateIndex, std::string* outError = nullptr);
    // ⚑⚑ REFUSES WHILE THEY HOLD A SHIP, rather than quietly unassigning.
    // In stage A that is tidiness; from stage B on, the hull is somewhere else
    // flying a route, and a dismissal that silently abandoned it is how a
    // freighter goes missing. One action does one thing.
    bool dismissCaptain(std::size_t captainIndex, std::string* outError = nullptr);
    bool assignCaptain(std::size_t captainIndex, std::size_t fleetIndex, std::string* outError = nullptr);
    bool recallCaptain(std::size_t captainIndex, std::string* outError = nullptr);

    // --- Factions & reputation (Phase 8b) ---
    static constexpr float kClanInitialStanding = -20.0f;   // dockable, wary
    static constexpr float kDefaultPirateRelation = -60.0f; // unspecified pairs

    [[nodiscard]] const std::vector<GameFaction>& factions() const { return m_factionTable; }

    // ⚑⚑⚑ WHERE THE SHADOW ROWS START, WHICH IS ALSO WHERE MAJORS + CLANS END
    // (Phase 37 stage B). `kNoFaction` when no `kind = "shadow"` def is loaded,
    // which is every galaxy built from a def set that does not carry one - a
    // mod, and the trimmed def sets several test suites build.
    //
    // ⚑⚑ THE ACCESSOR EXISTS BECAUSE THE ARITHMETIC STOPPED WORKING. The table
    // is majors, then clans, then shadow; before this stage its size minus the
    // clan count WAS the major count, and that identity is now false. It is the
    // same shape as `assignShadowOwners`'s clanBase - a boundary in a table
    // whose sections are ordered by construction - and it is exposed rather
    // than recomputed so the next reader does not re-derive it wrongly.
    [[nodiscard]] std::uint32_t shadowFactionBase() const { return m_shadowBase; }

    // The one black market, or `kNoFaction`. Ruling 1 of Phase 37 is that there
    // is exactly ONE hand-authored shadow faction, so the base index IS the
    // identity; this spelling is what callers who mean "the Ninth Shift" should
    // say, and it will stop compiling rather than quietly pick the first of two
    // if that ruling is ever reversed.
    [[nodiscard]] std::uint32_t shadowFactionIndex() const { return m_shadowBase; }

    [[nodiscard]] const sol::sim::FactionSim& factionSim() const { return m_factionSim; }

    [[nodiscard]] sol::sim::FactionSim& factionSim() { return m_factionSim; }

    // The faction holding a system (and its stations) NOW, or an out-of-table
    // value for ownerless systems (factionCount == 0 galaxies). Since Phase
    // 8u this is dynamic state in FactionSim rather than the generated plan:
    // SystemSpec::factionIndex is the founding claim and never moves, and
    // every consumer of ownership in this game reads it through here.
    [[nodiscard]] std::uint32_t systemOwnerFaction(std::uint32_t systemIndex) const
    {
        return systemIndex < m_galaxy.systems.size() ? m_factionSim.systemOwner(systemIndex) : kNoIndex;
    }

    // --- Law and legality (Phase 33 stage D, gdd.md 13) --------------------
    //
    // ⚑⚑⚑⚑ IT READS `systemOwnerFaction`, NOT `SystemSpec::factionIndex`, AND
    // THE PHASE SPEC NAMED THE SECOND. `factionIndex` is the FOUNDING CLAIM and
    // never moves; Phase 8u made ownership dynamic in `FactionSim`, and the
    // galaxy hands systems back and forth several times a minute. A legality
    // table keyed on the founding claim would tell a player that a system taken
    // by the Hegemony this morning still runs Compact law - and no test that
    // never transfers a system could see it, because at t=0 the two fields
    // agree. `systemOwnerFaction`'s own comment already said "every consumer of
    // ownership in this game reads it through here"; this is a consumer.
    //
    // ⚑⚑ A PIRATE CLAN IS A JURISDICTION. It holds the space and it stops
    // ships; it simply has no table, so nothing you carry is contraband to it.
    // That falls out of the mechanism rather than being a case, and it is why
    // clan space is where a smuggler breathes out. `Unpoliced` is reserved for
    // a system with no holder at all, which in the shipped galaxy is exactly
    // one dock (`sol.lantern`).
    [[nodiscard]] sol::assets::Legality commodityLegality(std::uint32_t systemIndex,
                                                          std::uint32_t commodity) const;
    // The faction whose law applies in a system, or nullptr where nobody holds
    // it. This is the TABLE and only the table - for what to print, ask
    // `jurisdictionName` below, because for a clan the two are different
    // factions and the def is the wrong one to show.
    [[nodiscard]] const sol::assets::FactionDef* jurisdictionOf(std::uint32_t systemIndex) const;

    // ⚑⚑⚑ WHO HOLDS THE SYSTEM, WHICH IS NOT WHOSE TABLE APPLIES. A generated
    // clan carries its TEMPLATE's def id, so `jurisdictionOf` correctly returns
    // one shared Reaver Kindred table for all ten Reaver clans - and equally
    // correctly returns the wrong NAME, because the clan holding the place is
    // called `Queunth Corsairs` and appears under that name in `sol.territory`,
    // the raid log, the map and every hail. Printing the template put a faction
    // name on a third of the galaxy's docks that exists nowhere else in the
    // game, and a player had no way to connect the two.
    //
    // ⚑⚑ THE SUFFIX MAKES IT WORSE RATHER THAN HINTING AT IT: a clan is named
    // `<system> <random suffix>` and the suffix list CONTAINS both template
    // names, so `Queunth Corsairs` reads as the Corsairs and is governed by the
    // Reaver Kindred table. The two draws are independent by design
    // (`spawnClans`), so the name has never carried the template and cannot be
    // read as if it did.
    //
    // ⚑ Empty exactly where `jurisdictionOf` is null - nobody holds the place.
    // Callers key the "no jurisdiction" wording off the DEF, because that is
    // what makes a legality answerable; this only supplies the words.
    [[nodiscard]] const char* jurisdictionName(std::uint32_t systemIndex) const;

    // --- System security (Phase 30 stage A, decisions/019) -----------------
    //
    // The static half: how much force the owner keeps here, off the generated
    // spec - and SIGNED HERE, by whoever holds the system now.
    //
    // ⚑⚑⚑⚑ THE SIGN IS COMPUTED RATHER THAN STORED, AND STAGE F LEARNED WHY
    // FROM TWO SHIPPED BUGS OF ITS OWN PHASE. decisions/019 decision 2 says
    // the sign names WHO POLICES THIS PLACE; `SystemSpec::security` is written
    // at generation, and Phase 8u made ownership DYNAMIC - so a stored sign is
    // a fact about whoever founded a system, and the galaxy hands systems back
    // and forth several times a minute. Stage B sized the resident wing off it
    // (`raidersFor(+0.85)` is ZERO, so a clan that took a core system garrisoned
    // it with nothing at all) and stage D took the readout's verb from it (the
    // map said "Policed by Norea Reavers"). One view, computed where both the
    // magnitude and the current owner are in hand, makes both unrepresentable.
    //
    // ⚑ Zero has no sign, which is not pedantry: `-0.0f` compares equal to zero
    // everywhere and then prints as "-0.00" in the readout a player reads.
    // ⚑ A system nobody holds reads zero whatever magnitude it carries - that
    // IS what "nobody comes" means, and it is the only reading a place with no
    // owner can honestly be given.
    [[nodiscard]] float systemSecurityBaseline(std::uint32_t systemIndex) const
    {
        if (systemIndex >= m_galaxy.systems.size()) {
            return 0.0f;
        }
        const float magnitude = m_galaxy.systems[systemIndex].security;
        const std::uint32_t owner = systemOwnerFaction(systemIndex);
        if (magnitude == 0.0f || owner >= m_factionTable.size()) {
            return 0.0f;
        }
        return m_factionTable[owner].pirate() ? -magnitude : magnitude;
    }

    // The galaxy's gradient, counted once (Phase 30 stage F). Two callers
    // format it two different ways - the boot log's one-liner and
    // `sol.security_map`'s table - and before this they COUNTED it twice as
    // well, in two copies that a counterfactual proved neither of was tested.
    // Both then read the raw spec, so both would have reported every clan
    // neighbourhood inside whichever region band it sits in.
    struct SecurityHistogram
    {
        std::uint32_t seen[3] = {0, 0, 0}; // core / frontier / fringe
        double sum[3] = {0.0, 0.0, 0.0};
        float lowest[3] = {0.0f, 0.0f, 0.0f};
        float highest[3] = {0.0f, 0.0f, 0.0f};
        std::uint32_t clanHeld = 0; // cuts across regions, so it is its own band
        double clanSum = 0.0;
        float deepest = 0.0f;
        std::uint32_t unpoliced = 0; // exactly zero: nobody holds it

        [[nodiscard]] double mean(std::size_t tier) const
        {
            return seen[tier] > 0 ? sum[tier] / seen[tier] : 0.0;
        }

        [[nodiscard]] double clanMean() const { return clanHeld > 0 ? clanSum / clanHeld : 0.0; }
    };

    [[nodiscard]] SecurityHistogram securityHistogram() const;

    // The live half: how safe the place actually is right now. This is the
    // number a consumer reads, and it joins the two halves nothing else can -
    // the baseline lives on the galaxy and `danger` lives in the faction sim,
    // and SpaceWorld is the only thing holding both.
    //
    // ⚑⚑⚑⚑ DANGER ERODES THE MAGNITUDE AND NEVER TOUCHES THE SIGN, AND THAT
    // IS A RULING TAKEN HERE RATHER THAN DISCOVERED AS A BUG. The obvious
    // arithmetic is `baseline - danger`, and it is wrong for a reason the band
    // makes plain: the sign is not "how much", it is WHO POLICES THIS PLACE.
    // A core system under sustained raiding would cross zero and start
    // reporting that a pirate clan holds it, which is false and which every
    // downstream reader of the sign - the response dispatcher in stage C, the
    // map colour in stage D - would then act on.
    //
    // So danger walks whoever holds it toward zero and stops there, which says
    // something true in both directions: a place under enough pressure is one
    // where NOBODY'S law reaches you, and it does not matter whose it was.
    [[nodiscard]] float systemSecurity(std::uint32_t systemIndex) const
    {
        const float baseline = systemSecurityBaseline(systemIndex);
        if (baseline == 0.0f) {
            return 0.0f; // nobody polices it, so there is nothing for danger to erode
        }
        const float danger = m_factionSim.danger(systemIndex);
        const float magnitude = std::max(0.0f, std::abs(baseline) - danger);
        // ⚑ The `magnitude == 0` arm is not redundant: negating a zero gives
        // NEGATIVE zero, which compares equal to zero everywhere and then
        // prints as "-0.000" in the readout stage D puts in front of a player.
        // Zero here means "nobody is coming", and it has no sign.
        if (magnitude == 0.0f) {
            return 0.0f;
        }
        return baseline < 0.0f ? -magnitude : magnitude;
    }

    // "hostile"/"neutral"/"friendly" for a faction table index; "" outside it.
    [[nodiscard]] const char* playerAttitudeName(std::uint32_t faction) const;
    // Whether the docked station's owner stocks a gated def (Phase 8a caveat
    // fix: catalogs are the owner faction's; pirates fence past min_rep).
    [[nodiscard]] bool stationSells(const sol::assets::CatalogGate& gate) const;

    // ⚑⚑⚑ THE SECOND COUNTER, EXPOSED FOR THE ONE CALLER THAT HAS TO SORT ROWS
    // ONTO TWO SHELVES (Phase 37 stage C). `stationSells` folds both owners into
    // one answer, which is what every purchase path wants; the dock screen wants
    // to know WHICH of them said yes, because that is the difference between the
    // Outfitting list and the back room. Not a permission check - a permission
    // check that disagreed with `stationSells` would be the shop window bug its
    // own comment was written against.
    [[nodiscard]] bool stationSellsAtFence(const sol::assets::CatalogGate& gate) const;

    // Which faction runs the back room of the dock the player is standing on,
    // or `kNoFaction` at the 117 docks that have none.
    [[nodiscard]] std::uint32_t dockedFenceFaction() const;

    // ⚑⚑⚑ WHOSE SHELF A ROW BELONGS ON, WHICH IS NOT WHETHER IT CAN BE BOUGHT.
    // An item is the fence's when its gate's allowlist names the fence's owner -
    // data, not policy. `stationSellsAtFence` then says whether they would
    // actually hand it over, and the two disagree on purpose: the fence stocks
    // one thing no player can afford in standing yet, and it is on the screen
    // with its price named rather than silently absent.
    [[nodiscard]] bool stationFenceCarries(const sol::assets::CatalogGate& gate) const;
    // The commodity half of the gate (Phase 33 stage B): a station will not
    // sell a fitting it has none of the material for. Called by `stationSells`
    // rather than beside it, so no caller can ask half the question.
    [[nodiscard]] bool stationStocksRequirement(const sol::assets::CatalogGate& gate) const;
    // Lua-chosen raid (sol.faction_raid); validates via the sim's candidates.
    bool commitFactionRaid(std::uint32_t faction, std::uint32_t targetSystem);

    // Scriptless fallback for one due decision (no faction_think hook).
    void applyDefaultFactionDecision(const sol::sim::FactionDecision& decision)
    {
        m_factionSim.applyDefaultDecision(m_galaxy, &m_economy, decision);
    }

    // --- Missions & contracts (Phase 8c) ---
    [[nodiscard]] const sol::sim::MissionSim& missionSim() const { return m_missions; }

    [[nodiscard]] sol::sim::MissionSim& missionSim() { return m_missions; }

    // Player accept/abandon with the game-side gates (docked; standing with
    // the poster enforces the offer's min_rep tier).
    bool acceptMission(std::uint32_t offerIndex, std::string* outError = nullptr);
    // Takes work offered in the room rather than posted on the board (Phase 35
    // stage D), and moves the regard of whoever offered it. Indexes
    // `MissionSim::leads()`.
    bool acceptLead(std::uint32_t leadIndex, std::string* outError = nullptr);
    bool abandonMission(std::uint32_t activeIndex);

    // True once after each successful dock (and death-respawn re-dock);
    // GameContent consumes it to re-open the board and run the Lua hook.
    [[nodiscard]] bool consumeDockEvent()
    {
        const bool pending = m_dockEventPending;
        m_dockEventPending = false;
        return pending;
    }

    // Drains mission events whose consequences (payout, standing) this world
    // already applied; GameContent forwards campaign ones to Lua.
    void takeMissionEvents(std::vector<sol::sim::MissionEvent>& out);

    // --- Exploration & scanning (Phase 8e) ---
    // Two verbs: a pulse that reveals contacts within the scanner's range,
    // and a target scan that resolves whatever is held in the reticle. The
    // knowledge, the ledger, and the loot all live in SurveySim.
    static constexpr double kPulseCooldownSeconds = 6.0;
    static constexpr double kTargetScanSeconds = 5.0;
    // A target scan works far closer than a pulse reaches: you find a contact
    // from across the playfield, then fly to it to learn what it is.
    static constexpr double kTargetScanRangeFraction = 0.02;
    static constexpr double kScanConeCosine = 0.94; // ~20 degrees off boresight
    static constexpr double kSalvageRange = 2'000.0;
    // Standing paid for survey data, per credit, on top of the sale.
    static constexpr double kSurveyStandingRate = 0.001;

    [[nodiscard]] const sol::sim::SurveySim& survey() const { return m_survey; }

    [[nodiscard]] sol::sim::SurveySim& survey() { return m_survey; }

    // Every site in the current system, discovered or not (the HUD only ever
    // shows the ones SurveySim says the player has found).
    [[nodiscard]] std::span<const SignalInstance> signals() const { return m_signals; }

    [[nodiscard]] float scanRange() const { return m_scanRange; }

    [[nodiscard]] float scanSpeed() const { return m_scanSpeed; }

    [[nodiscard]] double targetScanRange() const
    {
        return static_cast<double>(m_scanRange) * kTargetScanRangeFraction;
    }

    // 0 right after a pulse, 1 when the next one is ready.
    [[nodiscard]] float pulseCharge() const;

    // 0..1 progress of the scan in flight, and what it is resolving.
    [[nodiscard]] float scanProgress() const { return m_scanProgress; }

    [[nodiscard]] const char* scanTargetName() const;
    // Fires a pulse: returns how many new contacts it found, or -1 when the
    // scanner is still charging (or the ship is docked).
    int pulseScan();
    // Dev/console: resolves whatever is targeted without flying to it.
    bool scanCurrentTarget();

    // Nearest resolved, unemptied site within range, or a negative value.
    [[nodiscard]] double nearestSalvageDistance() const;
    // Empties the nearest site into the hold: cargo up to the space left, a
    // component into a free slot, credits always. A partial take leaves the rest.
    bool trySalvageNearest(double range);
    // Sells the survey ledger at the docked station; returns the credits paid.
    double sellSurveyData();
    // Loot composed by the Lua hook (validated in SurveySim).
    bool applySignalLoot(std::uint32_t system, std::uint32_t signal, sol::sim::SignalLoot loot);
    void takeSurveyEvents(std::vector<SurveyEvent>& out);
    // Plots a gate route from here to destination (galaxy map); false when
    // there is no route or the destination is where the player already is.
    bool plotRoute(std::uint32_t destination);

    // --- Mining, salvage & refining (Phase 8f) ---
    // One verb: hold a beam with mining_power on a rock or a wreck and what
    // comes off drifts as ore chunks the ship then has to gather.
    static constexpr double kChunkLifetimeSeconds = 90.0;
    static constexpr double kChunkDriftSpeed = 30.0;
    static constexpr double kChunkSpread = 0.45;     // how far off "toward you"
    static constexpr float kChunkUnitCeiling = 6.0f; // one chunk never holds more
    // Standing paid for a refinery order, per credit of fee.
    static constexpr double kRefineStandingRate = 0.0008;

    [[nodiscard]] const sol::sim::MiningSim& mining() const { return m_mining; }

    [[nodiscard]] sol::sim::MiningSim& mining() { return m_mining; }

    [[nodiscard]] float collectorRange() const { return m_collectorRange; }

    // What the boresight is on, for the prospecting readout. Rocks are read
    // off the crosshair rather than through the target cycle: a field holds
    // dozens of them, and T-cycling through forty rocks is not a UI.
    [[nodiscard]] ProspectInfo prospectAhead() const;

    // Units collected in the last moment, for the HUD ticker (0 when idle).
    [[nodiscard]] float lastCollectedUnits() const { return m_collectTicker; }

    [[nodiscard]] const char* lastCollectedName() const { return m_collectName.c_str(); }

    // Dev/console: empties the rock the boresight is on into the hold.
    bool mineAhead();
    // Dev/console (sol.warp_rock): parks the ship just off the nearest rock
    // with the nose on it — test pacing for the beam, in the same spirit as
    // sol.warp. False while docked or with no rock in the system.
    bool warpToNearestRock();
    // The general form: parks the ship `standoff` meters short of a point,
    // approaching from wherever it already is, with the nose on it.
    bool warpTo(const sol::core::DVec3& target, double standoff);

    // Composes a wreck's contents from the victim's fit and cargo (the
    // scriptless default; the Lua hook may replace it before it is cut).
    //
    // ⚑ PUBLIC SINCE PHASE 33 STAGE C, and only so that a test can ask it what
    // a given hull drops without killing a ship to find out. It is already a
    // pure function of a `ShipDef` and a seed - it reads no world state beyond
    // the def database - and it is already exposed to content through the
    // `wreck_loot` Lua hook, so this widens who can call it and not what it is.
    [[nodiscard]] sol::sim::SignalLoot defaultWreckLoot(const sol::assets::ShipDef* def,
                                                        std::uint64_t seed) const;
    // Wreck loot composed by the Lua hook (validated in MiningSim).
    bool applyWreckLoot(std::uint32_t id, sol::sim::SignalLoot loot);
    void takeWreckEvents(std::vector<WreckEvent>& out);
    void takeRockEvents(std::vector<RockEvent>& out);

    // Refining: the docked station takes ore off your hands and hands back
    // metal later, at the market you ordered it from.
    [[nodiscard]] bool dockedStationRefines() const;
    // The docked station archetype's refine_input/refine_output as commodity
    // indices, or ~0u when this station refines nothing.
    [[nodiscard]] std::uint32_t refineInputCommodity() const;
    [[nodiscard]] std::uint32_t refineOutputCommodity() const;
    // Ore waiting to be collected here, and the wait on the next order.
    [[nodiscard]] float refinedReadyHere() const;
    [[nodiscard]] double refineWaitHere() const;
    // Places an order: takes the ore and the fee, queues the job. False with
    // a logged reason (and outError) when refused.
    bool orderRefine(float units, std::string* outError = nullptr);
    // Collects finished metal into the hold; returns the units taken.
    float collectRefined();

    // --- Market intel (Phase 8g) ---
    // The counterpart to 8e's survey ledger: there the player sells what they
    // found, here they buy what someone else found.
    [[nodiscard]] double worldSeconds() const { return m_worldSeconds; }

    // Cost of the intel package sold at the docked station, and how many
    // markets it would actually add or refresh.
    [[nodiscard]] double intelPrice() const;
    [[nodiscard]] std::uint32_t intelMarketCount() const;
    // Buys it: writes a price snapshot for every market inside the radius.
    // False with a logged reason when refused (not docked, or too poor).
    bool buyMarketIntel(std::string* outError = nullptr);
    // Snapshots the docked station's own prices into memory. Called on dock:
    // standing on the pad is the one reading you never have to pay for.
    void recordDockedMarket();
    // Best remembered price for a commodity away from the docked station.
    // False when the player has never seen it anywhere else.
    bool bestKnownPrice(std::uint32_t commodity,
                        std::uint32_t* outSystem,
                        float* outPrice,
                        double* outAge,
                        bool* outStale) const;
    // How much of its nominal output the docked station is managing, and what
    // is holding it back (empty when nothing is).
    [[nodiscard]] float marketSatisfaction(std::uint32_t market) const;
    [[nodiscard]] const char* marketLimiting(std::uint32_t market) const;

    // Hardcore/ironman (decisions/007): set at new game, carried by the save.
    void setHardcore(bool hardcore) { m_hardcore = hardcore; }

    [[nodiscard]] bool hardcore() const { return m_hardcore; }

    // True once after a hardcore death; the caller deletes the save file.
    [[nodiscard]] bool consumeHardcoreDeath()
    {
        const bool pending = m_hardcoreDeathPending;
        m_hardcoreDeathPending = false;
        return pending;
    }

    [[nodiscard]] const sol::sim::Galaxy& galaxy() const { return m_galaxy; }

    // The rules the galaxy was generated under (Phase 13). Exposed so a console
    // probe can check placement against them over the WHOLE galaxy: the
    // coherence rule spans 80 systems and no amount of flying can assert it.
    [[nodiscard]] const sol::sim::GalaxyParams& galaxyParams() const { return m_galaxyParams; }

    // The mining rules the galaxy was generated under, which are an INPUT to
    // generation since Phase 13 (station siting consults a system's fields).
    // Exposed beside `galaxyParams` for the same reason and by the same rule:
    // regenerating this galaxy outside `generateUniverse` needs both halves,
    // and a test that passed only one would be photographing a galaxy the game
    // does not produce.
    [[nodiscard]] const sol::sim::MiningParams& miningParams() const { return m_miningParams; }

    // A digest of the AUTHORED content this galaxy was generated from - every
    // `[[system]]` and `[[constellation]]` in `game/data` and every mod layer
    // (Phase 29 stage D, decisions/018 decision 7). It rides the save beside
    // the seed, and a mismatch refuses the load.
    [[nodiscard]] std::uint64_t authoredContentDigest() const { return m_authoredDigest; }

    [[nodiscard]] std::uint32_t currentSystemIndex() const { return m_currentSystem; }

    [[nodiscard]] const char* currentSystemName() const
    {
        return m_currentSystem < m_galaxy.systems.size() ? m_galaxy.systems[m_currentSystem].name.c_str()
                                                         : "(void)";
    }

    void setShipInput(const sol::sim::FlightInput& input) { m_shipInput = input; }

    void tick(double dt);

    // --- Ship commands (engine plan Phase 28 stage A).
    //
    // ⚑ This used to be a bool called m_autopilotActive, and the phase's whole
    // shape follows from noticing that the bool was already a one-slot command
    // system: engage/disengage were its verbs and autopilotInput() already
    // returned "the FlightInput the ship flies itself with this tick". Widening
    // it to an enum is what turns one behaviour into a vocabulary.
    //
    // ⚑⚑ AUTOPILOT TERMINATES; EVERY OTHER MODE IS STANDING. Autopilot is going
    // somewhere and disengages on arrival, so a nudge of the stick REPLACES its
    // plan and cancels it. A standing order is a frame you are flying inside —
    // nudging the nose to look at something is not a request to stop orbiting —
    // so it is overridden while the stick is held and resumes when released. It
    // ends only when explicitly ended, when its subject is lost, or on docking.
    // That asymmetry is the one genuinely reversible decision in this phase and
    // the first thing its playtest must ask about.
    bool engageAutopilot();

    // Engages any command mode. Fails (returning false, changing nothing) while
    // docked, or when the mode needs a target and there is none.
    bool engageCommand(CommandMode mode);

    void clearCommand() { m_commandMode = CommandMode::None; }

    [[nodiscard]] CommandMode commandMode() const { return m_commandMode; }

    // Kept as-is for the HUD, the pause menu and the two Lua bindings, all of
    // which ask specifically about autopilot rather than about commands.
    void disengageAutopilot()
    {
        if (m_commandMode == CommandMode::Autopilot) {
            m_commandMode = CommandMode::None;
        }
    }

    [[nodiscard]] bool autopilotActive() const { return m_commandMode == CommandMode::Autopilot; }

    // The parameter a parametrised command takes when it is given without one:
    // whatever the player last used (phase decision 4), which is what keeps a
    // bound key and a menu entry doing the identical thing.
    [[nodiscard]] double orbitRange() const { return m_orbitRange; }

    void setOrbitRange(double meters);

    [[nodiscard]] double keepDistanceRange() const { return m_keepDistanceRange; }

    void setKeepDistanceRange(double meters);

    [[nodiscard]] double autopilotArrivalRange() const { return m_autopilotRange; }

    void setAutopilotArrivalRange(double meters);

    // Interpolated blend of previous->current tick state at alpha.
    [[nodiscard]] Transform shipRenderTransform(float alpha) const;

    // ⚑⚑ HOW BIG A HULL ACTUALLY IS, IN WORLD METRES (Phase 32 stage B). The
    // authored `radius` of the model it draws, times the instance scale it
    // draws at - the same product collision, avoidance, docking stand-off and
    // the salvage sweep have all read for phases, promoted here because the
    // chase camera needs it and nothing outside this class could reach
    // `modelBaseRadius`.
    //
    // ⚑ This is the ONLY notion of a hull's size the running game has. The
    // cooked `.smesh` header carries no bounds (`formats.hpp`), so nothing here
    // can measure a mesh; only the Forge can, which is why stage A's band check
    // lives there. A hand-authored collision sphere is therefore what the
    // camera frames, and it is authored generously on purpose.
    [[nodiscard]] double hullRadius(std::uint32_t entityIndex) const;

    [[nodiscard]] double playerHullRadius() const { return hullRadius(playerEntityIndex()); }

    [[nodiscard]] sol::sim::ShipState shipState() const;

    [[nodiscard]] const sol::sim::ShipTuning& shipTuning() const
    {
        return playerRegistry().storage<ShipControl>().get(playerEntityIndex()).tuning;
    }

    // The input the ship actually flew last tick (autopilot's when engaged,
    // the player's otherwise) — what the HUD flags should reflect.
    [[nodiscard]] const sol::sim::FlightInput& shipInput() const { return m_appliedInput; }

    // Player pip triage (keys 1/2/3, 4 to balance).
    void playerAddPip(sol::sim::PowerSystem system);
    void playerBalancePips();

    [[nodiscard]] const sol::sim::PowerState& playerPower() const
    {
        return playerRegistry().storage<ShipPower>().get(playerEntityIndex()).state;
    }

    [[nodiscard]] const sol::sim::PowerTuning& powerTuning() const
    {
        return playerRegistry().storage<ShipPower>().get(playerEntityIndex()).tuning;
    }

    [[nodiscard]] const ShipDefense& playerDefense() const
    {
        return playerRegistry().storage<ShipDefense>().get(playerEntityIndex());
    }

    // The same for ANY ship, or null for something that has no defences - a
    // rock, a station, a bolt. `shipHullFraction` beside it answers one
    // question off this and was the only way to ask any of them about a ship
    // that is not the player's (Phase 31 stage F2 wanted the shields too).
    [[nodiscard]] const ShipDefense* shipDefense(sol::ecs::Entity entity) const
    {
        return playerRegistry().tryGet<ShipDefense>(entity);
    }

    // ⚑⚑ THE BUBBLE CENSUS (Phase 38 stage A). How many systems are
    // instantiated, and which — the readout the phase's exit criterion is
    // measured against, and the only externally visible sign that the world is
    // plural at all.
    //
    // ⚑ STAGE C IS WHAT MADE IT INTERESTING. It is 1 for an ordinary played
    // session — a quiet crossing retains nothing — and more than 1 for
    // `kCoolingSeconds` after the player jumps out of a system with a live
    // fight in it, never above `kMaxInstantiatedSystems`.
    // Creates EVERY component pool the world uses, up front. Const
    // `storage<T>()` asserts the pool exists, and the read-only paths (the
    // HUD's prospect readout) run in systems that may hold no rock, no wreck
    // and no loose ore.
    //
    // ⚑⚑⚑ IT COVERED THE THREE MINING POOLS AND HAD TO GROW TO ALL OF THEM
    // (Phase 38 stage A). It was `ensureMiningPools` while one long-lived
    // registry served the whole run: everything else had been emplaced at
    // least once by the time anything read it, so the assert never fired. A
    // bubble is FRESH, and a system with no NPC in it has no `ShipPilot` pool
    // until one spawns — so the first const read is the assert.
    // ⚑ Public and static for the same reason `isPlayerEntity` is: it is a
    // pure operation on a Registry, and the guard for it needs a registry
    // this class did not make.
    static void ensureWorldPools(sol::ecs::Registry& registry);

    // ⚑⚑⚑⚑ IS THIS THE PLAYER, ASKED OF THE POOL RATHER THAN OF AN INDEX
    // (Phase 38 stage A). The question was written seventeen times as
    // `entityIndex == playerEntityIndex()`, which is an index into ONE
    // registry — and indices are per-registry and every registry starts at
    // zero, so with a second bubble in the world that comparison is a
    // coincidence rather than an identity. `Projectile::shooterIndex` is the
    // case that makes it concrete: a bolt outlives the jump that leaves it
    // behind, and `handleShipDestroyed` decides a kill was the player's this
    // way.
    //
    // `PlayerShip` has answered it correctly since Phase 7 and was never
    // asked: it appeared at exactly three places in this file — registered in
    // the snapshot schema, emplaced once, counted on load. It is false in
    // every registry but one, which is the whole point.
    [[nodiscard]] static bool isPlayerEntity(const sol::ecs::Registry& registry, std::uint32_t entityIndex)
    {
        return registry.storage<PlayerShip>().contains(entityIndex);
    }

    // ⚑ Public because the guard for it needs a registry that is NOT the
    // player's, and stage A never has one at rest — `a_foreign_registrys_slot_
    // zero_is_not_the_player` builds one to make the coincidence concrete.

    [[nodiscard]] std::size_t instantiatedSystemCount() const { return m_bubbles.size(); }

    // ⚑⚑⚑⚑ THE HARD CAP, AND IT IS PUBLIC BECAUSE THE SPEC ASKS FOR IT TO BE
    // ASSERTED BY A TEST RATHER THAN CLAIMED BY A COMMENT (Phase 38 stage C,
    // in those words). `sim::resolveCollisions` is O(n^2) with no broadphase
    // and says so in its own header, so k bubbles cost k*n^2 — and this is the
    // k. Measured on the shipped galaxy in a DEBUG build, ticking for 600
    // frames: one bubble 0.067 ms, four 0.47 ms, six 0.64 ms against a 16.7 ms
    // frame. Systems run 25–136 entities and 4–15 ships, so the fattest one
    // measured is 9,180 pairs on its own.
    //
    // ⚑⚑ NOT THE SAME NUMBER AS `kMaxBubbles`, AND THE DIFFERENCE IS THE
    // POINT. That one is a save-file sanity limit — the count past which a
    // number read off disk is corruption rather than a world. This one is a
    // policy about how much simulation the game will pay for at once, and it
    // is the fence the spec's risk section asks for around the cooling bubble.
    static constexpr std::size_t kMaxInstantiatedSystems = 6;

    // ⚑⚑⚑⚑ OPENS A SYSTEM THE PLAYER IS NOT IN, AND TICKS IT FROM THE NEXT
    // FRAME (Phase 38 stage B). This is the mechanism half of the cooling
    // bubble: a bubble with its own statics, its own rocks and its own ambient
    // traffic, ticked by the same eight per-system zones the player's is,
    // charging its kills and its wrecks to its own system.
    //
    // ⚑⚑⚑ THE POLICY HALF IS STAGE C AND NOTHING HERE PRE-EMPTS IT. Stage B
    // never calls this on its own - `instantiatedSystemCount()` is still 1 for
    // the whole of a played session, and `the_world_instantiates_exactly_one_
    // system_and_it_is_the_players` still holds - because WHEN to open one,
    // how long to keep it and how many may be open at once are the three
    // questions stage C exists to answer against the O(n^2) collision pass.
    // What this buys now is that the nesting is exercisable rather than merely
    // written, which is the difference this project has twice paid for.
    //
    // Idempotent: a system already instantiated is returned as it stands, sky
    // and all, rather than built a second time on top of itself - the bug the
    // plural registry actually caused in stage A, said once so it cannot
    // happen again through this door.
    bool instantiateSystem(std::uint32_t system);

    [[nodiscard]] std::uint32_t instantiatedSystemAt(std::size_t slot) const
    {
        return slot < m_bubbles.size() ? m_bubbles[slot]->system : kNoIndex;
    }

    // ⚑⚑ WHAT ONE BUBBLE HOLDS (Phase 38 stage B). The only way to look into a
    // system the player is not standing in - every other reader in this file
    // asks the player's, and goes on being right while doing it, which is
    // exactly how a bug in the nested tick would stay invisible. Stage D hangs
    // the console readout off this and stage C adds the retention clock; what
    // is here is the half stage B actually produces.
    struct BubbleReport
    {
        std::uint32_t system = kNoIndex;
        std::size_t entities = 0;
        std::size_t ships = 0; // hulls with a pilot: the traffic, not the scenery
        std::size_t projectiles = 0;
        // ⚑⚑ HOW MANY OF THOSE SHIPS ARE IN A FIGHT (Phase 38 stage C): pilots
        // in `Attack`. It is the retention predicate made visible, and it is
        // the only way from outside this class to see that the bubble you left
        // has stopped fighting YOU - which is a claim about the instant of
        // departure, before any tick, and therefore about something no count
        // taken later can distinguish.
        std::size_t fighting = 0;
        // Hulls that have been left in it. A wreck record carries its own system
        // and its own position, so which bubble materialises one is the whole
        // question the fine half of mining answers per system.
        std::size_t wrecks = 0;
        bool player = false;
        // The statics this bubble ticks against. Avoidance and the collision
        // build both push the star and every planet as spheres, so these two
        // numbers ARE the frame as far as the per-system tick is concerned -
        // a bubble reporting the player's star is one whose ships dodge a sun
        // that is not there and fly through the one that is.
        double starRadius = 0.0;
        std::size_t planets = 0;
        // Where its traffic is, averaged. Not decoration: it is the only way
        // from outside this class to see that a bubble is being TICKED rather
        // than merely held, because everything else a foreign bubble owns
        // (its counts, its hulls) can sit still for a hundred frames and be
        // right. Stage C's console readout wants it for the same reason.
        sol::core::DVec3 shipCentroid;
        // The hull fraction of the most damaged ship in it, 1.0 when nothing
        // has been shot and 0.0 when the bubble holds no ships at all. This is
        // the number the cooling bubble is a claim about - stage C's exit is
        // that it does not reset when the player leaves - and it is here so the
        // stage that builds the tick can watch the tick move it.
        float worstHull = 0.0f;
        // ⚑⚑ SIM-SECONDS THIS BUBBLE HAS LEFT (Phase 38 stage C), and 0 on the
        // player's own — which is held by the player being in it rather than by
        // a clock, so 0 here means "not on the clock" and never "about to go".
        // Reading it beside `worstHull` is how a test says the hull did not
        // reset AND that the bubble holding it was on its way out.
        double holdSeconds = 0.0;
    };

    [[nodiscard]] bool bubbleReportAt(std::size_t slot, BubbleReport& out) const;

    // ⚑⚑ PARTICLE BURSTS REFUSED BECAUSE THEY HAPPENED SOMEWHERE ELSE (Phase
    // 38 stage D). The audio half of the same claim lives on `GameAudio`; both
    // are on `sol.bubbles`, and both exist because what this stage produces is
    // an ABSENCE - a spark that is not drawn and a sound that is not played -
    // and an absence is not something a console can be pointed at.
    [[nodiscard]] std::uint64_t outOfFrameBursts() const { return m_combatEffects.outOfFrameBursts(); }

    // The player's sky. Both were plain members until stage B moved them onto
    // the bubble; every reader outside the tick wants the system being drawn,
    // which is the player's, and says so here rather than by omission.
    [[nodiscard]] const CelestialBody& sun() const { return m_bubbles.front()->star; }

    [[nodiscard]] std::span<const CelestialBody> planets() const { return m_bubbles.front()->planets; }

    [[nodiscard]] std::span<const GateInstance> gates() const { return m_gates; }

    // What a nav-target slot is, so the map can draw the right glyph and the
    // scanner can tell a body from a site (Phase 8e).
    enum class NavKind : std::uint32_t
    {
        Station = 0,
        Gate,
        Planet,
        Star,
        Signal,
        Field, // Phase 8f: an asteroid field, targeted as a whole
        Wreck,
        Bookmark,  // Phase 8h: a place the player wrote down
        Objective, // Phase 8i: where the tracked mission says to go
        Berth,     // Phase 8r: the port a station has just cleared you for
    };

    // ⚑ What the player knows about a nav slot (Phase 8z).
    //
    // The fog is a predicate BESIDE the target list and never a filter on it,
    // because the list is world state rather than player knowledge:
    // stationPosition() is m_targets[0] and every ambient Lua patrol anchors to
    // it, while m_signalTargetBase/m_planetTargetBase/m_starTargetIndex are
    // indices into the same vector. Shortening it would move NPC patrol anchors
    // the moment the player scanned something — the world reshaping itself
    // around what the player knows.
    //
    // Hidden is skipped by the cycle, the radar, the click pick and both maps,
    // which is why nothing downstream (autopilot, hail, dock request) needs a
    // guard of its own: you cannot select what you cannot see.
    enum class NavKnowledge : std::uint32_t
    {
        Hidden = 0, // not found yet
        Contact,    // a pulse found it; you do not know what it is
        Identified, // scanned, or never hidden in the first place
    };
    [[nodiscard]] NavKnowledge navKnowledge(std::size_t index) const;

    [[nodiscard]] bool navTargetVisible(std::size_t index) const
    {
        return navKnowledge(index) != NavKnowledge::Hidden;
    }

    // The kind a seam should DRAW. An unidentified thing draws as a Signal on
    // the radar and the map — the glyph that already means "something is there
    // and you do not know what" — so the shape never leaks what the name is
    // withholding.
    [[nodiscard]] NavKind navTargetDrawKind(std::size_t index) const;
    // The station or gate a slot refers to, or kNoIndex when it is neither.
    [[nodiscard]] std::uint32_t navTargetStation(std::size_t index) const;
    [[nodiscard]] std::uint32_t navTargetGate(std::size_t index) const;

    // "No slot" for the target-index queries below.
    static constexpr std::size_t kNoTarget = static_cast<std::size_t>(-1);

    // Cycling targets: the static nav points (station, planet, sun) then
    // every live pilot ship (combat targets with shield/hull readouts).
    //
    // Those two classes cycle on separate keys (Phase 8h) because they answer
    // different questions — where am I going, and what is shooting at me —
    // and walking them with one key made reaching a fighter take a dozen
    // presses past every planet and signal in the system. There is still
    // exactly ONE selection: weapons lead, autopilot, the HUD readout and the
    // map's Set Target all read m_targetIndex, so each class instead
    // remembers the slot it last held and switching class restores it.
    [[nodiscard]] TargetInfo currentTargetInfo() const;
    // The same readout for any live ship, by slot (the contact cycle, the
    // radar fill, and the console listing all want it, not just the
    // selection). Returns a default-constructed info for a stale slot.
    [[nodiscard]] TargetInfo contactInfo(std::size_t shipSlot) const;

    [[nodiscard]] std::size_t contactCount() const { return playerShips().size(); }

    // Phase 15: `step` is +1 forward or -1 backward. A parameter rather than a
    // second function so the two directions cannot drift apart - the fog walk
    // and the resume-where-this-class-left-off rule are the same code either
    // way, and only the sign of the step changes.
    void cycleNavTarget(int step = 1);
    void cycleContact(int step = 1);
    // Ship slots in cycleContact order: whoever is attacking the player
    // first, then hostiles, then the rest, each group nearest-first.
    void contactOrder(std::vector<std::size_t>& out) const;
    // The same order, with each slot's threat tier alongside (0 = attacking
    // the player, 1 = hostile, 2 = everything else). "Is the nearest contact
    // hostile" is then read off the sort that already ran, rather than from a
    // second definition of hostile that can drift away from this one.
    void contactOrder(std::vector<std::size_t>& out, std::vector<int>& tiers) const;

    [[nodiscard]] bool targetIsContact() const { return m_targetIndex >= m_targets.size(); }

    // The static nav points of this system, in target-cycle order.
    [[nodiscard]] std::span<const NavTarget> navTargets() const { return m_targets; }

    [[nodiscard]] NavKind navTargetKind(std::size_t index) const;
    // Body index (0 = star) for a Planet/Star slot; ~0u for anything else.
    [[nodiscard]] std::uint32_t navTargetBody(std::size_t index) const;
    // Signal index for a Signal slot; ~0u for anything else.
    [[nodiscard]] std::uint32_t navTargetSignal(std::size_t index) const;
    // Field index / wreck id for a Field or Wreck slot; ~0u for anything else.
    [[nodiscard]] std::uint32_t navTargetField(std::size_t index) const;
    [[nodiscard]] std::uint32_t navTargetWreck(std::size_t index) const;
    // Bookmark id for a Bookmark slot; ~0u for anything else (Phase 8h).
    [[nodiscard]] std::uint32_t navTargetBookmark(std::size_t index) const;

    // --- Mission objectives (Phase 8i) ---
    // The tracked mission's current objective, or nullptr when nothing is
    // tracked. The position it carries has been in the data since 8c and was
    // never drawn, which is what made the campaign unreachable.
    [[nodiscard]] const sol::sim::MissionObjective* trackedObjective() const;
    // Where the tracked objective sends the player, as prose for the HUD: the
    // station and system for Dock/Deliver, the system for a FlyTo elsewhere,
    // the victim faction for a Kill. Empty when nothing is tracked or when the
    // destination is a marker in this system the player can already see —
    // saying "over there" beside a dot that says where is noise.
    [[nodiscard]] std::string objectiveDestinationText() const;
    // Slot of the NavKind::Objective target, or kNoTarget when the tracked
    // objective has nothing in this system to point at (so nothing is there to
    // select).
    [[nodiscard]] std::size_t objectiveTargetIndex() const;
    // Selects that slot (the O key). False when there is nothing to select.
    bool selectObjective();
    // Selects the nearest hostile contact (the H key), which is the head of
    // the contact order when the order's head is hostile at all.
    bool selectNearestHostile();

    // --- Bookmarks (Phase 8h) ---
    // Writes down where the ship is now. An empty name gets one generated
    // from what is nearby, which is what makes the common case free.
    bool addBookmarkHere(const std::string& name);
    bool addBookmarkAt(const sol::core::DVec3& position, const std::string& name);
    // The name a bookmark at `position` would get by default: the nearest
    // named thing and how far off it is.
    [[nodiscard]] std::string suggestBookmarkName(const sol::core::DVec3& position) const;
    bool removeBookmark(std::uint32_t id);
    // Selects a bookmark's nav slot by id, so the map's list can target one.
    bool selectBookmark(std::uint32_t id);
    [[nodiscard]] std::size_t currentTargetIndex() const;
    // Selects a nav-target slot outright (the map's "Set Target", a click).
    bool selectTarget(std::size_t index);

    // --- Audio (Phase 8t) ---
    // The frame loop owns the device, so the world is handed a borrowed
    // pointer rather than owning one - the same shape as the dev UI. Null is
    // normal: a machine with no output endpoint runs the whole game silently,
    // and every call site is guarded rather than the sound being faked.
    // ⚑ Out of line since Phase 38 stage D: the ear is given the player's
    // frame as it is attached. `GameAudio` is only forward-declared here.
    void setAudio(GameAudio* audio);

    // --- Picking (Phase 8j) ---
    // How the world is being viewed, for the click-to-select in target_pick.hpp.
    void setViewFrame(const ViewFrame& view) { m_viewFrame = view; }

    [[nodiscard]] const ViewFrame& viewFrame() const { return m_viewFrame; }

    // Selects the first live spawned ship whose display name contains
    // namePart (dev/console QoL; T-cycling is the player path).
    bool targetShipByName(const char* namePart);

    // What the player is flying WITH, reduced to the four facts anything
    // outside the firing pass asks about (Phase 31 stage C1). This replaced
    // `playerWeapon()`, which handed out a reference to the one gun: with
    // guns plural there is no "the" weapon to return, and every caller of the
    // old accessor turned out to want a summary rather than a gun anyway.
    [[nodiscard]] ArmamentSummary playerArmament() const
    {
        return armamentSummary(playerRegistry(), playerEntityIndex());
    }

    // The same, for any entity that has an armament; all-zero for one that
    // does not (a station, a rock, an unarmed hull). ⚑ Takes the registry
    // since stage B: the firing pass asks this of every armed ship in the
    // bubble it is walking, and an index means nothing without one.
    [[nodiscard]] ArmamentSummary armamentSummary(const sol::ecs::Registry& registry,
                                                  std::uint32_t entityIndex) const;

    // ⚑ And the same question asked of the player's sky, which is the only one
    // a caller outside this class can name - the same shape, and for the same
    // reason, as `shipMounts` and `playerGuns` above.
    [[nodiscard]] ArmamentSummary armamentSummary(std::uint32_t entityIndex) const
    {
        return armamentSummary(playerRegistry(), entityIndex);
    }

    // The player's guns as the SIM holds them, in mount order. A view of the
    // live component rather than a copy, and the only way to see a gun the
    // def named and the sim dropped - which is why the console probe reads
    // this rather than the fleet entry.
    [[nodiscard]] std::span<const ShipWeapon> playerGuns() const
    {
        const ShipArmament& armament = playerRegistry().storage<ShipArmament>().get(playerEntityIndex());
        return {armament.weapons, armament.count};
    }

    // Every place on that hull and how much of it is left, in the hull's own
    // MOUNT ORDER (Phase 31 stage F) - so `ShipWeapon::mount` and
    // `FittedPart::mount` index straight into it. Empty for an entity with no
    // mounts, which is a rock, a station, or a ship built before the pool
    // existed.
    //
    // ⚑ `tryGet`, not `storage<T>().get`: this is asked while a frame is being
    // built and `Registry::storage<T>() const` ASSERTS the pool exists - the
    // latent trap stage E was the first to spring.
    [[nodiscard]] std::span<const MountCondition> shipMounts(std::uint32_t entityIndex) const
    {
        const ShipMounts* mounts =
            playerRegistry().tryGet<ShipMounts>(playerRegistry().entityFromIndex(entityIndex));
        return mounts != nullptr ? std::span<const MountCondition>{mounts->mounts, mounts->count}
                                 : std::span<const MountCondition>{};
    }

    [[nodiscard]] std::span<const MountCondition> playerMounts() const
    {
        return shipMounts(playerEntityIndex());
    }

    // What that ship's guns are laid against, read once per ship (Phase 31
    // stage C2). Public because the console probe reports a gun's live bearing
    // and stage E will draw one: `layGun` is the only definition of where a
    // gun points, and it needs this to be reachable from outside the tick.
    [[nodiscard]] GunneryFrame gunneryFrame(const sol::ecs::Registry& registry,
                                            std::uint32_t entityIndex) const;

    // The player's, which is the one the console probe asks for - the same
    // shape as `playerGuns()` beside it, and for the same reason: the entity
    // index of the player's own ship is not something a caller should know.
    [[nodiscard]] GunneryFrame playerGunneryFrame() const
    {
        return gunneryFrame(playerRegistry(), playerEntityIndex());
    }

    // --- Fire groups (Phase 31 stage C3) ---
    //
    // ⚑ THE TWO HALVES ARE DELIBERATELY DIFFERENT ACTS IN DIFFERENT PLACES.
    // Which trigger a gun answers to is SETUP - made once on the ship readout,
    // saved with the fit, and worth thinking about. Which trigger you are
    // holding is FLYING - one key, in the middle of a fight, with no screen
    // between the decision and the shot.

    // Which group the player's trigger is currently wired to, 1-based.
    [[nodiscard]] std::uint32_t playerFireGroup() const;

    // The groups the player's guns occupy, as a bit per group (see
    // `fireGroupsInUse`). Zero when nothing is fitted.
    [[nodiscard]] std::uint32_t playerFireGroupsInUse() const;

    // Steps the selection to the next group that HAS a gun in it, wrapping.
    // Returns the group it landed on - which is the one it started on when the
    // ship's guns all sit in one group, because a cycle of length one is a key
    // that correctly does nothing rather than a key that lies about it.
    std::uint32_t cycleFireGroup();

    // Puts the gun in `mountId` into `group`, on the active ship: writes the
    // saved fit AND the live gun, because rebuilding the armament to carry one
    // number across would refill the shields and clear every cooldown with it.
    // False (with a reason) when the mount is not on this hull, holds no gun,
    // or the group is outside 1..kFireGroupCount.
    bool setFireGroup(const char* mountId, std::uint32_t group, std::string* outError = nullptr);

    // ⚑ WHO THE PLAYER IS AT WAR WITH, IN ONE PLACE (Phase 31 stage C2). The
    // contact cycle's threat ranking and a turret's decision to open fire are
    // the same question, and two answers to it would be a radar that paints a
    // ship red beside a ring that will not shoot it.
    //
    // Lowest first: 0 is shooting at you right now, 1 is hostile by standing
    // policy, 2 is everybody else. "Hostile" means `<= kHostileThreatTier`.
    [[nodiscard]] int threatTier(std::uint32_t entityIndex) const;

    static constexpr int kHostileThreatTier = 1;

    // The entity index of the SHIP the player has selected, or kNoIndex when
    // the selection is a station, a planet, an ore field or nothing at all
    // (Phase 31 stage C2). Every other reader of the selection wants a name, a
    // distance or a shield readout; a turret wants the entity, because the
    // lead solution needs its velocity.
    [[nodiscard]] std::uint32_t targetShipEntityIndex() const;

    // 1 right after the player takes a hit, decaying to 0 (HUD flash).
    [[nodiscard]] float playerDamageFlash() const
    {
        return m_playerDamageTimer > 0.0f ? m_playerDamageTimer / kDamageFlashSeconds : 0.0f;
    }

    // One instance per RenderShape entity, interpolated; ship excluded when
    // includeShip is false (first-person view).
    //
    // ⚑⚑ SINCE PHASE 31 STAGE E IT ALSO APPENDS WHAT IS BOLTED TO THOSE
    // HULLS: one more instance per fitted, external, modelled mount, hung off
    // its owner's transform. A fitting is not an entity and deliberately never
    // becomes one (see `kMaxShipWeapons`), so it has no RenderShape of its own
    // and this is the only place it can be drawn from.
    //
    // ⚑⚑ A FITTING DOES NOT FOLLOW ITS HULL OUT OF THE LIST. `includeShip`
    // hides the hull from the SEAT, because the eye sits inside it; a fitting
    // bolted to the outside is not inside anything. So the shuttle's nose gun
    // is drawn 1.6 m ahead of the pilot, which is the whole of what the stage
    // is for. See `appendFittingInstances` for what that costs.
    void buildRenderInstances(float alpha, bool includeShip, std::vector<RenderInstance>& out) const;

    // The interior to draw when the player is in the seat (Phase 19), from the
    // ACTIVE ship's def and so following a fleet switch. The cockpit is the
    // one drawable the game layer pushes itself rather than reading off a
    // RenderShape, which is why it is not in the list above.
    //
    // ⚑ This used to be resolved once in `main.cpp` and held in a local for
    // the life of the process - correct only for as long as every hull shared
    // one interior. It lives here now for two reasons: the answer has to
    // change when the active ship does, and `main.cpp` is the executable while
    // everything else is in `sol_game_lib`, so as long as it sat there it was
    // the one model resolution in the game that no test could reach.
    [[nodiscard]] ModelId cockpitModel() const;

    void buildParticleInstances(float alpha, std::vector<ParticleInstance>& out) const
    {
        m_thrusters.buildInstances(alpha, out);
        m_combatEffects.appendInstances(alpha, out);
    }

    // ⚑ ACROSS EVERY INSTANTIATED BUBBLE, NOT JUST THE PLAYER'S (Phase 38
    // stage A). The boot log and the console read this; a figure that stopped
    // counting at the player's own system would report a shrinking world at
    // exactly the moment stage B makes it bigger.
    [[nodiscard]] std::uint32_t entityCount() const
    {
        std::size_t total = 0;
        for (const std::unique_ptr<SystemBubble>& bubble : m_bubbles) {
            total += bubble->registry.aliveCount();
        }
        return static_cast<std::uint32_t>(total);
    }

    // Puts the world back to the state it had at construction and spawns a
    // fresh player ship for `seed`. What "Quit to Main Menu" and then "New
    // Game" needs, and what buildPauseMenu's comment has said does not exist
    // since Phase 8d: "starting a second run in one process needs a world
    // reset that does not exist".
    //
    // ⚑⚑ IT MOVE-ASSIGNS A DEFAULT-CONSTRUCTED WORLD RATHER THAN CLEARING
    // FIELDS, AND THAT IS THE WHOLE POINT. A hand-written reset has to name
    // every member, so it is wrong the moment somebody adds one - and wrong
    // silently, as state that survives a new game. This cannot go stale:
    // whatever the class holds next year is default-constructed here for free.
    // The object's ADDRESS does not change, which is what keeps GameContent's
    // `SpaceWorld*` and every other observer valid across the reset.
    //
    // ⚑ It does NOT regenerate the galaxy - that needs the def database, which
    // is GameContent's. `GameContent::restartForNewGame` is the other half and
    // the two are always called together.
    void resetForNewGame(std::uint64_t seed);

    // `displayName` is what the save browser will show for this file. It is
    // the caller's string - a slot name the player typed, or an autosave
    // label - and the world never invents one; everything else in the header
    // (when, where, how rich, how far in) it already knows and fills itself.
    [[nodiscard]] bool saveTo(const char* path, std::string_view displayName);
    [[nodiscard]] bool loadFrom(const char* path);

    // Re-reads the player def's tuning and visuals plus every def-spawned
    // ship's visuals from the (re)loaded database; called after data
    // hot-reload so stat edits land without a restart.
    void applyDefs(const sol::assets::DefDatabase& defs);

    // Spawns a flyable def-driven ship just ahead of the player. Returns the
    // new entity.
    sol::ecs::Entity spawnShipFromDef(const sol::assets::ShipDef& def, const sol::assets::DefDatabase& defs);

    // As above, plus an AI pilot in the given role (starts Idle until Lua's
    // pilot_think assigns work). factionIndex tags the pilot's allegiance;
    // ~0u spawns the pre-8b unaffiliated kind.
    sol::ecs::Entity spawnPilotFromDef(const sol::assets::ShipDef& def,
                                       const sol::assets::DefDatabase& defs,
                                       PilotRole role,
                                       std::uint32_t factionIndex = kNoIndex);

    // --- Pilot commands (the Lua-facing strategy API, called via bindings) ---
    // All return false for a dead/pilotless entity.
    bool pilotAttackPlayer(sol::ecs::Entity entity);
    // Attacks the nearest enemy of the pilot's faction: another pilot whose
    // faction it is at war with, or the player when hostile (and not docked).
    // False (and no state change) with nothing hostile in sensor range.
    bool pilotEngageEnemy(sol::ecs::Entity entity);
    // Goes for a hauler (Phase 8x §D). Picks the nearest trader body the
    // pilot's faction would attack — the same "at war with, or hostile to"
    // test the coarse layer uses to choose a system to raid — and either
    // attacks it or cruises after it, because a trade lane is hundreds of
    // thousands of kilometres and the dogfight steering never leaves the
    // normal envelope. False with nothing worth taking in reach, and a hunter
    // that hears that stops travelling rather than flying at a stale point.
    bool pilotHuntTrader(sol::ecs::Entity entity);
    // Answers whoever last shot this ship, inside the threat window. This is
    // the one target a pilot has that does not come from a search: it is not
    // "the nearest enemy", it is the one that is actually shooting.
    bool pilotEngageThreat(sol::ecs::Entity entity);
    // Something has shot this ship recently (the window ShipPilot::threatTimer
    // holds). What lets a hauler run before it is nearly dead.
    [[nodiscard]] bool pilotUnderFire(sol::ecs::Entity entity) const;
    bool pilotFlee(sol::ecs::Entity entity);
    bool pilotIdle(sol::ecs::Entity entity);
    bool pilotPatrolTo(sol::ecs::Entity entity, sol::core::DVec3 waypoint);

    // ⚑⚑⚑⚑ THE PRIMITIVE decisions/019 §3 ASSUMED ALREADY EXISTED, AND THE
    // REASON THAT DECISION COULD NOT BE BUILT AS WRITTEN. It said a patrol
    // crossing 600,000 km to reach a gate is "`pilotPatrolTo` plus
    // `PilotState::Travel`, and no new steering". Those do not compose:
    // `pilotPatrolTo` above sets `PilotState::PATROL`, the combat-scale state
    // whose own comment says it "closes to 50 m and stops" - so the named
    // route produces a responder grinding across the system on dogfight
    // steering, which is the precise failure `Travel` was written to prevent.
    // Nothing in the tree put a pilot into `Travel` toward an arbitrary
    // waypoint; this does, and it is still no new STEERING - `steerTravel` is
    // the cruise drive the player's own autopilot flies.
    bool pilotTravelTo(sol::ecs::Entity entity, sol::core::DVec3 waypoint);

    // ⚑⚑⚑ SEND SOMEBODY. Diverts the nearest un-engaged local force toward
    // `position`, and spawns a wing at the nearest station or gate only when
    // there is nobody in range to divert - decisions/019 decision 3, which is
    // what makes response time A REAL TRANSIT ACROSS REAL DISTANCE rather than
    // a timer. Returns how many hulls were dispatched; 0 means nobody came,
    // which is a legitimate answer and the whole point of the zero band.
    //
    // ⚑⚑ WHICH HALF OF THE RATING EACH QUESTION READS, PER decisions/019.
    // HOW MANY come is drawn from the garrison, so it reads the BASELINE - the
    // spiral this phase refuses is a raid thinning the force that answers it.
    // HOW FAR one will come, and WHETHER one comes at all, read the LIVE
    // rating, because that is the single place 019 allows the live number to
    // touch enforcement: patrols that are busy really are slower to arrive.
    //
    // ⚑ THE PUBLIC ONE IS THE PLAYER'S SYSTEM, AND SAYS SO BY TAKING NO FRAME
    // (Phase 38 stage B). Lua and the tests dispatch into the sky the player is
    // standing in, which is the only one either of them can name; the private
    // overload below is what the damage path uses, and it answers with the
    // bubble the incident actually happened in.
    std::uint32_t respondTo(sol::core::DVec3 position, std::uint32_t offenderIndex, ResponseCause cause);

    // ⚑⚑ THE INCIDENT HOOK, AND IT IS PUBLIC BECAUSE IT IS THE POLICY HALF.
    // `respondTo` is the mechanism - it sends whoever the rating says - and
    // this decides whether a given hit is even something the local law answers,
    // then throttles a burst of them into one call. `noteDamage` calls it on
    // every hit, and Phase 36 is where other incident sources (a contraband
    // scan, a transponder that does not answer) hook the same seam rather than
    // growing their own. It is also the only way to state the policy in a test:
    // whether a bolt connects is not a thing a scripted drive can promise.
    void considerResponse(std::uint32_t targetIndex, std::uint32_t attackerIndex, sol::core::DVec3 at);

    // What the last dispatch did, for the console probe and the tests.
    struct ResponseReport
    {
        std::uint32_t diverted = 0;
        std::uint32_t spawned = 0;
        float live = 0.0f;
        double reach = 0.0;
        std::uint32_t responderFaction = 0xffff'ffffu;
    };

    [[nodiscard]] const ResponseReport& lastResponse() const { return m_lastResponse; }

    [[nodiscard]] double shipHullFraction(sol::ecs::Entity entity) const;

    // Where a pilot is, for the two callers that have an entity and need a
    // point: the patrol anchor (Phase 36 stage B) and the console probe.
    // Returns the origin for a dead or transform-less entity, which both
    // callers treat as "no useful answer" rather than as a position.
    [[nodiscard]] sol::core::DVec3 pilotPosition(sol::ecs::Entity entity) const;
    // Every hunter in the system and the hauler it is going for, for the
    // console.
    void hunterInfo(std::vector<HunterInfo>& out);

    // Who is currently on a call, nearest first.
    void responderInfo(std::vector<ResponderInfo>& out) const;

    // --- Postures (Phase 36 stage B) ---------------------------------------
    //
    // Where each patrol in this system is actually standing. Exists for the
    // same reason `responderInfo` and `lastResponse` do: the rule that decides
    // this is a few lines in `spawnAmbientPilots`, and "the rule I wrote is the
    // rule that ran" is a claim this project has repeatedly been wrong about.
    // A test and the console can both count postures with it.
    struct PatrolPost
    {
        // The ENTITY INDEX, not a handle: `ecs::Pool` exposes `entityIndices()`
        // and not the entities themselves, and every other consumer in this file
        // (`handleShipDestroyed`, `noteDamage`) is written against the index too.
        std::uint32_t pilotIndex = 0;
        sol::core::DVec3 position; // where the hull actually is
        sol::core::DVec3 post;     // the station or gate it holds
        double distanceToPost = 0.0;
        // ⚑⚑ WHERE ITS WAYPOINT IS, WHICH IS THE ONLY THING THAT PROVES THE
        // ANCHOR FIX. Spawn position alone cannot: a picket spawned at a gate
        // and then re-anchored on the station by `pilot_patrol_offset` looks
        // identical until the first think has run. This is what the flown state
        // reports, so a test can tick the world and ask.
        double waypointDistanceToPost = 0.0;
        // What it is DOING, which is a different question from where it is and
        // became a live one in stage C: a patrol running an inspection is off
        // its beat, and `Inspect` is the only state that says so.
        PilotState state = PilotState::Idle;
        bool atGate = false; // false = the station approach posture
        std::uint32_t factionIndex = 0xffff'ffffu;
        // ⚑ How far this hull is from the PLAYER (Phase 36 stage C), and it is
        // the number the whole stage turns on: how much warning a stop gives
        // you is how far off the patrol was when it opened one. Reported here
        // rather than derived by the caller because `ecs::Pool` hands out
        // entity INDICES and the registry is private, so nothing outside this
        // class can turn one back into a position.
        double distanceToPlayer = 0.0;
    };

    void patrolPosts(std::vector<PatrolPost>& out) const;

    // --- Notice (Phase 36 stage B) -----------------------------------------
    //
    // ⚑⚑⚑⚑ THE PHASE'S WHOLE BALANCE LIVES HERE, AND `017` SAID SO BEFORE A
    // LINE WAS WRITTEN: "an inspection loop that fires too often is a tax, not
    // a mechanic. Notice must be RARE in policed core space for a clean pilot
    // and COMMON for a dark one." Both halves are requirements, and the second
    // is the one a cautious rule quietly fails.
    //
    // ⚑⚑ IT IS A RATE PER PATROL PER SECOND, NOT A ROLL PER THINK. A think is
    // 2 Hz and the tick is 60 Hz, so anything expressed per-call silently
    // retunes itself the day either cadence changes - which is the shape of bug
    // Phase 8g spent a stage on. `dt` is the only honest unit.
    enum class NoticeReason : std::uint32_t
    {
        None = 0,
        RandomCheck, // a clean pilot, occasionally, because a checkpoint checks
        Dark,        // no transponder: the loud one
        Wanted,      // a price posted by the faction that polices this system
    };

    [[nodiscard]] static const char* noticeReasonName(NoticeReason reason);

    struct NoticeParams
    {
        // Per patrol per second, within `range`. A clean pilot sitting inside a
        // core patrol's envelope for a full minute is stopped about once every
        // twenty; a dark one about twice a minute.
        double cleanPerSecond = 0.000'8;
        double darkPerSecond = 0.03;
        double wantedPerSecond = 0.01;
        // The patrol has to be able to SEE you. Same 80 km `pilotEngageEnemy`
        // uses, deliberately: a player learns one number for "close enough for
        // that ship to care", and two would be two rules to discover.
        double range = 8.0e4;
        // After a stop, this patrol's whole system stops asking for a while.
        // Without it a refusal is re-rolled the next frame and the mechanic is
        // a machine gun rather than an event.
        double cooldownSeconds = 90.0;
    };

    [[nodiscard]] const NoticeParams& noticeParams() const { return m_noticeParams; }

    void setNoticeParams(const NoticeParams& params) { m_noticeParams = params; }

    // What the last notice was, for the probe, the console and the tests.
    struct NoticeReport
    {
        NoticeReason reason = NoticeReason::None;
        std::uint32_t patrolIndex = kNoIndex;
        std::uint32_t factionIndex = kNoIndex;
        double distance = 0.0;
        double atWorldSeconds = 0.0;
        std::uint32_t count = 0; // how many times this session
    };

    [[nodiscard]] const NoticeReport& lastNotice() const { return m_lastNotice; }

    // ⚑⚑ THE POLICY HALF, PUBLIC FOR THE SAME REASON `considerResponse` IS:
    // whether a patrol takes an interest is not a thing a scripted drive can
    // promise, so a test has to be able to state the rule directly. Called from
    // the tick; returns the reason when a patrol has just decided to stop the
    // player, and `None` on every other frame.
    //
    // ⚑ Stage C is what turns this into a hail, a hold and a scan. Stage B
    // stops at the DECISION on purpose - the frequency is the thing that has to
    // be right before there is an interaction hanging off it.
    NoticeReason considerNotice(double dt);

    // Dev/test lever: clears the cooldown so a drive does not have to wait 90 s
    // between attempts.
    void clearNoticeCooldown() { m_noticeCooldown = 0.0; }

    // --- The stop (Phase 36 stage C) ---------------------------------------
    //
    // ⚑⚑⚑⚑ THE HOLD IS MODELLED ON `DockClearance` AND NOT ON THE HAIL, WHICH
    // IS WHAT THE SPEC ASKED FOR AND ALSO WHAT THE SAVE FORMAT ALLOWS. A hail
    // is a one-shot request/reply with no duration at all; a timed, revocable
    // grant is structurally what "hold station while I scan you" IS. And the
    // shape has a second dividend the spec did not name: `m_clearance` has
    // never been serialised, so a hold that lives beside it costs no save bump
    // - where "per-patrol scan state" written as a FIELD ON `ShipPilot` would
    // have cost one, because `ShipPilot` is snapshot component 19 and an id in
    // that schema is a promise about a LAYOUT.
    //
    // ⚑⚑ ONE HOLD AT A TIME, WHICH IS WHAT "PER-PATROL" MEANS HERE. The scan
    // belongs to the patrol running it rather than to the player's reticle -
    // that is the reversal the spec named - but only one patrol is ever running
    // one, because notice is throttled to one stop per system per cooldown. A
    // vector of holds would be a second copy of that guarantee.
    enum class InspectionOutcome : std::uint32_t
    {
        None = 0,
        Complied, // the scan finished; stage D is what turns it into a verdict
        Ran,      // left the envelope, jumped, or docked out from under it
        Lapsed,   // the grant timed out with the scan unfinished
        Lost,     // the patrol died, left, or found something better to do
    };

    [[nodiscard]] static const char* inspectionOutcomeName(InspectionOutcome outcome);

    struct InspectionHold
    {
        std::uint32_t patrolIndex = kNoIndex; // entity index, as `PatrolPost` is
        std::uint32_t factionIndex = kNoIndex;
        NoticeReason reason = NoticeReason::None;
        double secondsLeft = 0.0; // the grant's clock; 0 = no hold
        // 0..1, and it RESETS rather than pauses when the patrol's nose comes
        // off you - which is `tickScanning`'s own rule for the player's scan,
        // mirrored deliberately so the two read the same from the cockpit.
        float scanProgress = 0.0f;
        // How far the patrol is, refreshed every tick. Held on the record
        // rather than recomputed by each reader because the drive lock below is
        // read from `tick`'s input pass, which runs BEFORE `tickInspection`.
        double distance = 0.0;
    };

    // ⚑⚑⚑⚑ HOW CLOSE A PATROL HAS TO BE TO READ YOUR HOLD, AND IT IS DERIVED
    // FROM TWO NUMBERS THAT ALREADY EXIST RATHER THAN CHOSEN. The player's own
    // scanner is already built out of exactly this pair - a pulse that reaches
    // `m_scanRange` and a target scan that works at `kTargetScanRangeFraction`
    // of it - and `space_world.hpp` says why in its own words: "A target scan
    // works far closer than a pulse reaches: you find a contact from across the
    // playfield, then fly to it to learn what it is." A patrol NOTICES you at
    // 80 km and has to CLOSE to read you, through the same 2%: 1.6 km.
    //
    // ⚑⚑⚑ AND THIS IS THE NUMBER THAT MAKES RUNNING POSSIBLE AT ALL, WHICH WAS
    // MEASURED RATHER THAN ASSUMED. Six manoeuvres were flown against a held
    // inspection - sitting still, full burn, boost, hard strafe, a barrel and a
    // hard pitch - and ALL SIX complied: a 340 m/s interceptor tracks a 220 m/s
    // shuttle inside a 20 degree cone without ever losing it, so with the cone
    // as the only way out the stop was unescapable and "interruptible by flying
    // away" was false. What the range restores is a WARNING PROPORTIONAL TO
    // DISTANCE: a picket that meets you 700 m off a gate has you before you can
    // react, and a patrol that spots you 40 km out across a station approach
    // has to spend two minutes closing while your drive is still yours.
    [[nodiscard]] double inspectionScanRange() const
    {
        return m_noticeParams.range * kTargetScanRangeFraction;
    }

    // ⚑⚑ WHAT THE CRUISE LOCK ACTUALLY REACHES (the user's stage C ruling,
    // 2026-09-01: "the hold cuts cruise"). Close enough for them to read you is
    // close enough for them to hold you - ONE rule, on the range and not on the
    // cone, because a cone that flickers for a frame would hand out escapes
    // nobody could aim for.
    [[nodiscard]] bool driveLockedByInspection() const
    {
        return heldForInspection() && m_inspection.distance <= inspectionScanRange();
    }

    struct InspectionParams
    {
        // Longer than the player's 5 s target scan (`kTargetScanSeconds`),
        // because this one is being done TO you and the waiting is the beat.
        double scanSeconds = 12.0;
        // The ceiling on the whole thing. A patrol that cannot get its nose on
        // you inside a minute has lost you, and a cruise drive locked for
        // longer than that stops reading as a stop and starts reading as a bug.
        double holdSeconds = 60.0;
        // Where the patrol settles. The attack case holds at 250 m; an
        // inspection stands off further because nobody is shooting.
        double standoff = 500.0;
    };

    [[nodiscard]] const InspectionParams& inspectionParams() const { return m_inspectionParams; }

    void setInspectionParams(const InspectionParams& params) { m_inspectionParams = params; }

    [[nodiscard]] const InspectionHold& inspection() const { return m_inspection; }

    [[nodiscard]] bool heldForInspection() const { return m_inspection.patrolIndex != kNoIndex; }

    // --- The verdict (Phase 36 stage D) ------------------------------------
    //
    // ⚑⚑⚑⚑ JUDGEMENT IS A FUNCTION CALL AND CONSEQUENCE IS THE STAGE, WHICH
    // IS THE ONE THING THE SPEC GOT EXACTLY RIGHT ABOUT THIS STAGE. Phase 33
    // stage D shipped `commodityLegality`, `jurisdictionOf` and a validated
    // table on every faction def eleven days before this phase was written, so
    // "what is in the hold and who objects to it" is one walk of
    // `m_playerCargo` against a function that already exists. What was never
    // built is what HAPPENS next, and that is everything below.
    enum class InspectionVerdict : std::uint32_t
    {
        None = 0,
        Clean,   // a table was consulted and nothing in the hold is on it
        NoLaw,   // the holder keeps no table: stopped by somebody with nothing to charge you with
        Duty,    // restricted goods: licensed, so it is a bill and the hold stays yours
        Seizure, // contraband: taken, priced, and remembered
        Fled,    // the hold never reached a verdict, and leaving is its own offence
    };

    [[nodiscard]] static const char* inspectionVerdictName(InspectionVerdict verdict);

    // What one walk of the hold found. `worst` is the highest legality any
    // non-zero entry answered; `commodity` names the first entry that answered
    // it, in commodity order, so a stop's sentence is stable across frames;
    // `units` is how much of that tier is aboard in TOTAL, because a fine on
    // one crate of a hold carrying five is a fine somebody would take.
    struct HoldJudgement
    {
        sol::assets::Legality worst = sol::assets::Legality::Legal;
        std::uint32_t commodity = kNoIndex;
        float units = 0.0f;
        double value = 0.0; // base-price worth of those units
        // ⚑⚑ WITHOUT THIS, "YOUR HOLD IS CLEAN" AND "WE HAVE NO OPINION ABOUT
        // YOUR HOLD" ARE THE SAME ANSWER, AND THEY ARE THE TWO HALVES OF WHAT
        // THIS PHASE IS ABOUT. `factionLegalityOf` returns `Legal` both for a
        // good a jurisdiction has considered and permitted and for one it has
        // never thought about, because a faction with no table has no way to
        // say either. The Freight Guild holds 25 of 85 systems and declares
        // nothing illegal, so this distinction covers a quarter of the galaxy.
        bool holderHasTable = false;
    };

    // ⚑ PUBLIC AND CONST, so a test can state what the law says about a hold
    // without opening a stop and the console can print it. The jurisdiction is
    // whoever holds the system RIGHT NOW - Phase 33 stage D's ruling, and the
    // reason you can clear a gate legal and reach the station a criminal with
    // nothing in the hold having moved.
    [[nodiscard]] HoldJudgement judgeHold() const;

    // ⚑⚑⚑⚑ THE PRICES, WHICH ARE THE USER'S THREE STAGE-D RULINGS
    // (2026-09-01) WRITTEN AS NUMBERS.
    //
    //   1. CONTRABAND: TAKE IT, POST A PRICE, DO NOT SHOOT. The crate is
    //      seized, a bounty goes up with that faction, standing is spent - and
    //      `contrabandStanding` is set so one stop can never cross the hostile
    //      threshold on its own. See the note on it: the ladder is real and
    //      nothing here authors it.
    //   2. RUNNING IS THE CRIME; LAPSING IS NOT. Leaving the envelope is an
    //      offence whatever was aboard, because fleeing tells them everything.
    //      A hold that timed out because the patrol never closed costs nothing:
    //      from their side they simply lost you, and stage C measured that the
    //      only pilots who CAN lapse are the ones spotted from 20 km out.
    //   3. RESTRICTED IS A BILL, NOT A SEIZURE. `Legality::Restricted`'s own
    //      comment is "licensed: carriable, and a patrol will want papers" -
    //      and there is no licence anywhere in this game to want. So a patrol
    //      charges duty on it instead: credits only, hold intact, nothing
    //      posted and nothing remembered.
    struct VerdictParams
    {
        // Of the goods' base value, charged on restricted cargo. A FRACTION
        // rather than a per-unit fee because the shipped galaxy's two
        // restricted goods are worth 64 and 11 credits a unit: one flat number
        // is a formality on hull plate and worse than confiscation on salvage.
        float dutyRate = 0.30f;
        // Credits posted per unit of contraband seized, with a floor so a
        // single crate still puts a price on you. Deliberately NOT derived
        // from what the goods are worth: a jurisdiction posts a price for the
        // OFFENCE, and salvage being cheap is not a reason to overlook it.
        float bountyPerUnit = 25.0f;
        float bountyFloor = 250.0f;
        // What running costs, flat. One offence, and the hold it was hiding is
        // exactly what nobody got to look at.
        float fledBounty = 400.0f;
        // ⚑⚑⚑⚑ THE LADDER IS EMERGENT, NOT AUTHORED, AND THESE TWO NUMBERS
        // ARE THE WHOLE OF IT. Player standing NEVER DRIFTS - `FactionSim`
        // touches `m_standings` only from an explicit call, so nothing here is
        // forgiven by waiting - a major starts you at 0 and `hostileThreshold`
        // is -30. So the THIRD seizure puts you under it, and at that point
        // `beginInspection` refuses to open a stop at all (stage C's "a faction
        // already shooting at you does not check your papers") while
        // `pilotEngageEnemy` starts picking you as a target. The fourth crate
        // is not inspected. It is met. Nothing in this file says that; it falls
        // out of two numbers that were already here.
        //
        // ⚑ And the way back is trade, at `commerceRate` 0.001 standing per
        // credit: 12 points of standing is 12,000 credits of honest business,
        // which is the phase's answer to "can I ever fly here again".
        float contrabandStanding = -12.0f;
        float fledStanding = -8.0f;
    };

    [[nodiscard]] const VerdictParams& verdictParams() const { return m_verdictParams; }

    void setVerdictParams(const VerdictParams& params) { m_verdictParams = params; }

    // What the last stop came to, and how many of each there have been. The
    // probe's half of the stage: an outcome nobody can count is an outcome
    // nobody can tune.
    struct InspectionReport
    {
        InspectionOutcome outcome = InspectionOutcome::None;
        NoticeReason reason = NoticeReason::None;
        std::uint32_t factionIndex = kNoIndex;
        double atWorldSeconds = 0.0;
        float progressAtEnd = 0.0f;
        std::uint32_t opened = 0;
        std::uint32_t complied = 0;
        std::uint32_t ran = 0;
        // Phase 36 stage D: what it came to and what it cost. Zero on every
        // one of these is a real answer rather than a missing one - it is
        // precisely what a clean hold in front of an honest patrol looks like.
        InspectionVerdict verdict = InspectionVerdict::None;
        double creditsTaken = 0.0;
        float unitsSeized = 0.0f;
        float bountyPosted = 0.0f;
        float standingSpent = 0.0f;
        std::uint32_t seizures = 0; // this session
    };

    [[nodiscard]] const InspectionReport& lastInspection() const { return m_lastInspection; }

    // ⚑⚑ PUBLIC FOR THE SAME REASON `considerNotice` AND `considerResponse`
    // ARE: whether a stop can be opened at all is a rule, and a scripted drive
    // cannot promise a random roll will fire. Refuses when one is already in
    // flight, when the pilot is not a live patrol, and when the owning faction
    // is already hostile - a faction that has decided to shoot you does not
    // stop to check your papers first.
    bool beginInspection(std::uint32_t patrolIndex, NoticeReason reason);

    // Ends whatever is in flight, says why, and records the outcome. Safe to
    // call with no hold.
    void endInspection(InspectionOutcome outcome, const char* reason);

    // --- The verdict hook (Phase 36 stage D) -------------------------------
    //
    // ⚑⚑ STAGE C HELD THE HOOK BACK ON PURPOSE - "the demand is one sentence
    // with no policy in it" - and this is the stage where there is finally a
    // decision worth authoring. The shape is `dock_request`'s, verbatim,
    // because it is the shape every hook in this game that answers a player
    // action already uses: the WORLD decides what was found and queues it,
    // `GameContent` drains the queue and asks `inspection_verdict` what to do
    // about it, and the three answers below validate what comes back. A mod
    // can write a Navy that fines where the Hegemony seizes, or a station whose
    // patrols wave a friend through, without touching a line of C++ - and the
    // scriptless default enforces the same law with no scripts loaded at all.
    //
    // ⚑ Queued only for `Complied` and `Ran`. `Lapsed` and `Lost` reach no
    // verdict by construction: nobody read the hold, so there is nothing to
    // rule on, which is ruling 2 ("running is the crime; lapsing is not")
    // being a fact about the queue rather than a branch inside it.
    struct PendingVerdict
    {
        std::uint32_t factionIndex = kNoIndex;
        NoticeReason reason = NoticeReason::None;
        InspectionOutcome outcome = InspectionOutcome::None;
        HoldJudgement found;
        double roll = 0.0; // the hook's only entropy, as dock_request's is
    };

    [[nodiscard]] bool takeInspectionVerdict(PendingVerdict& out);

    // The three answers, exactly one of which a hook should call.
    //
    // ⚑⚑ `inspectionSeize` SPENDS THE STANDING AND THE OTHER TWO DO NOT, AND
    // THAT IS NOT SCRIPTABLE ON PURPOSE. The standing cost is the whole ladder
    // - three seizures is what stops you being inspected and starts you being
    // shot at - so a hook that could pass its own number could enforce the law
    // in words while deleting its teeth, and the difference would be invisible
    // from the cockpit. Credits and the posted price are policy; what a
    // faction THINKS of a smuggler is a consequence.
    void inspectionPass(const std::string& message);
    // Capped at what the player actually has: a duty is a transaction, and a
    // transaction against an empty account is an empty account. Deliberately
    // no debt and no fallback seizure - ruling 3 says the hold stays yours.
    void inspectionFine(double credits, const std::string& message);
    // Takes every commodity the local table calls contraband, posts `bounty`,
    // and spends `contrabandStanding`. On a `Ran` outcome it takes nothing -
    // a patrol cannot lift a crate off a ship that is not there - so the same
    // answer means "post the price and remember it" for a runner.
    void inspectionSeize(double bounty, const std::string& message);

    // ⚑⚑ THE SCRIPTLESS HALF, AND IT LIVES HERE RATHER THAN IN `GameContent`
    // WHERE `dock_request`'s DEFAULT DOES. That one is four interchangeable
    // integers and a refusal; this one is the phase's whole price list, and
    // the numbers are in `VerdictParams` two screens up. Splitting the law
    // from its prices to match another hook's file layout would be tidiness
    // buying nothing. ⚑ It exists at all for stage A's reason: a rule that
    // lives only inside `init.lua` vanishes the moment somebody's script
    // errors, and this is the rule the phase is for.
    void applyDefaultVerdict(const PendingVerdict& pending);

    // --- Countermeasures (Phase 36 stage E) --------------------------------
    //
    // ⚑⚑⚑⚑ WHAT THE FIT MAKES OF YOU, AND `017` IS WHY IT IS NOT A DETECTION
    // ROLL. That record turned down detection-and-consequence-only because
    // "with no stop to survive, a signature dampener is a bigger number and
    // nothing else" - so the stat has to move the STOP and not only the odds
    // of one. It moves two things, and the user's ruling (2026-09-01) is which
    // two:
    //
    //   1. THE NOTICE RATE. `considerNotice`'s per-second chance is multiplied
    //      by this, whatever the reason - a dampener is a fact about the hull,
    //      not about why somebody is looking. Dark inside a patrol's envelope
    //      is a stop every 33 s at 1.0 and every 67 s at 0.5.
    //   2. THE SCAN CLOCK. `tickInspection` divides its progress rate by this,
    //      so a 12 s read becomes 24 s at 0.5 - against a 60 s hold that does
    //      NOT move. That is the half that is a tactic: the outcome flips from
    //      `Complied` to `Lapsed`, and stage D prices a lapse at nothing.
    //
    // ⚑⚑⚑⚑ AND THE OBVIOUS THIRD THING IS DELIBERATELY NOT MOVED: THE RANGE
    // THEY READ YOU AT. It was priced before a line was written and it is the
    // worst curve in the phase. `inspectionScanRange` is 1.6 km, the patrol's
    // steering settles at `InspectionParams::standoff` = 500 m, and it closes
    // at 340 m/s - so scaling the read range moves the lapse threshold from
    // 17.9 km to 17.1 km at 0.5 (4%, invisible) and then, below 0.31, puts the
    // read range INSIDE the standoff, where the patrol parks short of its own
    // scanner and can never finish. Flat, and then an I-win button. ⚑ The
    // cliff is worth carrying on its own: a constant in `InspectionParams`
    // silently bounds what a stat in `FitStat` is allowed to be, and nothing
    // in either place says so.
    //
    // ⚑⚑ CLAMPED, BECAUSE `signature_add` EXISTS AND ZERO IS A DIFFERENT KIND
    // OF ANSWER. The def loader refuses a HULL whose own `signature` is <= 0,
    // but `resolveLoadout` adds and multiplies whatever a component asks for,
    // so a `signature_mul = 0` or a negative `signature_add` reaches the reader
    // as "never noticed, and a scan clock divided by nothing". The floor is
    // enforced where the value is READ so that no fit, authored or modded, can
    // switch the phase off from a data file. At the floor a dampened ship is
    // still stopped - about a third as often, with a 34 s read - which is the
    // strongest a countermeasure gets to be while the mechanic still exists.
    static constexpr float kMinSignature = 0.35f;

    [[nodiscard]] float signature() const { return m_signature; }

    // What state a pilot is in. Small, but it is what lets a test state the
    // difference between `pilotPatrolTo` and `pilotTravelTo` in one line -
    // and that difference is the whole of decisions/019 §3's correction.
    [[nodiscard]] PilotState pilotStateOf(sol::ecs::Entity entity) const;

    // ⚑ Stand in a system directly, without flying the gates to it. This is
    // not new power: the death-respawn path already calls `loadSystem` with an
    // arbitrary index, and this is the same call with a name on it. It exists
    // because a response is a property of WHERE YOU ARE, so both the tests and
    // the drive need to be somewhere specific without an eight-hop route
    // deciding whether the stage gets verified.
    bool enterSystem(std::uint32_t systemIndex);

    // The playfield anchor Lua patrol offsets are relative to: the first
    // station of the current system, or the first nav target without one.
    [[nodiscard]] sol::core::DVec3 stationPosition() const
    {
        return m_targets.empty() ? sol::core::DVec3{} : m_targets[0].position;
    }

    // ⚑⚑⚑⚑ THE POST A PILOT IS STANDING AT (Phase 36 stage B): the nearest
    // STATION OR GATE in the current system. This is what makes "both postures"
    // possible, and it replaces `stationPosition()` as the anchor a patrol holds
    // its pattern around.
    //
    // ⚑⚑ IT READS THE SYSTEM SPEC, NOT `m_targets`. The nav-target list is the
    // right thing for a player's cycle key and the wrong thing for this: it also
    // carries asteroid fields, mission objectives, escort markers and the
    // player's own assigned berth, so a patrol anchored on "nearest nav target"
    // would form up around a rock, or around wherever the player was cleared to
    // dock. Stations and gates are the only two things a garrison is posted to.
    //
    // ⚑⚑ AND IT FIXES SOMETHING THAT WAS ALREADY WRONG. `stationPosition()` is
    // `m_targets[0]` - ONE point for the whole system - so in the 45 systems
    // with more than one station every patrol formed up around station 0
    // regardless of which station it was spawned over. That was invisible while
    // there was only one posture.
    //
    // Falls back to the primary planet when a system has neither, which no
    // shipped system does (every system has at least two gates).
    [[nodiscard]] sol::core::DVec3 nearestPost(const sol::core::DVec3& from) const;

    // Decrements think timers by dt and collects pilots due for a Lua think.
    struct PilotThink
    {
        sol::ecs::Entity entity;
        const char* role = "";
        const char* state = "";
        // Player standing vs the pilot's faction ("hostile"/"neutral"/
        // "friendly"), or "none" for unaffiliated console spawns.
        const char* attitude = "none";
        // Whether this pilot flies for a pirate clan (Phase 8x). Strategy is
        // Lua's, and "does a laden hauler outrank the enemy in front of me"
        // is strategy: a clan came out for cargo, a navy came out for the war.
        // Passed the same way faction_think and mission_board are already
        // handed a pirate flag.
        bool pirate = false;
    };

    void collectDuePilotThinks(double dt, std::vector<PilotThink>& out);

private:
    // ⚑⚑ ONE INSTANTIATED SYSTEM AND EVERYTHING IN IT (Phase 38 stage A,
    // extended by stage B). Declared here and defined down among the members,
    // because from stage B onward it is a PARAMETER of most of the private
    // simulation - the tick, the spawn path, the death path - and a nested
    // struct cannot be used before it is at least named.
    struct SystemBubble;

    // --- The model catalog (Phase 9) ---
    // These read `[[model]]` defs, which is why they are members: before this
    // they were a hardcoded switch over a five-member enum, and there was no
    // way for an authored mesh to answer any of them. All four tolerate a null
    // database and an out-of-range index, because a def layer is reloadable at
    // runtime and an index can outlive its row.
    [[nodiscard]] const sol::assets::ModelDef* modelDef(ModelId model) const;
    // Bounding-sphere radius at RenderShape scale 1, in meters; the collision
    // radius is this times the instance scale.
    [[nodiscard]] double modelBaseRadius(ModelId model) const;
    // What steering dodges, at scale 1. Wider than the collision radius for a
    // station, because 8r's berth approach was tuned against the wider figure.
    [[nodiscard]] double modelAvoidRadius(ModelId model) const;
    // False for a gate, which is a doorway you fly through (Phase 8w).
    [[nodiscard]] bool modelIsSolid(ModelId model) const;
    // The model filling one of the slots this game draws into, resolved once
    // at spawn time and never in a per-tick loop.
    //
    // ⚑ Phase 19 REPLACED `modelByName(const char*)` with this, and the
    // replacement is the point rather than a rename. There is now no way for
    // game code to ask for a model by NAME at all: a drawable either fills a
    // declared role, or it is named by a def the content author wrote. Take
    // `model_roles.hpp` ids only - a raw model id here compiles and then
    // silently fails `validateRoles` on nobody's watch.
    [[nodiscard]] ModelId roleModel(const char* role) const;
    // Resolves the seat for a def just applied to the player's entity.
    void applyCockpitOf(const sol::assets::ShipDef& def);
    // Lays the saved fit's fire groups over an armament `applyShipDef` has
    // just rebuilt at group 1 (Phase 31 stage C3). PLAYER ONLY, and that is
    // the rule rather than an omission: an NPC has no console to select a
    // group with, so a gun it carried in group 2 would be a gun it never
    // fired. Keeping the override out of `applyShipDef` is what makes that
    // inexpressible instead of merely unlikely.
    void
    applyPilotFireGroups(std::uint32_t entityIndex, const sol::assets::ShipDef& def, const OwnedShip& ship);

    struct SpawnedShip
    {
        sol::ecs::Entity entity;
        std::string defId;
        std::string name; // display name for targeting
    };

    // A destroyed ship and (when a weapon did it) who fired the killing
    // blow — reputation only moves on player kills (Phase 8b).
    struct DestroyedShip
    {
        std::uint32_t victim = 0;
        std::uint32_t attacker = 0xffff'ffffu;
    };

    static constexpr float kDamageFlashSeconds = 0.45f;
    // How long after the player's last hit a kill still counts as theirs for
    // mission purposes (Phase 8l). Long enough that a patrol stealing the last
    // shot of a dogfight still credits the bounty; short enough that a ship
    // clipped once and left behind does not.
    static constexpr double kAssistSeconds = 10.0;
    static constexpr std::uint32_t kNoIndex = 0xffff'ffffu;
    // ⚑ A FORMAT BOUND, NOT THE POLICY CAP. What may be instantiated at once
    // is stage C's decision, made against the O(n^2) collision pass; this is
    // only the number past which a bubble count read off disk is corruption
    // rather than a world. Deliberately loose enough that stage C cannot bump
    // into it by accident and tight enough that it cannot allocate a galaxy.
    static constexpr std::uint32_t kMaxBubbles = 64;

    // The parked-ship position for a station (clear of its collision sphere).
    [[nodiscard]] sol::core::DVec3 dockPoint(std::uint32_t stationIndex) const;

    // Instantiates systemIndex (statics + side data) and moves the player
    // there: at the gate arriving from fromSystem, or near the first station
    // when fromSystem is kNoFaction-tagged invalid (new game / load).
    void loadSystem(std::uint32_t systemIndex, std::uint32_t fromSystem);

    // ⚑⚑⚑⚑ THE ONE PLACE THE PLAYER'S FRAME CHANGES, AND THE TWO COSMETIC
    // SINKS ARE TOLD FROM IT (Phase 38 stage D). `m_currentSystem` is what
    // every positional output path means by "here": the mixer has one listener
    // and `CombatEffects` has one particle buffer, and a `DVec3` handed to
    // either of them is metres in a barycentre frame that has to be this one.
    // Both sinks refuse anything from another system now, so both have to be
    // told which system that is, and being told it in the same statement that
    // sets it is what keeps them from drifting apart.
    //
    // ⚑⚑ IT IS NOT THE RENDER LOOP'S JOB, WHICH IS WHY IT IS NOT WHERE THE
    // LISTENER'S POSE COMES FROM. A jump happens inside `tick`; the pose is
    // set after `tick` returns. A frame taken from the same place as the pose
    // would be one tick stale across exactly the crossing this phase exists
    // for, dropping the arrival's own sounds and playing the departed
    // system's - the two errors the stage is here to prevent, both at once.
    void enterFrame(std::uint32_t systemIndex);
    // Moves the player into `destination`'s bubble and releases the one they
    // were in, along with every transient view of it (spawned-ship list,
    // combat effects, thrusters, sound).
    // ⚑⚑⚑ WHAT USED TO BE A TEARDOWN IS A DROP NOW (Phase 38 stage A).
    // `despawnSystem` walked the Transform pool, collected everything that was
    // not the player and destroyed it one entity at a time — a filter, and one
    // that had to be right. A bubble is the system's contents, so leaving a
    // system is releasing the bubble, and nothing has to decide what belongs
    // to it. The player is moved out FIRST - into a fresh bubble for the
    // destination - and that crossing is what a jump actually is.
    //
    // ⚑⚑⚑⚑ AND SINCE STAGE C IT DOES NOT ALWAYS RELEASE, AND THE ARRIVAL IS
    // NOT ALWAYS FRESH. Returns true when the destination's bubble was built
    // here and still needs its sky, false when the player has jumped back into
    // a bubble that was RETAINED and is standing in the system as they left
    // it. That return value is the whole difference between the phase's exit
    // happening and not: `loadSystem` fills a sky only when it is told to, and
    // filling one over a retained bubble would build the destination's traffic
    // a second time on top of the traffic already flying in it — stage A's bug
    // through the third door.
    [[nodiscard]] bool leaveSystemFor(std::uint32_t destination);

    // ⚑⚑⚑⚑ WHY A SYSTEM THE PLAYER IS NOT IN STAYS INSTANTIATED, AND FOR HOW
    // LONG (Phase 38 stage C). Returns 0 for "let it go". This one function is
    // the entire retention policy, and it is ONE function on purpose: 015's
    // "the player's system, plus every system holding a player asset" is the
    // policy this stage's interface was written to accept unchanged, and
    // Phase 39 turns it on by adding a second clause HERE and nothing else
    // anywhere. It returns seconds rather than a bool for the same reason — a
    // parked asset is not held for two minutes, it is held while it is parked,
    // and a predicate could not say so.
    [[nodiscard]] double bubbleRetentionSeconds(const SystemBubble& bubble) const;

    // Is a fight going on in this bubble right now: anyone in `Attack`, anyone
    // shot inside the last `kThreatMemorySeconds`, or a bolt still in the air.
    // ⚑ Asked ONCE per departure, with the player still in the bubble — see
    // `kCoolingSeconds` for why asking it a second time answers no.
    [[nodiscard]] static bool bubbleHoldsLiveFight(const SystemBubble& bubble);

    // Ages every retained bubble by `dt` and drops the ones that have run out.
    // Runs after the per-system tick rather than before it, so a bubble whose
    // last second is this one is still simulated for it.
    void releaseCooledBubbles(double dt);

    // Drops retained bubbles, COLDEST first, until `kMaxInstantiatedSystems`
    // is met. Coldest is the one nearest release, so it loses the least — and
    // the bubble the player has just backed out of carries a full window and
    // is therefore never the one evicted, which is the case they are most
    // likely to turn around and fly back into.
    void enforceBubbleCap();

    // ⚑⚑⚑⚑ NOTHING IN THE BUBBLE YOU LEFT MAY STILL NAME YOU (Phase 38 stage
    // C), and this is the first stage where that sentence has teeth. An entity
    // index is per registry and `Registry::create` pops a LIFO free list, so
    // the player's slot is the very next one handed out in the system they
    // left — and both puppet reconciles spawn hulls into every instantiated
    // bubble on every tick. Left alone, the raider that was shooting at you
    // keeps its `targetIndex` and simply transfers the fight to whichever
    // hauler inherits your slot, at full aggression, in a system nobody is
    // watching.
    //
    // Exactly three fields in the whole component set can name the departed
    // player — `ShipPilot::targetIndex`, `ShipPilot::threatIndex` and
    // `Projectile::shooterIndex` — and all three are retired here.
    // (`MinerPuppet::rock` is the fourth entity reference and cannot be the
    // player; a survey that only greps the .cpp misses it, which is why this
    // one was taken against the component structs.)
    void forgetDepartedPlayer(SystemBubble& bubble, std::uint32_t departedIndex);
    // Creates the bubble for a system with every pool the world uses already
    // present, and returns it. Only `spawn` calls it: every other arrival goes
    // through `leaveSystemFor`, which needs the new bubble in hand before the
    // old one is released.
    sol::ecs::Registry& openBubble(std::uint32_t system);
    // ⚑⚑⚑⚑ ONE SYSTEM'S TICK (Phase 38 stage B), and the reason `tick` is a
    // loop now. Eight of the profiler's zones live in here - avoidance, pilots,
    // flight, the two collision halves, projectiles, weapons and the fine half
    // of mining - because each of them is a question about one system's
    // contents. Everything the tick does that is galaxy-wide or about the one
    // entity that is only ever in a single bubble stays in `tick` itself.
    void tickSystem(SystemBubble& bubble, double dt);
    // The player's own flight input, called from inside the per-system loop on
    // the player's bubble only. It has to sit between that system's avoidance
    // and that system's steering, which is why it is not simply hoisted out.
    void tickPlayerFlightInput(double dt);
    // Everything a bubble needs before anything ticks it or reads it: its
    // pools, the statics of its own system, and its own random stream. Three
    // sites make a bubble - `openBubble`, the arrival in `leaveSystemFor`, and
    // the load - and a bubble half-furnished by one of them is a system whose
    // ships dodge the wrong star. `bubble.system` must already be set.
    void furnishBubble(SystemBubble& bubble);
    // Everything that makes a system a place: its statics, its rocks and the
    // ambient traffic its owner and its raiders put there. Shared by the
    // player's arrival in `loadSystem` and by `instantiateSystem`, so a bubble
    // the player is not in is furnished exactly the way theirs is - which is
    // the whole claim the cooling bubble rests on.
    void fillSystemSky(SystemBubble& bubble);
    // ECS statics (stations, gates) for a system spec, into that system's own
    // bubble. Takes the registry since stage B: the sky it builds belongs to
    // the arriving frame, and until stage A there was only one to build into.
    void instantiateSystemEntities(sol::ecs::Registry& registry, const sol::sim::SystemSpec& spec);
    // Non-ECS state (celestials, nav targets, gate list) for a system spec;
    // used alone after a snapshot load, which already carries the statics.
    void rebuildSystemSideData(const sol::sim::SystemSpec& spec);

    // Records feedback for a damage result (sparks, player flash, explosion
    // on a kill is handled by handleShipDestroyed). `attackerIndex` is who
    // dealt it, or kNoIndex where nobody is to blame (a ram) - when it is the
    // player, the target's assist window is re-armed (Phase 8l).
    void noteDamage(SystemBubble& bubble,
                    std::uint32_t targetIndex,
                    const sol::core::DVec3& hitPosition,
                    const sol::sim::DamageResult& result,
                    std::uint32_t attackerIndex = kNoIndex);

    // ⚑⚑ WHICH PLACE ON THE HULL THE HIT LANDED ON, AND WHAT IT COST IT
    // (Phase 31 stage F). Called from `noteDamage` because that is the one
    // funnel every hit in the game passes through carrying all three facts
    // this needs - who was hit, WHERE, and how much got past what. The three
    // damage sites (a ram, a bolt, a beam) each compute an impact point
    // already and each would otherwise grow its own copy of this.
    //
    // ⚑ EXTERNAL MOUNTS ONLY, which is `decisions/014` rule 2 and the
    // roadmap's own wording for this stage: *external hits resolved against
    // `at`*. An internal mount is reached through armour rather than aimed at,
    // and picking WHICH of several a hull hides is a rule this does not have.
    // ⚑ `system` is the frame `hitPosition` is in (Phase 38 stage D). This is
    // the one output site in the death path that had no bubble in hand - it
    // takes a registry, because that is all it needs to find a mount - and the
    // spec's list of `playAt` sites does not name it. A fireball has to know
    // which star it went off beside, so the frame comes in as a value.
    void damageMounts(sol::ecs::Registry& registry,
                      std::uint32_t system,
                      std::uint32_t targetIndex,
                      const sol::core::DVec3& hitPosition,
                      const sol::sim::DamageResult& result);

    // The fitting half of `buildRenderInstances`, split out because it walks a
    // different pool under a different rule and because "which pose is this
    // in" has to be answerable about it on its own - see the definition.
    void appendFittingInstances(float alpha, bool includeShip, std::vector<RenderInstance>& out) const;

    void applyShipDef(sol::ecs::Registry& registry,
                      std::uint32_t entityIndex,
                      const sol::assets::ShipDef& def,
                      const sol::assets::DefDatabase& defs);
    // Re-applies the active ship's resolved def to the player entity
    // (refit/switch/def-reload; resets defenses like any def application).
    void applyActiveLoadout();
    // Shared refusal path for outfitting mutations.
    bool refuse(const std::string& reason, std::string* outError) const;

    // A docked ship is inside the station and takes no damage — otherwise
    // hostiles camp the pad and re-kill on respawn (fatal under decisions/007
    // hardcore, where each death would delete the save).
    // ⚑⚑⚑ THE ONE IDENTITY COMPARISON STAGE A DID NOT CONVERT, BECAUSE THE
    // SURVEY THAT FOUND THE OTHER SIXTEEN GREPPED THE .CPP. It is the same
    // coincidence as all of them: indices are issued per registry and every
    // registry starts at zero, so slot 0 of a bubble the player is not in was
    // "the player" here - and this one grants DAMAGE IMMUNITY, so the wrong
    // answer is an NPC that cannot be shot for as long as the player is docked.
    [[nodiscard]] bool isDamageImmune(const sol::ecs::Registry& registry, std::uint32_t entityIndex) const
    {
        return isPlayerEntity(registry, entityIndex) && isDocked();
    }

    // Resets the fleet to the single new-game starter ship.
    void resetFleetToStarter();
    // ⚑⚑ TAKES THE BUBBLE THE VICTIM DIED IN (Phase 38 stage B), which is the
    // stage's whole point one function down: the four coarse attributions in
    // here used to be keyed on `m_currentSystem` - the system the player is
    // STANDING IN - so a hauler killed in a bubble the player had left charged
    // the loss, the contest pressure and the wreck to the wrong system, and the
    // wreck's position is a DVec3 that would then sit in open space beside the
    // player's star. Phase 37 stage E's lesson exactly: key a consequence on
    // the facts, not on the call site.
    void handleShipDestroyed(SystemBubble& bubble,
                             std::uint32_t entityIndex,
                             std::uint32_t attackerIndex = kNoIndex);
    // Rebuilds the runtime faction table (majors + jittered clans) and
    // (re)initializes the FactionSim and MissionSim against the current
    // galaxy; called by generateUniverse and by loadFrom after a galaxy
    // regeneration.
    void initializeFactions();

    // One seller's answer to one gate: the allowlist against that faction's def
    // id, the `min_rep` against that faction's standing. `stationSells` and
    // `stationSellsAtFence` are the two questions built out of it.
    [[nodiscard]] bool counterSells(std::uint32_t seller, const sol::assets::CatalogGate& gate) const;
    // Rolls a module list for every station in the galaxy and turns each one
    // into an economy archetype of its own (Phase 34 stage B). Called by
    // generateUniverse and by loadFrom after a galaxy regeneration, for
    // `initializeFactions`'s reason: the layout re-derives, it is never saved.
    //
    // ⚑⚑⚑ IT RUNS AFTER `generateGalaxy` AND OUT OF ITS OWN Rng STREAM, WHICH IS
    // THE WHOLE REASON THE GALAXY DID NOT MOVE. `populateSystem` draws archetype
    // -> name -> distance -> direction per station out of that system's stream,
    // so a draw taken inside it would shift every station position and gate in
    // the galaxy and both geometry digests with them. Composing afterwards, from
    // a stream nothing else uses, cannot.
    void composeStations();
    // Names the clan running each composed station's black-market module (Phase
    // 34 stage E), out of its own stream again so that composing and staffing
    // are independent edits. Called at the tail of `composeStations`, which is
    // also what makes `loadFrom`'s re-compose cover it.
    void assignShadowOwners();

    // Seats somebody in every room, at the tail of `composeStations` beside
    // `assignShadowOwners` (Phase 35 stage C).
    //
    // ⚑⚑⚑ ONE STREAM, TWO PASSES, AND THE ORDER IS WHAT MAKES BOTH STABLE. Pass
    // one draws EXACTLY ONCE PER STATION in galaxy order - for stations with no
    // room too, which is the discipline the two passes above already follow -
    // so its draw count is a function of the galaxy alone and nothing about
    // rooms, recipes or the cast can move it. Pass two then seats the authored
    // cast in DEF ORDER off the same stream, which is why a row APPENDED to
    // `characters.toml` cannot move anybody already written. A second stream
    // would have bought nothing that this ordering does not already give.
    void assignCast();
    // Hands player cargo to any active Deliver objective at the docked
    // station: cargo leaves the hold, the market stock refills (the contract
    // literally fills the shortage it advertised).
    void processMissionDeliveries();
    // Applies queued mission consequences (payout, standing moves) and
    // buffers the events for GameContent's Lua forwarding.
    void processMissionEvents();
    // Survey layout for the current galaxy (sized like the economy's).
    void initializeSurvey();
    // Rebuilds the dynamic tail of m_targets — discovered signals, asteroid
    // fields, and wrecks. The tail sits after the statics so station/gate/
    // planet indices never move under Lua or the HUD, and it is append-only
    // so a site found (or a ship killed) later never shifts a slot the player
    // or a scan in flight is already pointing at. Slots whose object is gone
    // (a decayed wreck) are compacted, and the target index follows them.
    void rebuildDynamicTargets();
    // Rewrites the names of the static head (Phase 8z): a station or gate the
    // player has not identified reads as an anonymous contact, and takes its
    // real name the moment the bit flips. Names are stored rather than masked
    // at read time so that every existing reader of navTargets()[i].name — the
    // HUD, both maps, the console and Lua — keeps working untouched.
    void refreshStaticTargetNames();
    // Identifies the gate the player just arrived through and the station they
    // are docked at: you always know what you are touching, and without this a
    // new game starts the player inside an "Unknown contact".
    void identifyTouchedObjects(std::uint32_t fromSystem);
    // Identifies whichever station or gate a nav slot names, refreshing the
    // names when it changed something. False when the slot is neither, or was
    // already identified. One choke point so the held scan and the console
    // lever cannot identify a thing by two different routes (8u's rule).
    bool identifyStructure(std::size_t index);
    // Carries the selection off a nav slot the player cannot see. Only ever
    // needed after the fog changes or the list is rebuilt; a hidden selection
    // would otherwise print a masked name in the HUD and be flyable by
    // Autopilot, which is the fog leaking through the one seam that does not
    // go through the cycle.
    void snapSelectionToVisible();

    // Where the tracked objective's nav slot goes, when it has one here. Two
    // kinds do: a FlyTo, which has carried a position since 8c, and an Escort
    // whose hauler currently has a body in this system (Phase 8x §E).
    // Dock/Deliver name a station that is already a nav target, and Kill and
    // Hold have no position at all, so those are answered by the HUD line.
    //
    // It answers a POSITION rather than the objective, because an escort's
    // marker is the hauler itself and moves every tick — which is the whole
    // difference between the two kinds that get one.
    struct ObjectiveMarker
    {
        sol::core::DVec3 position;
        double radius = 0.0; // FlyTo's completion sphere; an escort has none
        bool moving = false; // an escort: re-read every tick, and named apart
    };

    [[nodiscard]] bool objectiveMarker(ObjectiveMarker* out) const;
    // Rebuilds the tail only when the objective slot's presence or position no
    // longer matches the tracked mission. Called each tick; costs a pointer
    // chase and a vector compare when nothing has changed.
    void syncObjectiveTarget();
    // How far short of a target autopilot parks. Shared by the engage message
    // and the steering itself, so what it announces is what it does.
    [[nodiscard]] double autopilotArrivalRange(const TargetInfo& target) const;
    // Mining layout for the current galaxy (sized like the economy's), and
    // the ore table read out of the commodity defs.
    void buildMiningParams();
    void initializeMining();
    // Instantiates this system's rocks and wrecks as entities. Rocks come
    // from the seed; a rock already cut to nothing is simply not spawned.
    void instantiateMiningEntities(SystemBubble& bubble);
    // Advances tumble, chunk drift, proximity collection and the wreck
    // reconcile, for one system. ⚑ The COARSE half - wreck decay and refinery
    // orders - left this function at Phase 38 stage B and runs once in `tick`,
    // above the loop: it is galaxy-wide work, and ticking it once per
    // instantiated system would have aged every wreck in the game k times.
    void tickSystemMining(SystemBubble& bubble, double dt);
    // Cuts `units` out of a rock or wreck entity and spills what came off as
    // drifting chunks. Returns what actually came out.
    float
    cutRock(SystemBubble& bubble, const sol::core::DVec3& cutter, std::uint32_t entityIndex, float units);
    float
    cutWreck(SystemBubble& bubble, const sol::core::DVec3& cutter, std::uint32_t entityIndex, float units);
    // One chunk off a cut surface: it leaves toward whoever is cutting, with
    // a spread, so mining is gathering rather than chasing.
    //
    // ⚑ `cutter` is that ship's position, and it is a PARAMETER since Phase 38
    // stage B rather than `shipState().position`. Cutting happens to be
    // player-only today - the mining branch of the firing pass is gated on
    // `isPlayerEntity` - so reading the player's own transform was right by
    // accident, through a guard in a different function. In a bubble the player
    // is not standing in it would have thrown every chunk toward a point in
    // another system's frame.
    void spawnCutChunk(SystemBubble& bubble,
                       const sol::core::DVec3& cutter,
                       const sol::core::DVec3& origin,
                       double surface,
                       std::uint32_t commodity,
                       float units);
    void spawnOreChunk(SystemBubble& bubble,
                       const sol::core::DVec3& position,
                       const sol::core::DVec3& velocity,
                       std::uint32_t commodity,
                       float units);
    // Fits a salvaged component if it is legal on the active ship; used by both
    // site salvage and wreck cutting.
    bool tryFitSalvagedComponent(const std::string& componentId, std::string& outName);
    // The rock or wreck entity the boresight is on within `range`, or ~0u.
    [[nodiscard]] std::uint32_t entityAhead(double range, bool& outIsWreck) const;
    // One commodity out of the table, weighted by what a hull that size would
    // plausibly have been carrying (Phase 33 stage C). `kNoIndex` when there is
    // no commodity table at all, or when every good in it is a module kit and
    // the hull is too small to have had one. Both loot composers use it.
    [[nodiscard]] std::uint32_t rollHauledCommodity(sol::core::Rng& rng, bool canCarryAssemblies) const;
    // The docked station archetype's refinery pair, resolved from the defs.
    bool dockedRefinePair(std::uint32_t& input, std::uint32_t& output) const;
    // Pulse cooldown plus target-scan progress for the player's current
    // target; resolves the target when the scan completes.
    void tickScanning(double dt);
    // Clearance countdown, comms fade, and the arrival test that turns flying
    // into a berth into being docked (Phase 8r).
    void tickDocking(double dt);
    // The stop's clock, its scan, and every way out of it (Phase 36 stage C).
    // Runs after `tickDocking` for the same reason that one runs where it does:
    // the positions it reads have to be this frame's.
    void tickInspection(double dt);
    // Who is doing the stopping, for the four lines that name them. The
    // faction's display name, or "Patrol" for an unaffiliated console spawn.
    [[nodiscard]] std::string inspectorName() const;
    // Queues a stop's outcome for the verdict hook (Phase 36 stage D). Called
    // from `endInspection` for `Complied` and `Ran` and from nowhere else, so
    // the two outcomes nobody read a hold for cannot reach a ruling.
    void queueVerdict(InspectionOutcome outcome);
    // Empties every hold entry the local table calls contraband. Returns the
    // units taken. The only writer of `m_playerCargo` in the game that is
    // neither a trade, a take, nor death.
    float seizeContraband();
    // ⚑⚑⚑⚑ THE OTHER END OF THE AXIS (Phase 37 stage E), AND IT IS ONE
    // SENTENCE APPLIED IN TWO PLACES: *where a good is contraband to the
    // jurisdiction you are standing in, moving it moves two reputations in
    // opposite directions.* This is the stop's half; `recordPlayerTrade` below
    // is the counter's half, and both call this.
    //
    // ⚑⚑⚑ THERE IS NO RATE AND THAT IS THE DESIGN. The gain is exactly the
    // negation of what the law spent - not a tuned fraction of it - so the
    // invariant a test can state is an IDENTITY rather than a number somebody
    // has to keep in step with `contrabandStanding`. decisions/017 asks for
    // standing "earned by the acts that cost standing with the law"; a rate
    // would have made it "earned by a fraction of" and left a knob whose only
    // safe value is 1. Phase 37 stage C's own lesson: a constant that can only
    // ever hold one value is a comment pretending to be code.
    //
    // ⚑⚑ AND THE PRICE IS STRUCTURAL RATHER THAN AUTHORED, WHICH IS THE
    // PHASE RISK REGISTER'S TEST ("if shadow standing has no price, the
    // allegiance is a bonus"). Every point credited here is a point some
    // jurisdiction just took away, because the caller only reaches this line on
    // the branch where the law charged. A pilot who never smuggles never earns
    // any, and there is no path to the Null Signature Suite's +25 that does not
    // run through 25 points of somebody's ill will. ⚑ Silent when no
    // `kind = "shadow"` def is loaded - a mod, and several trimmed test def
    // sets - because `shadowFactionIndex` is `kNoFaction` there and
    // `FactionSim::addStanding` bounds-checks.
    void creditShadowStanding(float delta);
    // ⚑⚑⚑ WHOSE GOODWILL A TRADE EARNS, WHICH WAS `systemOwnerFaction`
    // UNCONDITIONALLY FROM PHASE 8 UNTIL PHASE 37 STAGE E. The back room's rows
    // go through `playerBuy`/`playerSell` like every other row, so buying
    // contraband in a Hegemony-held station's back room EARNED HEGEMONY
    // GOODWILL - the law thanking you for smuggling in its own space. Four
    // answers now, and the two new ones are the stage:
    //
    //   1. their goods, banned here -> +The Ninth Shift, -the law, same size;
    //   2. contraband here but not the fence's trade (salvage under the
    //      Ironstar) -> NOTHING. You do not earn credit with a government for
    //      selling it something it bans, and inventing a punishment for an
    //      open-counter sale Phase 33 shipped is not this stage's business;
    //   3. the fence's trade where nobody objects (the Guild's two fences, all
    //      of clan space) -> NOTHING. A transaction no one with an opinion
    //      witnessed changes no one's opinion, and crediting the shadow here
    //      would be the free +25 the risk register warns about;
    //   4. anything else -> the owner's goodwill, exactly as before.
    void recordPlayerTrade(std::uint32_t commodity, double credits);
    // Records what an answer came to, says the line, and closes the ruling.
    // One choke point so `m_lastInspection` cannot be written three ways.
    void settleVerdict(InspectionVerdict verdict,
                       double credits,
                       float units,
                       float bounty,
                       float standing,
                       const std::string& message);
    // Arms a jump when the player's path this tick went through a gate's
    // opening (Phase 8v/8w). The sibling of tickDocking's berth arrival test.
    void tickGateCrossing();
    // Where autopilot should actually fly to for a given target: the target
    // itself, except for a gate, which is a doorway to pass through rather
    // than a point to stop at (Phase 8w).
    [[nodiscard]] sol::core::DVec3 autopilotDestination(const TargetInfo& target,
                                                        const sol::core::DVec3& from) const;
    // Everything that happens when the ship is inside the station, whichever
    // way it got there: the pad, the mission notify, the market snapshot and
    // the dock event. `berth` is kNoIndex for the shortcut and respawn paths.
    void completeDock(std::uint32_t station, std::uint32_t berth);
    // Nearest station in this system within `range`, or kNoIndex.
    [[nodiscard]] std::uint32_t nearestStationWithin(double range, double* outDistance) const;
    // Slot of the NavKind::Berth target, or kNoTarget with no clearance.
    [[nodiscard]] std::size_t berthTargetIndex() const;
    // Signal index of the current target, or kNoIndex when it is not a site.
    [[nodiscard]] std::uint32_t targetSignalIndex() const;
    // Body index (0 = star, 1.. = planets) of the current target, or kNoIndex.
    [[nodiscard]] std::uint32_t targetBodyIndex() const;
    // The scriptless loot table: deterministic in the signal's own seed, so a
    // site holds the same thing whenever the player gets around to it.
    [[nodiscard]] sol::sim::SignalLoot defaultLoot(const SignalInstance& signal) const;
    // Ambient NPC population for a freshly instantiated system: owner
    // patrol/raider wings by region security plus raid-intensity incursions.
    void spawnAmbientPilots(SystemBubble& bubble, const sol::sim::SystemSpec& spec);

    ResponseReport m_lastResponse;

    // ⚑⚑⚑ LIFTED OUT OF `spawnAmbientPilots` (Phase 30 stage C), WHERE IT WAS
    // A LAMBDA. decisions/019 called this "stage C's first requirement" and
    // described it as lifting out of `loadSystem`; it was never in
    // `loadSystem` - `spawnAmbientPilots` has been a member the whole time -
    // and the lambda captured nothing but members, so the lift is a move with
    // an empty capture list rather than the refactor it was priced as.
    // "Send a wing later" is callable now, which is the whole point.
    // ⚑ Takes a SPAN since Phase 32 stage C, because `factionRoster` answers
    // with one - and because the empty answer it gives for a declared cell is
    // the same empty answer this already returned on, so a faction that fields
    // none needs no branch at any call site.
    //
    // ⚑⚑ SINCE STAGE D IT TAKES THE CELL AND THE SECURITY RATHER THAN A
    // `PilotRole`. The cell is what the job comes from (`pilotRoleFor`) and it
    // is the cell ASKED FOR, so a substituted roster still flies the asked-for
    // job; the security is what `chooseWingHull` ranks the roster against.
    // Every one of the seven call sites already had both in hand.
    void spawnWing(SystemBubble& bubble,
                   std::uint32_t faction,
                   sol::assets::RosterCell cell,
                   std::span<const std::string> roster,
                   float baselineSecurity,
                   std::uint32_t count,
                   const sol::core::DVec3& anchor,
                   double spread,
                   PilotState state = PilotState::Idle,
                   const sol::core::DVec3* waypoint = nullptr);
    // Rebuilds m_avoidance for this tick from the same source the collision
    // bodies come from (Phase 8y). Runs before any steering, because a ship
    // that steers on last tick's picture of a moving fleet is steering at
    // where things were - and per bubble since stage B, because "what I can
    // fly into" is a question about one system.
    void rebuildAvoidance(const SystemBubble& bubble);
    // Warns the player off an obstruction they are cruising at, and cuts the
    // drive when there is no longer room to stop (Phase 8y §D). Manual flight
    // only: autopilot slows itself, and sub-cruise flight is the player's own.
    void guardManualCruise(double dt);
    // The two response entry points with the frame they happened in. The
    // public ones above are these with `playerBubble()` filled in - which is
    // what "an incident where the player is standing" means, and the only
    // thing a Lua caller or a test could have meant by naming no system.
    std::uint32_t respondTo(SystemBubble& bubble,
                            sol::core::DVec3 position,
                            std::uint32_t offenderIndex,
                            ResponseCause cause);
    void considerResponse(SystemBubble& bubble,
                          std::uint32_t targetIndex,
                          std::uint32_t attackerIndex,
                          sol::core::DVec3 at);

    // ⚑⚑ THE ONCE-PER-TICK HALF OF BOTH RECONCILES (Phase 38 stage B). The
    // presence marks are indexed by TRADER and by MARKET, which are galaxy-wide
    // things, so clearing them per bubble would let the second bubble unmark
    // everything the first had just claimed and spawn the whole fleet twice.
    // The miner hold clock ages here for the same reason: it is measured in
    // seconds, and k systems would have spent k*dt of it.
    void beginPuppetReconcile(double dt);
    // Reconciles trader bodies with the coarse fleet (Phase 8x): a body for
    // every EconomyTrader flying an in-system leg here, and none for anyone
    // else. Runs after the economy tick, which is the moment the set goes
    // stale. The record decides who exists; this only draws the consequence.
    //
    // ⚑ "Here" means this BUBBLE since stage B - `decisions/015`'s "the
    // player's system, plus..." spelled as a set rather than as a comparison.
    void syncTraderPuppets(SystemBubble& bubble);
    // Reconciles miner bodies with the extractor stations here (Phase 8x stage
    // 6): a ship at the rock for every outpost in this system that is actually
    // drawing, and none for one that has stopped — because its warehouse is
    // full, because the rock ran out, or because the player shot its last
    // miner. Runs beside the trader reconcile for the same reason: the economy
    // tick is the one moment any of that can change.
    void syncMinerPuppets(SystemBubble& bubble, double dt);
    // The rock a miner should be working: the nearest one holding what its
    // outpost digs on the first pick, then round the same field. Answers false
    // when there is nothing of that commodity left in the sky.
    [[nodiscard]] bool chooseMinerRock(const sol::ecs::Registry& registry,
                                       MinerPuppet& miner,
                                       const sol::core::DVec3& from,
                                       bool sameField) const;
    // Where a miner should sit to work the rock it has picked (its hold point
    // off the surface), and where that rock is. False when the rock is gone.
    [[nodiscard]] bool minerWorkPoint(const sol::ecs::Registry& registry,
                                      const MinerPuppet& miner,
                                      sol::core::DVec3& rock,
                                      sol::core::DVec3& hold) const;

    // The leg a trader is currently flying: its two ends in system space, how
    // far along the record says it is, and how long the leg is quoted at.
    // Answers false when the trader is not flying an in-system leg here.
    struct TraderLegPlacement
    {
        sol::core::DVec3 from;
        sol::core::DVec3 to;
        float progress = 0.0f;
        double legSeconds = 0.0;
    };

    // ⚑ Takes the system it is being asked about since stage B. It used to
    // compare the route against `m_currentSystem` and answer false for
    // everywhere else, which is the same thing while exactly one system is
    // instantiated and the wrong question the moment two are.
    [[nodiscard]] bool
    traderLegSegment(std::uint32_t traderIndex, std::uint32_t system, TraderLegPlacement& out) const;
    // Where the record says a trader is, on its schedule rather than its
    // engines. See keepTraderOnSchedule for why the two differ.
    [[nodiscard]] sol::core::DVec3 traderScheduledPoint(const TraderLegPlacement& leg) const;
    // True when the record moved the trader this tick rather than its engines,
    // which is what makes it uncatchable and so unhuntable.
    bool keepTraderOnSchedule(sol::ecs::Registry& registry,
                              sol::ecs::Entity entity,
                              const TraderLegPlacement& leg);
    // Removes a spawned ship with none of the death path's consequences, out
    // of the bubble it was spawned into - a puppet that has flown out of the
    // record's leg has to stop being drawn in ITS system, not in the player's.
    void despawnShip(SystemBubble& bubble, std::uint32_t entityIndex);
    // Spawn at an explicit position (ambient wings); the public
    // spawnShipFromDef wraps this at a point ahead of the player. ⚑ Takes the
    // bubble since stage B: a hull spawned into one system's registry has to be
    // recorded in that same system's ship list, because an entity INDEX means
    // nothing without the registry that issued it.
    sol::ecs::Entity spawnShipAt(SystemBubble& bubble,
                                 const sol::assets::ShipDef& def,
                                 const sol::assets::DefDatabase& defs,
                                 const sol::core::DVec3& position,
                                 const char* factionName);

    // The bubble the player is standing in. Every player-scoped question in
    // this file — what the HUD reads, what the autopilot flies, what a station
    // screen shows — is asked of this one, and that stays true when stage B
    // gives the tick more than one to walk.
    //
    // ⚑ `m_bubbles` is never empty after `spawn()`: the player's entity is
    // created in a bubble and moves between bubbles, and there is no state in
    // which they are in none.
    [[nodiscard]] sol::ecs::Registry& playerRegistry() { return m_bubbles.front()->registry; }

    [[nodiscard]] const sol::ecs::Registry& playerRegistry() const { return m_bubbles.front()->registry; }

    // The bubble itself, for the handful of places that want more of it than
    // the registry — the death path, the puppet reconcile, and the tick.
    [[nodiscard]] SystemBubble& playerBubble() { return *m_bubbles.front(); }

    [[nodiscard]] const SystemBubble& playerBubble() const { return *m_bubbles.front(); }

    // ⚑ `m_spawnedShips` until stage B, and the rename is the point: everything
    // that reads it — the target cycle, the contact list, the hail table, the
    // Lua bindings — wants the ships the PLAYER can see, and now says so. The
    // three sites that wanted a particular system's ships take the bubble.
    [[nodiscard]] std::vector<SpawnedShip>& playerShips() { return m_bubbles.front()->spawnedShips; }

    [[nodiscard]] const std::vector<SpawnedShip>& playerShips() const
    {
        return m_bubbles.front()->spawnedShips;
    }

    // The bubble for a system, or null when that system is not instantiated.
    // Null is the ordinary answer for 80 of the 81 systems and callers must
    // treat it as one, which is the whole reason this returns a pointer.
    [[nodiscard]] sol::ecs::Registry* registryFor(std::uint32_t system);
    [[nodiscard]] const sol::ecs::Registry* registryFor(std::uint32_t system) const;

    [[nodiscard]] std::uint32_t playerEntityIndex() const
    {
        return playerRegistry().storage<PlayerShip>().entityIndices()[0];
    }

    // ⚑⚑⚑ THERE IS DELIBERATELY NO ONE-ARGUMENT OVERLOAD, AND THAT IS STAGE
    // B'S WORKLIST. All sixteen call sites spell `playerRegistry()` out, so
    // `isPlayerEntity(playerRegistry(),` greps to exactly the set of places
    // that have to be re-read when the tick starts walking somebody else's
    // bubble - and a site inside a pool walk must then be handed THAT walk's
    // registry instead. A convenience overload would have made those sites
    // look already-answered.
    //
    // The seventeenth comparison is not in the list because it no longer
    // exists: `despawnSystem` kept `entityIndex != playerIndex` to decide what
    // to destroy, and dropping a bubble does not have to decide.

    // The flight input the commanded ship flies this tick, or the player's when
    // no command is running or the player has taken over; also arrives,
    // disengages and drops lost targets as a side effect.
    //
    // ⚑ Was autopilotInput(). The four guards it grew between Phase 8 and 8y —
    // cancel on manual deflection, the docked guard, the target-lost guard, and
    // the obstacle filter that excludes the destination's own sphere — are each
    // a shipped bug fix, so every one of them was generalised here rather than
    // rewritten per mode.
    [[nodiscard]] sol::sim::FlightInput commandInput();

    // The steering for one standing mode, split out only so commandInput's
    // guard sequence stays readable as a sequence.
    [[nodiscard]] sol::sim::FlightInput standingCommandInput(const TargetInfo& target);

    // ⚑⚑⚑⚑ ONE REGISTRY PER INSTANTIATED SYSTEM (Phase 38 stage A, amending
    // `decisions/015`, whose first bullet said "an entity gains a system
    // index"). The frame is a property of the REGISTRY, not a field on the
    // entity: a cross-system question is unaskable rather than merely wrong,
    // because the other system's entities are not in these pools at all.
    //
    // ⚑⚑⚑ WHY NOT A FIELD AND A FILTER. A filter is a thing you can forget,
    // and nothing notices — Phase 37 shipped two stages whose entire point was
    // invisible to every guard they wrote (361 of 361 green, then 366 of 366).
    // And `sim::resolveCollisions` is O(n^2) with no broadphase and says so in
    // its own header: one global body list with a frame filter is O((kn)^2)
    // where a registry per system is O(k*n^2) by construction.
    //
    // ⚑⚑ HELD BY POINTER SO REFERENCES STAY VALID. Stage B adds and drops
    // bubbles inside the tick, and `registryFor` hands out a reference; a
    // vector of values would rehome every live one on a reallocation.
    //
    // ⚑ STAGE A HAS EXACTLY ONE, ALWAYS. The plural is built and exercised
    // here — a jump creates the destination's bubble, migrates the player into
    // it and drops the old one — but nothing yet CREATES a second live bubble.
    // That is stage B, and it is what re-opens every pool walk below.
    struct SystemBubble
    {
        std::uint32_t system = kNoIndex;
        sol::ecs::Registry registry;
        // ⚑⚑⚑ THE STATICS ARE PART OF THE FRAME, NOT PART OF THE VIEW (Phase
        // 38 stage B). `rebuildAvoidance` and the collision build both push the
        // star and every planet as spheres, so a bubble ticked against the
        // PLAYER's star is a system whose ships dodge a sun that is not there
        // and fly through the one that is. Two systems both place their
        // contents around a barycentre origin, which is precisely why the
        // wrong answer looks plausible.
        //
        // ⚑ Only the two the tick reads. Gates, stations, signals, fields and
        // the target cycle stay player-scoped in `rebuildSystemSideData`,
        // because every one of them exists to be shown to somebody.
        CelestialBody star;
        std::vector<CelestialBody> planets;
        // The ships this system's sky was filled with, and the display names
        // the targeting list reads. Per bubble because an entity INDEX is per
        // registry: two bubbles both issue slot 7, and a world-scoped list
        // keyed on the index alone would hand one system's death the other
        // system's ship def. It is also what the death path looks a victim up
        // in, so a kill in a bubble the player is not in has to find it here.
        std::vector<SpawnedShip> spawnedShips;
        // ⚑⚑ A PER-SYSTEM STREAM, BECAUSE HOW MANY BUBBLES ARE INSTANTIATED IS
        // NOT A FACT ABOUT ANY OF THEM (Phase 38 stage B; Phase 37 stage B's
        // lesson exactly). `m_chunkRng` was one world-scoped stream, so an NPC
        // miner cutting rock in a second bubble displaced every draw the
        // player's system made after it — the sixteenth faction moving every
        // trader loss, one layer down. Seeded the way the generator already
        // seeds a system, at `:6090` and `:6206`.
        sol::core::Rng chunkRng{0x51ed'2701ull, 909};
        // ⚑⚑ THE LAW'S DISPATCH THROTTLE, PER JURISDICTION (Phase 38 stage B).
        // It was `m_responseCooldown`, one number for the whole galaxy, which
        // is the same thing while one system is instantiated and plainly wrong
        // once two are: a firefight in the bubble you left would have bought a
        // raider ten quiet seconds in the system you are standing in, from a
        // police force that had heard nothing. It ages in the per-system pilot
        // pass for the same reason `threatTimer` does.
        double responseCooldown = 0.0;
        // ⚑⚑⚑⚑ SIM-SECONDS THIS BUBBLE HAS LEFT BEFORE IT IS RELEASED (Phase
        // 38 stage C). Zero on the player's own bubble — that one is held open
        // by the player standing in it, not by a clock, and a jump back into a
        // retained bubble clears this rather than pausing it.
        //
        // Written once, at the moment the player jumps out of a system with a
        // live fight in it, and only ever counted DOWN. `kCoolingSeconds` is
        // where the reasoning lives, including why it is not re-evaluated.
        double holdSeconds = 0.0;
    };

    std::vector<std::unique_ptr<SystemBubble>> m_bubbles;
    // Scratch for syncTraderPuppets: which coarse traders already have a body.
    // A member so the per-tick reconcile does not allocate.
    std::vector<std::uint8_t> m_puppetPresent;
    // Per market, seconds an outpost's draw is stopped for because its miner
    // was killed (Phase 8x stage 6), and the scratch the reconcile counts
    // bodies in. Transient: a hold describes a ship that died in front of the
    // player, so it is dropped on a new game and never saved — the same rule
    // the puppets themselves follow.
    std::vector<double> m_minerHold;
    std::vector<std::uint8_t> m_minerPresent;
    // The field a miner is choosing its next rock out of, and the entities
    // those rocks belong to. Mutable scratch: choosing is a const question
    // about the world, asked at most once a minute per miner.
    mutable std::vector<sol::sim::MiningRock> m_minerRocks;
    mutable std::vector<std::uint32_t> m_minerRockEntities;
    // Cargo capacities of a faction's hauler roster, rebuilt per spawn so the
    // hull can be chosen against the load rather than against an index.
    std::vector<float> m_rosterCapacities;
    // Hull classes of the roster a wing is being drawn from (Phase 32 stage D),
    // in roster order, `kHullClassCount` where a hull declares none. Same
    // arrangement and same reason as the capacities above.
    std::vector<std::uint32_t> m_rosterClasses;
    // Scratch for pilotHuntTrader (Phase 8x): the haulers in the sky and the
    // hunter's hostility row. Members for the reconcile's reason - hunting is
    // a per-think decision and every raider in the system makes it.
    std::vector<sol::sim::PreyCandidate> m_preyCandidates;
    std::vector<std::uint8_t> m_preyHostile;
    sol::sim::FlightInput m_shipInput;    // player input latch, applied in tick
    sol::sim::FlightInput m_appliedInput; // what the ship flew last tick
    CommandMode m_commandMode = CommandMode::None;
    double m_autopilotRange = 1'500.0;                           // arrival standoff, meters (see engage)
    std::vector<sol::sim::AvoidanceSphere> m_autopilotObstacles; // per-tick scratch
    // Defaults for the parametrised commands, moved by the player and then
    // reused (phase decision 4). 5 km is the spec's own worked example for an
    // orbit; 1 km is a station-keeping distance that reads as "off the wing"
    // rather than "in the way".
    double m_orbitRange = 5'000.0;
    double m_keepDistanceRange = 1'000.0;
    // Where Hold was ordered. Hold is the one command with no target, so the
    // point it keeps has to be remembered when the order is given — reading the
    // ship's live position every tick would make it a no-op that drifts.
    sol::core::DVec3 m_holdPosition;
    // The offset MatchSpeed was ordered at, in the target's frame at that
    // moment. Same reasoning as m_holdPosition: "match speed" means keep the
    // geometry you had, so the geometry is captured on engage.
    sol::core::DVec3 m_matchOffset;
    ThrusterParticles m_thrusters;
    CombatEffects m_combatEffects;
    GameAudio* m_audio = nullptr; // borrowed; null when there is no device
    float m_playerDamageTimer = 0.0f;

    // Per-tick collision scratch + last tick's contacts (damage model input).
    std::vector<sol::sim::CollisionBody> m_collisionBodies;
    std::vector<std::uint32_t> m_collisionShipIndices;
    std::vector<sol::sim::Contact> m_contacts;

    // Fleet (Phase 8a): index 0 exists from generateUniverse on; m_defs is
    // the database applyDefs last saw (owned by GameContent, outlives us).
    std::vector<OwnedShip> m_fleet;
    std::size_t m_activeShip = 0;
    // Captains in the player's employ (Phase 39 stage A). Saved; the crew
    // hall's candidates are not, and are filtered against this.
    std::vector<Captain> m_captains;
    const sol::assets::DefDatabase* m_defs = nullptr;
    bool m_hardcore = false;
    bool m_hardcoreDeathPending = false;
    std::uint32_t m_startSystem = 0; // new-game system; hardcore respawn

    // Mining outposts eat real rock (Phase 8g). An extracting archetype's
    // output is drawn from the asteroid fields in its own system, through the
    // same MiningSim accounting the player's beam uses — so an outpost and
    // the player are competing over one finite thing. Installed into
    // Economy::tick as an abstract source, which is what keeps sim::Economy
    // from having to know sim::MiningSim exists.
    struct MiningFeedstock final : sol::sim::FeedstockSource
    {
        sol::sim::MiningSim* mining = nullptr;
        const sol::sim::Galaxy* galaxy = nullptr;
        const sol::sim::Economy* economy = nullptr;
        // Outposts whose miner was killed and has not been replaced yet
        // (Phase 8x stage 6), seconds remaining, indexed by market. Borrowed
        // from SpaceWorld: a hold is a fact about a ship that died in front of
        // the player, so it lives with the bodies and never reaches the save.
        const std::vector<double>* minerHold = nullptr;
        float draw(std::uint32_t market, std::uint32_t commodity, float units) override;
    };

    std::uint64_t m_universeSeed = 0;
    // What the authored half of this galaxy was made of (Phase 29 stage D).
    // Recomputed by every `generateUniverse`, never restored from a save: the
    // save carries the value it was WRITTEN with, and the comparison is the
    // whole point.
    std::uint64_t m_authoredDigest = 0;
    // Sim seconds since the run began; market intel is stamped against it, so
    // it has to survive a save like any other world state.
    double m_worldSeconds = 0.0;
    sol::sim::Galaxy m_galaxy;
    sol::sim::GalaxyParams m_galaxyParams; // kept for regeneration on load
    sol::sim::Economy m_economy;
    sol::sim::EconomyParams m_economyParams; // kept for re-init on load
    // What a `[[module]]` does, in def order, reduced to the same three rate
    // lists an archetype has plus its power figures (Phase 34 stage B). Cached
    // off the defs at `generateUniverse` so that `composeStations` needs no def
    // database - which is what lets `loadFrom` re-compose a regenerated galaxy.
    struct ModuleRuntime
    {
        std::vector<float> production;  // per commodity
        std::vector<float> consumption; // per commodity
        std::vector<float> feedstock;   // per commodity
        float powerOutput = 0.0f;
        float powerDraw = 0.0f;
        // The dock screens this module offers, as a bit per
        // `assets::StationScreen` (Phase 34 stage C). A mask rather than the
        // def's vector because a station's screen list is a UNION over its
        // modules, and a union of masks is an `|=` - which is the whole of the
        // filter gdd.md §12 asks for.
        std::uint32_t screens = 0;
        // The refining service (Phase 8f), resolved to commodity indices here
        // for the reason every other figure on this struct is: `composeStations`
        // and the screens that read it run with no def database in reach.
        // `kNoIndex` in both when the module offers no service.
        std::uint32_t refineInput = kNoIndex;
        std::uint32_t refineOutput = kNoIndex;
        // What this module can warehouse, per commodity (Phase 34 stage D).
        // Resolved from `ModuleStorage`'s goods class through
        // `m_commodityClass` at cache time, for the same reason every other
        // figure on this struct is: `composeStations` runs with no def
        // database in reach.
        std::vector<float> storage;
        // Is this one of gdd.md §7/§13's black-market services (Phase 34 stage
        // E)? The FAMILY, cached as a bool for the same reason `screens` is
        // cached as a mask: `assignShadowOwners` runs inside `composeStations`,
        // which has no def database in reach. A bool rather than the family
        // enum because exactly one family has a consequence here, and a stored
        // enum would invite a second reader to switch on it without the
        // resolution step that made it correct.
        bool shadow = false;
        // And whether it is a ROOM (Phase 35 stage C), for the same reason and
        // resolved the same way. A second bool rather than the family enum,
        // following the note above rather than reopening it: two families now
        // have a consequence here and neither of them is switched on.
        bool recreation = false;
    };
    // One line of a recipe with its module resolved to an index.
    struct RecipeEntry
    {
        std::uint32_t module = 0;
        float chance = 1.0f;
    };
    // What one composed station is made of. The ARCHETYPE is part of the
    // identity and not just provenance: two archetypes can roll the same module
    // list and still differ in what the rest of the game asks of them - whether
    // they extract, and how much they can hold - so sharing a row between them
    // would be a bug waiting for stage D.
    struct StationComposition
    {
        std::uint32_t archetype = 0;
        std::vector<std::uint32_t> modules; // def order indices, recipe order
    };
    std::vector<ModuleRuntime> m_modules;
    // Which goods class each commodity is, in commodity-index order (Phase 34
    // stage D). Cached beside the modules and for the same reason - it is read
    // per module per commodity while the holds are resolved, and by the fill
    // every frame the player is docked.
    //
    // ⚑ A commodity that declares no class is `Bulk` here, which is where that
    // default is APPLIED rather than merely documented; `CommodityDef` says
    // why. Bulk is the class that means "the warehouse", so a good nobody
    // classified is stocked wherever there is one.
    std::vector<sol::assets::GoodsClass> m_commodityClass;
    std::vector<std::vector<RecipeEntry>> m_recipes; // per station archetype
    std::vector<std::uint32_t> m_powerModules;       // by ascending output
    std::vector<StationComposition> m_compositions;
    // Who is in each room, indexed [system][station] - game-side rather than a
    // field on `sim::StationSpec`, because `sol::sim` has no business knowing
    // what a person is. Rebuilt by `composeStations`, never serialized.
    std::vector<std::vector<CastSeat>> m_cast;
    // The authored cast with every anchor resolved to an index once, in def
    // order (Phase 35 stage C). Cached for the reason `m_modules` and
    // `m_recipes` are: `composeStations` runs with no def database in reach.
    std::vector<CastEntry> m_castDefs;
    std::vector<std::string> m_castNames;  // parallel to m_castDefs
    std::vector<std::string> m_castTrades; // parallel to m_castDefs
    // What the player did, sparse and SAVED (v33). Everything else about the
    // cast is derived from the seed.
    std::vector<CastMemory> m_castMemory;
    // Where the composed rows start in `m_economyParams.archetypes`. Below it
    // are the authored archetypes, which are still what a `[[station]]` with no
    // recipe runs on.
    std::uint32_t m_baseArchetypeCount = 0;
    MiningFeedstock m_feedstock;
    std::vector<GameFaction> m_factionTable;           // majors + clans + shadow, sim order
    std::uint32_t m_shadowBase = sol::sim::kNoFaction; // first shadow row, or kNoFaction
    sol::sim::FactionSim m_factionSim;
    sol::sim::MissionSim m_missions;
    std::vector<sol::sim::MissionEvent> m_missionEvents; // buffered for Lua
    std::vector<sol::sim::MissionEvent> m_missionEventScratch;
    bool m_dockEventPending = false;
    std::vector<std::string> m_commodityIds; // economy index -> def id
    // Phase 19: economy index -> the model a chunk of that ore draws as.
    // Cached because a chunk is spawned on a CUT, long after the mining
    // entities were instantiated, and re-resolving per chunk would be a
    // string compare inside the weapon loop.
    std::vector<ModelId> m_chunkModels;
    // Phase 19: the active ship's interior, refreshed by applyActiveLoadout -
    // the one funnel every fleet switch, purchase and refit already goes
    // through. Cached because main.cpp asks for it once per frame in the seat
    // and the answer is two string scans.
    ModelId m_cockpitModel = kNoModel;
    double m_playerCredits = 1'000.0;
    std::vector<float> m_playerCargo; // per commodity
    float m_playerCargoCapacity = 50.0f;
    std::uint32_t m_currentSystem = 0;
    sol::core::DVec3 m_playerSpawn; // respawn point in the current system

    // Docking state; last dock is the death-rule respawn (system, station).
    std::uint32_t m_dockedStation = kNoIndex; // station index in current system
    std::uint32_t m_lastDockSystem = kNoIndex;
    std::uint32_t m_lastDockStation = kNoIndex;
    // Docking clearance (Phase 8r). Transient by design: dropped on a jump, on
    // docking, on expiry, and on load, so the save format is untouched.
    DockClearance m_clearance;
    // Which berth the player is parked in, or kNoIndex for the pre-8r pad
    // above the station (the dev shortcut and the death respawn both land
    // there, since neither asks a dispatcher for permission).
    std::uint32_t m_dockedBerth = kNoIndex;
    std::uint32_t m_pendingDockRequest = kNoIndex; // station awaiting an answer
    double m_dockRequestRoll = 0.0;                // the hook's only entropy
    std::uint32_t m_dockRequestCount = 0;          // so a re-hail can differ
    // Throttle for "you have no clearance" so sitting in an unassigned berth
    // says it once per approach rather than sixty times a second.
    double m_berthRefusalTimer = 0.0;
    std::vector<CommsMessage> m_comms;

    // Phase 36 stage A. Saved (v34): it is a decision the player made and a
    // reload that quietly re-lit it would be the game undoing a choice.
    bool m_transponderOn = true;

    // Pilot comms (Phase 8s). What a pilot has already said, so a second hail
    // repeats them instead of re-rolling. Keyed by the whole Entity — index AND
    // generation — because entity indices are reused when a ship dies, and a
    // fresh pilot inheriting a dead one's words is exactly the ghost this table
    // exists to prevent. Transient like the clearance: pilots do not survive a
    // jump, so this is cleared wherever the ship list is.
    struct HailMemory
    {
        sol::ecs::Entity pilot;
        std::string from;
        std::string text;
    };

    std::vector<HailMemory> m_hails;
    // Territory (Phase 8u): what the player has already been told about the
    // contest over their head, so an opening is announced once rather than
    // narrated tick by tick. Transient - a system change re-arms it, and it
    // is deliberately not serialized.
    std::uint32_t m_announcedContestSystem = kNoIndex;
    std::uint32_t m_announcedContestAttacker = kNoIndex;
    std::vector<sol::sim::ContestResolution> m_contestResolutions;
    // Attrition (Phase 8x): the losses drained this tick, and a session tally
    // for the console. Neither is serialized - the record of what was lost is
    // the fleet's own state, and these only exist to say it out loud.
    std::vector<sol::sim::TraderLoss> m_traderLossEvents;
    std::uint32_t m_traderLossCount = 0;
    // Traders the player shot themselves, since the last drain (Phase 8x §E).
    // The coarse record is told a hauler died and never by whom, so this is
    // the one bridge between the two, and it exists for exactly one decision:
    // whether an escort contract was failed or merely lost.
    std::vector<std::uint32_t> m_playerKilledTraders;
    HailRequest m_pendingHail;     // queued by hailTarget, drained by GameContent
    HailMemory m_answeringHail;    // who the three answers below are speaking as
    std::uint32_t m_hailCount = 0; // so a re-hail of a NEW pilot can differ
    // Death respawn into another system defers to end-of-tick: loadSystem
    // mid-tick would invalidate the pass scratch (collision slots, pools).
    std::uint32_t m_pendingRespawnSystem = kNoIndex;
    // The jump in flight (Phase 8v). Transient by design and never serialised:
    // it is cleared on load, because a save written between systems is not a
    // place the player can be put back.
    sol::sim::JumpTransition m_jump;

    // Exploration (Phase 8e). m_signals is rebuilt per system from the seed;
    // only SurveySim's state bits and ledger are saved.
    sol::sim::SurveySim m_survey;
    sol::sim::SurveyParams m_surveyParams;
    std::vector<SignalInstance> m_signals;

    // The dynamic target tail: what each slot past the statics stands for.
    struct DynamicTarget
    {
        NavKind kind = NavKind::Signal;
        std::uint32_t index = 0; // signal index, field index, or wreck id
    };

    std::vector<DynamicTarget> m_dynamicTargets;
    std::size_t m_signalTargetBase = 0;
    std::size_t m_planetTargetBase = 0;
    std::size_t m_starTargetIndex = 0;
    float m_scanRange = 6.0e7f; // from the active fit; see applyShipDef
    float m_scanSpeed = 1.0f;
    // ⚑ From the active fit too, and it rides with the scan stats rather than
    // with the phase-36 state on purpose: it is not a fact about a stop, it is
    // a fact about the ship, and it has to be refreshed everywhere the other
    // three are or a refit changes the readout and not the mechanic.
    float m_signature = 1.0f;
    double m_pulseCooldown = 0.0;
    float m_scanProgress = 0.0f;
    std::size_t m_scanTarget = 0; // target index the scan is running on
    bool m_scanActive = false;
    std::vector<SurveyEvent> m_surveyEvents;

    // Mining (Phase 8f). Fields and rocks regenerate from the system seed;
    // only depletion, wrecks, and refine jobs are saved (in MiningSim).
    sol::sim::MiningSim m_mining;
    sol::sim::MiningParams m_miningParams;
    std::vector<sol::sim::AsteroidFieldSpec> m_fields; // current system
    std::vector<WreckEvent> m_wreckEvents;
    std::vector<RockEvent> m_rockEvents;
    float m_collectorRange = 250.0f; // from the active fit; see applyShipDef
    float m_collectTicker = 0.0f;    // units collected, for the HUD readout
    double m_collectTickerAge = 0.0;
    std::string m_collectName;

    std::vector<GateInstance> m_gates;
    // Scratch: which gates a garrison posts to, ranked (Phase 36 stage B). A
    // member rather than a local for the same reason `m_rosterClasses` is one -
    // `spawnAmbientPilots` runs on every system load.
    std::vector<std::uint32_t> m_gatePostOrder;
    NoticeParams m_noticeParams;
    NoticeReport m_lastNotice;
    double m_noticeCooldown = 0.0;
    // Session state, not saved - the same footing `SystemBubble::responseCooldown` is on.
    // Seeded off the universe so two runs of the same galaxy roll alike.
    sol::core::Rng m_noticeRng{0x36'0b'11'ceull, 0x9e37'79b9'7f4a'7c15ull};
    // The stop (Phase 36 stage C). Transient, like `m_clearance` beside it and
    // for the same reason: a grant in flight belongs to a moment, not to a
    // save file - see the note on `InspectionHold`.
    InspectionHold m_inspection;
    InspectionParams m_inspectionParams;
    InspectionReport m_lastInspection;
    // The verdict (Phase 36 stage D). Transient for the same reason the hold
    // above it is: a ruling in flight lasts one frame, and the consequences it
    // hands out - credits, cargo, a bounty, standing - are all saved already by
    // whoever owns them.
    VerdictParams m_verdictParams;
    PendingVerdict m_pendingVerdict;
    bool m_hasPendingVerdict = false;
    // Which faction the answer applies to, latched while a ruling is pending
    // so the three answer functions cannot be aimed at somebody else by a hook
    // that calls one out of turn. kNoIndex means "no ruling is open", and all
    // three answers refuse on it.
    std::uint32_t m_verdictFaction = kNoIndex;
    // Throttles "you are not cleared to run" so a held finger on the cruise key
    // does not fill the panel. Same shape as `m_berthRefusalTimer`, which
    // exists because a refusal that only reaches the log is not a refusal.
    double m_holdRefusalTimer = 0.0;
    // ⚑ What a ship must not fly into (Phase 8y), rebuilt every tick from the
    // SAME pass and the same exclusions as m_collisionBodies — so the set you
    // avoid and the set you can hit cannot drift apart, which is the whole
    // point of the phase. Before it, this was a load-time list of stations and
    // celestials while the collision pass took rocks, wrecks and every ship
    // besides; 8v had already been caught once reading "nothing steers around
    // it" as "nothing stops you".
    //
    // Statics come first and movers after, so a caller that must not dodge
    // other ships takes the first m_avoidStatics entries and one that must
    // takes the lot. Handles are entity indices, which is how a ship skips
    // its own body.
    std::vector<sol::sim::AvoidanceSphere> m_avoidance;
    std::size_t m_avoidStatics = 0;
    // Throttle for the proximity warning, so a long approach says its piece
    // once rather than sixty times a second — m_berthRefusalTimer's rule.
    double m_cruiseWarningTimer = 0.0;
    std::vector<NavTarget> m_targets;
    std::size_t m_targetIndex = 0;
    // The slot each target class last held, so switching between them with
    // T and C resumes rather than restarting (Phase 8h). m_contactSlot is an
    // index into the player's ship list, not into the combined target space.
    std::size_t m_navSlot = 0;
    std::size_t m_contactSlot = 0;
    // Pushed in by the frame loop (Phase 8j); pure view state, never saved.
    ViewFrame m_viewFrame;
};

} // namespace game
