#pragma once

#include "combat_effects.hpp"
#include "scene_renderer.hpp"
#include "thruster_particles.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/core/math/math.hpp"
#include "sol/ecs/ecs.hpp"
#include "sol/sim/collision.hpp"
#include "sol/sim/damage.hpp"
#include "sol/sim/economy.hpp"
#include "sol/sim/faction_sim.hpp"
#include "sol/sim/flight.hpp"
#include "sol/sim/mining.hpp"
#include "sol/sim/missions.hpp"
#include "sol/sim/power.hpp"
#include "sol/sim/steering.hpp"
#include "sol/sim/survey.hpp"
#include "sol/sim/universe.hpp"

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
    sol::core::DVec3 velocity;      // m/s, sim space
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
    // Faction table index (Phase 8b), or ~0u for unaffiliated console spawns
    // (which Lua treats as unconditionally player-hostile, the pre-8b rule).
    std::uint32_t factionIndex = 0xffff'ffffu;
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
};

struct RenderShape
{
    sol::core::Vec3 scale = {1.0f, 1.0f, 1.0f};
    ModelId model = ModelId::Cube;
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
    sol::core::DVec3 velocity;   // sim space, m/s
    double lifetime = 0.0;       // seconds remaining before it is lost
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
    std::string defId;      // the victim's ship def
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
    bool wreck = false;      // a hull to cut rather than a rock
    std::string name;        // commodity name, or the wreck's
    float unitsLeft = 0.0f;
    float unitsTotal = 0.0f;
    double distance = 0.0;
    bool inRange = false;    // inside the fitted weapon's reach
};

struct NavTarget
{
    std::string name;
    sol::core::DVec3 position;
    double surfaceRadius = 0.0; // 0 for point targets (station)
};

// Snapshot of the selected nav/combat target for HUD and weapons.
struct TargetInfo
{
    NavTarget nav;
    bool isShip = false;
    sol::core::DVec3 velocity;  // ships only
    float shieldFore = 0.0f;    // fractions, ships only
    float shieldAft = 0.0f;
    float hull = 0.0f;
    std::string factionName;    // empty for unaffiliated/static targets
    const char* attitude = "";  // player standing vs its faction (HUD tag)
};

// Physical extent of the built structures. Used as the NPC avoidance sphere
// and, since Phase 8j, as the click box: a thing the player can see filling
// the view has to be selectable where they see it, not only within a few
// pixels of the point its centre projects to. The station's drawn hull is
// 100 m and its sphere is a little past that; a gate's frame is 70 m.
inline constexpr double kStationRadiusMeters = 130.0;
inline constexpr double kGateRadiusMeters = 70.0;

// How the player is looking at the world this frame (Phase 8j). The frame loop
// owns the camera and the UI scale, and pushes both in here once per frame;
// the world never computes them. It is held rather than passed because a click
// is not the only thing that needs it — the console performs the identical
// pick, and both have to be answered against the same view or the verification
// is testing a different game than the one on screen.
struct ViewFrame
{
    sol::core::DVec3 cameraPosition;
    sol::core::Quat cameraOrientation;
    sol::core::Vec2 screenSize; // VIRTUAL UI pixels, i.e. after the UI scale
    float tanHalfFovY = 1.0f;
    bool valid = false; // false before the first frame, and while a menu is up
};

// The new-game starter ship def; mods can override it (Phase 5 data pipeline).
inline constexpr const char* kPlayerShipDefId = "sol.shuttle";

