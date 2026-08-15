#pragma once

#include "scene_renderer.hpp"

#include "sol/core/math/math.hpp"
#include "sol/ecs/ecs.hpp"
#include "sol/sim/flight.hpp"

#include <cstdint>
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

struct RenderShape
{
    sol::core::Vec3 scale = {1.0f, 1.0f, 1.0f};
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

class SpaceWorld
{
public:
    void spawn();

    void setShipInput(const sol::sim::FlightInput& input) { m_shipInput = input; }
    void tick(double dt);

    // Interpolated blend of previous->current tick state at alpha.
    [[nodiscard]] Transform shipRenderTransform(float alpha) const;
    [[nodiscard]] sol::sim::ShipState shipState() const;
    [[nodiscard]] const sol::sim::ShipTuning& shipTuning() const { return m_tuning; }
    [[nodiscard]] const sol::sim::FlightInput& shipInput() const { return m_shipInput; }

    [[nodiscard]] const CelestialBody& sun() const { return m_sun; }
    [[nodiscard]] const CelestialBody& planet() const { return m_planet; }

    // Cycling nav targets for the provisional HUD (station, planet, sun).
    [[nodiscard]] const NavTarget& currentTarget() const { return m_targets[m_targetIndex]; }
    void cycleTarget() { m_targetIndex = (m_targetIndex + 1) % m_targets.size(); }

    // One instance per RenderShape entity, interpolated; ship excluded when
    // includeShip is false (first-person view).
    void buildRenderInstances(float alpha, bool includeShip, std::vector<RenderInstance>& out) const;

    [[nodiscard]] std::uint32_t entityCount() const
    {
        return static_cast<std::uint32_t>(m_registry.aliveCount());
    }

    [[nodiscard]] bool saveTo(const char* path);
    [[nodiscard]] bool loadFrom(const char* path);

private:
    sol::ecs::Registry m_registry;
    sol::ecs::Entity m_ship = {};
    sol::sim::ShipTuning m_tuning;
    sol::sim::FlightInput m_shipInput;

    CelestialBody m_sun;
    CelestialBody m_planet;
    std::vector<NavTarget> m_targets;
    std::size_t m_targetIndex = 0;
};

} // namespace game
