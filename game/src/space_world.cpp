#include "space_world.hpp"

#include "sol/assets/loadout.hpp"
#include "sol/core/log.hpp"
#include "sol/core/random.hpp"
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
constexpr std::uint32_t kSaveVersion = 9; // v9: market intel memory, world clock

// Market intel (Phase 8g): what a station's market report covers and costs.
// Deliberately shorter than the traders' own horizon — a station's brokers
// know their neighbourhood, not everywhere their freighters reach, and a
// report that covered the whole reachable galaxy would end scouting rather
// than reward it.
constexpr std::uint32_t kIntelJumpRadius = 3;
constexpr double kIntelBasePrice = 120.0;
constexpr double kIntelPricePerMarket = 18.0;
constexpr std::uint8_t kUnreachableHops = 0xff;

// Hop counts from one system over the gate graph, capped. One BFS instead of
// a routeBetween() per market — this runs over every market in the galaxy.
void hopsFrom(const sim::Galaxy& galaxy, std::uint32_t from, std::uint32_t maxHops,
              std::vector<std::uint8_t>& out)
{
    out.assign(galaxy.systems.size(), kUnreachableHops);
    if (from >= galaxy.systems.size()) {
        return;
    }
    out[from] = 0;
    std::vector<std::uint32_t> frontier{from};
    std::vector<std::uint32_t> next;
    for (std::uint8_t depth = 1; depth <= maxHops && !frontier.empty(); ++depth) {
        next.clear();
        for (const std::uint32_t index : frontier) {
            for (const sim::GateSpec& gate : galaxy.systems[index].gates) {
                if (out[gate.toSystem] == kUnreachableHops) {
                    out[gate.toSystem] = depth;
                    next.push_back(gate.toSystem);
                }
            }
        }
        frontier.swap(next);
    }
}

// Bounding-sphere radii per model at scale 1 (meters); collision radius =
// base * RenderShape scale. Rough spheres are fine for Phase 6 combat.
constexpr double kCollisionRestitution = 0.15;
[[nodiscard]] double modelBaseRadius(ModelId model)
{
    switch (model) {
    case ModelId::Cube: return 1.0;
    case ModelId::Station: return 100.0;
    case ModelId::Ship: return 8.0;
    case ModelId::Asteroid: return 1.0; // authored at radius 1; scale is meters
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
    schema.component<MineableRock>(20);
    schema.component<WreckMarker>(21);
    schema.component<OreChunk>(22);
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
    // Majors claim territory; pirate defs are clan templates (Phase 8b).
    for (const assets::FactionDef& faction : defs.factions()) {
        (faction.kind == assets::FactionKind::Pirate ? m_galaxyParams.pirateTemplateCount
                                                     : m_galaxyParams.factionCount) += 1;
    }
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
        archetype.feedstock.assign(commodityCount, 0.0f);
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
        applyRates(station.feedstock, archetype.feedstock);
        // An archetype that mines is one whose output comes out of the ground
        // rather than off anyone's dock. The def says so; nothing here keys
        // off a hard-coded station id.
        archetype.extracts = station.producesFrom == "field";
        m_economyParams.archetypes.push_back(std::move(archetype));
    }
    if (!m_economyParams.commodities.empty()) {
        m_economy.initialize(m_galaxy, m_economyParams, m_universeSeed);
    }
    m_feedstock.mining = &m_mining;
    m_feedstock.galaxy = &m_galaxy;
    m_feedstock.economy = &m_economy;
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
    m_startSystem = start;
    resetFleetToStarter();
    initializeFactions(); // before loadSystem: ambient wings need the table
    initializeSurvey();   // before loadSystem: arrival writes the first entry
    initializeMining();   // before loadSystem: it instantiates the rocks
    loadSystem(start, kNoIndex);
    SOL_LOG_INFO("universe: seed %llu, %zu systems, %zu lanes, %zu faction(s) "
                 "(%zu clans); starting in '%s'",
                 static_cast<unsigned long long>(m_universeSeed), m_galaxy.systems.size(),
                 m_galaxy.links.size(), m_factionTable.size(), m_galaxy.clans.size(),
                 currentSystemName());
}

void SpaceWorld::initializeFactions()
{
    m_factionTable.clear();
    if (m_defs == nullptr) {
        return;
    }
    // Majors in def order (their generator indices), then clans.
    std::vector<const assets::FactionDef*> pirateTemplates;
    for (const assets::FactionDef& def : m_defs->factions()) {
        if (def.kind == assets::FactionKind::Pirate) {
            pirateTemplates.push_back(&def);
            continue;
        }
        m_factionTable.push_back({.defId = def.id,
                                  .name = def.name,
                                  .color = {def.color[0], def.color[1], def.color[2]},
                                  .pirate = false,
                                  .aggression = def.aggression,
                                  .forgiveness = def.forgiveness,
                                  .shipsPatrol = def.shipsPatrol,
                                  .shipsRaider = def.shipsRaider});
    }
    const std::size_t majorCount = m_factionTable.size();
    for (const sim::ClanSpec& clan : m_galaxy.clans) {
        if (clan.templateIndex >= pirateTemplates.size()) {
            continue; // template roster shrank since generation; skip cleanly
        }
        const assets::FactionDef& base = *pirateTemplates[clan.templateIndex];
        core::Rng jitter(clan.seed, 1);
        const auto jitterChannel = [&](float value) {
            return core::clamp(value * (0.75f + 0.5f * jitter.nextFloat01()), 0.05f, 1.0f);
        };
        const auto jitterWeight = [&](float value) {
            return core::clamp(value + 0.3f * jitter.nextFloat01() - 0.15f, 0.0f, 1.0f);
        };
        m_factionTable.push_back(
            {.defId = base.id,
             .name = clan.name,
             .color = {jitterChannel(base.color[0]), jitterChannel(base.color[1]),
                       jitterChannel(base.color[2])},
             .pirate = true,
             .aggression = jitterWeight(base.aggression),
             .forgiveness = jitterWeight(base.forgiveness),
             .shipsPatrol = base.shipsPatrol,
             .shipsRaider = base.shipsRaider});
    }

    // FactionSim params: authored relations resolve def ids to table
    // indices (clans inherit their template's rows); unspecified
    // major-pirate pairs open at the default enmity.
    const std::uint32_t count = static_cast<std::uint32_t>(m_factionTable.size());
    sim::FactionSimParams params;
    params.agents.reserve(count);
    for (const GameFaction& faction : m_factionTable) {
        params.agents.push_back({.aggression = faction.aggression,
                                 .forgiveness = faction.forgiveness,
                                 .pirate = faction.pirate});
        params.initialStandings.push_back(faction.pirate ? kClanInitialStanding : 0.0f);
    }
    params.baselineRelations.assign(static_cast<std::size_t>(count) * count, 0.0f);
    const auto setPair = [&](std::uint32_t a, std::uint32_t b, float value) {
        params.baselineRelations[static_cast<std::size_t>(a) * count + b] = value;
        params.baselineRelations[static_cast<std::size_t>(b) * count + a] = value;
    };
    for (std::uint32_t a = 0; a < count; ++a) {
        for (std::uint32_t b = a + 1; b < count; ++b) {
            if (m_factionTable[a].pirate != m_factionTable[b].pirate) {
                setPair(a, b, kDefaultPirateRelation);
            }
        }
    }
    for (std::uint32_t a = 0; a < count; ++a) {
        const assets::FactionDef* def = m_defs->findFaction(m_factionTable[a].defId.c_str());
        if (def == nullptr) {
            continue;
        }
        for (const assets::FactionRelation& relation : def->relations) {
            bool found = false;
            for (std::uint32_t b = 0; b < count; ++b) {
                if (b != a && m_factionTable[b].defId == relation.otherId) {
                    setPair(a, b, relation.standing);
                    found = true;
                }
            }
            if (!found && a < majorCount) { // clans: silence per-clan repeats
                SOL_LOG_WARN("faction '%s': relation to unknown faction '%s' ignored",
                             def->id.c_str(), relation.otherId.c_str());
            }
        }
    }
    m_factionSim.initialize(m_galaxy, params, m_universeSeed);
    // Missions layout is pinned to the same faction table + commodity roster
    // (Phase 8c); a save's mission block loads over this fresh state.
    m_missions.initialize(m_galaxy, sim::MissionParams{}, count,
                          static_cast<std::uint32_t>(m_commodityIds.size()),
                          m_universeSeed);
    m_missionEvents.clear();
    m_dockEventPending = false;
}

const char* SpaceWorld::playerAttitudeName(std::uint32_t faction) const
{
    if (faction >= m_factionTable.size()) {
        return "";
    }
    if (m_factionSim.playerHostile(faction)) {
        return "hostile";
    }
    return m_factionSim.playerFriendly(faction) ? "friendly" : "neutral";
}

bool SpaceWorld::stationSells(const assets::CatalogGate& gate) const
{
    if (!isDocked()) {
        return false;
    }
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner >= m_factionTable.size()) {
        return true; // ownerless station: open market
    }
    const GameFaction& faction = m_factionTable[owner];
    if (!gate.factions.empty() &&
        std::find(gate.factions.begin(), gate.factions.end(), faction.defId) ==
            gate.factions.end()) {
        return false;
    }
    // Pirate stations fence anything their defs allow, standing be damned
    // (docking already required non-hostile standing).
    return faction.pirate || m_factionSim.standing(owner) >= gate.minRep;
}

bool SpaceWorld::commitFactionRaid(std::uint32_t faction, std::uint32_t targetSystem)
{
    if (!m_factionSim.commitRaid(m_galaxy, &m_economy, faction, targetSystem)) {
        return false;
    }
    if (faction < m_factionTable.size() && targetSystem < m_galaxy.systems.size()) {
        SOL_LOG_INFO("faction raid: %s hit '%s'", m_factionTable[faction].name.c_str(),
                     m_galaxy.systems[targetSystem].name.c_str());
    }
    return true;
}

bool SpaceWorld::warpToStationOffset(std::uint32_t station, const core::DVec3& offset)
{
    if (isDocked() || m_currentSystem >= m_galaxy.systems.size()) {
        return false;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    if (station >= spec.stations.size()) {
        return false;
    }
    m_autopilotActive = false;
    Transform& transform = m_registry.storage<Transform>().get(playerEntityIndex());
    const core::DVec3 position = spec.stations[station].position + offset;
    transform.position = position;
    transform.previousPosition = position;
    m_registry.storage<FlightBody>().get(playerEntityIndex()) = FlightBody{};
    SOL_LOG_WARN("dev warp: player moved to '%s' offset (%.0f, %.0f, %.0f)",
                 spec.stations[station].name.c_str(), offset.x, offset.y, offset.z);
    return true;
}

bool SpaceWorld::acceptMission(std::uint32_t offerIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("accept_mission: not docked", outError);
    }
    if (offerIndex >= m_missions.offers().size()) {
        return refuse("accept_mission: no such offer", outError);
    }
    const std::uint32_t poster = m_missions.offers()[offerIndex].poster;
    std::string error;
    if (!m_missions.accept(offerIndex, m_factionSim.standing(poster), &error)) {
        return refuse("accept_mission: " + error, outError);
    }
    return true;
}

bool SpaceWorld::abandonMission(std::uint32_t activeIndex)
{
    return m_missions.abandon(activeIndex);
}

void SpaceWorld::processMissionDeliveries()
{
    if (!isDocked()) {
        return;
    }
    const std::uint32_t market = dockedMarket();
    for (std::uint32_t i = 0; i < m_missions.active().size();) {
        const std::size_t countBefore = m_missions.active().size();
        const sim::Mission& mission = m_missions.active()[i];
        const sim::MissionObjective& objective =
            mission.objectives[mission.currentObjective];
        const std::uint32_t commodity = objective.commodity;
        const std::string title = mission.title; // survives a completion
        const float available =
            commodity < m_playerCargo.size() ? m_playerCargo[commodity] : 0.0f;
        const float delivered =
            m_missions.recordDelivery(i, m_currentSystem, m_dockedStation, available);
        if (delivered > 0.0f) {
            m_playerCargo[commodity] -= delivered;
            if (market < m_economy.markets().size()) {
                m_economy.deliver(market, commodity, delivered); // fills the shortage
            }
            SOL_LOG_INFO("[missions] '%s': handed in %.0f units", title.c_str(),
                         static_cast<double>(delivered));
        }
        if (m_missions.active().size() == countBefore) {
            ++i;
        }
    }
}

void SpaceWorld::processMissionEvents()
{
    m_missionEventScratch.clear();
    m_missions.takeEvents(m_missionEventScratch);
    for (const sim::MissionEvent& event : m_missionEventScratch) {
        const sim::Mission& mission = event.mission;
        const bool posterValid = mission.poster < m_factionTable.size();
        switch (event.kind) {
        case sim::MissionEventKind::Accepted:
            SOL_LOG_INFO("[missions] accepted '%s': %s", mission.title.c_str(),
                         mission.objectives.front().text.c_str());
            break;
        case sim::MissionEventKind::ObjectiveComplete:
            if (event.objective + 1 < mission.objectives.size()) {
                SOL_LOG_INFO("[missions] '%s': %s", mission.title.c_str(),
                             mission.objectives[event.objective + 1].text.c_str());
            }
            break;
        case sim::MissionEventKind::Completed:
            m_playerCredits += mission.rewardCredits;
            if (posterValid) {
                m_factionSim.addStanding(mission.poster, mission.standingReward);
            }
            SOL_LOG_INFO("[missions] completed '%s': +%.0f cr, %s +%.1f rep",
                         mission.title.c_str(), mission.rewardCredits,
                         posterValid ? m_factionTable[mission.poster].name.c_str() : "?",
                         static_cast<double>(mission.standingReward));
            break;
        case sim::MissionEventKind::Failed:
        case sim::MissionEventKind::Abandoned:
            // Campaign missions charge nothing (decisions/008: the spine is
            // ignorable); procedural contracts dock standing with the poster.
            if (!mission.campaign() && posterValid) {
                m_factionSim.addStanding(mission.poster, -mission.standingPenalty);
            }
            SOL_LOG_WARN("[missions] %s '%s'%s",
                         event.kind == sim::MissionEventKind::Failed ? "failed"
                                                                     : "abandoned",
                         mission.title.c_str(),
                         mission.campaign() ? " (campaign: no penalty)" : "");
            break;
        }
        m_missionEvents.push_back(event);
    }
}