// One ship the player owns (Phase 8a outfitting): its def, fit, crew, and —
// unless it is the active ship — where it is stored.
struct OwnedShip
{
    std::string defId;
    std::string weaponId;               // fitted weapon; empty = unarmed mount
    std::vector<std::string> moduleIds; // fitted modules (order irrelevant)
    std::vector<std::string> crewIds;   // hired crew aboard
    std::uint32_t storedSystem = 0xffff'ffffu;  // active ship ignores these
    std::uint32_t storedStation = 0xffff'ffffu;
};

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

    // Jumps through the nearest gate within activationRange meters of the
    // player: despawns this system, instantiates the destination, and places
    // the player at the arrival gate. Returns false if no gate is in range.
    [[nodiscard]] bool jumpNearestGate(double activationRange);

    // Distance to the nearest gate, or a negative value with no gates.
    [[nodiscard]] double nearestGateDistance() const;

    // Jumps via this system's gate to the named destination system,
    // regardless of distance (dev/console path; the J key stays
    // range-gated). Returns false if no gate leads there.
    [[nodiscard]] bool jumpToSystem(const char* destinationName);

    // --- Docking (GDD: request -> autodock; manual flight optional later) ---
    // Docks at the nearest station within range: ship parks at the station's
    // dock point, flight input is ignored until undock, and the station
    // becomes the death-rule respawn point (last dock).
    [[nodiscard]] bool tryDockNearestStation(double range);
    [[nodiscard]] bool undock();
    [[nodiscard]] bool isDocked() const { return m_dockedStation != kNoIndex; }
    [[nodiscard]] const char* dockedStationName() const;
    // Station index in the current system, or ~0u while flying.
    [[nodiscard]] std::uint32_t dockedStationIndex() const { return m_dockedStation; }
    // Distance to the nearest station, or a negative value with none.
    [[nodiscard]] double nearestStationDistance() const;

    // --- Trading (Phase 7 economy; player trades ride the same markets the
    // NPC agents move) ---
    [[nodiscard]] const sol::sim::Economy& economy() const { return m_economy; }
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
    [[nodiscard]] double insuranceDeductible() const
    {
        return kInsuranceRate * shipValue(activeShip());
    }

    bool buyModule(const char* moduleId, std::string* outError = nullptr);
    bool sellModule(const char* moduleId, std::string* outError = nullptr);
    bool buyWeapon(const char* weaponId, std::string* outError = nullptr);
    bool buyShip(const char* shipDefId, std::string* outError = nullptr);
    bool sellShip(std::size_t fleetIndex, std::string* outError = nullptr);
    bool switchShip(std::size_t fleetIndex, std::string* outError = nullptr);
    bool hireCrew(const char* crewId, std::string* outError = nullptr);
    bool fireCrew(const char* crewId, std::string* outError = nullptr);

    // --- Factions & reputation (Phase 8b) ---
    static constexpr float kClanInitialStanding = -20.0f; // dockable, wary
    static constexpr float kDefaultPirateRelation = -60.0f; // unspecified pairs

    [[nodiscard]] const std::vector<GameFaction>& factions() const { return m_factionTable; }
    [[nodiscard]] const sol::sim::FactionSim& factionSim() const { return m_factionSim; }
    [[nodiscard]] sol::sim::FactionSim& factionSim() { return m_factionSim; }
    // The faction holding a system (and its stations), or an out-of-table
    // value for ownerless systems (factionCount == 0 galaxies).
    [[nodiscard]] std::uint32_t systemOwnerFaction(std::uint32_t systemIndex) const
    {
        return systemIndex < m_galaxy.systems.size()
                   ? m_galaxy.systems[systemIndex].factionIndex
                   : kNoIndex;
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
    static constexpr double kChunkSpread = 0.45; // how far off "toward you"
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
    bool bestKnownPrice(std::uint32_t commodity, std::uint32_t* outSystem, float* outPrice,
                        double* outAge, bool* outStale) const;
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
    [[nodiscard]] std::uint32_t currentSystemIndex() const { return m_currentSystem; }
    [[nodiscard]] const char* currentSystemName() const
    {
        return m_currentSystem < m_galaxy.systems.size()
                   ? m_galaxy.systems[m_currentSystem].name.c_str()
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
    };

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
    void cycleNavTarget();
    void cycleContact();
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
    // objective is not a FlyTo in this system (so nothing is there to select).
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
    sol::ecs::Entity spawnShipFromDef(const sol::assets::ShipDef& def,
                                      const sol::assets::DefDatabase& defs);

    // As above, plus an AI pilot in the given role (starts Idle until Lua's
    // pilot_think assigns work). factionIndex tags the pilot's allegiance;
    // ~0u spawns the pre-8b unaffiliated kind.
    sol::ecs::Entity spawnPilotFromDef(const sol::assets::ShipDef& def,
                                       const sol::assets::DefDatabase& defs, PilotRole role,
                                       std::uint32_t factionIndex = kNoIndex);

    // --- Pilot commands (the Lua-facing strategy API, called via bindings) ---
    // All return false for a dead/pilotless entity.
    bool pilotAttackPlayer(sol::ecs::Entity entity);
    // Attacks the nearest enemy of the pilot's faction: another pilot whose
    // faction it is at war with, or the player when hostile (and not docked).
    // False (and no state change) with nothing hostile in sensor range.
    bool pilotEngageEnemy(sol::ecs::Entity entity);
    bool pilotFlee(sol::ecs::Entity entity);
    bool pilotIdle(sol::ecs::Entity entity);
    bool pilotPatrolTo(sol::ecs::Entity entity, sol::core::DVec3 waypoint);
    [[nodiscard]] double shipHullFraction(sol::ecs::Entity entity) const;
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
    };
    void collectDuePilotThinks(double dt, std::vector<PilotThink>& out);

