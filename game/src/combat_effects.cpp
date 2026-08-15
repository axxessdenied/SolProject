#include "combat_effects.hpp"

namespace game {

using namespace sol;

void CombatEffects::burst(const core::DVec3& position, int count, float minSpeed, float maxSpeed,
                          core::Vec3 color, float size, float lifetime)
{
    for (int i = 0; i < count && m_particles.size() < kMaxParticles; ++i) {
        // Uniform-ish direction from a cube sample; rejection would be
        // overkill for sparks.
        core::Vec3 direction = {m_rng.rangeFloat(-1.0f, 1.0f), m_rng.rangeFloat(-1.0f, 1.0f),
                                m_rng.rangeFloat(-1.0f, 1.0f)};
        if (lengthSquared(direction) < 1.0e-4f) {
            direction = {0.0f, 1.0f, 0.0f};
        }
        direction = normalize(direction);

        Particle particle;
        particle.position = position;
        particle.previousPosition = position;
        particle.velocity =
            core::toDVec3(direction * m_rng.rangeFloat(minSpeed, maxSpeed));
        particle.color = color * m_rng.rangeFloat(0.7f, 1.3f);
        particle.lifetime = lifetime * m_rng.rangeFloat(0.6f, 1.4f);
        particle.size = size * m_rng.rangeFloat(0.7f, 1.4f);
        m_particles.push_back(particle);
    }
}

// Sized to read at combat ranges (~250-800 m), not just up close.
void CombatEffects::spawnImpact(const core::DVec3& position, bool shieldHit)
{
    const core::Vec3 color =
        shieldHit ? core::Vec3{0.7f, 1.4f, 2.6f} : core::Vec3{2.6f, 1.1f, 0.35f};
    burst(position, 14, 8.0f, 45.0f, color, 1.5f, 0.35f);
}

void CombatEffects::spawnExplosion(const core::DVec3& position, float scale)
{
    // Hot white core, orange fireball, lingering embers.
    burst(position, 30, 2.0f, 20.0f * scale, {3.0f, 2.6f, 2.2f}, 6.5f * scale, 0.5f);
    burst(position, 80, 10.0f, 70.0f * scale, {2.8f, 1.2f, 0.3f}, 4.5f * scale, 0.9f);
    burst(position, 40, 20.0f, 100.0f * scale, {1.8f, 0.6f, 0.15f}, 2.4f * scale, 1.5f);
}

void CombatEffects::tick(double dt)
{
    const float dtf = static_cast<float>(dt);
    std::size_t alive = 0;
    for (Particle& particle : m_particles) {
        particle.age += dtf;
        if (particle.age >= particle.lifetime) {
            continue;
        }
        particle.previousPosition = particle.position;
        particle.position += particle.velocity * dt;
        m_particles[alive++] = particle;
    }
    m_particles.resize(alive);
}

void CombatEffects::appendInstances(float alpha, std::vector<ParticleInstance>& out) const
{
    const double alphaD = static_cast<double>(alpha);
    out.reserve(out.size() + m_particles.size());
    for (const Particle& particle : m_particles) {
        const float life = particle.age / particle.lifetime;
        const float fade = (1.0f - life) * (1.0f - life);
        out.push_back(ParticleInstance{
            .position = particle.previousPosition +
                        (particle.position - particle.previousPosition) * alphaD,
            .size = particle.size * (1.0f + life * 2.2f),
            .color = {particle.color.x, particle.color.y, particle.color.z, fade},
        });
    }
}

} // namespace game
