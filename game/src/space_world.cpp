#include "space_world.hpp"

#include "sol/core/log.hpp"
#include "sol/core/serialize.hpp"
#include "sol/sim/collision.hpp"
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

// Impact damage: k * v^2 (50 m/s ram = 25 damage); scrapes below ~10 m/s
// are ignored so docking bumps stay free.
constexpr double kImpactDamageFactor = 0.01;
constexpr double kImpactDamageMinimum = 1.0;

constexpr core::DVec3 kPlayerStart = kStationPosition + core::DVec3{0.0, 0.0, 800.0};

// Bounding-sphere radii per model at scale 1 (meters); collision radius =
// base * RenderShape scale. Rough spheres are fine for Phase 6 combat.
constexpr double kCollisionRestitution = 0.15;
[[nodiscard]] double modelBaseRadius(ModelId model)
{
    switch (model) {
    case ModelId::Cube: return 1.0;
    case ModelId::Station: return 100.0;
    case ModelId::Ship: return 8.0;
    }
    return 8.0;
}

// Stable component ids for the save format; never reuse or renumber. Ids 1-3
// belonged to the retired Phase 3 swarm-world format.
ecs::Snapshot makeSnapshotSchema()
{
    ecs::Snapshot schema;
    schema.component<Transform>(10);
    schema.component<FlightBody>(11);
    schema.component<RenderShape>(12);
    schema.component<PlayerShip>(13);
    schema.component<ShipControl>(14);
    schema.component<ShipPower>(15);
    schema.component<ShipDefense>(16);
    return schema;
}

ModelId modelIdFromName(const std::string& name)
{
    if (name == "cube") {
        return ModelId::Cube;
    }
    if (name == "station") {
        return ModelId::Station;
    }
    if (name != "ship") {
        SOL_LOG_WARN("unknown model '%s' in ship def; using 'ship'", name.c_str());
    }
    return ModelId::Ship;
}

sim::ShipTuning toShipTuning(const assets::ShipFlightTuning& flight)
{
    return {
        .forwardAccel = flight.forwardAccel,
        .reverseAccel = flight.reverseAccel,
        .lateralAccel = flight.lateralAccel,
        .verticalAccel = flight.verticalAccel,
        .maxSpeed = flight.maxSpeed,
        .maxTurnRate = {flight.maxTurnRate[0], flight.maxTurnRate[1], flight.maxTurnRate[2]},
        .angularAccel = {flight.angularAccel[0], flight.angularAccel[1], flight.angularAccel[2]},
        .boostAccelScale = flight.boostAccelScale,
        .boostSpeedScale = flight.boostSpeedScale,
        .cruiseSpeedScale = flight.cruiseSpeedScale,
        .cruiseAccelScale = flight.cruiseAccelScale,
    };
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

    auto addStatic = [&](core::DVec3 position, core::Quat orientation, core::Vec3 scale,
                         ModelId model) {
        const ecs::Entity e = m_registry.create();
        m_registry.emplace<Transform>(e, Transform{.position = position,
                                                   .previousPosition = position,
                                                   .orientation = orientation,
                                                   .previousOrientation = orientation});
        m_registry.emplace<RenderShape>(e, RenderShape{.scale = scale, .model = model});
    };

    // Station "Aster Gateway" (mesh authored in meters, ~200 m across).
    addStatic(kStationPosition, core::Quat::identity(), {1.0f, 1.0f, 1.0f}, ModelId::Station);

    // Waypoint cubes marching toward the planet, then an arrival cluster just
    // above the surface: precision canaries at both ends of the flight.
    for (const double kilometers : {2.0, 5.0, 10.0, 25.0, 50.0}) {
        addStatic(kStationPosition + core::DVec3{120.0, 40.0, -kilometers * 1000.0},
                  core::Quat::identity(), {12.0f, 12.0f, 12.0f}, ModelId::Cube);
    }
    for (int i = 0; i < 3; ++i) {
        const core::DVec3 arrival =
            kPlanetPosition + core::DVec3{i * 400.0, 0.0, kPlanetRadius + 2.0e5};
        addStatic(arrival, core::Quat::identity(), {40.0f, 40.0f, 40.0f}, ModelId::Cube);
    }

    // The player ship, 800 m sunward of the station, nose (-Z) toward it.
    {
        const ecs::Entity e = m_registry.create();
        m_registry.emplace<Transform>(
            e, Transform{.position = kPlayerStart, .previousPosition = kPlayerStart});
        m_registry.emplace<FlightBody>(e);
        m_registry.emplace<RenderShape>(e, RenderShape{.model = ModelId::Ship});
        m_registry.emplace<PlayerShip>(e);
        m_registry.emplace<ShipControl>(e);
        m_registry.emplace<ShipPower>(e);
        m_registry.emplace<ShipDefense>(e);
    }
}