void SpaceWorld::takeMissionEvents(std::vector<sim::MissionEvent>& out)
{
    out.insert(out.end(), m_missionEvents.begin(), m_missionEvents.end());
    m_missionEvents.clear();
}

// --- Exploration & scanning (Phase 8e) ---------------------------------------

void SpaceWorld::initializeSurvey()
{
    m_surveyParams = sim::SurveyParams{};
    m_survey.initialize(m_galaxy, m_surveyParams,
                        static_cast<std::uint32_t>(m_commodityIds.size()), m_universeSeed);
    m_surveyEvents.clear();
    m_signals.clear();
    m_dynamicTargets.clear();
    m_pulseCooldown = 0.0;
    m_scanProgress = 0.0f;
    m_scanActive = false;
}

namespace {

// Defined with the outfitting helpers further down: salvaging a module runs
// through the same fit validation a purchase does.
[[nodiscard]] std::vector<const assets::ModuleDef*> fitModules(const assets::DefDatabase& defs,
                                                               const OwnedShip& ship);
[[nodiscard]] std::vector<const assets::CrewDef*> fitCrew(const assets::DefDatabase& defs,
                                                          const OwnedShip& ship);

std::string signalTargetName(const SignalInstance& signal, bool resolved, bool emptied,
                             std::size_t slot)
{
    if (!resolved) {
        return "Contact " + std::to_string(slot + 1);
    }
    std::string name =
        signal.kind == sim::SignalKind::Derelict ? "Derelict Hull" : "Supply Cache";
    if (emptied) {
        name += " (empty)";
    }
    return name;
}

} // namespace

void SpaceWorld::rebuildDynamicTargets()
{
    // Slots are append-only in discovery order: a scan in flight and the
    // player's target index both point into this tail, so nothing may shift
    // under them when a later pulse finds a lower-numbered site or a fight
    // leaves a new wreck. Only a slot whose object is gone (a wreck that
    // decayed) is removed, and the indices that pointed past it follow.
    auto hasSlot = [&](NavKind kind, std::uint32_t index) {
        for (const DynamicTarget& slot : m_dynamicTargets) {
            if (slot.kind == kind && slot.index == index) {
                return true;
            }
        }
        return false;
    };

    for (const SignalInstance& signal : m_signals) {
        if (m_survey.signalDiscovered(m_currentSystem, signal.index)
            && !hasSlot(NavKind::Signal, signal.index)) {
            m_dynamicTargets.push_back({.kind = NavKind::Signal, .index = signal.index});
        }
    }
    // Fields need no finding: an asteroid field is a visible thing in the
    // playfield, and it is the field you fly to, not the individual rock.
    for (std::uint32_t i = 0; i < m_fields.size(); ++i) {
        if (!hasSlot(NavKind::Field, i)) {
            m_dynamicTargets.push_back({.kind = NavKind::Field, .index = i});
        }
    }
    std::vector<std::uint32_t> wreckIds;
    m_mining.wrecksIn(m_currentSystem, wreckIds);
    for (const std::uint32_t id : wreckIds) {
        if (!hasSlot(NavKind::Wreck, id)) {
            m_dynamicTargets.push_back({.kind = NavKind::Wreck, .index = id});
        }
    }

    // Compact slots whose object is gone, carrying the player's selection and
    // any scan in flight with them.
    std::size_t write = 0;
    for (std::size_t read = 0; read < m_dynamicTargets.size(); ++read) {
        const DynamicTarget& slot = m_dynamicTargets[read];
        const bool alive =
            slot.kind == NavKind::Signal   ? slot.index < m_signals.size()
            : slot.kind == NavKind::Field  ? slot.index < m_fields.size()
                                           : m_mining.wreck(slot.index) != nullptr;
        if (!alive) {
            const std::size_t removed = m_signalTargetBase + write;
            if (m_targetIndex > removed) {
                --m_targetIndex;
            }
            if (m_scanActive && m_scanTarget > removed) {
                --m_scanTarget;
            }
            continue;
        }
        m_dynamicTargets[write++] = slot;
    }
    m_dynamicTargets.resize(write);

    if (m_targets.size() > m_signalTargetBase) {
        m_targets.resize(m_signalTargetBase);
    }
    std::size_t signalSlot = 0;
    for (const DynamicTarget& slot : m_dynamicTargets) {
        switch (slot.kind) {
        case NavKind::Signal: {
            const SignalInstance& signal = m_signals[slot.index];
            m_targets.push_back(
                {.name = signalTargetName(signal,
                                          m_survey.signalResolved(m_currentSystem, slot.index),
                                          m_survey.signalEmptied(m_currentSystem, slot.index),
                                          signalSlot++),
                 .position = signal.position,
                 .surfaceRadius = 0.0});
            break;
        }
        case NavKind::Field: {
            const sim::AsteroidFieldSpec& field = m_fields[slot.index];
            m_targets.push_back({.name = "Asteroid Field " + std::to_string(slot.index + 1),
                                 .position = field.center,
                                 .surfaceRadius = 0.0});
            break;
        }
        default: {
            const sim::WreckRecord* wreck = m_mining.wreck(slot.index);
            m_targets.push_back({.name = "Wreck: " + wreck->name,
                                 .position = wreck->position,
                                 .surfaceRadius = 0.0});
            break;
        }
        }
    }
}

std::uint32_t SpaceWorld::targetSignalIndex() const
{
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    if (total == 0) {
        return kNoIndex;
    }
    const std::size_t index = m_targetIndex % total;
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slot.kind == NavKind::Signal ? slot.index : kNoIndex;
}

std::uint32_t SpaceWorld::targetBodyIndex() const
{
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    if (total == 0) {
        return kNoIndex;
    }
    const std::size_t index = m_targetIndex % total;
    if (index == m_starTargetIndex) {
        return 0; // body 0 is the star, matching SurveySim's numbering
    }
    if (index >= m_planetTargetBase && index < m_planetTargetBase + m_planets.size()) {
        return static_cast<std::uint32_t>(index - m_planetTargetBase + 1);
    }
    return kNoIndex;
}

float SpaceWorld::pulseCharge() const
{
    if (m_pulseCooldown <= 0.0) {
        return 1.0f;
    }
    return static_cast<float>(1.0 - m_pulseCooldown / kPulseCooldownSeconds);
}

const char* SpaceWorld::scanTargetName() const
{
    if (!m_scanActive || m_scanTarget >= m_targets.size()) {
        return "";
    }
    return m_targets[m_scanTarget].name.c_str();
}

int SpaceWorld::pulseScan()
{
    if (isDocked() || m_pulseCooldown > 0.0) {
        return -1;
    }
    m_pulseCooldown = kPulseCooldownSeconds;
    const core::DVec3 position = shipState().position;
    const double range = static_cast<double>(m_scanRange);
    int found = 0;
    for (const SignalInstance& signal : m_signals) {
        if (m_survey.signalDiscovered(m_currentSystem, signal.index)
            || length(signal.position - position) > range) {
            continue;
        }
        if (m_survey.notifySignalDiscovered(m_currentSystem, signal.index)) {
            ++found;
            m_surveyEvents.push_back({.kind = SurveyEvent::Kind::SignalDiscovered,
                                      .system = m_currentSystem,
                                      .index = signal.index,
                                      .signalKind = signal.kind,
                                      .seed = signal.seed,
                                      .name = "contact"});
        }
    }
    if (found > 0) {
        rebuildDynamicTargets();
    }
    SOL_LOG_INFO("scan pulse: %d new contact(s) within %.0f km", found, range / 1000.0);
    return found;
}

void SpaceWorld::tickScanning(double dt)
{
    if (m_pulseCooldown > 0.0) {
        m_pulseCooldown -= dt;
        if (m_pulseCooldown < 0.0) {
            m_pulseCooldown = 0.0;
        }
    }
    const auto stopScan = [&]() {
        m_scanActive = false;
        m_scanProgress = 0.0f;
    };
    if (isDocked()) {
        stopScan();
        return;
    }
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    if (total == 0) {
        stopScan();
        return;
    }
    const std::size_t index = m_targetIndex % total;
    const std::uint32_t signalIndex = targetSignalIndex();
    const std::uint32_t bodyIndex = targetBodyIndex();
    const bool scannableSignal =
        signalIndex != kNoIndex && !m_survey.signalResolved(m_currentSystem, signalIndex);
    const bool scannableBody =
        bodyIndex != kNoIndex && !m_survey.bodyScanned(m_currentSystem, bodyIndex);
    if (!scannableSignal && !scannableBody) {
        stopScan();
        return;
    }

    const sim::ShipState state = shipState();
    const core::DVec3 toTarget = m_targets[index].position - state.position;
    const double distance = length(toTarget);
    // Sites must be approached; bodies are read at whatever range they sit at
    // (they are AU-scale scenery — a survey scan of a planet is a telescope
    // pointed at it, not a flyby).
    if (scannableSignal && distance > targetScanRange()) {
        stopScan();
        return;
    }
    if (distance > 1.0) {
        const core::Vec3 forwardF = rotate(state.orientation, core::Vec3{0.0f, 0.0f, -1.0f});
        const core::DVec3 direction = toTarget * (1.0 / distance);
        const double aim = static_cast<double>(forwardF.x) * direction.x
                           + static_cast<double>(forwardF.y) * direction.y
                           + static_cast<double>(forwardF.z) * direction.z;
        if (aim < kScanConeCosine) {
            stopScan(); // scanning is a held aim, not a checkbox
            return;
        }
    }

    if (!m_scanActive || m_scanTarget != index) {
        m_scanTarget = index;
        m_scanActive = true;
        m_scanProgress = 0.0f;
    }
    m_scanProgress +=
        static_cast<float>(dt * static_cast<double>(m_scanSpeed) / kTargetScanSeconds);
    if (m_scanProgress < 1.0f) {
        return;
    }
    stopScan();
    if (scannableSignal) {
        if (m_survey.notifySignalResolved(m_galaxy, m_currentSystem, signalIndex)) {
            const SignalInstance& signal = m_signals[signalIndex];
            // Scriptless default first, so a site always holds something even
            // if no script answers; the Lua hook may replace it this frame.
            (void)m_survey.setLoot(m_currentSystem, signalIndex, defaultLoot(signal));
            m_surveyEvents.push_back({.kind = SurveyEvent::Kind::SignalResolved,
                                      .system = m_currentSystem,
                                      .index = signalIndex,
                                      .signalKind = signal.kind,
                                      .seed = signal.seed,
                                      .name = sim::signalKindName(signal.kind)});
            rebuildDynamicTargets();
            SOL_LOG_INFO("scan resolved: %s", sim::signalKindName(signal.kind));
        }
        return;
    }
    if (m_survey.notifyBodyScanned(m_galaxy, m_currentSystem, bodyIndex)) {
        m_surveyEvents.push_back({.kind = SurveyEvent::Kind::BodyScanned,
                                  .system = m_currentSystem,
                                  .index = bodyIndex,
                                  .signalKind = sim::SignalKind::Derelict,
                                  .seed = 0,
                                  .name = m_targets[index].name});
        SOL_LOG_INFO("scan resolved: %s", m_targets[index].name.c_str());
    }
}

bool SpaceWorld::scanCurrentTarget()
{
    const std::uint32_t signalIndex = targetSignalIndex();
    if (signalIndex != kNoIndex) {
        if (!m_survey.notifySignalResolved(m_galaxy, m_currentSystem, signalIndex)) {
            return false;
        }
        const SignalInstance& signal = m_signals[signalIndex];
        (void)m_survey.setLoot(m_currentSystem, signalIndex, defaultLoot(signal));
        m_surveyEvents.push_back({.kind = SurveyEvent::Kind::SignalResolved,
                                  .system = m_currentSystem,
                                  .index = signalIndex,
                                  .signalKind = signal.kind,
                                  .seed = signal.seed,
                                  .name = sim::signalKindName(signal.kind)});
        rebuildDynamicTargets();
        return true;
    }
    const std::uint32_t bodyIndex = targetBodyIndex();
    if (bodyIndex == kNoIndex || !m_survey.notifyBodyScanned(m_galaxy, m_currentSystem, bodyIndex)) {
        return false;
    }
    m_surveyEvents.push_back({.kind = SurveyEvent::Kind::BodyScanned,
                              .system = m_currentSystem,
                              .index = bodyIndex,
                              .signalKind = sim::SignalKind::Derelict,
                              .seed = 0,
                              .name = ""});
    return true;
}

sim::SignalLoot SpaceWorld::defaultLoot(const SignalInstance& signal) const
{
    sim::SignalLoot loot;
    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_commodityIds.size());
    if (commodityCount == 0) {
        return loot;
    }
    core::Rng rng(signal.seed, 7);
    const std::uint32_t stacks = signal.kind == sim::SignalKind::Derelict ? 1 + rng.range(2) : 1;
    for (std::uint32_t i = 0; i < stacks; ++i) {
        loot.cargo.push_back({.commodity = rng.range(commodityCount),
                              .units = static_cast<float>(5 + rng.range(21))});
    }
    if (signal.kind == sim::SignalKind::Cache) {
        loot.credits = 200.0 + 1'000.0 * rng.nextDouble01();
    } else if (m_defs != nullptr && !m_defs->modules().empty() && rng.nextFloat01() < 0.25f) {
        const std::vector<assets::ModuleDef>& modules = m_defs->modules();
        loot.moduleId = modules[rng.range(static_cast<std::uint32_t>(modules.size()))].id;
    }
    return loot;
}

bool SpaceWorld::applySignalLoot(std::uint32_t system, std::uint32_t signal, sim::SignalLoot loot)
{
    return m_survey.setLoot(system, signal, std::move(loot));
}

void SpaceWorld::takeSurveyEvents(std::vector<SurveyEvent>& out)
{
    out.insert(out.end(), m_surveyEvents.begin(), m_surveyEvents.end());
    m_surveyEvents.clear();
}

double SpaceWorld::nearestSalvageDistance() const
{
    if (isDocked()) {
        return -1.0;
    }
    const core::DVec3 position = shipState().position;
    double nearest = -1.0;
    for (const SignalInstance& signal : m_signals) {
        if (!m_survey.signalResolved(m_currentSystem, signal.index)
            || m_survey.signalEmptied(m_currentSystem, signal.index)) {
            continue;
        }
        const double distance = length(signal.position - position);
        if (nearest < 0.0 || distance < nearest) {
            nearest = distance;
        }
    }
    return nearest;
}

