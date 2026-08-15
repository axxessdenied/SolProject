#pragma once

#include "scene_renderer.hpp"

#include "sol/core/random.hpp"
#include "sol/sim/flight.hpp"

#include <vector>

namespace game {

// CPU thruster exhaust tied to what the flight model actually applies
// (shipLinearAccelBody), so assist counter-thrust puffs too. Ticked on the
// fixed sim step; cosmetic only - not part of the save state.
class ThrusterParticles
{
public:
    void tick(const sol::sim::ShipState& ship, const sol::sim::ShipTuning& tuning,
              const sol::sim::FlightInput& input, double dt);

    void buildInstances(float alpha, std::vector<ParticleInstance>& out) const;

private:
    struct Particle
    {
        sol::core::DVec3 position;
        sol::core::DVec3 previousPosition;
        sol::core::DVec3 velocity;
        sol::core::Vec3 color; // linear HDR core color
        float age = 0.0f;
        float lifetime = 1.0f;
        float size = 0.5f;
    };

    void emit(const sol::sim::ShipState& ship, sol::core::Vec3 nozzleBody,
              sol::core::Vec3 exhaustBody, sol::core::Vec3 color, float size, float lifetime);

    static constexpr std::size_t kMaxParticles = 1500;

    std::vector<Particle> m_particles;
    sol::core::Rng m_rng{0x501CAFEull, 3};
    double m_emitCarry[4] = {}; // main, reverse, lateral, vertical
};

} // namespace game