void SpaceWorld::playerAddPip(sim::PowerSystem system)
{
    ShipPower& power = m_registry.storage<ShipPower>().get(playerEntityIndex());
    sim::addPip(power.state.pips, system, power.tuning);
}

void SpaceWorld::playerBalancePips()
{
    ShipPower& power = m_registry.storage<ShipPower>().get(playerEntityIndex());
    sim::balancePips(power.state.pips, power.tuning);
}

void SpaceWorld::applyDefs(const assets::DefDatabase& defs)
{
    const assets::ShipDef* playerDef = defs.findShip(kPlayerShipDefId);
    if (playerDef != nullptr) {
        applyShipDef(playerEntityIndex(), *playerDef);
    } else {
        SOL_LOG_WARN("player ship def '%s' missing; keeping current tuning", kPlayerShipDefId);
    }

    for (const SpawnedShip& spawned : m_spawnedShips) {
        if (const assets::ShipDef* def = defs.findShip(spawned.defId.c_str())) {
            applyShipDef(spawned.entity.index, *def);
        }
    }
}

void SpaceWorld::applyShipDef(std::uint32_t entityIndex, const assets::ShipDef& def)
{
    RenderShape& shape = m_registry.storage<RenderShape>().get(entityIndex);
    shape.scale = {def.scale, def.scale, def.scale};
    shape.model = modelIdFromName(def.model);
    m_registry.storage<ShipControl>().get(entityIndex).tuning = toShipTuning(def.flight);

    ShipPower& power = m_registry.storage<ShipPower>().get(entityIndex);
    power.tuning.weaponCapacitor = def.power.weaponCapacitor;
    power.tuning.weaponRechargeRate = def.power.weaponRecharge;
    if (power.state.weaponCharge > power.tuning.weaponCapacitor) {
        power.state.weaponCharge = power.tuning.weaponCapacitor;
    }

    // Def edits (and hot-reloads) refit the defenses at full strength.
    ShipDefense& defense = m_registry.storage<ShipDefense>().get(entityIndex);
    defense.tuning = sim::DefenseTuning{.shieldStrength = def.defense.shieldStrength,
                                        .shieldRegenRate = def.defense.shieldRegen,
                                        .shieldRegenDelay = def.defense.shieldRegenDelay,
                                        .armor = def.defense.armor,
                                        .hull = def.defense.hull};
    sim::resetDefense(defense.state, defense.tuning);
}

ecs::Entity SpaceWorld::spawnShipFromDef(const assets::ShipDef& def)
{
    const sim::ShipState player = shipState();
    const core::Vec3 forward = rotate(player.orientation, core::Vec3{0.0f, 0.0f, -1.0f});
    const double distance = 150.0 + 100.0 * static_cast<double>(def.scale);
    const core::DVec3 position =
        player.position + core::DVec3{forward.x * distance, forward.y * distance,
                                      forward.z * distance};

    const ecs::Entity e = m_registry.create();
    m_registry.emplace<Transform>(e, Transform{.position = position,
                                               .previousPosition = position,
                                               .orientation = player.orientation,
                                               .previousOrientation = player.orientation});
    m_registry.emplace<RenderShape>(e, RenderShape{});
    m_registry.emplace<FlightBody>(e);
    // Default input is assist-on with zero commands = station-keeping until a
    // pilot (Phase 6 AI) writes real commands.
    m_registry.emplace<ShipControl>(e);
    m_registry.emplace<ShipPower>(e);
    m_registry.emplace<ShipDefense>(e);
    applyShipDef(e.index, def);
    m_spawnedShips.push_back({.entity = e, .defId = def.id});
    return e;
}

