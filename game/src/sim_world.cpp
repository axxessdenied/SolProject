#include "sim_world.hpp"

#include "sol/core/random.hpp"
#include "sol/core/serialize.hpp"
#include "sol/ecs/snapshot.hpp"
#include "sol/platform/file_io.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace game {

using namespace sol;

namespace {

// Central harmonic pull a = -omega^2 * p gives every entity a closed elliptic
// orbit with period 2*pi/omega regardless of radius - stable forever under
// semi-implicit Euler at 60 Hz, and visibly in motion at screenshot scale.
constexpr double kOrbitOmega = 0.35;

// Stable component ids for the save format; never reuse or renumber.
ecs::Snapshot makeSnapshotSchema()
{
    ecs::Snapshot schema;
    schema.component<SimTransform>(1);
    schema.component<LinearVelocity>(2);
    schema.component<Spin>(3);
    return schema;
}

} // namespace

void SimWorld::spawn(std::uint32_t movingCount, std::uint64_t seed)
{
    core::Rng rng(seed, 0);

    // Central spinning "sun" marker.
    {
        const ecs::Entity sun = m_registry.create();
        m_registry.emplace<SimTransform>(sun);
        m_registry.emplace<LinearVelocity>(sun);
        m_registry.emplace<Spin>(
            sun, Spin{.axis = {0.0f, 1.0f, 0.0f}, .rate = 0.4f, .phase = 0.0f, .scale = 3.0f});
    }

    for (std::uint32_t i = 0; i < movingCount; ++i) {
        const double radius = 15.0 + 55.0 * rng.nextDouble01();
        const double angle = rng.nextDouble01() * 2.0 * core::kPiD;
        const double height = (rng.nextDouble01() - 0.5) * 16.0;
        const core::DVec3 position = {radius * std::cos(angle), height,
                                      radius * std::sin(angle)};

        // Tangential velocity for a near-circular orbit in the XZ plane; the
        // speed jitter turns circles into ellipses. Vertical motion is an
        // independent harmonic bob starting from its extreme (vy = 0).
        const double speed = kOrbitOmega * radius * (0.9 + 0.2 * rng.nextDouble01());
        const core::DVec3 velocity = {-std::sin(angle) * speed, 0.0, std::cos(angle) * speed};

        const ecs::Entity e = m_registry.create();
        m_registry.emplace<SimTransform>(
            e, SimTransform{.position = position, .previousPosition = position});
        m_registry.emplace<LinearVelocity>(e, LinearVelocity{.value = velocity});

        const core::Vec3 axis = core::normalize(core::Vec3{rng.rangeFloat(-1.0f, 1.0f),
                                                           rng.rangeFloat(-1.0f, 1.0f),
                                                           rng.rangeFloat(-1.0f, 1.0f)} +
                                                core::Vec3{0.0f, 0.01f, 0.0f});
        m_registry.emplace<Spin>(e, Spin{.axis = axis,
                                         .rate = rng.rangeFloat(0.3f, 2.5f),
                                         .phase = rng.rangeFloat(0.0f, core::kTwoPi),
                                         .scale = rng.rangeFloat(0.3f, 0.9f)});
    }
}

void SimWorld::tick(core::JobSystem& jobs, double dt)
{
    ecs::Pool<LinearVelocity>& velocities = m_registry.storage<LinearVelocity>();
    ecs::Pool<SimTransform>& transforms = m_registry.storage<SimTransform>();
    const std::uint32_t count = static_cast<std::uint32_t>(velocities.size());
    const std::uint32_t* entityIndices = velocities.entityIndices().data();
    LinearVelocity* velocity = velocities.values().data();

    jobs.parallelFor(count, 1024, [&](std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t i = begin; i < end; ++i) {
            SimTransform& transform = transforms.get(entityIndices[i]);
            transform.previousPosition = transform.position;
            velocity[i].value =
                velocity[i].value + transform.position * (-kOrbitOmega * kOrbitOmega * dt);
            transform.position = transform.position + velocity[i].value * dt;
        }
    });
}

void SimWorld::buildRenderInstances(core::JobSystem& jobs, float alpha, double simTimeSeconds,
                                    std::vector<RenderInstance>& out)
{
    ecs::Pool<Spin>& spins = m_registry.storage<Spin>();
    ecs::Pool<SimTransform>& transforms = m_registry.storage<SimTransform>();
    const std::uint32_t count = static_cast<std::uint32_t>(spins.size());
    const std::uint32_t* entityIndices = spins.entityIndices().data();
    const Spin* spin = spins.values().data();

    out.resize(count);
    RenderInstance* instances = out.data();
    const double alphaD = static_cast<double>(alpha);
    const float spinTime = static_cast<float>(simTimeSeconds);

    jobs.parallelFor(count, 1024, [&](std::uint32_t begin, std::uint32_t end) {
        for (std::uint32_t i = begin; i < end; ++i) {
            const SimTransform& transform = transforms.get(entityIndices[i]);
            const core::DVec3 position =
                transform.previousPosition +
                (transform.position - transform.previousPosition) * alphaD;
            const Spin& s = spin[i];
            instances[i] = RenderInstance{
                .position = position,
                .rotation = core::fromAxisAngle(s.axis, s.phase + s.rate * spinTime),
                .scale = {s.scale, s.scale, s.scale},
            };
        }
    });
}

bool SimWorld::saveTo(const char* path)
{
    core::BinaryWriter writer;
    makeSnapshotSchema().save(m_registry, writer);
    return platform::writeFileBytes(path, writer.data().data(), writer.size());
}

bool SimWorld::loadFrom(const char* path)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path, bytes)) {
        return false;
    }
    ecs::Registry fresh;
    core::BinaryReader reader(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (!makeSnapshotSchema().load(fresh, reader)) {
        return false;
    }
    m_registry = std::move(fresh);
    return true;
}

} // namespace game