private:
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
    // on a kill is handled by handleShipDestroyed).
    void noteDamage(std::uint32_t targetIndex, const sol::core::DVec3& hitPosition,
                    const sol::sim::DamageResult& result);

    void applyShipDef(std::uint32_t entityIndex, const sol::assets::ShipDef& def,
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
    void handleShipDestroyed(std::uint32_t entityIndex,
                             std::uint32_t attackerIndex = kNoIndex);
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
    // True when the tracked mission's current objective is a FlyTo in this
    // system — the one case that has a position and nothing else to draw it.
    // Dock/Deliver name a station that is already a nav target and Kill has no
    // position at all, so both are answered by the HUD line instead.
    [[nodiscard]] bool objectiveDestination(const sol::sim::MissionObjective** out) const;
    // Rebuilds the tail only when the objective slot's presence or position no
    // longer matches the tracked mission. Called each tick; costs a pointer
    // chase and a vector compare when nothing has changed.
    void syncObjectiveTarget();
    // How far short of a target autopilot parks. Shared by the engage message
    // and the steering itself, so what it announces is what it does.
    [[nodiscard]] double autopilotArrivalRange(const TargetInfo& target) const;
    // Mining layout for the current galaxy (sized like the economy's), and
    // the ore table read out of the commodity defs.
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
    void spawnCutChunk(const sol::core::DVec3& origin, double surface, std::uint32_t commodity,
                       float units);
    void spawnOreChunk(const sol::core::DVec3& position, const sol::core::DVec3& velocity,
                       std::uint32_t commodity, float units);
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
    // Spawn at an explicit position (ambient wings); the public
    // spawnShipFromDef wraps this at a point ahead of the player.
    sol::ecs::Entity spawnShipAt(const sol::assets::ShipDef& def,
                                 const sol::assets::DefDatabase& defs,
                                 const sol::core::DVec3& position, const char* factionName);
    [[nodiscard]] std::uint32_t playerEntityIndex() const
    {
        return m_registry.storage<PlayerShip>().entityIndices()[0];
    }
    // The autopilot's flight input for this tick, or the player's when it is
    // off/cancelled; also arrives/disengages as a side effect.
    [[nodiscard]] sol::sim::FlightInput autopilotInput();

    sol::ecs::Registry m_registry;
    std::vector<SpawnedShip> m_spawnedShips;
    sol::sim::FlightInput m_shipInput;    // player input latch, applied in tick
    sol::sim::FlightInput m_appliedInput; // what the ship flew last tick
    bool m_autopilotActive = false;
    double m_autopilotRange = 1'500.0; // arrival standoff, meters (see engage)
    std::vector<sol::sim::AvoidanceSphere> m_autopilotObstacles; // per-tick scratch
    ThrusterParticles m_thrusters;
    CombatEffects m_combatEffects;
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
    double m_playerCredits = 1'000.0;
    std::vector<float> m_playerCargo; // per commodity
    float m_playerCargoCapacity = 50.0f;
    std::uint32_t m_currentSystem = 0;
    sol::core::DVec3 m_playerSpawn; // respawn point in the current system

    // Docking state; last dock is the death-rule respawn (system, station).
    std::uint32_t m_dockedStation = kNoIndex; // station index in current system
    std::uint32_t m_lastDockSystem = kNoIndex;
    std::uint32_t m_lastDockStation = kNoIndex;
    // Death respawn into another system defers to end-of-tick: loadSystem
    // mid-tick would invalidate the pass scratch (collision slots, pools).
    std::uint32_t m_pendingRespawnSystem = kNoIndex;

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
    std::size_t m_scanTarget = 0;      // target index the scan is running on
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
    std::vector<sol::sim::AvoidanceSphere> m_obstacles; // stations + celestials
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