sim::ShipState SpaceWorld::shipState() const
{
    const std::uint32_t shipIndex = playerEntityIndex();
    const Transform& transform = m_registry.storage<Transform>().get(shipIndex);
    const FlightBody& body = m_registry.storage<FlightBody>().get(shipIndex);
    return {
        .position = transform.position,
        .velocity = body.velocity,
        .orientation = transform.orientation,
        .angularVelocity = body.angularVelocity,
    };
}

void SpaceWorld::tick(double dt)
{
    const std::uint32_t playerIndex = playerEntityIndex();
    m_registry.storage<ShipControl>().get(playerIndex).input = m_shipInput;

    // Step every flying ship with its own tuning and commanded input (NPC
    // input is written by pilots — zero/station-keeping until Phase 6 AI).
    // ENG pips scale the flight envelope; WEP pips recharge the capacitor;
    // SYS pips scale shield regen.
    ecs::Pool<ShipPower>& powers = m_registry.storage<ShipPower>();
    ecs::Pool<ShipDefense>& defenses = m_registry.storage<ShipDefense>();
    m_registry.view<Transform, FlightBody, ShipControl>().each(
        [&](ecs::Entity entity, Transform& transform, FlightBody& body, ShipControl& control) {
            transform.previousPosition = transform.position;
            transform.previousOrientation = transform.orientation;

            sim::ShipTuning tuning = control.tuning;
            if (ShipPower* power = powers.tryGet(entity.index)) {
                sim::stepPower(power->state, power->tuning, dt);
                tuning = sim::applyEnginePips(control.tuning, power->state.pips, power->tuning);
            }
            if (ShipDefense* defense = defenses.tryGet(entity.index)) {
                const ShipPower* power = powers.tryGet(entity.index);
                const float regenScale =
                    power != nullptr
                        ? sim::shieldRegenScale(power->state.pips, power->tuning)
                        : 1.0f;
                sim::stepDefense(defense->state, defense->tuning, regenScale, dt);
            }

            sim::ShipState state = {
                .position = transform.position,
                .velocity = body.velocity,
                .orientation = transform.orientation,
                .angularVelocity = body.angularVelocity,
            };
            sim::stepShipFlight(state, tuning, control.input, dt);

            transform.position = state.position;
            transform.orientation = state.orientation;
            body.velocity = state.velocity;
            body.angularVelocity = state.angularVelocity;
        });

    // Collision pass: ships (movers) vs each other, scenery, and celestials.
    // Swept spheres, so cruise speeds cannot tunnel through the planet.
    m_collisionBodies.clear();
    m_collisionShipIndices.clear();

    // Ships first, so body slot i corresponds to m_collisionShipIndices[i];
    // statics (scenery without FlightBody, then celestials) follow.
    const ecs::Pool<FlightBody>& bodies = m_registry.storage<FlightBody>();
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::uint32_t entityIndex = shapes.entityIndices()[i];
        if (!bodies.contains(entityIndex)) {
            continue;
        }
        const RenderShape& shape = shapes.values()[i];
        const Transform& transform = transforms.get(entityIndex);
        const double scale = static_cast<double>(shape.scale.x);
        m_collisionShipIndices.push_back(entityIndex);
        m_collisionBodies.push_back({
            .previousPosition = transform.previousPosition,
            .position = transform.position,
            .velocity = bodies.get(entityIndex).velocity,
            .radius = modelBaseRadius(shape.model) * scale,
            .inverseMass = 1.0 / (scale * scale * scale), // mass ~ volume
        });
    }
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::uint32_t entityIndex = shapes.entityIndices()[i];
        if (bodies.contains(entityIndex)) {
            continue;
        }
        const RenderShape& shape = shapes.values()[i];
        const Transform& transform = transforms.get(entityIndex);
        m_collisionBodies.push_back(
            {.previousPosition = transform.position,
             .position = transform.position,
             .velocity = {},
             .radius = modelBaseRadius(shape.model) * static_cast<double>(shape.scale.x),
             .inverseMass = 0.0});
    }
    for (const CelestialBody* celestial : {&m_sun, &m_planet}) {
        m_collisionBodies.push_back({.previousPosition = celestial->position,
                                     .position = celestial->position,
                                     .velocity = {},
                                     .radius = celestial->radius,
                                     .inverseMass = 0.0});
    }

    m_contacts.clear();
    sim::resolveCollisions(m_collisionBodies, kCollisionRestitution, m_contacts);

    for (std::size_t i = 0; i < m_collisionShipIndices.size(); ++i) {
        const sim::CollisionBody& body = m_collisionBodies[i];
        const std::uint32_t entityIndex = m_collisionShipIndices[i];
        m_registry.storage<Transform>().get(entityIndex).position = body.position;
        m_registry.storage<FlightBody>().get(entityIndex).velocity = body.velocity;
    }

    // Impact damage (k*v^2) through the facing the hit arrived on.
    const std::size_t shipCount = m_collisionShipIndices.size();
    std::vector<std::uint32_t> destroyedShips;
    auto applyImpact = [&](std::uint32_t bodySlot, core::DVec3 toSource, double impactSpeed) {
        if (bodySlot >= shipCount) {
            return;
        }
        const double damage = kImpactDamageFactor * impactSpeed * impactSpeed;
        if (damage < kImpactDamageMinimum) {
            return;
        }
        const std::uint32_t entityIndex = m_collisionShipIndices[bodySlot];
        ShipDefense* defense = defenses.tryGet(entityIndex);
        if (defense == nullptr || !defense->state.alive()) {
            return;
        }
        const core::Quat orientation =
            m_registry.storage<Transform>().get(entityIndex).orientation;
        const sim::ShieldFacing facing = sim::facingForHit(orientation, toSource);
        const sim::DamageResult result = sim::applyDamage(
            defense->state, defense->tuning, facing, static_cast<float>(damage));
        if (result.destroyed) {
            destroyedShips.push_back(entityIndex);
        }
    };
    for (const sim::Contact& contact : m_contacts) {
        applyImpact(contact.bodyA, -contact.normal, contact.impactSpeed);
        applyImpact(contact.bodyB, contact.normal, contact.impactSpeed);
    }
    for (const std::uint32_t entityIndex : destroyedShips) {
        handleShipDestroyed(entityIndex);
    }

    // Thruster visuals are player-only for now (NPC plumes: Phase 6 feedback).
    m_thrusters.tick(shipState(), shipTuning(), m_shipInput, dt);
}