bool SpaceWorld::trySalvageNearest(double range)
{
    if (isDocked()) {
        return false;
    }
    const core::DVec3 position = shipState().position;
    const SignalInstance* best = nullptr;
    double bestDistance = range;
    for (const SignalInstance& signal : m_signals) {
        if (!m_survey.signalResolved(m_currentSystem, signal.index)
            || m_survey.signalEmptied(m_currentSystem, signal.index)) {
            continue;
        }
        const double distance = length(signal.position - position);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = &signal;
        }
    }
    if (best == nullptr) {
        return false;
    }
    const sim::SignalLoot* stored = m_survey.loot(m_currentSystem, best->index);
    sim::SignalLoot remaining = stored != nullptr ? *stored : sim::SignalLoot{};

    const double credits = remaining.credits;
    m_playerCredits += credits;
    remaining.credits = 0.0;

    float unitsTaken = 0.0f;
    std::vector<sim::SignalCargo> left;
    for (const sim::SignalCargo& stack : remaining.cargo) {
        const float space = m_playerCargoCapacity - playerCargoTotal();
        const float take = std::min(stack.units, space > 0.0f ? space : 0.0f);
        if (take > 0.0f && stack.commodity < m_playerCargo.size()) {
            m_playerCargo[stack.commodity] += take;
            unitsTaken += take;
        }
        if (stack.units - take > 0.001f) {
            left.push_back({.commodity = stack.commodity, .units = stack.units - take});
        }
    }
    remaining.cargo = std::move(left);

    std::string moduleTaken;
    if (tryFitSalvagedModule(remaining.moduleId, moduleTaken)) {
        remaining.moduleId.clear();
    }

    const bool empty = remaining.cargo.empty() && remaining.moduleId.empty();
    if (empty) {
        (void)m_survey.notifySignalEmptied(m_currentSystem, best->index);
    } else {
        (void)m_survey.setLoot(m_currentSystem, best->index, remaining);
    }
    rebuildDynamicTargets();
    SOL_LOG_INFO("salvaged %s: %.0f units, %.0f cr%s%s", sim::signalKindName(best->kind),
                 static_cast<double>(unitsTaken), credits,
                 moduleTaken.empty() ? "" : ", fitted ", moduleTaken.c_str());
    if (!empty) {
        SOL_LOG_INFO("salvage: no room for the rest - it stays aboard the wreck");
    }
    return true;
}

double SpaceWorld::sellSurveyData()
{
    if (!isDocked()) {
        return 0.0;
    }
    const double credits = m_survey.sellLedger();
    if (credits <= 0.0) {
        return 0.0;
    }
    m_playerCredits += credits;
    // Data is commerce: the buyer's faction warms to you like any trade.
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner < m_factionTable.size()) {
        m_factionSim.addStanding(owner, static_cast<float>(credits * kSurveyStandingRate));
    }
    SOL_LOG_INFO("sold survey data for %.0f cr", credits);
    return credits;
}

bool SpaceWorld::plotRoute(std::uint32_t destination)
{
    if (destination >= m_galaxy.systems.size() || destination == m_currentSystem) {
        m_survey.clearRoute();
        return false;
    }
    m_survey.setRoute(sim::routeBetween(m_galaxy, m_currentSystem, destination));
    return !m_survey.route().empty();
}

// --- Mining, salvage & refining (Phase 8f) -----------------------------------

namespace {

// Display name for a commodity id, falling back to the id when the def has
// gone (a mod may drop a commodity a save still carries in the hold).
[[nodiscard]] std::string commodityDisplayName(const assets::DefDatabase& defs,
                                               const std::string& id)
{
    const assets::CommodityDef* def = defs.findCommodity(id.c_str());
    return def != nullptr ? def->name : id;
}

} // namespace

bool SpaceWorld::tryFitSalvagedModule(const std::string& moduleId, std::string& outName)
{
    outName.clear();
    if (moduleId.empty() || m_defs == nullptr || m_fleet.empty()) {
        return false;
    }
    const assets::ModuleDef* module = m_defs->findModule(moduleId.c_str());
    const assets::ShipDef* base = m_defs->findShip(m_fleet[m_activeShip].defId.c_str());
    if (module == nullptr || base == nullptr) {
        return true; // the def is gone; there is nothing left to salvage
    }
    std::vector<const assets::ModuleDef*> modules = fitModules(*m_defs, m_fleet[m_activeShip]);
    modules.push_back(module);
    if (!assets::validateLoadout(*base, modules, fitCrew(*m_defs, m_fleet[m_activeShip]))) {
        return false; // no legal slot, power, or mass for it: it stays put
    }
    m_fleet[m_activeShip].moduleIds.push_back(module->id);
    applyActiveLoadout();
    outName = module->name;
    return true;
}

void SpaceWorld::ensureMiningPools()
{
    (void)m_registry.storage<MineableRock>();
    (void)m_registry.storage<WreckMarker>();
    (void)m_registry.storage<OreChunk>();
}

void SpaceWorld::initializeMining()
{
    ensureMiningPools();
    m_miningParams = sim::MiningParams{};
    m_miningParams.ores.clear();
    // What a rock can be made of is a data question: any commodity whose def
    // carries an ore weight is something the galaxy has deposits of.
    if (m_defs != nullptr) {
        for (std::uint32_t i = 0; i < m_commodityIds.size(); ++i) {
            const assets::CommodityDef* def = m_defs->findCommodity(m_commodityIds[i].c_str());
            if (def == nullptr
                || (def->oreWeightCore <= 0.0f && def->oreWeightFrontier <= 0.0f
                    && def->oreWeightFringe <= 0.0f)) {
                continue;
            }
            sim::OreEntry entry;
            entry.commodity = i;
            entry.weight[0] = def->oreWeightCore;
            entry.weight[1] = def->oreWeightFrontier;
            entry.weight[2] = def->oreWeightFringe;
            m_miningParams.ores.push_back(entry);
        }
    }
    m_mining.initialize(m_galaxy, m_miningParams,
                        static_cast<std::uint32_t>(m_commodityIds.size()), m_universeSeed);
    m_fields.clear();
    m_wreckEvents.clear();
    m_rockEvents.clear();
    m_collectTicker = 0.0f;
    m_collectTickerAge = 0.0;
    m_collectName.clear();
}

void SpaceWorld::instantiateMiningEntities()
{
    std::vector<sim::RockSpec> rocks;
    for (std::uint32_t field = 0; field < m_fields.size(); ++field) {
        m_mining.rocksFor(m_galaxy, m_currentSystem, field, rocks);
        for (std::uint32_t index = 0; index < rocks.size(); ++index) {
            const sim::RockSpec& rock = rocks[index];
            if (m_mining.unitsLeft(m_currentSystem, field, index, rock.yieldUnits) <= 0.0f) {
                continue; // cut to nothing on an earlier visit; it broke up
            }
            const ecs::Entity entity = m_registry.create();
            m_registry.emplace<Transform>(entity, Transform{.position = rock.position,
                                                            .previousPosition = rock.position});
            const float scale = static_cast<float>(rock.radius);
            m_registry.emplace<RenderShape>(
                entity, RenderShape{.scale = {scale, scale, scale}, .model = ModelId::Asteroid});
            m_registry.emplace<MineableRock>(entity,
                                             MineableRock{.field = field,
                                                          .index = index,
                                                          .commodity = rock.commodity,
                                                          .totalUnits = rock.yieldUnits,
                                                          .tumbleAxis = rock.tumbleAxis,
                                                          .tumbleRate = rock.tumbleRate});
        }
    }
}

void SpaceWorld::spawnCutChunk(const core::DVec3& origin, double surface,
                               std::uint32_t commodity, float units)
{
    // Ore breaks off *toward the beam* — with a spread, but not at random.
    // Scattering it evenly means most of what you cut simply leaves, and the
    // loop becomes chasing debris rather than mining.
    const core::DVec3 toShip = shipState().position - origin;
    const double distance = length(toShip);
    core::DVec3 direction = distance > 1.0 ? toShip * (1.0 / distance)
                                           : sim::randomPlayfieldDirection(m_chunkRng);
    direction = direction + sim::randomPlayfieldDirection(m_chunkRng) * kChunkSpread;
    const double spread = length(direction);
    direction = spread > 1.0e-6 ? direction * (1.0 / spread) : core::DVec3{0.0, 0.0, 1.0};
    spawnOreChunk(origin + direction * surface,
                  direction * (kChunkDriftSpeed * (0.6 + 0.8 * m_chunkRng.nextDouble01())),
                  commodity, units);
}

void SpaceWorld::spawnOreChunk(const core::DVec3& position, const core::DVec3& velocity,
                               std::uint32_t commodity, float units)
{
    const ecs::Entity entity = m_registry.create();
    m_registry.emplace<Transform>(
        entity, Transform{.position = position, .previousPosition = position});
    m_registry.emplace<RenderShape>(
        entity, RenderShape{.scale = {6.0f, 6.0f, 6.0f}, .model = ModelId::Cube});
    m_registry.emplace<OreChunk>(entity, OreChunk{.velocity = velocity,
                                                  .lifetime = kChunkLifetimeSeconds,
                                                  .commodity = commodity,
                                                  .units = units});
}

float SpaceWorld::cutRock(std::uint32_t entityIndex, float units)
{
    MineableRock* rock = m_registry.storage<MineableRock>().tryGet(entityIndex);
    if (rock == nullptr) {
        return 0.0f;
    }
    const float taken = m_mining.mineRock(m_currentSystem, rock->field, rock->index,
                                          rock->totalUnits, units);
    if (taken <= 0.0f) {
        return 0.0f;
    }
    // What comes off drifts: the beam breaks the rock, the ship still has to
    // go and get it. Chunks are capped so a fat bite arrives as several.
    const core::DVec3 origin = m_registry.storage<Transform>().get(entityIndex).position;
    const double surface =
        static_cast<double>(m_registry.storage<RenderShape>().get(entityIndex).scale.x);
    float remaining = taken;
    while (remaining > 0.0f) {
        const float chunk = std::min(remaining, kChunkUnitCeiling);
        remaining -= chunk;
        spawnCutChunk(origin, surface, rock->commodity, chunk);
    }
    return taken;
}

float SpaceWorld::cutWreck(std::uint32_t entityIndex, float units)
{
    const WreckMarker* marker = m_registry.storage<WreckMarker>().tryGet(entityIndex);
    if (marker == nullptr) {
        return 0.0f;
    }
    std::uint32_t commodity = 0;
    const float taken = m_mining.cutWreckCargo(marker->id, units, &commodity);
    const core::DVec3 origin = m_registry.storage<Transform>().get(entityIndex).position;
    if (taken > 0.0f) {
        float remaining = taken;
        while (remaining > 0.0f) {
            const float chunk = std::min(remaining, kChunkUnitCeiling);
            remaining -= chunk;
            spawnCutChunk(origin, 25.0, commodity, chunk);
        }
        return taken;
    }

    // Nothing left to cut loose: the hull gives up what it was carrying that
    // does not float — credits and, if it fits, a module off its own mounts.
    const sim::WreckRecord* wreck = m_mining.wreck(marker->id);
    if (wreck == nullptr) {
        return 0.0f;
    }
    const double credits = wreck->contents.credits;
    const std::string moduleId = wreck->contents.moduleId;
    const std::string name = wreck->name;
    m_playerCredits += credits;
    std::string moduleTaken;
    (void)tryFitSalvagedModule(moduleId, moduleTaken);
    (void)m_mining.removeWreck(marker->id);
    SOL_LOG_INFO("cut open the wreck of %s: %.0f cr%s%s", name.c_str(), credits,
                 moduleTaken.empty() ? "" : ", fitted ", moduleTaken.c_str());
    return 0.0f;
}

std::uint32_t SpaceWorld::entityAhead(double range, bool& outIsWreck) const
{
    outIsWreck = false;
    if (isDocked() || range <= 0.0) {
        return kNoIndex;
    }
    const sim::ShipState state = shipState();
    const core::Vec3 forward = rotate(state.orientation, core::Vec3{0.0f, 0.0f, -1.0f});
    const core::DVec3 muzzle = state.position;
    const core::DVec3 beamEnd = muzzle + toDVec3(forward) * range;

    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
    const ecs::Pool<MineableRock>& rocks = m_registry.storage<MineableRock>();
    const ecs::Pool<WreckMarker>& wrecks = m_registry.storage<WreckMarker>();
    double bestT = 2.0;
    std::uint32_t best = kNoIndex;
    const auto sweep = [&](std::uint32_t entityIndex, bool isWreck) {
        const RenderShape& shape = shapes.get(entityIndex);
        const double radius = modelBaseRadius(shape.model) * static_cast<double>(shape.scale.x);
        double hitT = 0.0;
        if (sim::segmentHitsSphere(muzzle, beamEnd, transforms.get(entityIndex).position, radius,
                                   hitT)
            && hitT < bestT) {
            bestT = hitT;
            best = entityIndex;
            outIsWreck = isWreck;
        }
    };
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        sweep(rocks.entityIndices()[i], false);
    }
    for (std::size_t i = 0; i < wrecks.size(); ++i) {
        sweep(wrecks.entityIndices()[i], true);
    }
    return best;
}

