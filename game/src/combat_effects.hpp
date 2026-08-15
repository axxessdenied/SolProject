#pragma once

#include "scene_renderer.hpp"

#include "sol/core/math/math.hpp"
#include "sol/core/random.hpp"

#include <vector>

namespace game {

// Combat feedback particles: impact sparks and destruction fireballs,
// rendered through the same additive billboard path as the thrusters.
// Cosmetic only - not part of the save state.
class CombatEffects
{
public:
    // shieldHit tints the sparks (shield energy blue vs hull-metal orange).
    void spawnImpact(const sol::core::DVec3& position, bool shieldHit);
    void spawnExplosion(const sol::core::DVec3& position, float scale);

    void tick(double dt);
    void appendInstances(float alpha, std::vector<ParticleInstance>& out) const;

private:
    struct Particle
    {
        sol::core::DVec3 position;
        sol::core::DVec3 previousPosition;
        sol::core::DVec3 velocity;
        sol::core::Vec3 color;
        float age = 0.0f;
        float lifetime = 1.0f;
        float size = 0.5f;
    };

    void burst(const sol::core::DVec3& position, int count, float minSpeed, float maxSpeed,
               sol::core::Vec3 color, float size, float lifetime);

    static constexpr std::size_t kMaxParticles = 2'000;

    std::vector<Particle> m_particles;
    sol::core::Rng m_rng{0xB007'F1AEull, 5};
};

} // namespace game
