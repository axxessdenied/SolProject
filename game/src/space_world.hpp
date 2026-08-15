#pragma once

#include "scene_renderer.hpp"
#include "thruster_particles.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/core/math/math.hpp"
#include "sol/ecs/ecs.hpp"
#include "sol/sim/collision.hpp"
#include "sol/sim/flight.hpp"
#include "sol/sim/power.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace game {

// Phase 4 vertical-slice world: one star system in double-precision sim
// space — sun at the origin, a planet ~0.67 AU out, a station 150,000 km
// sunward of it, and the player ship. Positions are meters (large-world rule:
// DVec3 in sim, camera-relative floats only at render time).

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

// Per-ship power state (decisions/003: Elite-style pips). ENG allocation
// scales the flight envelope each tick; WEP feeds the weapon capacitor.
struct ShipPower
{
    sol::sim::PowerState state;
};

struct RenderShape
{
    sol::core::Vec3 scale = {1.0f, 1.0f, 1.0f};
    ModelId model = ModelId::Cube;
};

// Non-entity scenery: rendered as impostors, referenced as nav targets.
struct CelestialBody
{
    const char* name = "";
    sol::core::DVec3 position;
    double radius = 0.0; // meters
};

struct NavTarget
{
    const char* name = "";
    sol::core::DVec3 position;
    double surfaceRadius = 0.0; // 0 for point targets (station)
};

// The player flies this def; mods can override it (Phase 5 data pipeline).
inline constexpr const char* kPlayerShipDefId = "sol.shuttle";

class SpaceWorld
{
public:
    // Spawns with hardcoded defaults; GameContent::initialize applies the
    // data-driven tuning/visuals right after via applyDefs.
    void spawn();

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
    [[nodiscard]] const sol::sim::PowerTuning& powerTuning() const;

    [[nodiscard]] const CelestialBody& sun() const { return m_sun; }
    [[nodiscard]] const CelestialBody& planet() const { return m_planet; }

    // Cycling nav targets for the provisional HUD (station, planet, sun).
    [[nodiscard]] const NavTarget& currentTarget() const { return m_targets[m_targetIndex]; }
    void cycleTarget() { m_targetIndex = (m_targetIndex + 1) % m_targets.size(); }

    // One instance per RenderShape entity, interpolated; ship excluded when
    // includeShip is false (first-person view).
    void buildRenderInstances(float alpha, bool includeShip, std::vector<RenderInstance>& out) const;

    void buildParticleInstances(float alpha, std::vector<ParticleInstance>& out) const
    {
        m_thrusters.buildInstances(alpha, out);
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

    // Spawns a def-driven ship just ahead of the player (no FlightBody until
    // Phase 6 gives NPCs pilots). Returns the new entity.
    sol::ecs::Entity spawnShipFromDef(const sol::assets::ShipDef& def);

private:
    struct SpawnedShip
    {
        sol::ecs::Entity entity;
        std::string defId;
    };

    void applyShipDef(std::uint32_t entityIndex, const sol::assets::ShipDef& def);
    [[nodiscard]] std::uint32_t playerEntityIndex() const
    {
        return m_registry.storage<PlayerShip>().entityIndices()[0];
    }
    sol::ecs::Registry m_registry;
    std::vector<SpawnedShip> m_spawnedShips;
    sol::sim::FlightInput m_shipInput; // player input latch, applied in tick
    ThrusterParticles m_thrusters;

    // Per-tick collision scratch + last tick's contacts (damage model input).
    std::vector<sol::sim::CollisionBody> m_collisionBodies;
    std::vector<std::uint32_t> m_collisionShipIndices;
    std::vector<sol::sim::Contact> m_contacts;

    CelestialBody m_sun;
    CelestialBody m_planet;
    std::vector<NavTarget> m_targets;
    std::size_t m_targetIndex = 0;
};

} // namespace game