ProspectInfo SpaceWorld::prospectAhead() const
{
    ProspectInfo info;
    // The scanner is what lets you read a rock at a distance; the beam is what
    // lets you cut it. Reading further than you can cut is the point.
    const double readRange = std::max(targetScanRange(), 5'000.0);
    bool isWreck = false;
    const std::uint32_t entityIndex = entityAhead(readRange, isWreck);
    if (entityIndex == kNoIndex) {
        return info;
    }
    info.valid = true;
    info.wreck = isWreck;
    info.distance =
        length(m_registry.storage<Transform>().get(entityIndex).position - shipState().position);
    const ShipWeapon& weapon = playerWeapon();
    info.inRange = weapon.miningPower > 0.0f
                   && info.distance <= static_cast<double>(weapon.range);
    if (isWreck) {
        const WreckMarker& marker = m_registry.storage<WreckMarker>().get(entityIndex);
        const sim::WreckRecord* wreck = m_mining.wreck(marker.id);
        if (wreck == nullptr) {
            info.valid = false;
            return info;
        }
        info.name = wreck->name;
        for (const sim::SignalCargo& cargo : wreck->contents.cargo) {
            info.unitsLeft += cargo.units;
        }
        info.unitsTotal = info.unitsLeft;
        return info;
    }
    const MineableRock& rock = m_registry.storage<MineableRock>().get(entityIndex);
    info.name = rock.commodity < m_commodityIds.size() && m_defs != nullptr
                    ? commodityDisplayName(*m_defs, m_commodityIds[rock.commodity])
                    : "Ore";
    info.unitsTotal = rock.totalUnits;
    info.unitsLeft = m_mining.unitsLeft(m_currentSystem, rock.field, rock.index, rock.totalUnits);
    return info;
}

bool SpaceWorld::mineAhead()
{
    bool isWreck = false;
    const std::uint32_t entityIndex = entityAhead(std::max(targetScanRange(), 5'000.0), isWreck);
    if (entityIndex == kNoIndex) {
        return false;
    }
    // Dev path: one press empties what the beam would take a while to grind.
    if (isWreck) {
        (void)cutWreck(entityIndex, 1.0e6f);
        return true;
    }
    const MineableRock rock = m_registry.storage<MineableRock>().get(entityIndex);
    if (cutRock(entityIndex, 1.0e6f) <= 0.0f) {
        return false;
    }
    m_registry.destroy(m_registry.entityFromIndex(entityIndex));
    m_rockEvents.push_back({.commodity = rock.commodity, .units = rock.totalUnits});
    return true;
}

bool SpaceWorld::warpToNearestRock()
{
    if (isDocked()) {
        return false;
    }
    const core::DVec3 position = shipState().position;
    const ecs::Pool<MineableRock>& rocks = m_registry.storage<MineableRock>();
    const ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    const ecs::Pool<RenderShape>& shapes = m_registry.storage<RenderShape>();
    std::uint32_t best = kNoIndex;
    double bestDistance = 0.0;
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        const std::uint32_t entityIndex = rocks.entityIndices()[i];
        const double distance = length(transforms.get(entityIndex).position - position);
        if (best == kNoIndex || distance < bestDistance) {
            best = entityIndex;
            bestDistance = distance;
        }
    }
    if (best == kNoIndex) {
        return false;
    }
    const core::DVec3 target = transforms.get(best).position;
    const double standoff =
        static_cast<double>(shapes.get(best).scale.x) + 400.0; // clear of the hull
    // Approach from wherever the ship already is, so the parked view looks
    // like an arrival rather than a fixed camera angle.
    core::DVec3 approach = position - target;
    const double length2 = length(approach);
    approach = length2 > 1.0 ? approach * (1.0 / length2) : core::DVec3{0.0, 0.0, 1.0};
    const core::DVec3 parked = target + approach * standoff;

    m_autopilotActive = false;
    Transform& transform = m_registry.storage<Transform>().get(playerEntityIndex());
    transform.position = parked;
    transform.previousPosition = parked;
    // Point the nose at the rock: the rotation taking -Z to the look vector.
    const core::Vec3 look = toVec3(approach * -1.0);
    const core::Vec3 nose{0.0f, 0.0f, -1.0f};
    const float alignment = dot(look, nose);
    core::Quat orientation = core::Quat::identity();
    if (alignment < 0.9999f) {
        core::Vec3 axis = cross(nose, look);
        if (length(axis) < 1.0e-5f) {
            axis = {0.0f, 1.0f, 0.0f}; // exactly behind: any perpendicular does
        }
        orientation = core::fromAxisAngle(normalize(axis), std::acos(std::clamp(alignment, -1.0f, 1.0f)));
    }
    transform.orientation = orientation;
    transform.previousOrientation = orientation;
    m_registry.storage<FlightBody>().get(playerEntityIndex()) = FlightBody{};
    SOL_LOG_WARN("dev warp: parked %.0f m off a rock", standoff);
    return true;
}

bool SpaceWorld::applyWreckLoot(std::uint32_t id, sim::SignalLoot loot)
{
    return m_mining.setWreckContents(id, std::move(loot));
}

void SpaceWorld::takeWreckEvents(std::vector<WreckEvent>& out)
{
    out.insert(out.end(), m_wreckEvents.begin(), m_wreckEvents.end());
    m_wreckEvents.clear();
}

void SpaceWorld::takeRockEvents(std::vector<RockEvent>& out)
{
    out.insert(out.end(), m_rockEvents.begin(), m_rockEvents.end());
    m_rockEvents.clear();
}

sim::SignalLoot SpaceWorld::defaultWreckLoot(const assets::ShipDef* def, std::uint64_t seed) const
{
    sim::SignalLoot loot;
    const std::uint32_t commodityCount = static_cast<std::uint32_t>(m_commodityIds.size());
    if (commodityCount == 0) {
        return loot;
    }
    core::Rng rng(seed, 11);
    // Scrap: the hull itself, as ore, scaled by how big the ship was.
    const float hullScrap =
        def != nullptr ? std::max(4.0f, def->mass * 0.0016f) : 8.0f;
    const std::uint32_t ore = commodityIndex("sol.ore");
    loot.cargo.push_back({.commodity = ore < commodityCount ? ore : 0,
                          .units = hullScrap * (0.7f + 0.6f * rng.nextFloat01())});
    // Whatever it was hauling, sometimes.
    if (rng.nextFloat01() < 0.5f) {
        loot.cargo.push_back({.commodity = rng.range(commodityCount),
                              .units = static_cast<float>(3 + rng.range(15))});
    }
    loot.credits = 40.0 + 260.0 * rng.nextDouble01();
    // Its own hardware, at salvage odds: the gun or a module off its mounts.
    if (def != nullptr && !def->weaponId.empty() && rng.nextFloat01() < 0.2f && m_defs != nullptr
        && !m_defs->modules().empty()) {
        const std::vector<assets::ModuleDef>& modules = m_defs->modules();
        loot.moduleId = modules[rng.range(static_cast<std::uint32_t>(modules.size()))].id;
    }
    return loot;
}

bool SpaceWorld::dockedRefinePair(std::uint32_t& input, std::uint32_t& output) const
{
    input = kNoIndex;
    output = kNoIndex;
    if (!isDocked() || m_defs == nullptr || m_currentSystem >= m_galaxy.systems.size()) {
        return false;
    }
    const sim::SystemSpec& spec = m_galaxy.systems[m_currentSystem];
    if (m_dockedStation >= spec.stations.size()) {
        return false;
    }
    const std::vector<assets::StationDef>& stations = m_defs->stations();
    const std::uint32_t archetype = spec.stations[m_dockedStation].archetype;
    if (archetype >= stations.size()) {
        return false;
    }
    const assets::StationDef& def = stations[archetype];
    if (def.refineInput.empty() || def.refineOutput.empty()) {
        return false; // this archetype offers no refining service
    }
    const std::uint32_t in = commodityIndex(def.refineInput.c_str());
    const std::uint32_t out = commodityIndex(def.refineOutput.c_str());
    if (in == kNoIndex || out == kNoIndex) {
        return false; // the commodity defs went away under the station def
    }
    input = in;
    output = out;
    return true;
}

bool SpaceWorld::dockedStationRefines() const
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    return dockedRefinePair(input, output);
}

std::uint32_t SpaceWorld::refineInputCommodity() const
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    (void)dockedRefinePair(input, output);
    return input;
}

std::uint32_t SpaceWorld::refineOutputCommodity() const
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    (void)dockedRefinePair(input, output);
    return output;
}

float SpaceWorld::refinedReadyHere() const
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    if (!dockedRefinePair(input, output)) {
        return 0.0f;
    }
    return m_mining.readyAt(dockedMarket(), output);
}

double SpaceWorld::refineWaitHere() const
{
    if (!dockedStationRefines()) {
        return -1.0;
    }
    return m_mining.soonestAt(dockedMarket());
}

float SpaceWorld::MiningFeedstock::draw(std::uint32_t market, std::uint32_t commodity,
                                        float units)
{
    if (mining == nullptr || galaxy == nullptr || economy == nullptr
        || market >= economy->markets().size()) {
        return 0.0f;
    }
    // An outpost works the rock in its own system and nowhere else, which is
    // what makes "where does ore come from" a question the map can answer.
    const std::uint32_t system = economy->markets()[market].systemIndex;
    return mining->drawFromSystem(*galaxy, system, commodity, units);
}

double SpaceWorld::intelPrice() const
{
    // Priced off how much is actually out there to learn, so a package in a
    // dense core cluster costs more than one on the frontier and a system
    // with nothing in reach is nearly free.
    return kIntelBasePrice + kIntelPricePerMarket * static_cast<double>(intelMarketCount());
}

std::uint32_t SpaceWorld::intelMarketCount() const
{
    if (!isDocked()) {
        return 0;
    }
    std::vector<std::uint8_t> hops;
    hopsFrom(m_galaxy, m_currentSystem, kIntelJumpRadius, hops);
    std::uint32_t count = 0;
    for (const sim::StationMarket& market : m_economy.markets()) {
        const std::uint32_t system = market.systemIndex;
        // You are standing in the local one; that reading is always free.
        if (system != m_currentSystem && hops[system] != kUnreachableHops) {
            ++count;
        }
    }
    return count;
}

bool SpaceWorld::buyMarketIntel(std::string* outError)
{
    if (!isDocked()) {
        return refuse("not docked", outError);
    }
    const std::uint32_t count = intelMarketCount();
    if (count == 0) {
        return refuse("no markets in reach to report on", outError);
    }
    const double price = intelPrice();
    if (price > m_playerCredits) {
        return refuse("cannot afford the market report", outError);
    }
    m_playerCredits -= price;

    std::vector<std::uint8_t> hops;
    hopsFrom(m_galaxy, m_currentSystem, kIntelJumpRadius, hops);
    std::vector<float> prices(m_commodityIds.size(), 0.0f);
    for (std::uint32_t m = 0; m < m_economy.markets().size(); ++m) {
        const std::uint32_t system = m_economy.markets()[m].systemIndex;
        if (system == m_currentSystem || hops[system] == kUnreachableHops) {
            continue;
        }
        for (std::uint32_t c = 0; c < prices.size(); ++c) {
            prices[c] = m_economy.price(m, c);
        }
        m_survey.recordMarket(m, prices, m_worldSeconds);
    }
    SOL_LOG_INFO("bought market data on %u markets within %u jumps for %.0f cr", count,
                 kIntelJumpRadius, price);
    return true;
}

void SpaceWorld::recordDockedMarket()
{
    const std::uint32_t market = dockedMarket();
    if (market == kNoIndex) {
        return;
    }
    std::vector<float> prices(m_commodityIds.size(), 0.0f);
    for (std::uint32_t c = 0; c < prices.size(); ++c) {
        prices[c] = m_economy.price(market, c);
    }
    m_survey.recordMarket(market, prices, m_worldSeconds);
}

bool SpaceWorld::bestKnownPrice(std::uint32_t commodity, std::uint32_t* outSystem, float* outPrice,
                                double* outAge, bool* outStale) const
{
    std::uint32_t market = 0;
    double age = 0.0;
    if (!m_survey.bestRemembered(commodity, dockedMarket(), m_worldSeconds, &market, outPrice,
                                 &age)) {
        return false;
    }
    if (outSystem != nullptr) {
        *outSystem = m_economy.markets()[market].systemIndex;
    }
    if (outAge != nullptr) {
        *outAge = age;
    }
    if (outStale != nullptr) {
        *outStale = age > m_survey.params().intelStaleSeconds;
    }
    return true;
}

float SpaceWorld::marketSatisfaction(std::uint32_t market) const
{
    return m_economy.satisfaction(market);
}

const char* SpaceWorld::marketLimiting(std::uint32_t market) const
{
    const std::uint32_t commodity = m_economy.limitingCommodity(market);
    return commodity < m_commodityIds.size() ? m_commodityIds[commodity].c_str() : "";
}

bool SpaceWorld::orderRefine(float units, std::string* outError)
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    if (!dockedRefinePair(input, output)) {
        return refuse("no refinery here", outError);
    }
    const float available = playerCargo(input);
    const float order = std::min(units, available);
    if (!(order > 0.0f)) {
        return refuse("no ore aboard to refine", outError);
    }
    const double fee = m_mining.refineFee(order);
    if (fee > m_playerCredits) {
        return refuse("cannot afford the refining fee", outError);
    }
    if (!m_mining.startRefineJob(dockedMarket(), input, order, output)) {
        return refuse("the refinery queue is full", outError);
    }
    m_playerCargo[input] -= order;
    m_playerCredits -= fee;
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner < m_factionTable.size()) {
        m_factionSim.addStanding(owner, static_cast<float>(fee * kRefineStandingRate));
    }
    SOL_LOG_INFO("refining %.0f units for %.0f cr; ready in %.0f s", static_cast<double>(order),
                 fee, m_mining.refineDuration(order));
    return true;
}

float SpaceWorld::collectRefined()
{
    std::uint32_t input = kNoIndex;
    std::uint32_t output = kNoIndex;
    if (!dockedRefinePair(input, output)) {
        return 0.0f;
    }
    const float space = m_playerCargoCapacity - playerCargoTotal();
    if (!(space > 0.0f)) {
        SOL_LOG_WARN("no hold space for the refined metal; it waits here");
        return 0.0f;
    }
    const float taken = m_mining.collectAt(dockedMarket(), output, space);
    if (taken <= 0.0f) {
        return 0.0f;
    }
    m_playerCargo[output] += taken;
    SOL_LOG_INFO("collected %.0f units of refined output", static_cast<double>(taken));
    return taken;
}

