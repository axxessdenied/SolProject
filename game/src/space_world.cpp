#include "space_world.hpp"

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

// System layout (meters, sim space, origin = sun).
constexpr core::DVec3 kPlanetPosition = {0.0, 0.0, -1.0e11}; // ~0.67 AU
constexpr double kPlanetRadius = 6.371e6;
constexpr core::DVec3 kStationPosition = {0.0, 0.0, kPlanetPosition.z + 1.5e8}; // 150,000 km sunward
constexpr double kSunRadius = 6.96e8;

// Stable component ids for the save format; never reuse or renumber. Ids 1-3
// belonged to the retired Phase 3 swarm-world format.
ecs::Snapshot makeSnapshotSchema()
{
    ecs::Snapshot schema;
    schema.component<Transform>(10);
    schema.component<FlightBody>(11);
    schema.component<RenderShape>(12);
    return schema;
}

} // namespace

void SpaceWorld::spawn()
{
    m_sun = {.name = "Sol", .position = {}, .radius = kSunRadius};
    m_planet = {.name = "Aster", .position = kPlanetPosition, .radius = kPlanetRadius};
    m_targets = {
        {.name = "Aster Gateway", .position = kStationPosition, .surfaceRadius = 0.0},
        {.name = "Aster", .position = kPlanetPosition, .surfaceRadius = kPlanetRadius},
        {.name = "Sol", .position = {}, .surfaceRadius = kSunRadius},
    };
    m_targetIndex = 0;

    auto addStatic = [&](core::DVec3 position, core::Quat orientation, core::Vec3 scale) {
        const ecs::Entity e = m_registry.create();
        m_registry.emplace<Transform>(e, Transform{.position = position,
                                                   .previousPosition = position,
                                                   .orientation = orientation,
                                                   .previousOrientation = orientation});
        m_registry.emplace<RenderShape>(e, RenderShape{.scale = scale});
    };

    // Station "Aster Gateway": a compound of cubes until a cooked station
    // mesh lands — core, habitat ring, spokes, panels.
    {
        const core::DVec3 s = kStationPosition;
        addStatic(s, core::Quat::identity(), {30.0f, 30.0f, 30.0f});
        constexpr int kRingSegments = 8;
        for (int i = 0; i < kRingSegments; ++i) {
            const float angle = static_cast<float>(i) * (core::kTwoPi / kRingSegments);
            const double radius = 95.0;
            const core::DVec3 offset = {radius * std::cos(angle), 0.0, radius * std::sin(angle)};
            const core::Quat facing = core::fromAxisAngle({0.0f, 1.0f, 0.0f}, -angle);
            addStatic(s + offset, facing, {14.0f, 10.0f, 30.0f});
        }
        for (int i = 0; i < 4; ++i) {
            const float angle = static_cast<float>(i) * (core::kTwoPi / 4) + core::kPi / 4.0f;
            const core::Quat facing = core::fromAxisAngle({0.0f, 1.0f, 0.0f}, -angle);
            const core::DVec3 offset = {45.0 * std::cos(angle), 0.0, 45.0 * std::sin(angle)};
            addStatic(s + offset, facing, {4.0f, 4.0f, 42.0f});
        }
        addStatic(s + core::DVec3{0.0, 70.0, 0.0}, core::Quat::identity(), {60.0f, 2.0f, 22.0f});
        addStatic(s + core::DVec3{0.0, -70.0, 0.0}, core::Quat::identity(), {60.0f, 2.0f, 22.0f});
    }

    // Waypoint cubes marching toward the planet, then an arrival cluster just
    // above the surface: precision canaries at both ends of the flight.
    for (const double kilometers : {2.0, 5.0, 10.0, 25.0, 50.0}) {
        addStatic(kStationPosition + core::DVec3{120.0, 40.0, -kilometers * 1000.0},
                  core::Quat::identity(), {12.0f, 12.0f, 12.0f});
    }
    for (int i = 0; i < 3; ++i) {
        const core::DVec3 arrival =
            kPlanetPosition + core::DVec3{i * 400.0, 0.0, kPlanetRadius + 2.0e5};
        addStatic(arrival, core::Quat::identity(), {40.0f, 40.0f, 40.0f});
    }

    // The player ship, 800 m sunward of the station, nose (-Z) toward it.
    {
        const core::DVec3 start = kStationPosition + core::DVec3{0.0, 0.0, 800.0};
        const ecs::Entity e = m_registry.create();
        m_registry.emplace<Transform>(
            e, Transform{.position = start, .previousPosition = start});
        m_registry.emplace<FlightBody>(e);
        m_registry.emplace<RenderShape>(e, RenderShape{.scale = {3.0f, 1.5f, 6.0f}});
    }
}

