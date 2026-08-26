#pragma once

#include "combat_effects.hpp"
#include "scene_renderer.hpp"
#include "thruster_particles.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/core/math/math.hpp"
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

#include <cstdint>
#include <span>
#include <string>
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

// The ship's mounted weapon, flattened from its def (POD for the snapshot).
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
    // What its bolt draws as (Phase 19), resolved once when the loadout is
    // applied. It lives here rather than being looked up at the muzzle
    // because this component is FLATTENED from its def and keeps no def id -
    // which is exactly why the bolt was a hardcoded "cube" until now.
    ModelId boltModel = kNoModel;
};

// A live bolt: Transform carries the position, this the rest.
struct Projectile
{
    sol::core::DVec3 velocity; // sim space, m/s
    double lifetime = 0.0;     // seconds remaining
    float damage = 0.0f;
    std::uint32_t shooterIndex = 0; // entity index; never hits its shooter
};

enum class PilotRole : std::uint32_t
{
    Fighter = 0,
    Trader,
    Patrol,
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
    bool pirate = false;
    float aggression = 0.5f;
    float forgiveness = 0.5f;
    std::vector<std::string> shipsPatrol;
    std::vector<std::string> shipsRaider;
    std::vector<std::string> shipsTrader; // Phase 8x: hulls its haulers fly
};

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
struct OwnedShip
{
    std::string defId;
    std::string weaponId;                      // fitted weapon; empty = unarmed mount
    std::vector<std::string> moduleIds;        // fitted modules (order irrelevant)
    std::vector<std::string> crewIds;          // hired crew aboard
    std::uint32_t storedSystem = 0xffff'ffffu; // active ship ignores these
    std::uint32_t storedStation = 0xffff'ffffu;
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
    void generateUniverse(const sol::assets::DefDatabase& defs);

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
    // game would - through its body if it has one here, so the wreck, the loot
    // and the standing hit are all real.
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

    // Hull + fitted modules + weapon at list price (crew hires excluded).
    [[nodiscard]] double shipValue(const OwnedShip& ship) const;

    [[nodiscard]] double insuranceDeductible() const { return kInsuranceRate * shipValue(activeShip()); }

    bool buyModule(const char* moduleId, std::string* outError = nullptr);
    bool sellModule(const char* moduleId, std::string* outError = nullptr);
    bool buyWeapon(const char* weaponId, std::string* outError = nullptr);
    bool buyShip(const char* shipDefId, std::string* outError = nullptr);
    bool sellShip(std::size_t fleetIndex, std::string* outError = nullptr);
    bool switchShip(std::size_t fleetIndex, std::string* outError = nullptr);
    bool hireCrew(const char* crewId, std::string* outError = nullptr);
    bool fireCrew(const char* crewId, std::string* outError = nullptr);

    // --- Factions & reputation (Phase 8b) ---
    static constexpr float kClanInitialStanding = -20.0f;   // dockable, wary
    static constexpr float kDefaultPirateRelation = -60.0f; // unspecified pairs

    [[nodiscard]] const std::vector<GameFaction>& factions() const { return m_factionTable; }

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

    // "hostile"/"neutral"/"friendly" for a faction table index; "" outside it.
    [[nodiscard]] const char* playerAttitudeName(std::uint32_t faction) const;
    // Whether the docked station's owner stocks a gated def (Phase 8a caveat
    // fix: catalogs are the owner faction's; pirates fence past min_rep).
    [[nodiscard]] bool stationSells(const sol::assets::CatalogGate& gate) const;
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
    // module into a free slot, credits always. A partial take leaves the rest.
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

    [[nodiscard]] std::uint32_t currentSystemIndex() const { return m_currentSystem; }

    [[nodiscard]] const char* currentSystemName() const
    {
        return m_currentSystem < m_galaxy.systems.size() ? m_galaxy.systems[m_currentSystem].name.c_str()
                                                         : "(void)";
    }

    void setShipInput(const sol::sim::FlightInput& input) { m_shipInput = input; }

    void tick(double dt);

    // --- Autopilot (playtest QoL): flies the ship to the selected nav target
    // and stops at the arrival range; any manual steering/thrust cancels it.
    // Engage fails while docked or with nothing targeted.
    bool engageAutopilot();

    void disengageAutopilot() { m_autopilotActive = false; }

    [[nodiscard]] bool autopilotActive() const { return m_autopilotActive; }

    [[nodiscard]] double autopilotArrivalRange() const { return m_autopilotRange; }

