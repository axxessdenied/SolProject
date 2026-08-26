#include "thruster_particles.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace sol;

namespace {

constexpr float kExhaustSpeed = 40.0f; // m/s relative to the ship
constexpr float kMainRate = 260.0f;    // particles/s at full thrust
constexpr float kRcsRate = 90.0f;

} // namespace

void ThrusterParticles::emit(const sim::ShipState& ship,
                             core::Vec3 nozzleBody,
                             core::Vec3 exhaustBody,
                             core::Vec3 color,
                             float size,
                             float lifetime)
{
    if (m_particles.size() >= kMaxParticles) {
        return;
    }
    const core::Vec3 jitter = {
        m_rng.rangeFloat(-0.2f, 0.2f), m_rng.rangeFloat(-0.2f, 0.2f), m_rng.rangeFloat(-0.2f, 0.2f)};
    const core::Vec3 exhaustWorld = rotate(ship.orientation, exhaustBody + jitter * kExhaustSpeed);

    Particle particle;
    particle.position = ship.position + core::toDVec3(rotate(ship.orientation, nozzleBody));
    particle.previousPosition = particle.position;
    particle.velocity = ship.velocity + core::toDVec3(exhaustWorld);
    particle.color = color;
    particle.lifetime = lifetime * m_rng.rangeFloat(0.7f, 1.3f);
    particle.size = size * m_rng.rangeFloat(0.8f, 1.25f);
    m_particles.push_back(particle);
}

void ThrusterParticles::tick(const sim::ShipState& ship,
                             const sim::ShipTuning& tuning,
                             const sim::FlightInput& input,
                             double dt)
{
    // Age and advect.
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

    // Emission mirrors the applied body-space acceleration, so the assist's
    // counter-thrust fires the correct opposing nozzles.
    const core::Vec3 accel = sim::shipLinearAccelBody(ship, tuning, input, dt);

    struct Channel
    {
        float amount; // fraction of nominal full thrust, may exceed 1 on boost
        float rate;
        core::Vec3 nozzleA;
        core::Vec3 nozzleB;
        core::Vec3 exhaust; // body space, unit-ish
        core::Vec3 color;
        float size;
        float lifetime;
    };

    const Channel channels[4] = {
        // Main drive (forward accel = -z): twin nozzles at the stern.
        {std::max(-accel.z, 0.0f) / tuning.forwardAccel,
         kMainRate,
         {-1.6f, -0.1f, 5.1f},
         {1.6f, -0.1f, 5.1f},
         {0.0f, 0.0f, 1.0f},
         {2.6f, 1.5f, 0.55f},
         0.35f,
         0.7f},
        // Retro (reverse accel = +z): nose thrusters, exhaust forward.
        {std::max(accel.z, 0.0f) / tuning.reverseAccel,
         kRcsRate,
         {-1.0f, 0.0f, -4.5f},
         {1.0f, 0.0f, -4.5f},
         {0.0f, 0.0f, -1.0f},
         {1.1f, 1.2f, 1.5f},
         0.3f,
         0.4f},
        // Lateral RCS: exhaust opposes the acceleration.
        {std::abs(accel.x) / tuning.lateralAccel,
         kRcsRate,
         {accel.x > 0.0f ? -2.8f : 2.8f, 0.0f, 1.0f},
         {accel.x > 0.0f ? -2.8f : 2.8f, 0.0f, 3.0f},
         {accel.x > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f},
         {1.1f, 1.2f, 1.5f},
         0.3f,
         0.4f},
        // Vertical RCS.
        {std::abs(accel.y) / tuning.verticalAccel,
         kRcsRate,
         {-1.2f, accel.y > 0.0f ? -1.4f : 1.6f, 2.0f},
         {1.2f, accel.y > 0.0f ? -1.4f : 1.6f, 2.0f},
         {0.0f, accel.y > 0.0f ? -1.0f : 1.0f, 0.0f},
         {1.1f, 1.2f, 1.5f},
         0.3f,
         0.4f},
    };

    for (int channel = 0; channel < 4; ++channel) {
        const Channel& c = channels[channel];
        const float amount = core::clamp(c.amount, 0.0f, 3.0f);
        if (amount < 0.02f) {
            m_emitCarry[channel] = 0.0;
            continue;
        }
        m_emitCarry[channel] += static_cast<double>(c.rate * amount) * dt;
        while (m_emitCarry[channel] >= 1.0) {
            m_emitCarry[channel] -= 1.0;
            const core::Vec3 nozzle = m_rng.rangeFloat(0.0f, 1.0f) < 0.5f ? c.nozzleA : c.nozzleB;
            emit(ship, nozzle, c.exhaust * kExhaustSpeed, c.color, c.size, c.lifetime);
        }
    }
}

void ThrusterParticles::buildInstances(float alpha, std::vector<ParticleInstance>& out) const
{
    const double alphaD = static_cast<double>(alpha);
    out.clear();
    out.reserve(m_particles.size());
    for (const Particle& particle : m_particles) {
        const float life = particle.age / particle.lifetime;
        const float fade = (1.0f - life) * (1.0f - life);
        out.push_back(ParticleInstance{
            .position = particle.previousPosition + (particle.position - particle.previousPosition) * alphaD,
            .size = particle.size * (1.0f + life * 1.8f),
            .color = {particle.color.x, particle.color.y, particle.color.z, fade},
        });
    }
}

} // namespace game
