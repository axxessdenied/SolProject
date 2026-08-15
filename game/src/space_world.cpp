#include "space_world.hpp"

#include "sol/core/log.hpp"
#include "sol/core/serialize.hpp"
#include "sol/sim/collision.hpp"
#include "sol/sim/steering.hpp"
#include "sol/sim/weapons.hpp"
#include "sol/ecs/snapshot.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace game {

using namespace sol;

namespace {

// Impact damage: k * v^2 (50 m/s ram = 25 damage); scrapes below ~10 m/s
// are ignored so docking bumps stay free.
constexpr double kImpactDamageFactor = 0.01;
constexpr double kImpactDamageMinimum = 1.0;

// Save format: header (magic, version, universe seed, current system) then
// the ECS snapshot. Bump the version on any layout change; old saves are
// rejected cleanly by the magic/version check.
constexpr std::uint32_t kSaveMagic = 0x37'4c'4f'53u; // "SOL7"
constexpr std::uint32_t kSaveVersion = 3; // v3: economy + player credits/cargo

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
    schema.component<Projectile>(17);
    schema.component<ShipWeapon>(18);
    schema.component<ShipPilot>(19);
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

void SpaceWorld::spawn(std::uint64_t universeSeed)
{
    m_universeSeed = universeSeed;
    // Placeholder star so rendering before generateUniverse stays sane.
    m_star = {.name = "(void)", .position = {}, .radius = 6.96e8};

    const ecs::Entity e = m_registry.create();
    m_registry.emplace<Transform>(e);
    m_registry.emplace<FlightBody>(e);
    m_registry.emplace<RenderShape>(e, RenderShape{.model = ModelId::Ship});
    m_registry.emplace<PlayerShip>(e);
    m_registry.emplace<ShipControl>(e);
    m_registry.emplace<ShipPower>(e);
    m_registry.emplace<ShipDefense>(e);
    m_registry.emplace<ShipWeapon>(e);
}

void SpaceWorld::generateUniverse(const assets::DefDatabase& defs)
{
    m_galaxyParams = sim::GalaxyParams{};
    m_galaxyParams.seed = m_universeSeed;
    m_galaxyParams.factionCount = static_cast<std::uint32_t>(defs.factions().size());
    for (const assets::StationDef& station : defs.stations()) {
        m_galaxyParams.stationRules.push_back(
            {{station.weightCore, station.weightFrontier, station.weightFringe}});
    }
    m_galaxy = sim::generateGalaxy(m_galaxyParams);

    // Economy: commodities + archetype rates from the defs (unknown
    // commodity ids in a rate list are warnings, not errors — a mod may
    // remove a commodity a base station references).
    m_economyParams = sim::EconomyParams{};
    m_commodityIds.clear();
    for (const assets::CommodityDef& commodity : defs.commodities()) {
        m_economyParams.commodities.push_back({.basePrice = commodity.basePrice});
        m_commodityIds.push_back(commodity.id);
    }
    const std::uint32_t commodityCount =
        static_cast<std::uint32_t>(m_commodityIds.size());
    for (const assets::StationDef& station : defs.stations()) {
        sim::EconomyArchetype archetype;
        archetype.production.assign(commodityCount, 0.0f);
        archetype.consumption.assign(commodityCount, 0.0f);
        archetype.stockCapacity = station.stockCapacity;
        const auto applyRates = [&](const std::vector<assets::StationRate>& rates,
                                    std::vector<float>& out) {
            for (const assets::StationRate& rate : rates) {
                const std::uint32_t index = commodityIndex(rate.commodityId.c_str());
                if (index < commodityCount) {
                    out[index] = rate.rate;
                } else {
                    SOL_LOG_WARN("station '%s': unknown commodity '%s'", station.id.c_str(),
                                 rate.commodityId.c_str());
                }
            }
        };
        applyRates(station.produces, archetype.production);
        applyRates(station.consumes, archetype.consumption);
        m_economyParams.archetypes.push_back(std::move(archetype));
    }
    if (!m_economyParams.commodities.empty()) {
        m_economy.initialize(m_galaxy, m_economyParams, m_universeSeed);
    }
    m_playerCargo.assign(commodityCount, 0.0f);

    // Start in the first core system with a station (deterministic per seed).
    std::uint32_t start = 0;
    for (std::uint32_t i = 0; i < m_galaxy.systems.size(); ++i) {
        if (m_galaxy.systems[i].region == sim::Region::Core &&
            !m_galaxy.systems[i].stations.empty()) {
            start = i;
            break;
        }
    }
    loadSystem(start, kNoIndex);
    SOL_LOG_INFO("universe: seed %llu, %zu systems, %zu lanes; starting in '%s'",
                 static_cast<unsigned long long>(m_universeSeed), m_galaxy.systems.size(),
                 m_galaxy.links.size(), currentSystemName());
}

void SpaceWorld::despawnSystem()
{
    const std::uint32_t playerIndex = playerEntityIndex();
    std::vector<ecs::Entity> doomed;
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        const std::uint32_t entityIndex = transforms.entityIndices()[i];
        if (entityIndex != playerIndex) {
            doomed.push_back(m_registry.entityFromIndex(entityIndex));
        }
    }
    for (const ecs::Entity entity : doomed) {
        m_registry.destroy(entity);
    }
    m_spawnedShips.clear();
    m_combatEffects.clear();
    m_thrusters.clear();
}