    void setAutopilotArrivalRange(double meters);

    // Interpolated blend of previous->current tick state at alpha.
    [[nodiscard]] Transform shipRenderTransform(float alpha) const;
    [[nodiscard]] sol::sim::ShipState shipState() const;

    [[nodiscard]] const sol::sim::ShipTuning& shipTuning() const
    {
        return m_registry.storage<ShipControl>().get(playerEntityIndex()).tuning;
    }

    // The input the ship actually flew last tick (autopilot's when engaged,
    // the player's otherwise) — what the HUD flags should reflect.
    [[nodiscard]] const sol::sim::FlightInput& shipInput() const { return m_appliedInput; }

    // Player pip triage (keys 1/2/3, 4 to balance).
    void playerAddPip(sol::sim::PowerSystem system);
    void playerBalancePips();

    [[nodiscard]] const sol::sim::PowerState& playerPower() const
    {
        return m_registry.storage<ShipPower>().get(playerEntityIndex()).state;
    }

    [[nodiscard]] const sol::sim::PowerTuning& powerTuning() const
    {
        return m_registry.storage<ShipPower>().get(playerEntityIndex()).tuning;
    }

    [[nodiscard]] const ShipDefense& playerDefense() const
    {
        return m_registry.storage<ShipDefense>().get(playerEntityIndex());
    }

    [[nodiscard]] const CelestialBody& sun() const { return m_star; }

    [[nodiscard]] std::span<const CelestialBody> planets() const { return m_planets; }

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

    [[nodiscard]] std::size_t contactCount() const { return m_spawnedShips.size(); }

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
    void setAudio(GameAudio* audio) { m_audio = audio; }

    // --- Picking (Phase 8j) ---
    // How the world is being viewed, for the click-to-select in target_pick.hpp.
    void setViewFrame(const ViewFrame& view) { m_viewFrame = view; }

    [[nodiscard]] const ViewFrame& viewFrame() const { return m_viewFrame; }

    // Selects the first live spawned ship whose display name contains
    // namePart (dev/console QoL; T-cycling is the player path).
    bool targetShipByName(const char* namePart);

    [[nodiscard]] const ShipWeapon& playerWeapon() const
    {
        return m_registry.storage<ShipWeapon>().get(playerEntityIndex());
    }

    // 1 right after the player takes a hit, decaying to 0 (HUD flash).
    [[nodiscard]] float playerDamageFlash() const
    {
        return m_playerDamageTimer > 0.0f ? m_playerDamageTimer / kDamageFlashSeconds : 0.0f;
    }

    // One instance per RenderShape entity, interpolated; ship excluded when
    // includeShip is false (first-person view).
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

    [[nodiscard]] std::uint32_t entityCount() const
    {
        return static_cast<std::uint32_t>(m_registry.aliveCount());
    }

    [[nodiscard]] bool saveTo(const char* path);
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
    [[nodiscard]] double shipHullFraction(sol::ecs::Entity entity) const;
    // Every hunter in the system and the hauler it is going for, for the
    // console.
    void hunterInfo(std::vector<HunterInfo>& out);

    // The playfield anchor Lua patrol offsets are relative to: the first
    // station of the current system, or the first nav target without one.
    [[nodiscard]] sol::core::DVec3 stationPosition() const
    {
        return m_targets.empty() ? sol::core::DVec3{} : m_targets[0].position;
    }

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

    // The parked-ship position for a station (clear of its collision sphere).
    [[nodiscard]] sol::core::DVec3 dockPoint(std::uint32_t stationIndex) const;

    // Instantiates systemIndex (statics + side data) and moves the player
    // there: at the gate arriving from fromSystem, or near the first station
    // when fromSystem is kNoFaction-tagged invalid (new game / load).
    void loadSystem(std::uint32_t systemIndex, std::uint32_t fromSystem);
    // Destroys every entity of the current system except the player.
    void despawnSystem();
    // ECS statics (stations, gates) for a system spec.
    void instantiateSystemEntities(const sol::sim::SystemSpec& spec);
    // Non-ECS state (celestials, nav targets, gate list) for a system spec;
    // used alone after a snapshot load, which already carries the statics.
    void rebuildSystemSideData(const sol::sim::SystemSpec& spec);

