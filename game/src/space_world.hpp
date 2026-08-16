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
#include "sol/sim/power.hpp"
#include "sol/sim/steering.hpp"
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

    // Cycling targets: the static nav points (station, planet, sun) then
    // every live pilot ship (combat targets with shield/hull readouts).
    [[nodiscard]] TargetInfo currentTargetInfo() const;
    void cycleTarget();
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
    // (re)initializes the FactionSim against the current galaxy; called by
    // generateUniverse and by loadFrom after a galaxy regeneration.
    void initializeFactions();
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

    std::uint64_t m_universeSeed = 0;
    sol::sim::Galaxy m_galaxy;
    sol::sim::GalaxyParams m_galaxyParams; // kept for regeneration on load
    sol::sim::Economy m_economy;
    sol::sim::EconomyParams m_economyParams; // kept for re-init on load
    std::vector<GameFaction> m_factionTable; // majors + clans, sim order
    sol::sim::FactionSim m_factionSim;
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

    CelestialBody m_star;
    std::vector<CelestialBody> m_planets;
    std::vector<GateInstance> m_gates;
    std::vector<sol::sim::AvoidanceSphere> m_obstacles; // stations + celestials
    std::vector<NavTarget> m_targets;
    std::size_t m_targetIndex = 0;
};

} // namespace game