void SpaceWorld::tickMining(double dt)
{
    // Coarse layer first: wrecks age and refinery orders cook whether the
    // player is watching them or three systems away (decisions/005).
    m_mining.tick(dt);

    // Rocks tumble. It is the cheapest thing that makes a field read as a
    // place rather than a diagram.
    ecs::Pool<MineableRock>& rocks = m_registry.storage<MineableRock>();
    ecs::Pool<Transform>& transforms = m_registry.storage<Transform>();
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        const MineableRock& rock = rocks.values()[i];
        Transform& transform = transforms.get(rocks.entityIndices()[i]);
        transform.previousOrientation = transform.orientation;
        transform.orientation = normalize(
            transform.orientation
            * core::fromAxisAngle(rock.tumbleAxis, rock.tumbleRate * static_cast<float>(dt)));
    }

    // Chunks drift, are gathered, or are lost.
    const core::DVec3 shipPosition = shipState().position;
    const double collectRange = static_cast<double>(m_collectorRange);
    float space = m_playerCargoCapacity - playerCargoTotal();
    ecs::Pool<OreChunk>& chunks = m_registry.storage<OreChunk>();
    std::vector<std::uint32_t> spent;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const std::uint32_t entityIndex = chunks.entityIndices()[i];
        OreChunk& chunk = chunks.values()[i];
        Transform& transform = transforms.get(entityIndex);
        transform.previousPosition = transform.position;
        transform.position = transform.position + chunk.velocity * dt;
        chunk.lifetime -= dt;
        if (chunk.lifetime <= 0.0) {
            spent.push_back(entityIndex);
            continue;
        }
        if (isDocked() || length(transform.position - shipPosition) > collectRange) {
            continue;
        }
        // A full hold cannot gather: the chunks keep drifting until they are
        // lost, which is the pressure that sends a miner home.
        const float take = std::min(chunk.units, space > 0.0f ? space : 0.0f);
        if (take <= 0.0f) {
            continue;
        }
        if (chunk.commodity < m_playerCargo.size()) {
            m_playerCargo[chunk.commodity] += take;
            space -= take;
            m_collectTicker += take;
            m_collectTickerAge = 0.0;
            m_collectName = m_defs != nullptr && chunk.commodity < m_commodityIds.size()
                                ? commodityDisplayName(*m_defs, m_commodityIds[chunk.commodity])
                                : "ore";
        }
        chunk.units -= take;
        if (chunk.units <= 0.001f) {
            spent.push_back(entityIndex);
        }
    }
    for (const std::uint32_t entityIndex : spent) {
        m_registry.destroy(m_registry.entityFromIndex(entityIndex));
    }

    // Reconcile wreck entities with the sim: newly killed ships get a hull to
    // cut, decayed and emptied ones stop being there. Done here rather than
    // in handleShipDestroyed so no pool changes shape mid-iteration.
    bool wrecksChanged = false;
    ecs::Pool<WreckMarker>& markers = m_registry.storage<WreckMarker>();
    std::vector<std::uint32_t> goneEntities;
    std::vector<std::uint32_t> present;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const std::uint32_t entityIndex = markers.entityIndices()[i];
        const std::uint32_t id = markers.values()[i].id;
        if (m_mining.wreck(id) == nullptr) {
            goneEntities.push_back(entityIndex);
        } else {
            present.push_back(id);
        }
    }
    for (const std::uint32_t entityIndex : goneEntities) {
        m_registry.destroy(m_registry.entityFromIndex(entityIndex));
        wrecksChanged = true;
    }
    std::vector<std::uint32_t> here;
    m_mining.wrecksIn(m_currentSystem, here);
    for (const std::uint32_t id : here) {
        if (std::find(present.begin(), present.end(), id) != present.end()) {
            continue;
        }
        const sim::WreckRecord* wreck = m_mining.wreck(id);
        const ecs::Entity entity = m_registry.create();
        m_registry.emplace<Transform>(entity, Transform{.position = wreck->position,
                                                        .previousPosition = wreck->position});
        // No wreck mesh yet: a dead hull is the ship model, oversized and
        // adrift. A proper broken hull is polish, not mechanism.
        m_registry.emplace<RenderShape>(
            entity, RenderShape{.scale = {1.4f, 1.4f, 1.4f}, .model = ModelId::Ship});
        m_registry.emplace<WreckMarker>(entity, WreckMarker{.id = id});
        wrecksChanged = true;
    }
    if (wrecksChanged) {
        rebuildDynamicTargets();
    }

    // The collection ticker is a HUD readout, not state: it fades.
    if (m_collectTicker > 0.0f) {
        m_collectTickerAge += dt;
        if (m_collectTickerAge > 2.0) {
            m_collectTicker = 0.0f;
            m_collectTickerAge = 0.0;
        }
    }
}