    // Records feedback for a damage result (sparks, player flash, explosion
    // on a kill is handled by handleShipDestroyed). `attackerIndex` is who
    // dealt it, or kNoIndex where nobody is to blame (a ram) - when it is the
    // player, the target's assist window is re-armed (Phase 8l).
    void noteDamage(std::uint32_t targetIndex,
                    const sol::core::DVec3& hitPosition,
                    const sol::sim::DamageResult& result,
                    std::uint32_t attackerIndex = kNoIndex);

    void applyShipDef(std::uint32_t entityIndex,
                      const sol::assets::ShipDef& def,
                      const sol::assets::DefDatabase& defs);
    // The ship's base def with its fit and crew resolved (Phase 8a); falls
    // back to the base def when ids have gone missing from the data.
    [[nodiscard]] sol::assets::ShipDef resolvedShipDef(const OwnedShip& ship) const;
    // Re-applies the active ship's resolved def to the player entity
    // (refit/switch/def-reload; resets defenses like any def application).
    void applyActiveLoadout();
    // Shared refusal path for outfitting mutations.
    bool refuse(const std::string& reason, std::string* outError) const;

    // A docked ship is inside the station and takes no damage — otherwise
    // hostiles camp the pad and re-kill on respawn (fatal under decisions/007
    // hardcore, where each death would delete the save).
    [[nodiscard]] bool isDamageImmune(std::uint32_t entityIndex) const
    {
        return entityIndex == playerEntityIndex() && isDocked();
    }

    // Resets the fleet to the single new-game starter ship.
    void resetFleetToStarter();
    void handleShipDestroyed(std::uint32_t entityIndex, std::uint32_t attackerIndex = kNoIndex);
    // Rebuilds the runtime faction table (majors + jittered clans) and
    // (re)initializes the FactionSim and MissionSim against the current
    // galaxy; called by generateUniverse and by loadFrom after a galaxy
    // regeneration.
    void initializeFactions();
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
    void instantiateMiningEntities();
    // Advances tumble, chunk drift, and proximity collection.
    void tickMining(double dt);
    // Cuts `units` out of a rock or wreck entity and spills what came off as
    // drifting chunks. Returns what actually came out.
    float cutRock(std::uint32_t entityIndex, float units);
    float cutWreck(std::uint32_t entityIndex, float units);
    // One chunk off a cut surface: it leaves toward whoever is cutting, with
    // a spread, so mining is gathering rather than chasing.
    void spawnCutChunk(const sol::core::DVec3& origin, double surface, std::uint32_t commodity, float units);
    void spawnOreChunk(const sol::core::DVec3& position,
                       const sol::core::DVec3& velocity,
                       std::uint32_t commodity,
                       float units);
    // Fits a salvaged module if it is legal on the active ship; used by both
    // site salvage and wreck cutting.
    bool tryFitSalvagedModule(const std::string& moduleId, std::string& outName);
    // The rock or wreck entity the boresight is on within `range`, or ~0u.
    [[nodiscard]] std::uint32_t entityAhead(double range, bool& outIsWreck) const;
    // Composes a wreck's contents from the victim's fit and cargo (the
    // scriptless default; the Lua hook may replace it before it is cut).
    [[nodiscard]] sol::sim::SignalLoot defaultWreckLoot(const sol::assets::ShipDef* def,
                                                        std::uint64_t seed) const;
    // The docked station archetype's refinery pair, resolved from the defs.
    bool dockedRefinePair(std::uint32_t& input, std::uint32_t& output) const;
    // Creates the mining component pools up front. Const storage<T>() asserts
    // the pool exists, and the read-only paths (the HUD's prospect readout)
    // run in systems that may hold no rock, no wreck, and no loose ore.
    void ensureMiningPools();
    // Pulse cooldown plus target-scan progress for the player's current
    // target; resolves the target when the scan completes.
    void tickScanning(double dt);
    // Clearance countdown, comms fade, and the arrival test that turns flying
    // into a berth into being docked (Phase 8r).
    void tickDocking(double dt);
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
    void spawnAmbientPilots(std::uint32_t systemIndex, const sol::sim::SystemSpec& spec);
    // Rebuilds m_avoidance for this tick from the same source the collision
    // bodies come from (Phase 8y). Runs before any steering, because a ship
    // that steers on last tick's picture of a moving fleet is steering at
    // where things were.
    void rebuildAvoidance();
    // Warns the player off an obstruction they are cruising at, and cuts the
    // drive when there is no longer room to stop (Phase 8y §D). Manual flight
    // only: autopilot slows itself, and sub-cruise flight is the player's own.
    void guardManualCruise(double dt);
    // Reconciles trader bodies with the coarse fleet (Phase 8x): a body for
    // every EconomyTrader flying an in-system leg here, and none for anyone
    // else. Runs after the economy tick, which is the moment the set goes
    // stale. The record decides who exists; this only draws the consequence.
    void syncTraderPuppets();
    // Reconciles miner bodies with the extractor stations here (Phase 8x stage
    // 6): a ship at the rock for every outpost in this system that is actually
    // drawing, and none for one that has stopped — because its warehouse is
    // full, because the rock ran out, or because the player shot its last
    // miner. Runs beside the trader reconcile for the same reason: the economy
    // tick is the one moment any of that can change.
    void syncMinerPuppets(double dt);
    // The rock a miner should be working: the nearest one holding what its
    // outpost digs on the first pick, then round the same field. Answers false
    // when there is nothing of that commodity left in the sky.
    [[nodiscard]] bool
    chooseMinerRock(MinerPuppet& miner, const sol::core::DVec3& from, bool sameField) const;
    // Where a miner should sit to work the rock it has picked (its hold point
    // off the surface), and where that rock is. False when the rock is gone.
    [[nodiscard]] bool
    minerWorkPoint(const MinerPuppet& miner, sol::core::DVec3& rock, sol::core::DVec3& hold) const;

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

