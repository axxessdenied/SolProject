#pragma once

#include "combat_effects.hpp"
#include "scene_renderer.hpp"
#include "thruster_particles.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/core/math/math.hpp"
#include "sol/ecs/ecs.hpp"
#include "sol/sim/collision.hpp"
#include "sol/sim/damage.hpp"
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
};

// The player flies this def; mods can override it (Phase 5 data pipeline).
inline constexpr const char* kPlayerShipDefId = "sol.shuttle";

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

    // --- Docking (GDD: request -> autodock; manual flight optional later) ---
    // Docks at the nearest station within range: ship parks at the station's
    // dock point, flight input is ignored until undock, and the station
    // becomes the death-rule respawn point (last dock).
    [[nodiscard]] bool tryDockNearestStation(double range);
    [[nodiscard]] bool undock();
    [[nodiscard]] bool isDocked() const { return m_dockedStation != kNoIndex; }
    [[nodiscard]] const char* dockedStationName() const;
    // Distance to the nearest station, or a negative value with none.
    [[nodiscard]] double nearestStationDistance() const;

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

    // Interpolated blend of previous->current tick state at alpha.
    [[nodiscard]] Transform shipRenderTransform(float alpha) const;
    [[nodiscard]] sol::sim::ShipState shipState() const;
    [[nodiscard]] const sol::sim::ShipTuning& shipTuning() const
    {
        return m_registry.storage<ShipControl>().get(playerEntityIndex()).tuning;
    }
    [[nodiscard]] const sol::sim::FlightInput& shipInput() const { return m_shipInput; }

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
    // pilot_think assigns work).
    sol::ecs::Entity spawnPilotFromDef(const sol::assets::ShipDef& def,
                                       const sol::assets::DefDatabase& defs, PilotRole role);

    // --- Pilot commands (the Lua-facing strategy API, called via bindings) ---
    // All return false for a dead/pilotless entity.
    bool pilotAttackPlayer(sol::ecs::Entity entity);
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
    };
    void collectDuePilotThinks(double dt, std::vector<PilotThink>& out);

private:
    struct SpawnedShip
    {
        sol::ecs::Entity entity;
        std::string defId;
        std::string name; // display name for targeting
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
    void handleShipDestroyed(std::uint32_t entityIndex);
    [[nodiscard]] std::uint32_t playerEntityIndex() const
    {
        return m_registry.storage<PlayerShip>().entityIndices()[0];
    }
    sol::ecs::Registry m_registry;
    std::vector<SpawnedShip> m_spawnedShips;
    sol::sim::FlightInput m_shipInput; // player input latch, applied in tick
    ThrusterParticles m_thrusters;
    CombatEffects m_combatEffects;
    float m_playerDamageTimer = 0.0f;

    // Per-tick collision scratch + last tick's contacts (damage model input).
    std::vector<sol::sim::CollisionBody> m_collisionBodies;
    std::vector<std::uint32_t> m_collisionShipIndices;
    std::vector<sol::sim::Contact> m_contacts;

    std::uint64_t m_universeSeed = 0;
    sol::sim::Galaxy m_galaxy;
    sol::sim::GalaxyParams m_galaxyParams; // kept for regeneration on load
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
