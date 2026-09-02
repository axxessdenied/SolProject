#pragma once

#include "scene_renderer.hpp"

#include "sol/core/math/math.hpp"
#include "sol/core/random.hpp"

#include <cstdint>
#include <vector>

namespace game {

// Combat feedback particles: impact sparks and destruction fireballs,
// rendered through the same additive billboard path as the thrusters.
// Cosmetic only - not part of the save state.
//
// ⚑⚑⚑⚑ AND IT BELONGS TO EXACTLY ONE FRAME, WHICH IS A THING THIS CLASS HAD
// TO BE TOLD (Phase 38 stage D). Every position in here is metres in one
// system's barycentre frame, and there is one buffer for the whole game - so
// once stage C left a system running behind the player, five per-system sites
// were pushing another star's sparks into the sky being drawn. The frame is a
// PARAMETER on the two spawn calls rather than a guard at those five sites,
// so a sixth site cannot be added without answering the question.
//
// ⚑⚑⚑ AND THE REASON IT IS CHECKED AT THE SPAWN RATHER THAN AT THE DRAW: a
// particle is a bare `DVec3` by the time `appendInstances` sees it, and
// `burst` draws six numbers out of `m_rng` for every one of them. `m_rng` is
// ONE stream for the whole world, so a fight the player cannot see would
// change what the player's own sparks look like - the determinism shape the
// phase's risk list names for `m_chunkRng` and `m_noticeRng`, live in the
// cosmetic layer since stage C, and in a class the risk list never mentions.
// Refusing before `burst` is what keeps the stream untouched by a bubble the
// player is not in.
//
// ⚑⚑ THE SECOND HALF, AND IT IS THE ONE A PLAYER WOULD ACTUALLY NOTICE:
// `kMaxParticles` is 2,000 for the whole game and `burst` silently drops the
// overflow. A foreign fight does not merely add sparks in the wrong place, it
// SPENDS the budget the player's own fight is drawn out of. `sounds.toml`'s
// `max_instances` is the same shape one output path over.
class CombatEffects
{
public:
    // Which system's frame this buffer is in. Set where the player's frame is
    // set, beside the listener's - see SpaceWorld::enterFrame.
    void setFrame(std::uint32_t system) { m_system = system; }

    [[nodiscard]] std::uint32_t frame() const { return m_system; }

    // shieldHit tints the sparks (shield energy blue vs hull-metal orange).
    // `system` is the frame `position` is in: an event in any other one is a
    // different star's, and is dropped before the random stream is touched.
    void spawnImpact(std::uint32_t system, const sol::core::DVec3& position, bool shieldHit);
    void spawnExplosion(std::uint32_t system, const sol::core::DVec3& position, float scale);

    void tick(double dt);
    void appendInstances(float alpha, std::vector<ParticleInstance>& out) const;

    // Drop everything in flight (system change: effects belong to the old one).
    void clear() { m_particles.clear(); }

    // Console readout (sol.bubbles): bursts refused because they happened in a
    // system the player is not standing in. A number that climbs while a
    // cooling bubble fights is this stage working; one that climbs with a
    // single system instantiated is a frame being got wrong.
    [[nodiscard]] std::uint64_t outOfFrameBursts() const { return m_outOfFrame; }

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

    void burst(const sol::core::DVec3& position,
               int count,
               float minSpeed,
               float maxSpeed,
               sol::core::Vec3 color,
               float size,
               float lifetime);

    static constexpr std::size_t kMaxParticles = 2'000;

    std::vector<Particle> m_particles;
    sol::core::Rng m_rng{0xB007'F1AEull, 5};
    // Starts at the system `SpaceWorld::spawn` opens its first bubble in,
    // which is 0 until a galaxy exists - exactly what `m_currentSystem` does.
    std::uint32_t m_system = 0;
    std::uint64_t m_outOfFrame = 0;
};

} // namespace game