sim::ShipState SpaceWorld::shipState() const
{
    const ecs::Pool<FlightBody>& bodies = m_registry.storage<FlightBody>();
    const std::uint32_t shipIndex = bodies.entityIndices()[0];
    const Transform& transform = m_registry.storage<Transform>().get(shipIndex);
    const FlightBody& body = bodies.values()[0];
    return {
        .position = transform.position,
        .velocity = body.velocity,
        .orientation = transform.orientation,
        .angularVelocity = body.angularVelocity,
    };
}

void SpaceWorld::tick(double dt)
{
    ecs::Pool<FlightBody>& bodies = m_registry.storage<FlightBody>();
    const std::uint32_t shipIndex = bodies.entityIndices()[0];
    Transform& transform = m_registry.storage<Transform>().get(shipIndex);
    FlightBody& body = bodies.values()[0];

    transform.previousPosition = transform.position;
    transform.previousOrientation = transform.orientation;

    sim::ShipState state = {
        .position = transform.position,
        .velocity = body.velocity,
        .orientation = transform.orientation,
        .angularVelocity = body.angularVelocity,
    };
    sim::stepShipFlight(state, m_tuning, m_shipInput, dt);

    transform.position = state.position;
    transform.orientation = state.orientation;
    body.velocity = state.velocity;
    body.angularVelocity = state.angularVelocity;
}

Transform SpaceWorld::shipRenderTransform(float alpha) const
{
    const ecs::Pool<FlightBody>& bodies = m_registry.storage<FlightBody>();
    const std::uint32_t shipIndex = bodies.entityIndices()[0];
    const Transform& transform = m_registry.storage<Transform>().get(shipIndex);

    Transform blended = transform;
    blended.position = transform.previousPosition +
                       (transform.position - transform.previousPosition) * static_cast<double>(alpha);
    blended.orientation = nlerp(transform.previousOrientation, transform.orientation, alpha);
    return blended;
}

void SpaceWorld::buildRenderInstances(float alpha, bool includeShip,
                                      std::vector<RenderInstance>& out) const
{
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    const ecs::Pool<FlightBody>& bodies = m_registry.storage<FlightBody>();
    const std::uint32_t shipIndex = bodies.entityIndices()[0];

    const std::uint32_t count = static_cast<std::uint32_t>(shapes.size());
    const std::uint32_t* entityIndices = shapes.entityIndices().data();
    const RenderShape* shape = shapes.values().data();
    const double alphaD = static_cast<double>(alpha);

    out.clear();
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!includeShip && entityIndices[i] == shipIndex) {
            continue;
        }
        const Transform& transform = transforms.get(entityIndices[i]);
        out.push_back(RenderInstance{
            .position = transform.previousPosition +
                        (transform.position - transform.previousPosition) * alphaD,
            .rotation = nlerp(transform.previousOrientation, transform.orientation, alpha),
            .scale = shape[i].scale,
        });
    }
}

bool SpaceWorld::saveTo(const char* path)
{
    core::BinaryWriter writer;
    makeSnapshotSchema().save(m_registry, writer);
    return platform::writeFileBytes(path, writer.data().data(), writer.size());
}

bool SpaceWorld::loadFrom(const char* path)
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
    if (fresh.storage<FlightBody>().size() != 1) {
        return false; // not a slice-world save
    }
    m_registry = std::move(fresh);
    return true;
}

} // namespace game