void SpaceWorld::handleShipDestroyed(std::uint32_t entityIndex)
{
    if (entityIndex == playerEntityIndex()) {
        // GDD death rule (respawn at last dock, insurance cost) arrives with
        // docking; until then: reset at the spawn point with full defenses.
        SOL_LOG_WARN("ship destroyed - respawning at Aster Gateway");
        Transform& transform = m_registry.storage<Transform>().get(entityIndex);
        transform = Transform{.position = kPlayerStart, .previousPosition = kPlayerStart};
        m_registry.storage<FlightBody>().get(entityIndex) = FlightBody{};
        ShipDefense& defense = m_registry.storage<ShipDefense>().get(entityIndex);
        sim::resetDefense(defense.state, defense.tuning);
        ShipPower& power = m_registry.storage<ShipPower>().get(entityIndex);
        power.state = sim::PowerState{.weaponCharge = power.tuning.weaponCapacitor};
        return;
    }

    for (std::size_t i = 0; i < m_spawnedShips.size(); ++i) {
        if (m_spawnedShips[i].entity.index == entityIndex) {
            SOL_LOG_INFO("'%s' destroyed", m_spawnedShips[i].defId.c_str());
            m_registry.destroy(m_spawnedShips[i].entity);
            m_spawnedShips.erase(m_spawnedShips.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

Transform SpaceWorld::shipRenderTransform(float alpha) const
{
    const std::uint32_t shipIndex = playerEntityIndex();
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
    const std::uint32_t shipIndex = playerEntityIndex();

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
            .model = shape[i].model,
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
    if (fresh.storage<PlayerShip>().size() != 1) {
        return false; // not a current-format save (or player identity lost)
    }
    m_registry = std::move(fresh);
    // Def-spawned entities were replaced wholesale; their def association is
    // gone (visuals persist via the saved RenderShape).
    m_spawnedShips.clear();
    return true;
}

} // namespace game