void SpaceWorld::instantiateSystemEntities(const sim::SystemSpec& spec)
{
    auto addStatic = [&](core::DVec3 position, core::Vec3 scale, ModelId model) {
        const ecs::Entity e = m_registry.create();
        m_registry.emplace<Transform>(e, Transform{.position = position,
                                                   .previousPosition = position});
        m_registry.emplace<RenderShape>(e, RenderShape{.scale = scale, .model = model});
    };
    for (const sim::StationSpec& station : spec.stations) {
        addStatic(station.position, {1.0f, 1.0f, 1.0f}, ModelId::Station);
    }
    // Gates render as flat frames (provisional visual: a squashed cube).
    for (const sim::GateSpec& gate : spec.gates) {
        addStatic(gate.position, {70.0f, 70.0f, 10.0f}, ModelId::Cube);
    }
}

void SpaceWorld::rebuildSystemSideData(const sim::SystemSpec& spec)
{
    m_star = {.name = spec.name, .position = {}, .radius = spec.starRadius};
    m_planets.clear();
    for (const sim::PlanetSpec& planet : spec.planets) {
        m_planets.push_back(
            {.name = planet.name, .position = planet.position, .radius = planet.radius});
    }
    m_gates.clear();
    for (const sim::GateSpec& gate : spec.gates) {
        m_gates.push_back({.name = "Gate: " + m_galaxy.systems[gate.toSystem].name,
                           .toSystem = gate.toSystem,
                           .position = gate.position});
    }

    // Target cycle: stations, gates, planets, star. Lua's stationPosition()
    // anchor is m_targets[0], so stations must stay first.
    m_targets.clear();
    for (const sim::StationSpec& station : spec.stations) {
        m_targets.push_back(
            {.name = station.name, .position = station.position, .surfaceRadius = 0.0});
    }
    for (const GateInstance& gate : m_gates) {
        m_targets.push_back({.name = gate.name, .position = gate.position, .surfaceRadius = 0.0});
    }
    for (const CelestialBody& planet : m_planets) {
        m_targets.push_back(
            {.name = planet.name, .position = planet.position, .surfaceRadius = planet.radius});
    }
    m_targets.push_back(
        {.name = m_star.name, .position = m_star.position, .surfaceRadius = m_star.radius});
    m_targetIndex = 0;

    // Steering obstacles for NPC avoidance: stations plus every celestial.
    m_obstacles.clear();
    for (const sim::StationSpec& station : spec.stations) {
        m_obstacles.push_back({.position = station.position, .radius = 130.0});
    }
    for (const CelestialBody& planet : m_planets) {
        m_obstacles.push_back({.position = planet.position, .radius = planet.radius});
    }
    m_obstacles.push_back({.position = m_star.position, .radius = m_star.radius});
}

void SpaceWorld::loadSystem(std::uint32_t systemIndex, std::uint32_t fromSystem)
{
    despawnSystem();
    m_currentSystem = systemIndex;
    m_dockedStation = kNoIndex;
    const sim::SystemSpec& spec = m_galaxy.systems[systemIndex];
    instantiateSystemEntities(spec);
    rebuildSystemSideData(spec);

    // Arrival point: off the gate we came through, facing the playfield; on
    // a fresh start, just off the first station (or the hub failing that).
    const core::DVec3 hub = spec.planets[spec.primaryPlanet].position;
    core::DVec3 arrival = hub + core::DVec3{0.0, 0.0, 2.0e5};
    if (fromSystem != kNoIndex) {
        for (const sim::GateSpec& gate : spec.gates) {
            if (gate.toSystem == fromSystem) {
                arrival = gate.position + normalize(hub - gate.position) * 500.0;
                break;
            }
        }
    } else if (!spec.stations.empty()) {
        arrival = spec.stations[0].position + core::DVec3{0.0, 0.0, 800.0};
    }
    m_playerSpawn = arrival;

    const std::uint32_t playerIndex = playerEntityIndex();
    Transform& transform = m_registry.storage<Transform>().get(playerIndex);
    transform.position = arrival;
    transform.previousPosition = arrival;
    transform.previousOrientation = transform.orientation;
    m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
    m_playerDamageTimer = 0.0f;
}

bool SpaceWorld::jumpNearestGate(double activationRange)
{
    if (isDocked()) {
        return false; // undock first
    }
    const core::DVec3 playerPosition = shipState().position;
    const GateInstance* nearest = nullptr;
    double nearestDistance = activationRange;
    for (const GateInstance& gate : m_gates) {
        const double distance = length(gate.position - playerPosition);
        if (distance <= nearestDistance) {
            nearestDistance = distance;
            nearest = &gate;
        }
    }
    if (nearest == nullptr) {
        return false;
    }
    const std::uint32_t destination = nearest->toSystem;
    SOL_LOG_INFO("jumping: %s -> %s", currentSystemName(),
                 m_galaxy.systems[destination].name.c_str());
    loadSystem(destination, m_currentSystem);
    return true;
}