    [[nodiscard]] bool traderLegSegment(std::uint32_t traderIndex, TraderLegPlacement& out) const;
    // Where the record says a trader is, on its schedule rather than its
    // engines. See keepTraderOnSchedule for why the two differ.
    [[nodiscard]] sol::core::DVec3 traderScheduledPoint(const TraderLegPlacement& leg) const;
    // True when the record moved the trader this tick rather than its engines,
    // which is what makes it uncatchable and so unhuntable.
    bool keepTraderOnSchedule(sol::ecs::Entity entity, const TraderLegPlacement& leg);
    // Removes a spawned ship with none of the death path's consequences.
    void despawnShip(std::uint32_t entityIndex);
    // Spawn at an explicit position (ambient wings); the public
    // spawnShipFromDef wraps this at a point ahead of the player.
    sol::ecs::Entity spawnShipAt(const sol::assets::ShipDef& def,
                                 const sol::assets::DefDatabase& defs,
                                 const sol::core::DVec3& position,
                                 const char* factionName);

    [[nodiscard]] std::uint32_t playerEntityIndex() const
    {
        return m_registry.storage<PlayerShip>().entityIndices()[0];
    }

    // The autopilot's flight input for this tick, or the player's when it is
    // off/cancelled; also arrives/disengages as a side effect.
    [[nodiscard]] sol::sim::FlightInput autopilotInput();

    sol::ecs::Registry m_registry;
    std::vector<SpawnedShip> m_spawnedShips;
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
    // Scratch for pilotHuntTrader (Phase 8x): the haulers in the sky and the
    // hunter's hostility row. Members for the reconcile's reason - hunting is
    // a per-think decision and every raider in the system makes it.
    std::vector<sol::sim::PreyCandidate> m_preyCandidates;
    std::vector<std::uint8_t> m_preyHostile;
    sol::sim::FlightInput m_shipInput;    // player input latch, applied in tick
    sol::sim::FlightInput m_appliedInput; // what the ship flew last tick
    bool m_autopilotActive = false;
    double m_autopilotRange = 1'500.0;                           // arrival standoff, meters (see engage)
    std::vector<sol::sim::AvoidanceSphere> m_autopilotObstacles; // per-tick scratch
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
    // Sim seconds since the run began; market intel is stamped against it, so
    // it has to survive a save like any other world state.
    double m_worldSeconds = 0.0;
    sol::sim::Galaxy m_galaxy;
    sol::sim::GalaxyParams m_galaxyParams; // kept for regeneration on load
    sol::sim::Economy m_economy;
    sol::sim::EconomyParams m_economyParams; // kept for re-init on load
    MiningFeedstock m_feedstock;
    std::vector<GameFaction> m_factionTable; // majors + clans, sim order
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
    sol::core::Rng m_chunkRng{0x51ed'2701ull, 909}; // chunk drift only; not saved

    CelestialBody m_star;
    std::vector<CelestialBody> m_planets;
    std::vector<GateInstance> m_gates;
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
    // index into m_spawnedShips, not into the combined target space.
    std::size_t m_navSlot = 0;
    std::size_t m_contactSlot = 0;
    // Pushed in by the frame loop (Phase 8j); pure view state, never saved.
    ViewFrame m_viewFrame;
};

} // namespace game
