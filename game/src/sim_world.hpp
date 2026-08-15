#pragma once

#include "scene_renderer.hpp"

#include "sol/core/jobs.hpp"
#include "sol/core/math/math.hpp"
#include "sol/ecs/ecs.hpp"

#include <cstdint>
#include <vector>

namespace game {

// Phase 3 test world: a swarm of entities on harmonic orbits around the
// origin, simulated on the fixed timestep and rendered with interpolation.

struct SimTransform
{
    sol::core::DVec3 position;
    sol::core::DVec3 previousPosition;
};

struct LinearVelocity
{
    sol::core::DVec3 value;
};

struct Spin
{
    sol::core::Vec3 axis = {0.0f, 1.0f, 0.0f}; // unit length
    float rate = 0.0f;                         // radians per sim second
    float phase = 0.0f;
    float scale = 1.0f; // uniform render scale
};

class SimWorld
{
public:
    // Deterministic for a given seed: movingCount orbiters plus a central sun.
    void spawn(std::uint32_t movingCount, std::uint64_t seed);

    // One fixed sim tick: semi-implicit Euler under central harmonic pull.
    void tick(sol::core::JobSystem& jobs, double dt);

    // Blend previous->current sim state at alpha and derive rotations from
    // the (interpolated) sim time; fills one RenderInstance per entity.
    void buildRenderInstances(sol::core::JobSystem& jobs, float alpha, double simTimeSeconds,
                              std::vector<RenderInstance>& out);

    [[nodiscard]] std::uint32_t entityCount() const
    {
        return static_cast<std::uint32_t>(m_registry.aliveCount());
    }

private:
    sol::ecs::Registry m_registry;
};

} // namespace game