sol::core::DVec3 SpaceWorld::dockPoint(std::uint32_t stationIndex) const
{
    // 250 m above the station: outside its ~100 m collision sphere, close
    // enough to read as "parked at the pad".
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    return spec.stations[stationIndex].position + core::DVec3{0.0, 250.0, 0.0};
}

bool SpaceWorld::tryDockNearestStation(double range)
{
    if (isDocked() || m_currentSystem >= m_galaxy.systems.size()) {
        return false;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const core::DVec3 playerPosition = shipState().position;
    std::uint32_t nearest = kNoIndex;
    double nearestDistance = range;
    for (std::uint32_t i = 0; i < spec.stations.size(); ++i) {
        const double distance = length(spec.stations[i].position - playerPosition);
        if (distance <= nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }
    if (nearest == kNoIndex) {
        return false;
    }
    m_dockedStation = nearest;
    m_lastDockSystem = m_currentSystem;
    m_lastDockStation = nearest;

    // Autodock: park at the pad, kill relative motion, refresh the spawn
    // anchor (the death rule respawns at the last dock).
    const std::uint32_t playerIndex = playerEntityIndex();
    Transform& transform = m_registry.storage<Transform>().get(playerIndex);
    const core::DVec3 pad = dockPoint(nearest);
    transform.position = pad;
    transform.previousPosition = pad;
    m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
    m_playerSpawn = pad;
    SOL_LOG_INFO("docked at '%s'", spec.stations[nearest].name.c_str());
    return true;
}

bool SpaceWorld::undock()
{
    if (!isDocked()) {
        return false;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    const std::uint32_t playerIndex = playerEntityIndex();
    Transform& transform = m_registry.storage<Transform>().get(playerIndex);
    // Release 500 m off the pad so the station sphere is comfortably clear.
    const core::DVec3 release =
        spec.stations[m_dockedStation].position + core::DVec3{0.0, 500.0, 0.0};
    transform.position = release;
    transform.previousPosition = release;
    m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
    SOL_LOG_INFO("undocked from '%s'", spec.stations[m_dockedStation].name.c_str());
    m_dockedStation = kNoIndex;
    return true;
}

const char* SpaceWorld::dockedStationName() const
{
    if (!isDocked()) {
        return "";
    }
    return m_galaxy.systems[m_currentSystem].stations[m_dockedStation].name.c_str();
}

std::uint32_t SpaceWorld::commodityIndex(const char* id) const
{
    for (std::uint32_t i = 0; i < m_commodityIds.size(); ++i) {
        if (m_commodityIds[i] == id) {
            return i;
        }
    }
    return kNoIndex;
}

float SpaceWorld::playerCargoTotal() const
{
    float total = 0.0f;
    for (const float units : m_playerCargo) {
        total += units;
    }
    return total;
}

std::uint32_t SpaceWorld::dockedMarket() const
{
    return isDocked() ? m_economy.marketFor(m_currentSystem, m_dockedStation) : kNoIndex;
}

sim::TradeResult SpaceWorld::playerBuy(std::uint32_t commodity, float units)
{
    const std::uint32_t market = dockedMarket();
    if (market >= m_economy.markets().size() || commodity >= m_playerCargo.size() ||
        units <= 0.0f) {
        return {};
    }
    units = std::min(units, m_playerCargoCapacity - playerCargoTotal());
    const float unitPrice = m_economy.price(market, commodity);
    if (unitPrice > 0.0f) {
        units = std::min(units, static_cast<float>(m_playerCredits / unitPrice));
    }
    const sim::TradeResult result = m_economy.buy(market, commodity, units);
    m_playerCredits -= result.credits;
    m_playerCargo[commodity] += result.units;
    return result;
}

sim::TradeResult SpaceWorld::playerSell(std::uint32_t commodity, float units)
{
    const std::uint32_t market = dockedMarket();
    if (market >= m_economy.markets().size() || commodity >= m_playerCargo.size() ||
        units <= 0.0f) {
        return {};
    }
    units = std::min(units, m_playerCargo[commodity]);
    const sim::TradeResult result = m_economy.sell(market, commodity, units);
    m_playerCredits += result.credits;
    m_playerCargo[commodity] -= result.units;
    return result;
}

double SpaceWorld::nearestStationDistance() const
{
    if (m_currentSystem >= m_galaxy.systems.size()) {
        return -1.0;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    if (spec.stations.empty()) {
        return -1.0;
    }
    const core::DVec3 playerPosition =
        m_registry.storage<Transform>().get(playerEntityIndex()).position;
    double nearest = 1.0e30;
    for (const sim::StationSpec& station : spec.stations) {
        nearest = std::min(nearest, length(station.position - playerPosition));
    }
    return nearest;
}

bool SpaceWorld::jumpToSystem(const char* destinationName)
{
    if (isDocked()) {
        return false;
    }
    for (const GateInstance& gate : m_gates) {
        if (m_galaxy.systems[gate.toSystem].name == destinationName) {
            SOL_LOG_INFO("jumping: %s -> %s", currentSystemName(), destinationName);
            loadSystem(gate.toSystem, m_currentSystem);
            return true;
        }
    }
    return false;
}

double SpaceWorld::nearestGateDistance() const
{
    if (m_gates.empty()) {
        return -1.0;
    }
    const core::DVec3 playerPosition =
        m_registry.storage<Transform>().get(playerEntityIndex()).position;
    double nearest = 1.0e30;
    for (const GateInstance& gate : m_gates) {
        nearest = std::min(nearest, length(gate.position - playerPosition));
    }
    return nearest;
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
        applyShipDef(playerEntityIndex(), *playerDef, defs);
    } else {
        SOL_LOG_WARN("player ship def '%s' missing; keeping current tuning", kPlayerShipDefId);
    }

    for (const SpawnedShip& spawned : m_spawnedShips) {
        if (const assets::ShipDef* def = defs.findShip(spawned.defId.c_str())) {
            applyShipDef(spawned.entity.index, *def, defs);
        }
    }
}

void SpaceWorld::applyShipDef(std::uint32_t entityIndex, const assets::ShipDef& def,
                              const assets::DefDatabase& defs)
{
    RenderShape& shape = m_registry.storage<RenderShape>().get(entityIndex);
    shape.scale = {def.scale, def.scale, def.scale};
    shape.model = modelIdFromName(def.model);
    m_registry.storage<ShipControl>().get(entityIndex).tuning = toShipTuning(def.flight);
    if (entityIndex == playerEntityIndex()) {
        m_playerCargoCapacity = def.cargoCapacity;
    }

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

    ShipWeapon& weapon = m_registry.storage<ShipWeapon>().get(entityIndex);
    weapon = ShipWeapon{};
    if (!def.weaponId.empty()) {
        if (const assets::WeaponDef* weaponDef = defs.findWeapon(def.weaponId.c_str())) {
            weapon.kind =
                weaponDef->kind == "hitscan" ? WeaponKind::Hitscan : WeaponKind::Projectile;
            weapon.damage = weaponDef->damage;
            weapon.rateOfFire = weaponDef->rateOfFire;
            weapon.range = weaponDef->range;
            weapon.projectileSpeed = weaponDef->projectileSpeed;
            weapon.energyCost = weaponDef->energyCost;
        } else {
            SOL_LOG_WARN("ship '%s': unknown weapon def '%s'", def.id.c_str(),
                         def.weaponId.c_str());
        }
    }
}

ecs::Entity SpaceWorld::spawnShipFromDef(const assets::ShipDef& def,
                                         const assets::DefDatabase& defs)
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
    m_registry.emplace<ShipWeapon>(e);
    applyShipDef(e.index, def, defs);
    m_spawnedShips.push_back({.entity = e, .defId = def.id, .name = def.name});
    return e;
}

TargetInfo SpaceWorld::currentTargetInfo() const
{
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    // m_targetIndex can go stale when a targeted ship dies; wrap it here.
    const std::size_t index = total > 0 ? m_targetIndex % total : 0;

    TargetInfo info;
    if (index < m_targets.size()) {
        info.nav = m_targets[index];
        return info;
    }
    const SpawnedShip& ship = m_spawnedShips[index - m_targets.size()];
    const Transform& transform = m_registry.storage<Transform>().get(ship.entity.index);
    info.nav = NavTarget{.name = ship.name, .position = transform.position, .surfaceRadius = 0.0};
    info.isShip = true;
    info.velocity = m_registry.storage<FlightBody>().get(ship.entity.index).velocity;
    if (const ShipDefense* defense = m_registry.tryGet<ShipDefense>(ship.entity)) {
        const float strength =
            defense->tuning.shieldStrength > 0.0f ? defense->tuning.shieldStrength : 1.0f;
        info.shieldFore = defense->state.shieldFore / strength;
        info.shieldAft = defense->state.shieldAft / strength;
        info.hull =
            defense->tuning.hull > 0.0f ? defense->state.hull / defense->tuning.hull : 0.0f;
    }
    return info;
}

void SpaceWorld::cycleTarget()
{
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    if (total > 0) {
        m_targetIndex = (m_targetIndex % total + 1) % total;
    }
}

ecs::Entity SpaceWorld::spawnPilotFromDef(const assets::ShipDef& def,
                                          const assets::DefDatabase& defs, PilotRole role)
{
    const ecs::Entity e = spawnShipFromDef(def, defs);
    m_registry.emplace<ShipPilot>(e, ShipPilot{.role = role});
    return e;
}

namespace {

// Role/state pip policies (decisions/003 consequence: simple per-role triage).
sim::PowerPips pipsForPilot(PilotState state)
{
    switch (state) {
    case PilotState::Attack: return {3, 2, 1};
    case PilotState::Flee: return {0, 4, 2};
    case PilotState::Idle:
    case PilotState::Patrol: break;
    }
    return {2, 2, 2};
}

} // namespace

bool SpaceWorld::pilotAttackPlayer(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Attack;
    pilot->targetIndex = playerEntityIndex();
    pilot->hasTarget = 1;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotFlee(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Flee;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotIdle(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Idle;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

bool SpaceWorld::pilotPatrolTo(ecs::Entity entity, core::DVec3 waypoint)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr) {
        return false;
    }
    pilot->state = PilotState::Patrol;
    pilot->waypoint = waypoint;
    if (ShipPower* power = m_registry.tryGet<ShipPower>(entity)) {
        power->state.pips = pipsForPilot(pilot->state);
    }
    return true;
}

double SpaceWorld::shipHullFraction(ecs::Entity entity) const
{
    if (!m_registry.isAlive(entity)) {
        return 0.0;
    }
    const ShipDefense* defense = m_registry.tryGet<ShipDefense>(entity);
    if (defense == nullptr || defense->tuning.hull <= 0.0f) {
        return 0.0;
    }
    return static_cast<double>(defense->state.hull / defense->tuning.hull);
}

void SpaceWorld::collectDuePilotThinks(double dt, std::vector<PilotThink>& out)
{
    static constexpr const char* kRoleNames[] = {"fighter", "trader", "patrol"};
    static constexpr const char* kStateNames[] = {"idle", "patrol", "attack", "flee"};
    constexpr float kThinkInterval = 0.5f; // 2 Hz strategy; steering runs at 60

    ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        ShipPilot& pilot = pilots.values()[i];
        pilot.thinkTimer -= static_cast<float>(dt);
        if (pilot.thinkTimer > 0.0f) {
            continue;
        }
        pilot.thinkTimer = kThinkInterval;
        out.push_back({
            .entity = m_registry.entityFromIndex(pilots.entityIndices()[i]),
            .role = kRoleNames[static_cast<std::uint32_t>(pilot.role) % 3],
            .state = kStateNames[static_cast<std::uint32_t>(pilot.state) % 4],
        });
    }
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
    if (isDocked()) {
        // Parked: flight input is ignored and the ship stays pinned to the
        // pad (collision impulses must not drift a docked ship).
        m_registry.storage<ShipControl>().get(playerIndex).input = sim::FlightInput{};
        Transform& transform = m_registry.storage<Transform>().get(playerIndex);
        const core::DVec3 pad = dockPoint(m_dockedStation);
        transform.position = pad;
        transform.previousPosition = pad;
        m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
    } else {
        m_registry.storage<ShipControl>().get(playerIndex).input = m_shipInput;
    }

    // NPC pilots: C++ steering flies whatever state Lua's pilot_think chose.
    {
        ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
        const std::span<const sim::AvoidanceSphere> obstacles = m_obstacles;
        for (std::size_t i = 0; i < pilots.size(); ++i) {
            ShipPilot& pilot = pilots.values()[i];
            const std::uint32_t entityIndex = pilots.entityIndices()[i];
            ShipControl* control = m_registry.storage<ShipControl>().tryGet(entityIndex);
            const Transform* transform = m_registry.storage<Transform>().tryGet(entityIndex);
            const FlightBody* body = m_registry.storage<FlightBody>().tryGet(entityIndex);
            if (control == nullptr || transform == nullptr || body == nullptr) {
                continue;
            }
            const sim::ShipState self = {
                .position = transform->position,
                .velocity = body->velocity,
                .orientation = transform->orientation,
                .angularVelocity = body->angularVelocity,
            };

            sim::FlightInput input; // Idle default: assist-on station keeping
            switch (pilot.state) {
            case PilotState::Idle:
                break;
            case PilotState::Patrol:
                if (length(pilot.waypoint - self.position) < 120.0) {
                    pilot.state = PilotState::Idle; // arrived; Lua picks the next leg
                    break;
                }
                input = sim::steerPursue(self, control->tuning, pilot.waypoint, {}, 50.0);
                break;
            case PilotState::Attack: {
                const Transform* targetTransform =
                    m_registry.storage<Transform>().tryGet(pilot.targetIndex);
                const FlightBody* targetBody =
                    m_registry.storage<FlightBody>().tryGet(pilot.targetIndex);
                if (pilot.hasTarget == 0 || targetTransform == nullptr ||
                    targetBody == nullptr) {
                    pilot.state = PilotState::Idle;
                    break;
                }
                const core::DVec3 toTarget = targetTransform->position - self.position;
                const double distance = length(toTarget);
                const core::DVec3 direction =
                    distance > 1.0 ? toTarget * (1.0 / distance) : core::DVec3{0.0, 0.0, -1.0};
                core::DVec3 desiredVelocity =
                    targetBody->velocity + direction * ((distance - 250.0) * 0.5);
                sim::avoidObstacles(desiredVelocity, self, obstacles, 8.0);

                const ShipWeapon* weapon = m_registry.storage<ShipWeapon>().tryGet(entityIndex);
                const double projectileSpeed =
                    weapon != nullptr && weapon->projectileSpeed > 1.0f
                        ? static_cast<double>(weapon->projectileSpeed)
                        : 1.0e9; // hitscan: effectively instant
                core::DVec3 aimDirection;
                (void)sim::computeInterceptDirection(self.position, self.velocity,
                                                     targetTransform->position,
                                                     targetBody->velocity, projectileSpeed,
                                                     aimDirection);
                const core::DVec3 aimPoint =
                    self.position + aimDirection * (distance > 100.0 ? distance : 100.0);
                input = sim::steerAimAndMove(self, control->tuning, aimPoint, desiredVelocity);
                if (weapon != nullptr && weapon->kind != WeaponKind::None) {
                    input.trigger = sim::aimError(self, aimPoint) < 0.06 &&
                                    distance < static_cast<double>(weapon->range) * 0.9;
                }
                break;
            }
            case PilotState::Flee: {
                const Transform* threatTransform =
                    m_registry.storage<Transform>().tryGet(pilot.targetIndex);
                const core::DVec3 threat = threatTransform != nullptr
                                               ? threatTransform->position
                                               : self.position + core::DVec3{0.0, 0.0, 1.0};
                pilot.weavePhase += static_cast<float>(dt) * 3.0f;
                input = sim::steerEvade(self, control->tuning, threat, pilot.weavePhase);
                break;
            }
            }
            control->input = input;
        }
    }

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
    ecs::Pool<Projectile>& projectiles = m_registry.storage<Projectile>();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::uint32_t entityIndex = shapes.entityIndices()[i];
        if (bodies.contains(entityIndex) || projectiles.contains(entityIndex)) {
            continue; // ships were pushed above; bolts never block anything
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
    m_collisionBodies.push_back({.previousPosition = m_star.position,
                                 .position = m_star.position,
                                 .velocity = {},
                                 .radius = m_star.radius,
                                 .inverseMass = 0.0});
    for (const CelestialBody& planet : m_planets) {
        m_collisionBodies.push_back({.previousPosition = planet.position,
                                     .position = planet.position,
                                     .velocity = {},
                                     .radius = planet.radius,
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
        noteDamage(entityIndex,
                   m_collisionBodies[bodySlot].position +
                       toSource * m_collisionBodies[bodySlot].radius,
                   result);
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
    destroyedShips.clear();

    // Projectiles: advance, expire, resolve hits. Ship spheres come from the
    // collision list built above (slot i <-> m_collisionShipIndices[i]);
    // remaining slots are statics that simply soak bolts.
    std::vector<std::uint32_t> deadProjectiles;
    for (std::size_t p = 0; p < projectiles.size(); ++p) {
        Projectile& projectile = projectiles.values()[p];
        const std::uint32_t projectileIndex = projectiles.entityIndices()[p];
        Transform& transform = m_registry.storage<Transform>().get(projectileIndex);
        transform.previousPosition = transform.position;
        transform.position += projectile.velocity * dt;
        projectile.lifetime -= dt;

        bool dead = projectile.lifetime <= 0.0;
        double bestT = 2.0;
        std::size_t bestSlot = m_collisionBodies.size();
        for (std::size_t slot = 0; slot < m_collisionBodies.size(); ++slot) {
            if (slot < shipCount && m_collisionShipIndices[slot] == projectile.shooterIndex) {
                continue;
            }
            double hitT = 0.0;
            if (sim::segmentHitsSphere(transform.previousPosition, transform.position,
                                       m_collisionBodies[slot].position,
                                       m_collisionBodies[slot].radius, hitT) &&
                hitT < bestT) {
                bestT = hitT;
                bestSlot = slot;
            }
        }
        if (bestSlot < m_collisionBodies.size()) {
            dead = true;
            if (bestSlot < shipCount) {
                const std::uint32_t targetIndex = m_collisionShipIndices[bestSlot];
                if (ShipDefense* defense = defenses.tryGet(targetIndex);
                    defense != nullptr && defense->state.alive()) {
                    const core::DVec3 toSource = normalize(projectile.velocity) * -1.0;
                    const sim::ShieldFacing facing = sim::facingForHit(
                        m_registry.storage<Transform>().get(targetIndex).orientation, toSource);
                    const sim::DamageResult result = sim::applyDamage(
                        defense->state, defense->tuning, facing, projectile.damage);
                    noteDamage(targetIndex,
                               transform.previousPosition +
                                   (transform.position - transform.previousPosition) * bestT,
                               result);
                    if (result.destroyed) {
                        destroyedShips.push_back(targetIndex);
                    }
                }
            }
        }
        if (dead) {
            deadProjectiles.push_back(projectileIndex);
        }
    }
    for (const std::uint32_t index : deadProjectiles) {
        m_registry.destroy(m_registry.entityFromIndex(index));
    }
    for (const std::uint32_t entityIndex : destroyedShips) {
        handleShipDestroyed(entityIndex);
    }
    destroyedShips.clear();

    // Weapons: tick cooldowns; a held trigger fires when charged and ready.
    ecs::Pool<ShipWeapon>& weapons = m_registry.storage<ShipWeapon>();
    struct PendingBolt
    {
        core::DVec3 position;
        core::Quat orientation;
        core::DVec3 velocity;
        double lifetime;
        float damage;
        std::uint32_t shooterIndex;
    };
    std::vector<PendingBolt> newBolts;
    for (std::size_t w = 0; w < weapons.size(); ++w) {
        ShipWeapon& weapon = weapons.values()[w];
        const std::uint32_t entityIndex = weapons.entityIndices()[w];
        if (weapon.cooldown > 0.0f) {
            weapon.cooldown -= static_cast<float>(dt);
        }
        if (weapon.kind == WeaponKind::None || weapon.cooldown > 0.0f) {
            continue;
        }
        const ShipControl* control = m_registry.storage<ShipControl>().tryGet(entityIndex);
        if (control == nullptr || !control->input.trigger) {
            continue;
        }
        if (ShipPower* power = powers.tryGet(entityIndex);
            power != nullptr && !sim::drawWeaponCharge(power->state, weapon.energyCost)) {
            continue;
        }
        weapon.cooldown = 1.0f / (weapon.rateOfFire > 0.01f ? weapon.rateOfFire : 0.01f);

        const Transform& transform = transforms.get(entityIndex);
        const RenderShape& shape = m_registry.storage<RenderShape>().get(entityIndex);
        const core::Vec3 forward = rotate(transform.orientation, core::Vec3{0.0f, 0.0f, -1.0f});
        const core::DVec3 forwardD = toDVec3(forward);
        const double noseOffset =
            modelBaseRadius(shape.model) * static_cast<double>(shape.scale.x) + 6.0;
        const core::DVec3 muzzle = transform.position + forwardD * noseOffset;

        if (weapon.kind == WeaponKind::Hitscan) {
            // Instant pulse along the boresight; first ship hit takes it.
            const core::DVec3 beamEnd = muzzle + forwardD * static_cast<double>(weapon.range);
            double bestT = 2.0;
            std::uint32_t bestTarget = 0;
            bool hit = false;
            for (std::size_t slot = 0; slot < shipCount; ++slot) {
                const std::uint32_t targetIndex = m_collisionShipIndices[slot];
                if (targetIndex == entityIndex) {
                    continue;
                }
                double hitT = 0.0;
                if (sim::segmentHitsSphere(muzzle, beamEnd, m_collisionBodies[slot].position,
                                           m_collisionBodies[slot].radius, hitT) &&
                    hitT < bestT) {
                    bestT = hitT;
                    bestTarget = targetIndex;
                    hit = true;
                }
            }
            if (hit) {
                if (ShipDefense* defense = defenses.tryGet(bestTarget);
                    defense != nullptr && defense->state.alive()) {
                    const sim::ShieldFacing facing = sim::facingForHit(
                        m_registry.storage<Transform>().get(bestTarget).orientation,
                        forwardD * -1.0);
                    const sim::DamageResult result = sim::applyDamage(
                        defense->state, defense->tuning, facing, weapon.damage);
                    noteDamage(bestTarget, muzzle + (beamEnd - muzzle) * bestT, result);
                    if (result.destroyed) {
                        destroyedShips.push_back(bestTarget); // deferred: mid-iteration
                    }
                }
            }
        } else {
            const FlightBody& body = bodies.get(entityIndex);
            newBolts.push_back({
                .position = muzzle,
                .orientation = transform.orientation,
                .velocity = body.velocity + forwardD * static_cast<double>(weapon.projectileSpeed),
                .lifetime = static_cast<double>(weapon.range) /
                            static_cast<double>(weapon.projectileSpeed > 1.0f
                                                    ? weapon.projectileSpeed
                                                    : 1.0f),
                .damage = weapon.damage,
                .shooterIndex = entityIndex,
            });
        }
    }
    for (const std::uint32_t entityIndex : destroyedShips) {
        handleShipDestroyed(entityIndex);
    }
    for (const PendingBolt& bolt : newBolts) {
        const ecs::Entity e = m_registry.create();
        m_registry.emplace<Transform>(e, Transform{.position = bolt.position,
                                                   .previousPosition = bolt.position,
                                                   .orientation = bolt.orientation,
                                                   .previousOrientation = bolt.orientation});
        m_registry.emplace<RenderShape>(
            e, RenderShape{.scale = {0.3f, 0.3f, 4.0f}, .model = ModelId::Cube});
        m_registry.emplace<Projectile>(e, Projectile{.velocity = bolt.velocity,
                                                     .lifetime = bolt.lifetime,
                                                     .damage = bolt.damage,
                                                     .shooterIndex = bolt.shooterIndex});
    }

    // Feedback bookkeeping.
    m_combatEffects.tick(dt);
    if (m_playerDamageTimer > 0.0f) {
        m_playerDamageTimer -= static_cast<float>(dt);
        if (m_playerDamageTimer < 0.0f) {
            m_playerDamageTimer = 0.0f;
        }
    }

    // Thruster visuals are player-only for now (NPC plumes: Phase 6 feedback).
    m_thrusters.tick(shipState(), shipTuning(), m_shipInput, dt);

    // Coarse-layer economy: galaxy-wide, same clock as everything else
    // (decisions/005 — no time compression).
    m_economy.tick(m_galaxy, dt);

    // Deferred death respawn into the last-dock system (see member comment).
    if (m_pendingRespawnSystem != kNoIndex) {
        const std::uint32_t system = m_pendingRespawnSystem;
        m_pendingRespawnSystem = kNoIndex;
        loadSystem(system, kNoIndex);
        if (m_lastDockStation != kNoIndex &&
            m_lastDockStation < m_galaxy.systems[system].stations.size()) {
            m_dockedStation = m_lastDockStation;
            const core::DVec3 pad = dockPoint(m_dockedStation);
            Transform& transform = m_registry.storage<Transform>().get(playerEntityIndex());
            transform.position = pad;
            transform.previousPosition = pad;
            m_playerSpawn = pad;
        }
    }
}

void SpaceWorld::noteDamage(std::uint32_t targetIndex, const core::DVec3& hitPosition,
                            const sim::DamageResult& result)
{
    const bool shieldHit = result.shieldAbsorbed >= result.armorAbsorbed + result.hullDamage;
    m_combatEffects.spawnImpact(hitPosition, shieldHit);
    if (targetIndex == playerEntityIndex()) {
        m_playerDamageTimer = kDamageFlashSeconds;
    }
}

void SpaceWorld::handleShipDestroyed(std::uint32_t entityIndex)
{
    // Fireball at the wreck site, scaled by the hull.
    m_combatEffects.spawnExplosion(m_registry.storage<Transform>().get(entityIndex).position,
                                   m_registry.storage<RenderShape>().get(entityIndex).scale.x);
    if (entityIndex == playerEntityIndex()) {
        // GDD death rule: wake up docked at the last dock (insurance cost
        // arrives with outfitting, Phase 8). Never docked yet: the system
        // spawn point stands in. Cross-system respawn defers to end of tick.
        if (m_lastDockSystem != kNoIndex && m_lastDockSystem != m_currentSystem) {
            SOL_LOG_WARN("ship destroyed - waking at last dock in '%s'",
                         m_galaxy.systems[m_lastDockSystem].name.c_str());
            m_pendingRespawnSystem = m_lastDockSystem;
        } else if (m_lastDockSystem == m_currentSystem && m_lastDockStation != kNoIndex) {
            SOL_LOG_WARN("ship destroyed - waking at the last dock");
            m_dockedStation = m_lastDockStation;
            m_playerSpawn = dockPoint(m_dockedStation);
        } else {
            SOL_LOG_WARN("ship destroyed - respawning in %s", currentSystemName());
        }
        Transform& transform = m_registry.storage<Transform>().get(entityIndex);
        transform = Transform{.position = m_playerSpawn, .previousPosition = m_playerSpawn};
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
    writer.write(kSaveMagic);
    writer.write(kSaveVersion);
    writer.write(m_universeSeed);
    writer.write(m_currentSystem);
    writer.write(m_dockedStation);
    writer.write(m_lastDockSystem);
    writer.write(m_lastDockStation);
    writer.write(m_playerCredits);
    writer.write(static_cast<std::uint32_t>(m_playerCargo.size()));
    for (const float units : m_playerCargo) {
        writer.write(units);
    }
    m_economy.save(writer);
    makeSnapshotSchema().save(m_registry, writer);
    return platform::writeFileBytes(path, writer.data().data(), writer.size());
}

bool SpaceWorld::loadFrom(const char* path)
{
    std::vector<std::uint8_t> bytes;
    if (!platform::readFileBytes(path, bytes)) {
        return false;
    }
    core::BinaryReader reader(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t seed = 0;
    std::uint32_t systemIndex = 0;
    std::uint32_t dockedStation = 0;
    std::uint32_t lastDockSystem = 0;
    std::uint32_t lastDockStation = 0;
    if (!reader.read(magic) || magic != kSaveMagic || !reader.read(version) ||
        version != kSaveVersion || !reader.read(seed) || !reader.read(systemIndex) ||
        !reader.read(dockedStation) || !reader.read(lastDockSystem) ||
        !reader.read(lastDockStation)) {
        return false; // pre-galaxy or foreign save: rejected cleanly
    }

    // Same seed => same galaxy, so the galaxy itself regenerates instead of
    // being serialized (dynamic state — the economy — saves separately).
    const bool galaxyChanged = seed != m_universeSeed || m_galaxy.systems.empty();
    if (galaxyChanged) {
        m_universeSeed = seed;
        m_galaxyParams.seed = seed;
        m_galaxy = sim::generateGalaxy(m_galaxyParams);
    }
    if (systemIndex >= m_galaxy.systems.size()) {
        return false;
    }

    double credits = 0.0;
    std::uint32_t cargoCount = 0;
    if (!reader.read(credits) || !reader.read(cargoCount) ||
        cargoCount != m_playerCargo.size()) {
        return false; // commodity roster changed since the save
    }
    std::vector<float> cargo(cargoCount, 0.0f);
    for (float& units : cargo) {
        if (!reader.read(units)) {
            return false;
        }
    }
    // The economy layout is derived from galaxy+params; rebuild it against
    // the (possibly regenerated) galaxy, then restore its dynamic state.
    if (!m_economyParams.commodities.empty()) {
        if (galaxyChanged) {
            m_economy.initialize(m_galaxy, m_economyParams, seed);
        }
        if (!m_economy.load(reader)) {
            return false;
        }
    }

    ecs::Registry fresh;
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
    m_combatEffects.clear();
    m_thrusters.clear();
    m_playerCredits = credits;
    m_playerCargo = std::move(cargo);

    // The snapshot carries the system's statics; only the non-ECS side data
    // (celestials, targets, gates, spawn anchor) needs rebuilding.
    m_currentSystem = systemIndex;
    const sim::SystemSpec& spec = m_galaxy.systems[systemIndex];
    rebuildSystemSideData(spec);
    m_dockedStation =
        dockedStation < spec.stations.size() ? dockedStation : kNoIndex;
    m_lastDockSystem = lastDockSystem;
    m_lastDockStation = lastDockStation;
    m_pendingRespawnSystem = kNoIndex;
    m_playerSpawn =
        isDocked() ? dockPoint(m_dockedStation)
        : !spec.stations.empty()
            ? spec.stations[0].position + core::DVec3{0.0, 0.0, 800.0}
            : spec.planets[spec.primaryPlanet].position + core::DVec3{0.0, 0.0, 2.0e5};
    return true;
}

} // namespace game