void SpaceWorld::spawnAmbientPilots(std::uint32_t systemIndex, const sim::SystemSpec& spec)
{
    if (m_defs == nullptr || m_factionTable.empty()) {
        return;
    }
    const core::DVec3 hub = spec.planets[spec.primaryPlanet].position;
    const auto spawnWing = [&](std::uint32_t faction, const std::vector<std::string>& roster,
                               PilotRole role, std::uint32_t count, const core::DVec3& anchor,
                               double spread) {
        if (roster.empty()) {
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const assets::ShipDef* def = m_defs->findShip(roster[i % roster.size()].c_str());
            if (def == nullptr) {
                SOL_LOG_WARN("ambient wing: no ship def '%s'", roster[i % roster.size()].c_str());
                return;
            }
            const core::DVec3 position =
                anchor + core::DVec3{spread * (1.0 + i), 300.0 + 250.0 * i,
                                     -spread * 0.5 * i};
            const ecs::Entity entity =
                spawnShipAt(*def, *m_defs, position, m_factionTable[faction].name.c_str());
            m_registry.emplace<ShipPilot>(entity,
                                          ShipPilot{.role = role, .factionIndex = faction});
        }
    };

    // Owner presence: patrol wings by region security for majors, resident
    // raider wings for clan systems.
    const std::uint32_t owner = spec.factionIndex;
    const core::DVec3 anchor = spec.stations.empty() ? hub : spec.stations[0].position;
    if (owner < m_factionTable.size()) {
        const GameFaction& faction = m_factionTable[owner];
        if (faction.pirate) {
            spawnWing(owner, faction.shipsRaider, PilotRole::Fighter, 2, anchor, 900.0);
        } else {
            constexpr std::uint32_t kPatrolsPerRegion[3] = {3, 2, 1}; // core/frontier/fringe
            spawnWing(owner, faction.shipsPatrol, PilotRole::Patrol,
                      kPatrolsPerRegion[static_cast<std::size_t>(spec.region)], anchor, 700.0);
        }
    }

    // Raid incursion: the last raider keeps ships in-system while the
    // intensity is warm (fresh raids read as an active raiding party).
    const float intensity = m_factionSim.raidIntensity(systemIndex);
    const std::uint32_t raider = m_factionSim.lastRaider(systemIndex);
    if (intensity >= 0.5f && raider < m_factionTable.size() && raider != owner) {
        const std::uint32_t count =
            std::min(3u, static_cast<std::uint32_t>(intensity + 0.5f));
        spawnWing(raider, m_factionTable[raider].shipsRaider, PilotRole::Fighter, count,
                  anchor + core::DVec3{9'000.0, 1'500.0, 6'000.0}, 1'200.0);
    }
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
    m_planetTargetBase = spec.stations.size() + m_gates.size();
    m_starTargetIndex = m_targets.size() - 1;
    m_signalTargetBase = m_targets.size();
    m_dynamicTargets.clear();
    m_targetIndex = 0;

    // Scannable sites (Phase 8e): content regenerates from the system seed;
    // which ones the player has found comes out of SurveySim.
    m_signals.clear();
    std::vector<sim::SignalSpec> signalSpecs;
    m_survey.signalsFor(m_galaxy, m_currentSystem, signalSpecs);
    for (std::uint32_t i = 0; i < signalSpecs.size(); ++i) {
        m_signals.push_back({.index = i,
                             .kind = signalSpecs[i].kind,
                             .position = signalSpecs[i].position,
                             .seed = signalSpecs[i].seed});
    }
    // Asteroid fields (Phase 8f) regenerate from the seed the same way, and
    // are known on sight — a field is a visible thing, not a contact.
    m_mining.fieldsFor(m_galaxy, m_currentSystem, m_fields);
    rebuildDynamicTargets();

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
    // Knowledge (Phase 8e): being here is what makes a system known, and a
    // gate names where it leads — the map grows along the lanes you fly.
    m_survey.notifyArrival(m_galaxy, systemIndex);
    m_dockedStation = kNoIndex;
    m_autopilotActive = false; // the target list is about to change under it
    const sim::SystemSpec& spec = m_galaxy.systems[systemIndex];
    instantiateSystemEntities(spec);
    rebuildSystemSideData(spec);
    // Rocks and wrecks (Phase 8f): rebuildSystemSideData has just refreshed
    // m_fields, and depletion decides which rocks are still there to spawn.
    instantiateMiningEntities();

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

    // Ambient faction presence (Phase 8b): owner wings + raid incursions.
    spawnAmbientPilots(systemIndex, spec);
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
    // Docking rights (Phase 8b): a hostile owner refuses the request. Death
    // respawn bypasses this path on purpose — dock stays the safe room.
    const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
    if (owner < m_factionTable.size() && m_factionSim.playerHostile(owner)) {
        SOL_LOG_WARN("docking denied at '%s': %s is hostile",
                     spec.stations[nearest].name.c_str(),
                     m_factionTable[owner].name.c_str());
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
    // Missions (Phase 8c): Dock objectives first, so a following Deliver at
    // this station can hand in on the same visit; the dock event tells
    // GameContent to re-open the board.
    m_missions.notifyDock(m_currentSystem, nearest);
    processMissionDeliveries();
    // Market intel (Phase 8g): standing on the pad is the one price reading
    // you never have to pay for, and it is what seeds the "elsewhere" column
    // on every other station's Trade tab.
    recordDockedMarket();
    m_dockEventPending = true;
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
    if (const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
        owner < m_factionTable.size()) {
        m_factionSim.recordTrade(owner, result.credits); // commerce goodwill
    }
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
    if (const std::uint32_t owner = systemOwnerFaction(m_currentSystem);
        owner < m_factionTable.size()) {
        m_factionSim.recordTrade(owner, result.credits);
    }
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
    m_defs = &defs;
    if (!m_fleet.empty()) {
        applyActiveLoadout();
    } else if (const assets::ShipDef* playerDef = defs.findShip(kPlayerShipDefId)) {
        // Pre-universe (fleet not initialized yet): raw starter def.
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

// --- Outfitting & fleet (Phase 8a) ---

namespace {

[[nodiscard]] std::vector<const assets::ModuleDef*> fitModules(const assets::DefDatabase& defs,
                                                               const OwnedShip& ship)
{
    std::vector<const assets::ModuleDef*> modules;
    modules.reserve(ship.moduleIds.size());
    for (const std::string& id : ship.moduleIds) {
        const assets::ModuleDef* module = defs.findModule(id.c_str());
        if (module == nullptr) {
            SOL_LOG_WARN("fit: module def '%s' missing; ignoring", id.c_str());
        }
        modules.push_back(module); // nulls are skipped by the loadout math
    }
    return modules;
}

[[nodiscard]] std::vector<const assets::CrewDef*> fitCrew(const assets::DefDatabase& defs,
                                                          const OwnedShip& ship)
{
    std::vector<const assets::CrewDef*> crew;
    crew.reserve(ship.crewIds.size());
    for (const std::string& id : ship.crewIds) {
        const assets::CrewDef* member = defs.findCrew(id.c_str());
        if (member == nullptr) {
            SOL_LOG_WARN("fit: crew def '%s' missing; ignoring", id.c_str());
        }
        crew.push_back(member);
    }
    return crew;
}

} // namespace

void SpaceWorld::resetFleetToStarter()
{
    m_fleet.clear();
    m_activeShip = 0;
    OwnedShip starter{.defId = kPlayerShipDefId};
    if (m_defs != nullptr) {
        if (const assets::ShipDef* def = m_defs->findShip(kPlayerShipDefId)) {
            starter.weaponId = def->weaponId;
        }
    }
    m_fleet.push_back(std::move(starter));
}

assets::ShipDef SpaceWorld::resolvedShipDef(const OwnedShip& ship) const
{
    if (m_defs == nullptr) {
        return assets::ShipDef{.id = ship.defId};
    }
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    if (base == nullptr) {
        SOL_LOG_WARN("fleet: ship def '%s' missing; using defaults", ship.defId.c_str());
        return assets::ShipDef{.id = ship.defId};
    }
    const std::vector<const assets::ModuleDef*> modules = fitModules(*m_defs, ship);
    const std::vector<const assets::CrewDef*> crew = fitCrew(*m_defs, ship);
    assets::ShipDef effective = assets::resolveLoadout(*base, modules, crew);
    effective.weaponId = ship.weaponId;
    return effective;
}

void SpaceWorld::applyActiveLoadout()
{
    if (m_defs == nullptr || m_fleet.empty()) {
        return;
    }
    applyShipDef(playerEntityIndex(), resolvedShipDef(activeShip()), *m_defs);
}

bool SpaceWorld::refuse(const std::string& reason, std::string* outError) const
{
    SOL_LOG_WARN("outfitting: %s", reason.c_str());
    if (outError != nullptr) {
        *outError = reason;
    }
    return false;
}

double SpaceWorld::shipValue(const OwnedShip& ship) const
{
    if (m_defs == nullptr) {
        return 0.0;
    }
    double value = 0.0;
    if (const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str())) {
        value += base->price;
    }
    for (const std::string& id : ship.moduleIds) {
        if (const assets::ModuleDef* module = m_defs->findModule(id.c_str())) {
            value += module->price;
        }
    }
    if (!ship.weaponId.empty()) {
        if (const assets::WeaponDef* weapon = m_defs->findWeapon(ship.weaponId.c_str())) {
            value += weapon->price;
        }
    }
    return value;
}

bool SpaceWorld::buyModule(const char* moduleId, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr || m_fleet.empty()) {
        return refuse("must be docked to refit", outError);
    }
    const assets::ModuleDef* module = m_defs->findModule(moduleId);
    if (module == nullptr) {
        return refuse(std::string("no module def '") + moduleId + "'", outError);
    }
    if (!stationSells(module->gate)) {
        return refuse("'" + module->name + "' is not sold here (faction catalog)", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    if (base == nullptr) {
        return refuse("active ship def '" + ship.defId + "' missing", outError);
    }
    std::vector<const assets::ModuleDef*> modules = fitModules(*m_defs, ship);
    modules.push_back(module);
    std::string reason;
    if (!assets::validateLoadout(*base, modules, fitCrew(*m_defs, ship), &reason)) {
        return refuse(reason, outError);
    }
    if (m_playerCredits < module->price) {
        return refuse("insufficient credits", outError);
    }
    m_playerCredits -= module->price;
    ship.moduleIds.push_back(module->id);
    applyActiveLoadout();
    SOL_LOG_INFO("fitted '%s' (-%.0f cr)", module->name.c_str(),
                 static_cast<double>(module->price));
    return true;
}

bool SpaceWorld::sellModule(const char* moduleId, std::string* outError)
{
    if (!isDocked() || m_fleet.empty()) {
        return refuse("must be docked to refit", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const auto it = std::find(ship.moduleIds.begin(), ship.moduleIds.end(), moduleId);
    if (it == ship.moduleIds.end()) {
        return refuse(std::string("module '") + moduleId + "' is not fitted", outError);
    }
    // Refuse a removal that would strand cargo over the reduced capacity.
    OwnedShip candidate = ship;
    candidate.moduleIds.erase(candidate.moduleIds.begin() +
                              (it - ship.moduleIds.begin()));
    if (resolvedShipDef(candidate).cargoCapacity < playerCargoTotal()) {
        return refuse("cargo hold would overflow; sell cargo first", outError);
    }
    double refund = 0.0;
    if (m_defs != nullptr) {
        if (const assets::ModuleDef* module = m_defs->findModule(moduleId)) {
            refund = kResaleRate * module->price;
        }
    }
    ship.moduleIds.erase(it);
    m_playerCredits += refund;
    applyActiveLoadout();
    SOL_LOG_INFO("removed '%s' (+%.0f cr)", moduleId, refund);
    return true;
}

bool SpaceWorld::buyWeapon(const char* weaponId, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr || m_fleet.empty()) {
        return refuse("must be docked to refit", outError);
    }
    const assets::WeaponDef* weapon = m_defs->findWeapon(weaponId);
    if (weapon == nullptr) {
        return refuse(std::string("no weapon def '") + weaponId + "'", outError);
    }
    if (!stationSells(weapon->gate)) {
        return refuse("'" + weapon->name + "' is not sold here (faction catalog)", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    if (ship.weaponId == weaponId) {
        return refuse("already fitted", outError);
    }
    // The old weapon sells back at the resale rate in the same transaction.
    double resale = 0.0;
    if (!ship.weaponId.empty()) {
        if (const assets::WeaponDef* old = m_defs->findWeapon(ship.weaponId.c_str())) {
            resale = kResaleRate * old->price;
        }
    }
    if (m_playerCredits + resale < weapon->price) {
        return refuse("insufficient credits", outError);
    }
    m_playerCredits += resale - weapon->price;
    ship.weaponId = weapon->id;
    applyActiveLoadout();
    SOL_LOG_INFO("mounted '%s' (net %.0f cr)", weapon->name.c_str(), resale - weapon->price);
    return true;
}

bool SpaceWorld::buyShip(const char* shipDefId, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr) {
        return refuse("must be docked to buy ships", outError);
    }
    const assets::ShipDef* def = m_defs->findShip(shipDefId);
    if (def == nullptr) {
        return refuse(std::string("no ship def '") + shipDefId + "'", outError);
    }
    if (!stationSells(def->gate)) {
        return refuse("'" + def->name + "' is not sold here (faction catalog)", outError);
    }
    if (m_playerCredits < def->price) {
        return refuse("insufficient credits", outError);
    }
    m_playerCredits -= def->price;
    m_fleet.push_back(OwnedShip{.defId = def->id,
                                .weaponId = def->weaponId,
                                .storedSystem = m_currentSystem,
                                .storedStation = m_dockedStation});
    SOL_LOG_INFO("bought '%s' (-%.0f cr); stored at %s", def->name.c_str(),
                 static_cast<double>(def->price), dockedStationName());
    return true;
}

bool SpaceWorld::sellShip(std::size_t fleetIndex, std::string* outError)
{
    if (!isDocked()) {
        return refuse("must be docked to sell ships", outError);
    }
    if (fleetIndex >= m_fleet.size() || fleetIndex == m_activeShip) {
        return refuse("can only sell a stored ship", outError);
    }
    const OwnedShip& ship = m_fleet[fleetIndex];
    if (ship.storedSystem != m_currentSystem || ship.storedStation != m_dockedStation) {
        return refuse("that ship is stored elsewhere", outError);
    }
    const double refund = kResaleRate * shipValue(ship);
    SOL_LOG_INFO("sold '%s' (+%.0f cr)", ship.defId.c_str(), refund);
    m_playerCredits += refund;
    m_fleet.erase(m_fleet.begin() + static_cast<std::ptrdiff_t>(fleetIndex));
    if (m_activeShip > fleetIndex) {
        --m_activeShip;
    }
    return true;
}

bool SpaceWorld::switchShip(std::size_t fleetIndex, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr) {
        return refuse("must be docked to switch ships", outError);
    }
    if (fleetIndex >= m_fleet.size() || fleetIndex == m_activeShip) {
        return refuse("pick a stored ship", outError);
    }
    OwnedShip& target = m_fleet[fleetIndex];
    if (target.storedSystem != m_currentSystem || target.storedStation != m_dockedStation) {
        return refuse("that ship is stored elsewhere", outError);
    }
    if (resolvedShipDef(target).cargoCapacity < playerCargoTotal()) {
        return refuse("cargo would not fit that ship's hold", outError);
    }
    OwnedShip& current = m_fleet[m_activeShip];
    current.storedSystem = m_currentSystem;
    current.storedStation = m_dockedStation;
    target.storedSystem = kNoIndex;
    target.storedStation = kNoIndex;
    m_activeShip = fleetIndex;
    applyActiveLoadout();
    SOL_LOG_INFO("now flying '%s'", m_fleet[m_activeShip].defId.c_str());
    return true;
}

bool SpaceWorld::hireCrew(const char* crewId, std::string* outError)
{
    if (!isDocked() || m_defs == nullptr || m_fleet.empty()) {
        return refuse("must be docked to hire crew", outError);
    }
    const assets::CrewDef* member = m_defs->findCrew(crewId);
    if (member == nullptr) {
        return refuse(std::string("no crew def '") + crewId + "'", outError);
    }
    if (!stationSells(member->gate)) {
        return refuse("'" + member->name + "' is not for hire here (faction catalog)", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const assets::ShipDef* base = m_defs->findShip(ship.defId.c_str());
    if (base == nullptr) {
        return refuse("active ship def '" + ship.defId + "' missing", outError);
    }
    std::vector<const assets::CrewDef*> crew = fitCrew(*m_defs, ship);
    crew.push_back(member);
    std::string reason;
    if (!assets::validateLoadout(*base, fitModules(*m_defs, ship), crew, &reason)) {
        return refuse(reason, outError);
    }
    if (m_playerCredits < member->price) {
        return refuse("insufficient credits", outError);
    }
    m_playerCredits -= member->price;
    ship.crewIds.push_back(member->id);
    applyActiveLoadout();
    SOL_LOG_INFO("hired %s '%s' (-%.0f cr)", member->role.c_str(), member->name.c_str(),
                 static_cast<double>(member->price));
    return true;
}

bool SpaceWorld::fireCrew(const char* crewId, std::string* outError)
{
    if (!isDocked() || m_fleet.empty()) {
        return refuse("must be docked to dismiss crew", outError);
    }
    OwnedShip& ship = m_fleet[m_activeShip];
    const auto it = std::find(ship.crewIds.begin(), ship.crewIds.end(), crewId);
    if (it == ship.crewIds.end()) {
        return refuse(std::string("crew '") + crewId + "' is not aboard", outError);
    }
    ship.crewIds.erase(it); // hires are one-time fees: no refund
    applyActiveLoadout();
    SOL_LOG_INFO("dismissed '%s'", crewId);
    return true;
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
        m_scanRange = def.scanRange > 0.0f ? def.scanRange : 1.0f;
        m_scanSpeed = def.scanSpeed > 0.0f ? def.scanSpeed : 1.0f;
        m_collectorRange = def.collectorRange > 0.0f ? def.collectorRange : 1.0f;
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
            weapon.miningPower = weaponDef->miningPower;
        } else {
            SOL_LOG_WARN("ship '%s': unknown weapon def '%s'", def.id.c_str(),
                         def.weaponId.c_str());
        }
    }
}

ecs::Entity SpaceWorld::spawnShipAt(const assets::ShipDef& def, const assets::DefDatabase& defs,
                                    const core::DVec3& position, const char* factionName)
{
    const ecs::Entity e = m_registry.create();
    m_registry.emplace<Transform>(e, Transform{.position = position,
                                               .previousPosition = position});
    m_registry.emplace<RenderShape>(e, RenderShape{});
    m_registry.emplace<FlightBody>(e);
    // Default input is assist-on with zero commands = station-keeping until a
    // pilot (Phase 6 AI) writes real commands.
    m_registry.emplace<ShipControl>(e);
    m_registry.emplace<ShipPower>(e);
    m_registry.emplace<ShipDefense>(e);
    m_registry.emplace<ShipWeapon>(e);
    applyShipDef(e.index, def, defs);
    std::string name = def.name;
    if (factionName != nullptr && factionName[0] != '\0') {
        name += std::string(" (") + factionName + ")";
    }
    m_spawnedShips.push_back({.entity = e, .defId = def.id, .name = std::move(name)});
    return e;
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
    const ecs::Entity e = spawnShipAt(def, defs, position, nullptr);
    Transform& transform = m_registry.storage<Transform>().get(e.index);
    transform.orientation = player.orientation;
    transform.previousOrientation = player.orientation;
    return e;
}

TargetInfo SpaceWorld::currentTargetInfo() const
{
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    // m_targetIndex can go stale when a targeted ship dies; wrap it here.
    const std::size_t index = total > 0 ? m_targetIndex % total : 0;

    if (index < m_targets.size()) {
        TargetInfo info;
        info.nav = m_targets[index];
        return info;
    }
    return contactInfo(index - m_targets.size());
}

TargetInfo SpaceWorld::contactInfo(std::size_t shipSlot) const
{
    TargetInfo info;
    if (shipSlot >= m_spawnedShips.size()) {
        return info;
    }
    const SpawnedShip& ship = m_spawnedShips[shipSlot];
    const Transform& transform = m_registry.storage<Transform>().get(ship.entity.index);
    info.nav = NavTarget{.name = ship.name, .position = transform.position, .surfaceRadius = 0.0};
    info.isShip = true;
    info.velocity = m_registry.storage<FlightBody>().get(ship.entity.index).velocity;
    if (const ShipPilot* pilot = m_registry.tryGet<ShipPilot>(ship.entity);
        pilot != nullptr && pilot->factionIndex < m_factionTable.size()) {
        info.factionName = m_factionTable[pilot->factionIndex].name;
        info.attitude = playerAttitudeName(pilot->factionIndex);
    }
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

SpaceWorld::NavKind SpaceWorld::navTargetKind(std::size_t index) const
{
    const std::size_t gateBase = m_planetTargetBase - m_gates.size();
    if (index < gateBase) {
        return NavKind::Station;
    }
    if (index < m_planetTargetBase) {
        return NavKind::Gate;
    }
    if (index < m_planetTargetBase + m_planets.size()) {
        return NavKind::Planet;
    }
    if (index == m_starTargetIndex) {
        return NavKind::Star;
    }
    const std::size_t slot = index - m_signalTargetBase;
    return index >= m_signalTargetBase && slot < m_dynamicTargets.size()
               ? m_dynamicTargets[slot].kind
               : NavKind::Signal;
}

std::uint32_t SpaceWorld::navTargetBody(std::size_t index) const
{
    if (index == m_starTargetIndex) {
        return 0;
    }
    if (index >= m_planetTargetBase && index < m_planetTargetBase + m_planets.size()) {
        return static_cast<std::uint32_t>(index - m_planetTargetBase + 1);
    }
    return kNoIndex;
}

namespace {

// One accessor per dynamic kind: a slot only answers for what it actually is.
[[nodiscard]] std::uint32_t slotIndexOfKind(SpaceWorld::NavKind want, SpaceWorld::NavKind got,
                                            std::uint32_t index)
{
    return want == got ? index : 0xffff'ffffu;
}

} // namespace

std::uint32_t SpaceWorld::navTargetSignal(std::size_t index) const
{
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slotIndexOfKind(NavKind::Signal, slot.kind, slot.index);
}

std::uint32_t SpaceWorld::navTargetField(std::size_t index) const
{
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slotIndexOfKind(NavKind::Field, slot.kind, slot.index);
}

std::uint32_t SpaceWorld::navTargetWreck(std::size_t index) const
{
    if (index < m_signalTargetBase || index >= m_targets.size()) {
        return kNoIndex;
    }
    const DynamicTarget& slot = m_dynamicTargets[index - m_signalTargetBase];
    return slotIndexOfKind(NavKind::Wreck, slot.kind, slot.index);
}

std::size_t SpaceWorld::currentTargetIndex() const
{
    const std::size_t total = m_targets.size() + m_spawnedShips.size();
    return total > 0 ? m_targetIndex % total : 0;
}

bool SpaceWorld::selectTarget(std::size_t index)
{
    if (index >= m_targets.size() + m_spawnedShips.size()) {
        return false;
    }
    m_targetIndex = index;
    // Selecting outright (the map's Set Target, a console call) also updates
    // that class's remembered slot, so a later T or C resumes from what the
    // player actually chose rather than from a stale cycle position.
    if (index < m_targets.size()) {
        m_navSlot = index;
    } else {
        m_contactSlot = index - m_targets.size();
    }
    return true;
}

void SpaceWorld::cycleNavTarget()
{
    if (m_targets.empty()) {
        return;
    }
    // Already on a nav point: step to the next one. Coming back from the
    // contact cycle: return to where this class left off, so switching
    // classes costs one press rather than a walk back around the list.
    m_navSlot = m_targetIndex < m_targets.size() ? (m_targetIndex + 1) % m_targets.size()
                                                 : m_navSlot % m_targets.size();
    m_targetIndex = m_navSlot;
}

void SpaceWorld::contactOrder(std::vector<std::size_t>& out) const
{
    out.clear();
    if (m_spawnedShips.empty()) {
        return;
    }
    const core::DVec3 playerPosition =
        m_registry.storage<Transform>().get(playerEntityIndex()).position;
    const std::uint32_t player = playerEntityIndex();

    // Threat tier, lowest first. Being shot at right now beats standing
    // policy: a patrol that has decided to kill you is more urgent than a
    // hostile freighter minding its own business three hundred klicks out.
    auto tierOf = [&](const SpawnedShip& ship) {
        const ShipPilot* pilot = m_registry.tryGet<ShipPilot>(ship.entity);
        if (pilot == nullptr) {
            return 2;
        }
        if (pilot->state == PilotState::Attack && pilot->hasTarget != 0
            && pilot->targetIndex == player) {
            return 0;
        }
        // An unaffiliated console spawn has no faction to consult and Lua
        // treats it as unconditionally player-hostile (the pre-8b rule).
        if (pilot->factionIndex >= m_factionTable.size()) {
            return 1;
        }
        return m_factionSim.playerHostile(pilot->factionIndex) ? 1 : 2;
    };

    struct Ranked
    {
        std::size_t slot;
        int tier;
        double distanceSquared;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(m_spawnedShips.size());
    for (std::size_t i = 0; i < m_spawnedShips.size(); ++i) {
        const SpawnedShip& ship = m_spawnedShips[i];
        const core::DVec3 offset =
            m_registry.storage<Transform>().get(ship.entity.index).position - playerPosition;
        ranked.push_back({.slot = i,
                          .tier = tierOf(ship),
                          .distanceSquared = dot(offset, offset)});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.tier != b.tier ? a.tier < b.tier : a.distanceSquared < b.distanceSquared;
    });
    out.reserve(ranked.size());
    for (const Ranked& entry : ranked) {
        out.push_back(entry.slot);
    }
}

void SpaceWorld::cycleContact()
{
    std::vector<std::size_t> order;
    contactOrder(order);
    if (order.empty()) {
        return;
    }
    // Coming from a nav target, the first press lands on the head of the
    // threat order — the thing shooting at you, which is the whole point of
    // giving contacts their own key. Already on a ship, step along that
    // order from wherever the current one sits in it.
    std::size_t next = 0;
    if (m_targetIndex >= m_targets.size()) {
        const std::size_t current = m_targetIndex - m_targets.size();
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (order[i] == current) {
                next = (i + 1) % order.size();
                break;
            }
        }
    }
    m_contactSlot = order[next];
    m_targetIndex = m_targets.size() + m_contactSlot;
}

bool SpaceWorld::targetShipByName(const char* namePart)
{
    for (std::size_t i = 0; i < m_spawnedShips.size(); ++i) {
        if (m_spawnedShips[i].name.find(namePart) != std::string::npos) {
            m_contactSlot = i;
            m_targetIndex = m_targets.size() + i;
            return true;
        }
    }
    return false;
}

ecs::Entity SpaceWorld::spawnPilotFromDef(const assets::ShipDef& def,
                                          const assets::DefDatabase& defs, PilotRole role,
                                          std::uint32_t factionIndex)
{
    const ecs::Entity e = spawnShipFromDef(def, defs);
    m_registry.emplace<ShipPilot>(e, ShipPilot{.role = role, .factionIndex = factionIndex});
    if (factionIndex < m_factionTable.size()) {
        m_spawnedShips.back().name =
            def.name + " (" + m_factionTable[factionIndex].name + ")";
    }
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

bool SpaceWorld::pilotEngageEnemy(ecs::Entity entity)
{
    ShipPilot* pilot = m_registry.isAlive(entity) ? m_registry.tryGet<ShipPilot>(entity) : nullptr;
    if (pilot == nullptr || pilot->factionIndex >= m_factionTable.size()) {
        return false;
    }
    constexpr double kSensorRange = 8.0e4; // meters
    const core::DVec3 self = m_registry.storage<Transform>().get(entity.index).position;

    std::uint32_t bestTarget = kNoIndex;
    double bestDistance = kSensorRange;
    const auto consider = [&](std::uint32_t targetIndex) {
        const Transform* transform = m_registry.storage<Transform>().tryGet(targetIndex);
        const ShipDefense* defense = m_registry.storage<ShipDefense>().tryGet(targetIndex);
        if (transform == nullptr || defense == nullptr || !defense->state.alive()) {
            return;
        }
        const double distance = length(transform->position - self);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestTarget = targetIndex;
        }
    };

    const ecs::Pool<ShipPilot>& pilots = m_registry.storage<ShipPilot>();
    for (std::size_t i = 0; i < pilots.size(); ++i) {
        const std::uint32_t otherIndex = pilots.entityIndices()[i];
        const std::uint32_t otherFaction = pilots.values()[i].factionIndex;
        if (otherIndex == entity.index || otherFaction >= m_factionTable.size() ||
            !m_factionSim.atWar(pilot->factionIndex, otherFaction)) {
            continue;
        }
        consider(otherIndex);
    }
    if (m_factionSim.playerHostile(pilot->factionIndex) && !isDocked()) {
        consider(playerEntityIndex());
    }
    if (bestTarget == kNoIndex) {
        return false;
    }
    pilot->state = PilotState::Attack;
    pilot->targetIndex = bestTarget;
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
        const char* attitude = pilot.factionIndex < m_factionTable.size()
                                   ? playerAttitudeName(pilot.factionIndex)
                                   : "none";
        out.push_back({
            .entity = m_registry.entityFromIndex(pilots.entityIndices()[i]),
            .role = kRoleNames[static_cast<std::uint32_t>(pilot.role) % 3],
            .state = kStateNames[static_cast<std::uint32_t>(pilot.state) % 4],
            .attitude = attitude,
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

bool SpaceWorld::engageAutopilot()
{
    if (isDocked() || (m_targets.empty() && m_spawnedShips.empty())) {
        return false;
    }
    m_autopilotActive = true;
    SOL_LOG_INFO("Autopilot: flying to '%s' (arrive %.1f km out)",
                 currentTargetInfo().nav.name.c_str(), m_autopilotRange / 1000.0);
    return true;
}

void SpaceWorld::setAutopilotArrivalRange(double meters)
{
    m_autopilotRange = core::clamp(meters, 100.0, 1.0e7);
}

sim::FlightInput SpaceWorld::autopilotInput()
{
    // Any manual steering/thrust interrupts (the mapper's assist/cruise
    // toggles alone don't); the threshold ignores mouse-stick noise.
    const auto deflected = [](const core::Vec3& v) {
        return std::fabs(v.x) > 0.25f || std::fabs(v.y) > 0.25f || std::fabs(v.z) > 0.25f;
    };
    if (deflected(m_shipInput.linear) || deflected(m_shipInput.angular) || m_shipInput.boost) {
        m_autopilotActive = false;
        SOL_LOG_INFO("Autopilot: cancelled by manual input");
        return m_shipInput;
    }

    const TargetInfo target = currentTargetInfo();
    if (target.nav.name.empty()) {
        m_autopilotActive = false;
        return m_shipInput;
    }

    // Stand off by the surface plus the arrival range; big bodies get at
    // least half a radius of clearance so the goal sits outside their
    // avoidance shell.
    const double effectiveRange =
        target.nav.surfaceRadius + std::max(m_autopilotRange, target.nav.surfaceRadius * 0.5);
    const core::DVec3 targetVelocity = target.isShip ? target.velocity : core::DVec3{};
    const sim::ShipState state = shipState();
    const double remaining = length(target.nav.position - state.position) - effectiveRange;
    if (remaining <= 0.0 && length(state.velocity - targetVelocity) < 25.0) {
        m_autopilotActive = false;
        SOL_LOG_INFO("Autopilot: arrived at '%s'", target.nav.name.c_str());
        return m_shipInput;
    }

    // The destination's own sphere must not deflect the final approach.
    m_autopilotObstacles.clear();
    for (const sim::AvoidanceSphere& sphere : m_obstacles) {
        if (length(sphere.position - target.nav.position) > sphere.radius + effectiveRange) {
            m_autopilotObstacles.push_back(sphere);
        }
    }

    sim::FlightInput input = sim::steerTravel(state, shipTuning(), target.nav.position,
                                              targetVelocity, effectiveRange,
                                              m_autopilotObstacles);
    input.assist = true;
    return input;
}

void SpaceWorld::tick(double dt)
{
    // The run's own clock. Market intel is stamped against it, so it advances
    // whether the player is docked or flying — a price you read an hour ago
    // is an hour old either way.
    m_worldSeconds += dt;
    const std::uint32_t playerIndex = playerEntityIndex();
    if (isDocked()) {
        // Parked: flight input is ignored and the ship stays pinned to the
        // pad (collision impulses must not drift a docked ship).
        m_autopilotActive = false;
        m_appliedInput = sim::FlightInput{};
        m_registry.storage<ShipControl>().get(playerIndex).input = sim::FlightInput{};
        Transform& transform = m_registry.storage<Transform>().get(playerIndex);
        const core::DVec3 pad = dockPoint(m_dockedStation);
        transform.position = pad;
        transform.previousPosition = pad;
        m_registry.storage<FlightBody>().get(playerIndex) = FlightBody{};
    } else {
        m_appliedInput = m_autopilotActive ? autopilotInput() : m_shipInput;
        m_registry.storage<ShipControl>().get(playerIndex).input = m_appliedInput;
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

            // A patrol only holds a grudge while the player is actually
            // hostile (Phase 8b live find: permanent aggro besieged the pad
            // after a standing recovered; raiders, by contrast, keep theirs).
            if (pilot.state == PilotState::Attack && pilot.role == PilotRole::Patrol &&
                pilot.hasTarget != 0 && pilot.targetIndex == playerIndex &&
                pilot.factionIndex < m_factionTable.size() &&
                !m_factionSim.playerHostile(pilot.factionIndex)) {
                pilot.state = PilotState::Idle;
                pilot.hasTarget = 0;
            }

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
    const ecs::Pool<OreChunk>& oreChunks = m_registry.storage<OreChunk>();
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::uint32_t entityIndex = shapes.entityIndices()[i];
        if (bodies.contains(entityIndex) || projectiles.contains(entityIndex)
            || oreChunks.contains(entityIndex)) {
            // Ships were pushed above; bolts and loose ore never block
            // anything — you fly through your own ore to collect it.
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
    std::vector<DestroyedShip> destroyedShips;
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
        if (defense == nullptr || !defense->state.alive() || isDamageImmune(entityIndex)) {
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
            destroyedShips.push_back({.victim = entityIndex}); // rams credit no one
        }
    };
    for (const sim::Contact& contact : m_contacts) {
        applyImpact(contact.bodyA, -contact.normal, contact.impactSpeed);
        applyImpact(contact.bodyB, contact.normal, contact.impactSpeed);
    }
    for (const DestroyedShip& destroyed : destroyedShips) {
        handleShipDestroyed(destroyed.victim, destroyed.attacker);
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
                    defense != nullptr && defense->state.alive() &&
                    !isDamageImmune(targetIndex)) {
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
                        destroyedShips.push_back(
                            {.victim = targetIndex, .attacker = projectile.shooterIndex});
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
    for (const DestroyedShip& destroyed : destroyedShips) {
        handleShipDestroyed(destroyed.victim, destroyed.attacker);
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
    // Mining beams land after the loop: cutting spawns ore chunks and can
    // break a rock up, and a structural change mid-iteration corrupts pools.
    struct PendingCut
    {
        std::uint32_t entityIndex;
        core::DVec3 impact;
        float units;
        bool wreck;
    };
    std::vector<PendingCut> pendingCuts;
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
            // A beam with mining_power cuts rock and hulls too (Phase 8f).
            // Whichever is nearer along the beam is what it lands on, so you
            // cannot mine through a fighter that flew into the line.
            double miningT = 2.0;
            std::uint32_t miningEntity = kNoIndex;
            bool miningWreck = false;
            if (weapon.miningPower > 0.0f && entityIndex == playerEntityIndex()) {
                const ecs::Pool<MineableRock>& rockPool = m_registry.storage<MineableRock>();
                const ecs::Pool<WreckMarker>& wreckPool = m_registry.storage<WreckMarker>();
                const auto sweepCuttable = [&](std::uint32_t candidate, bool isWreck) {
                    const RenderShape& candidateShape =
                        m_registry.storage<RenderShape>().get(candidate);
                    const double radius = modelBaseRadius(candidateShape.model)
                                          * static_cast<double>(candidateShape.scale.x);
                    double hitT = 0.0;
                    if (sim::segmentHitsSphere(muzzle, beamEnd,
                                               transforms.get(candidate).position, radius, hitT)
                        && hitT < miningT) {
                        miningT = hitT;
                        miningEntity = candidate;
                        miningWreck = isWreck;
                    }
                };
                for (std::size_t r = 0; r < rockPool.size(); ++r) {
                    sweepCuttable(rockPool.entityIndices()[r], false);
                }
                for (std::size_t r = 0; r < wreckPool.size(); ++r) {
                    sweepCuttable(wreckPool.entityIndices()[r], true);
                }
            }
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
            if (miningEntity != kNoIndex && (!hit || miningT <= bestT)) {
                // Per shot, so the yield works out to mining_power per second
                // of held beam whatever the weapon's rate of fire is. The cut
                // itself is deferred: it spawns chunk entities, and nothing
                // may change a pool's shape while this loop walks one.
                pendingCuts.push_back(
                    {.entityIndex = miningEntity,
                     .impact = muzzle + (beamEnd - muzzle) * miningT,
                     .units = weapon.miningPower
                              / (weapon.rateOfFire > 0.01f ? weapon.rateOfFire : 0.01f),
                     .wreck = miningWreck});
            } else if (hit) {
                if (ShipDefense* defense = defenses.tryGet(bestTarget);
                    defense != nullptr && defense->state.alive() &&
                    !isDamageImmune(bestTarget)) {
                    const sim::ShieldFacing facing = sim::facingForHit(
                        m_registry.storage<Transform>().get(bestTarget).orientation,
                        forwardD * -1.0);
                    const sim::DamageResult result = sim::applyDamage(
                        defense->state, defense->tuning, facing, weapon.damage);
                    noteDamage(bestTarget, muzzle + (beamEnd - muzzle) * bestT, result);
                    if (result.destroyed) { // deferred: mid-iteration
                        destroyedShips.push_back(
                            {.victim = bestTarget, .attacker = entityIndex});
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
    for (const DestroyedShip& destroyed : destroyedShips) {
        handleShipDestroyed(destroyed.victim, destroyed.attacker);
    }
    for (const PendingCut& cut : pendingCuts) {
        if (cut.wreck) {
            (void)cutWreck(cut.entityIndex, cut.units);
            m_combatEffects.spawnImpact(cut.impact, false);
            continue;
        }
        const MineableRock* rock = m_registry.storage<MineableRock>().tryGet(cut.entityIndex);
        if (rock == nullptr) {
            continue; // two beams on one rock in a tick; the first broke it up
        }
        const std::uint32_t field = rock->field;
        const std::uint32_t index = rock->index;
        const std::uint32_t commodity = rock->commodity;
        const float total = rock->totalUnits;
        (void)cutRock(cut.entityIndex, cut.units);
        m_combatEffects.spawnImpact(cut.impact, false);
        if (m_mining.unitsLeft(m_currentSystem, field, index, total) <= 0.0f) {
            m_registry.destroy(m_registry.entityFromIndex(cut.entityIndex)); // it broke up
            m_rockEvents.push_back({.commodity = commodity, .units = total});
        }
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
    m_thrusters.tick(shipState(), shipTuning(), m_appliedInput, dt);

    // Mining (Phase 8f): rock tumble, chunk drift and collection, wreck decay
    // and reconciliation, refinery orders.
    tickMining(dt);

    // Coarse-layer economy: galaxy-wide, same clock as everything else
    // (decisions/005 — no time compression). The feedstock source is what
    // makes a mining outpost's ore come out of its own system's rock
    // (Phase 8g) instead of out of nothing.
    m_economy.tick(m_galaxy, dt, &m_feedstock);

    // Coarse-layer faction sim (Phase 8b): drift/decay here; due decisions
    // are dispatched by GameContent (Lua faction_think or the default rule).
    m_factionSim.tick(dt);

    // Coarse-layer missions (Phase 8c): deadlines and position objectives
    // here; the board hook and campaign flavor run in GameContent against
    // the events this drains.
    m_missions.tick(dt);
    if (!isDocked()) {
        m_missions.notifyPosition(m_currentSystem, shipState().position);
    }
    processMissionEvents();

    // Scanning (Phase 8e): pulse recharge plus the held target scan.
    tickScanning(dt);

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
            m_dockEventPending = true; // fresh board at the respawn dock
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

void SpaceWorld::handleShipDestroyed(std::uint32_t entityIndex, std::uint32_t attackerIndex)
{
    // Fireball at the wreck site, scaled by the hull.
    m_combatEffects.spawnExplosion(m_registry.storage<Transform>().get(entityIndex).position,
                                   m_registry.storage<RenderShape>().get(entityIndex).scale.x);
    if (entityIndex == playerEntityIndex()) {
        // Cargo is lost either way (decisions/007).
        std::fill(m_playerCargo.begin(), m_playerCargo.end(), 0.0f);
        if (m_hardcore) {
            // Hardcore: the run is over — the caller deletes the save (see
            // consumeHardcoreDeath) and a fresh run starts at the new-game
            // system in the starter ship. The world itself keeps running.
            SOL_LOG_WARN("ship destroyed - HARDCORE: run over; save will be deleted");
            m_hardcoreDeathPending = true;
            m_playerCredits = 1'000.0;
            resetFleetToStarter();
            m_lastDockSystem = kNoIndex;
            m_lastDockStation = kNoIndex;
            m_dockedStation = kNoIndex;
            m_pendingRespawnSystem = m_startSystem;
            applyActiveLoadout();
        } else if (m_lastDockSystem != kNoIndex && m_lastDockSystem != m_currentSystem) {
            // Default death rule: wake at the last dock, insurance deductible
            // charged (never docked yet: the system spawn point stands in).
            // Cross-system respawn defers to end of tick.
            const double deductible = std::min(insuranceDeductible(), m_playerCredits);
            m_playerCredits -= deductible;
            SOL_LOG_WARN("ship destroyed - waking at last dock in '%s' (insurance %.0f cr)",
                         m_galaxy.systems[m_lastDockSystem].name.c_str(), deductible);
            m_pendingRespawnSystem = m_lastDockSystem;
        } else if (m_lastDockSystem == m_currentSystem && m_lastDockStation != kNoIndex) {
            const double deductible = std::min(insuranceDeductible(), m_playerCredits);
            m_playerCredits -= deductible;
            SOL_LOG_WARN("ship destroyed - waking at the last dock (insurance %.0f cr)",
                         deductible);
            m_dockedStation = m_lastDockStation;
            m_playerSpawn = dockPoint(m_dockedStation);
            m_dockEventPending = true; // fresh board at the respawn dock
        } else {
            const double deductible = std::min(insuranceDeductible(), m_playerCredits);
            m_playerCredits -= deductible;
            SOL_LOG_WARN("ship destroyed - respawning in %s (insurance %.0f cr)",
                         currentSystemName(), deductible);
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

    // Reputation (Phase 8b): only player kills move standings, and only for
    // affiliated victims; the web pays the victim's enemies.
    if (attackerIndex == playerEntityIndex()) {
        if (const ShipPilot* pilot = m_registry.storage<ShipPilot>().tryGet(entityIndex);
            pilot != nullptr && pilot->factionIndex < m_factionTable.size()) {
            m_factionSim.recordShipKill(pilot->factionIndex);
            m_missions.notifyKill(pilot->factionIndex, m_currentSystem);
            SOL_LOG_INFO("kill vs %s: standing now %.1f (%s)",
                         m_factionTable[pilot->factionIndex].name.c_str(),
                         m_factionSim.standing(pilot->factionIndex),
                         playerAttitudeName(pilot->factionIndex));
        }
    }

    for (std::size_t i = 0; i < m_spawnedShips.size(); ++i) {
        if (m_spawnedShips[i].entity.index != entityIndex) {
            continue;
        }
        SOL_LOG_INFO("'%s' destroyed", m_spawnedShips[i].defId.c_str());

        // A wreck stays where it fell (Phase 8f). The record is what persists
        // — the entity to cut is materialized by tickMining, here and after a
        // jump back or a reload — and its contents are composed now, from the
        // ship that actually died, so a later def edit cannot rewrite it.
        const core::DVec3 where = m_registry.storage<Transform>().get(entityIndex).position;
        const std::string defId = m_spawnedShips[i].defId;
        const std::string name = m_spawnedShips[i].name;
        const ShipPilot* pilot = m_registry.storage<ShipPilot>().tryGet(entityIndex);
        const std::uint32_t faction = pilot != nullptr ? pilot->factionIndex : kNoIndex;
        const std::uint64_t seed = core::Rng(m_universeSeed ^ (static_cast<std::uint64_t>(
                                                 entityIndex + 1)
                                                 * 0x9e37'79b9'7f4a'7c15ull),
                                             13)
                                       .nextU64();
        const std::uint32_t wreckId =
            m_mining.addWreck(m_currentSystem, where, defId, name, seed);
        if (wreckId != 0) {
            const assets::ShipDef* def =
                m_defs != nullptr ? m_defs->findShip(defId.c_str()) : nullptr;
            // Scriptless default first, so a hull always holds something even
            // if no script answers; the Lua hook may replace it before it is
            // cut into.
            (void)m_mining.setWreckContents(wreckId, defaultWreckLoot(def, seed));
            m_wreckEvents.push_back({.id = wreckId,
                                     .system = m_currentSystem,
                                     .defId = defId,
                                     .factionName = faction < m_factionTable.size()
                                                        ? m_factionTable[faction].name
                                                        : std::string(),
                                     .seed = seed});
        }

        m_registry.destroy(m_spawnedShips[i].entity);
        m_spawnedShips.erase(m_spawnedShips.begin() + static_cast<std::ptrdiff_t>(i));
        return;
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
    writer.write(static_cast<std::uint8_t>(m_hardcore ? 1 : 0));
    writer.write(m_worldSeconds); // v9: what market intel timestamps mean
    writer.write(m_playerCredits);
    writer.write(static_cast<std::uint32_t>(m_playerCargo.size()));
    for (const float units : m_playerCargo) {
        writer.write(units);
    }
    // Fleet (v4): fits and crew as def-id strings; stored location per ship.
    writer.write(static_cast<std::uint32_t>(m_fleet.size()));
    writer.write(static_cast<std::uint32_t>(m_activeShip));
    for (const OwnedShip& ship : m_fleet) {
        writer.writeString(ship.defId);
        writer.writeString(ship.weaponId);
        writer.write(static_cast<std::uint32_t>(ship.moduleIds.size()));
        for (const std::string& id : ship.moduleIds) {
            writer.writeString(id);
        }
        writer.write(static_cast<std::uint32_t>(ship.crewIds.size()));
        for (const std::string& id : ship.crewIds) {
            writer.writeString(id);
        }
        writer.write(ship.storedSystem);
        writer.write(ship.storedStation);
    }
    m_economy.save(writer);
    m_factionSim.save(writer); // v5: relations, war flags, standings, raids
    m_missions.save(writer);   // v6: journal, board, campaign stage
    m_survey.save(writer);     // v7: knowledge, signal state, ledger, route
    m_mining.save(writer);     // v8: rock depletion, wrecks, refinery orders
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
    std::uint8_t hardcore = 0;
    double worldSeconds = 0.0;
    if (!reader.read(magic) || magic != kSaveMagic || !reader.read(version) ||
        version != kSaveVersion || !reader.read(seed) || !reader.read(systemIndex) ||
        !reader.read(dockedStation) || !reader.read(lastDockSystem) ||
        !reader.read(lastDockStation) || !reader.read(hardcore) || !reader.read(worldSeconds)) {
        return false; // pre-fleet or foreign save: rejected cleanly
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
    // Fleet (v4).
    std::uint32_t fleetCount = 0;
    std::uint32_t activeIndex = 0;
    if (!reader.read(fleetCount) || !reader.read(activeIndex) || fleetCount == 0 ||
        activeIndex >= fleetCount) {
        return false;
    }
    std::vector<OwnedShip> fleet(fleetCount);
    for (OwnedShip& ship : fleet) {
        std::uint32_t moduleCount = 0;
        std::uint32_t crewCount = 0;
        if (!reader.readString(ship.defId) || !reader.readString(ship.weaponId) ||
            !reader.read(moduleCount)) {
            return false;
        }
        ship.moduleIds.resize(moduleCount);
        for (std::string& id : ship.moduleIds) {
            if (!reader.readString(id)) {
                return false;
            }
        }
        if (!reader.read(crewCount)) {
            return false;
        }
        ship.crewIds.resize(crewCount);
        for (std::string& id : ship.crewIds) {
            if (!reader.readString(id)) {
                return false;
            }
        }
        if (!reader.read(ship.storedSystem) || !reader.read(ship.storedStation)) {
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
    // Faction layout re-derives from galaxy + defs (v5); dynamic state loads
    // over a fresh initialize, same rule as the economy.
    if (galaxyChanged) {
        initializeFactions(); // m_universeSeed already updated above
    }
    if (!m_factionSim.load(reader)) {
        return false;
    }
    // Missions (v6): same rule — layout re-derives, dynamic state loads.
    if (!m_missions.load(reader)) {
        return false;
    }
    // Survey (v7): knowledge, per-signal state, ledger, and the plotted route.
    if (galaxyChanged) {
        initializeSurvey();
    }
    if (!m_survey.load(reader)) {
        return false;
    }
    // Mining (v8): depletion, the wreck store, and outstanding refine jobs.
    // Fields and rocks themselves re-derive from the seed, as ever.
    if (galaxyChanged) {
        initializeMining();
    }
    if (!m_mining.load(reader)) {
        return false;
    }

    ecs::Registry fresh;
    if (!makeSnapshotSchema().load(fresh, reader)) {
        return false;
    }
    if (fresh.storage<PlayerShip>().size() != 1) {
        return false; // not a current-format save (or player identity lost)
    }
    m_registry = std::move(fresh);
    ensureMiningPools(); // the snapshot only carries pools the save had in it
    // Def-spawned entities were replaced wholesale; their def association is
    // gone (visuals persist via the saved RenderShape).
    m_spawnedShips.clear();
    m_combatEffects.clear();
    m_thrusters.clear();
    m_playerCredits = credits;
    m_playerCargo = std::move(cargo);
    m_hardcore = hardcore != 0;
    m_worldSeconds = worldSeconds; // intel ages continue where they left off
    m_hardcoreDeathPending = false;
    m_fleet = std::move(fleet);
    m_activeShip = activeIndex;
    if (galaxyChanged) {
        // Recompute the new-game anchor (hardcore respawn) for this galaxy.
        m_startSystem = 0;
        for (std::uint32_t i = 0; i < m_galaxy.systems.size(); ++i) {
            if (m_galaxy.systems[i].region == sim::Region::Core &&
                !m_galaxy.systems[i].stations.empty()) {
                m_startSystem = i;
                break;
            }
        }
    }
    // The ECS snapshot already carries the fitted tuning exactly; only the
    // non-ECS cargo capacity derives from the fit and must be recomputed.
    if (m_defs != nullptr && !m_fleet.empty()) {
        const assets::ShipDef resolved = resolvedShipDef(activeShip());
        m_playerCargoCapacity = resolved.cargoCapacity;
        m_scanRange = resolved.scanRange > 0.0f ? resolved.scanRange : 1.0f;
        m_scanSpeed = resolved.scanSpeed > 0.0f ? resolved.scanSpeed : 1.0f;
        m_collectorRange = resolved.collectorRange > 0.0f ? resolved.collectorRange : 1.0f;
    }

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
    // A scan in flight does not survive a load, and neither does an autopilot
    // leg: the target list is rebuilt below, so an engaged autopilot would
    // wake up flying at whatever now sits in slot 0.
    m_autopilotActive = false;
    m_scanActive = false;
    m_scanProgress = 0.0f;
    m_pulseCooldown = 0.0;
    m_surveyEvents.clear();
    // Board offers came back with the save; no re-roll on a docked load.
    m_dockEventPending = false;
    m_missionEvents.clear();
    m_playerSpawn =
        isDocked() ? dockPoint(m_dockedStation)
        : !spec.stations.empty()
            ? spec.stations[0].position + core::DVec3{0.0, 0.0, 800.0}
            : spec.planets[spec.primaryPlanet].position + core::DVec3{0.0, 0.0, 2.0e5};
    return true;
}

} // namespace game
