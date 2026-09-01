#include "sol/sim/missions.hpp"

#include "sol/core/assert.hpp"

#include <algorithm>
#include <cstddef>

namespace sol::sim {

namespace {

constexpr std::uint64_t kMissionStream = 203;

void setError(std::string* outError, const char* message)
{
    if (outError != nullptr) {
        *outError = message;
    }
}

} // namespace

void MissionSim::initialize(const Galaxy& galaxy,
                            const MissionParams& params,
                            std::uint32_t factionCount,
                            std::uint32_t commodityCount,
                            std::uint32_t traderCount,
                            std::uint64_t seed)
{
    m_params = params;
    m_systemCount = static_cast<std::uint32_t>(galaxy.systems.size());
    m_factionCount = factionCount;
    m_commodityCount = commodityCount;
    m_traderCount = traderCount;
    m_offers.clear();
    m_active.clear();
    m_events.clear();
    m_boardSystem = kNoFaction;
    m_boardStation = kNoFaction;
    m_boardAccumulator = 0.0;
    m_tracked = 0;
    m_campaignStage = 0;
    m_rng.seed(seed, kMissionStream);
}

void MissionSim::tick(double dt)
{
    for (std::uint32_t i = 0; i < m_active.size();) {
        Mission& mission = m_active[i];
        if (mission.deadline > 0.0) {
            mission.deadline -= dt;
            if (mission.deadline <= 0.0) {
                mission.deadline = 0.0;
                m_events.push_back({.kind = MissionEventKind::Failed,
                                    .mission = mission,
                                    .objective = mission.currentObjective});
                removeActive(i);
                continue;
            }
        }
        ++i;
    }
}

bool MissionSim::tickBoard(double dt)
{
    if (m_boardSystem == kNoFaction || m_params.boardRefreshInterval <= 0.0) {
        return false;
    }
    m_boardAccumulator += dt;
    if (m_boardAccumulator >= m_params.boardRefreshInterval) {
        m_boardAccumulator = 0.0;
        return true;
    }
    return false;
}

void MissionSim::jumpDepths(const Galaxy& galaxy,
                            std::uint32_t fromSystem,
                            std::vector<std::uint8_t>& out) const
{
    constexpr std::uint8_t kUnvisited = 0xff;
    out.assign(m_systemCount, kUnvisited);
    if (fromSystem >= m_systemCount) {
        return;
    }
    out[fromSystem] = 0;
    std::vector<std::uint32_t> frontier{fromSystem};
    std::vector<std::uint32_t> next;
    for (std::uint8_t d = 1; d <= m_params.candidateReach && !frontier.empty(); ++d) {
        next.clear();
        for (const std::uint32_t index : frontier) {
            for (const GateSpec& gate : galaxy.systems[index].gates) {
                if (out[gate.toSystem] == kUnvisited) {
                    out[gate.toSystem] = d;
                    next.push_back(gate.toSystem);
                }
            }
        }
        frontier.swap(next);
    }
}

void MissionSim::haulCandidates(const Galaxy& galaxy,
                                const Economy& economy,
                                std::uint32_t fromSystem,
                                std::uint32_t fromStation,
                                std::vector<HaulCandidate>& out) const
{
    out.clear();
    if (galaxy.systems.size() != m_systemCount || fromSystem >= m_systemCount) {
        return;
    }
    std::vector<std::uint8_t> depth;
    jumpDepths(galaxy, fromSystem, depth);
    for (std::uint32_t marketIndex = 0; marketIndex < economy.markets().size(); ++marketIndex) {
        const StationMarket& market = economy.markets()[marketIndex];
        if (depth[market.systemIndex] == 0xff) {
            continue;
        }
        if (market.systemIndex == fromSystem && market.stationIndex == fromStation) {
            continue; // hauling to where you already stand is no contract
        }
        const EconomyArchetype* archetype = market.archetype < economy.params().archetypes.size()
                                                ? &economy.params().archetypes[market.archetype]
                                                : nullptr;
        if (archetype == nullptr) {
            continue;
        }
        for (std::uint32_t c = 0; c < m_commodityCount; ++c) {
            // ⚑⚑ PER COMMODITY SINCE PHASE 34 STAGE D, AND THE SKIP MOVED INSIDE
            // THE LOOP WITH IT. A station with no hold for a good is not SHORT
            // of it - it is a station that good has no business at - so posting
            // a haul there would send the player across the galaxy to a dock
            // that will refuse the crate on arrival. Zero capacity used to be
            // impossible here; now it is the ordinary case for salvage.
            const float capacity = archetype->capacityFor(c);
            if (capacity <= 0.0f) {
                continue;
            }
            const float stock = economy.stock(marketIndex, c);
            if (stock >= m_params.shortageThreshold * capacity) {
                continue;
            }
            out.push_back({.system = market.systemIndex,
                           .station = market.stationIndex,
                           .commodity = c,
                           .units = 0.5f * capacity - stock,
                           .severity = 1.0f - stock / capacity,
                           .jumps = depth[market.systemIndex]});
        }
    }
}

void MissionSim::bountyCandidates(const Galaxy& galaxy,
                                  const FactionSim& factions,
                                  std::uint32_t fromSystem,
                                  std::vector<BountyCandidate>& out) const
{
    out.clear();
    if (galaxy.systems.size() != m_systemCount || fromSystem >= m_systemCount) {
        return;
    }
    std::vector<std::uint8_t> depth;
    jumpDepths(galaxy, fromSystem, depth);
    for (std::uint32_t s = 0; s < m_systemCount; ++s) {
        if (depth[s] == 0xff) {
            continue;
        }
        const float intensity = factions.raidIntensity(s);
        const std::uint32_t raider = factions.lastRaider(s);
        if (intensity < m_params.bountyIntensityThreshold || raider == kNoFaction ||
            raider >= m_factionCount) {
            continue;
        }
        out.push_back({.system = s, .clan = raider, .intensity = intensity, .jumps = depth[s]});
    }
}

void MissionSim::frontCandidates(const Galaxy& galaxy,
                                 const FactionSim& factions,
                                 std::uint32_t fromSystem,
                                 std::vector<ContestCandidate>& out) const
{
    out.clear();
    if (galaxy.systems.size() != m_systemCount || fromSystem >= m_systemCount) {
        return;
    }
    std::vector<std::uint8_t> depth;
    jumpDepths(galaxy, fromSystem, depth);
    for (std::uint32_t s = 0; s < m_systemCount; ++s) {
        if (depth[s] == 0xff || !factions.contested(s)) {
            continue;
        }
        const SystemContest contest = factions.contestOf(s);
        const std::uint32_t owner = factions.systemOwner(s);
        if (owner >= m_factionCount || contest.attacker >= m_factionCount) {
            continue;
        }
        out.push_back({.system = s,
                       .owner = owner,
                       .attacker = contest.attacker,
                       .pressure = contest.pressure,
                       .jumps = depth[s]});
    }
}

void MissionSim::contestCandidates(const Galaxy& galaxy,
                                   const FactionSim& factions,
                                   std::uint32_t fromSystem,
                                   std::uint32_t boardOwner,
                                   std::vector<ContestCandidate>& out) const
{
    // ⚑ The ownerless-board early return is kept AHEAD of the enumeration
    // rather than folded into the filter below, because `kNoFaction` reaching
    // that filter would drop every row anyway and the two would be
    // indistinguishable in a profile - and because the header promises it.
    out.clear();
    if (boardOwner >= m_factionCount) {
        return;
    }
    frontCandidates(galaxy, factions, fromSystem, out);
    // A station only posts about a fight it is in. It will not pay a pilot to
    // help the faction taking its own system. Phase 35 stage B split this out:
    // it is a rule about who will PAY, and a barkeep two jumps from a war does
    // not need anybody's permission to mention it.
    std::erase_if(out, [boardOwner](const ContestCandidate& c) {
        return c.owner != boardOwner && c.attacker != boardOwner;
    });
}

void MissionSim::escortCandidates(const Galaxy& galaxy,
                                  const Economy& economy,
                                  const FactionSim& factions,
                                  std::uint32_t fromSystem,
                                  std::vector<EscortCandidate>& out) const
{
    out.clear();
    if (galaxy.systems.size() != m_systemCount || fromSystem >= m_systemCount) {
        return;
    }
    const std::vector<EconomyTrader>& fleet = economy.traders();
    const std::uint32_t count = std::min(m_traderCount, static_cast<std::uint32_t>(fleet.size()));
    for (std::uint32_t t = 0; t < count; ++t) {
        const TraderRoute route = economy.route(t);
        // Leaving HERE, right now. An arriving hauler is already home and a
        // jumping one is nowhere at all, so Depart is the only leg a pilot
        // standing on this dock could still fly alongside.
        if (route.leg != TraderLeg::Depart || route.system != fromSystem ||
            route.toMarket >= economy.markets().size()) {
            continue;
        }
        const StationMarket& destination = economy.markets()[route.toMarket];
        out.push_back({.trader = t,
                       .system = destination.systemIndex,
                       .station = destination.stationIndex,
                       .commodity = fleet[t].commodity,
                       .cargo = fleet[t].cargo,
                       .danger = factions.danger(destination.systemIndex),
                       .jumps = route.hops});
    }
}

void MissionSim::openBoard(std::uint32_t system, std::uint32_t station)
{
    m_offers.clear();
    m_boardSystem = system;
    m_boardStation = station;
    m_boardAccumulator = 0.0;
}

float MissionSim::boardRoll()
{
    return m_rng.nextFloat01();
}

bool MissionSim::objectiveInRange(const Galaxy& galaxy, const MissionObjective& objective) const
{
    const bool killAnywhere = objective.kind == ObjectiveKind::Kill && objective.system == kAnySystem;
    if (!killAnywhere && objective.system >= m_systemCount) {
        return false;
    }
    switch (objective.kind) {
    case ObjectiveKind::Dock:
    case ObjectiveKind::Deliver:
        if (objective.station >= galaxy.systems[objective.system].stations.size()) {
            return false;
        }
        if (objective.kind == ObjectiveKind::Deliver &&
            (objective.commodity >= m_commodityCount || objective.units <= 0.0f)) {
            return false;
        }
        return true;
    case ObjectiveKind::Kill:
        return objective.faction < m_factionCount && objective.kills > 0;
    case ObjectiveKind::FlyTo:
        return objective.radius > 0.0;
    case ObjectiveKind::Hold:
        return objective.faction < m_factionCount;
    case ObjectiveKind::Escort:
        // The fleet is fixed for the life of a galaxy (a lost hauler goes back
        // to Idle rather than being struck off), so an index that was valid
        // when the contract was written stays valid until the save is loaded
        // against a different one — which is what this catches.
        return objective.trader < m_traderCount;
    }
    return false;
}

bool MissionSim::postOffer(const Galaxy& galaxy,
                           const Economy& economy,
                           const FactionSim& factions,
                           Mission mission,
                           std::string* outError)
{
    if (m_boardSystem == kNoFaction) {
        setError(outError, "no board open");
        return false;
    }
    if (m_offers.size() >= m_params.maxOffers) {
        setError(outError, "board full");
        return false;
    }
    if (mission.poster >= m_factionCount) {
        setError(outError, "bad poster faction");
        return false;
    }
    if (mission.objectives.empty()) {
        setError(outError, "no objectives");
        return false;
    }
    if (mission.rewardCredits < 0.0 || mission.deadline < 0.0) {
        setError(outError, "bad reward or deadline");
        return false;
    }
    for (const MissionObjective& objective : mission.objectives) {
        if (!objectiveInRange(galaxy, objective)) {
            setError(outError, "objective out of range");
            return false;
        }
    }
    if (mission.campaign()) {
        const auto sameId = [&](const Mission& other) { return other.campaignId == mission.campaignId; };
        if (std::any_of(m_offers.begin(), m_offers.end(), sameId) ||
            std::any_of(m_active.begin(), m_active.end(), sameId)) {
            setError(outError, "campaign mission already offered or active");
            return false;
        }
    } else {
        // Procedural contracts are one objective backed by a live candidate —
        // the board can only sell work the simulation actually generated.
        if (mission.objectives.size() != 1) {
            setError(outError, "procedural contracts are single-objective");
            return false;
        }
        const MissionObjective& objective = mission.objectives[0];
        if (objective.kind == ObjectiveKind::Deliver) {
            std::vector<HaulCandidate> candidates;
            haulCandidates(galaxy, economy, m_boardSystem, m_boardStation, candidates);
            const auto match =
                std::find_if(candidates.begin(), candidates.end(), [&](const HaulCandidate& c) {
                    return c.system == objective.system && c.station == objective.station &&
                           c.commodity == objective.commodity && objective.units <= c.units;
                });
            if (match == candidates.end()) {
                setError(outError, "no such shortage");
                return false;
            }
        } else if (objective.kind == ObjectiveKind::Kill) {
            std::vector<BountyCandidate> candidates;
            bountyCandidates(galaxy, factions, m_boardSystem, candidates);
            const auto match =
                std::find_if(candidates.begin(), candidates.end(), [&](const BountyCandidate& c) {
                    return c.system == objective.system && c.clan == objective.faction;
                });
            if (match == candidates.end() || objective.kills > m_params.maxBountyKills) {
                setError(outError, "no such bounty");
                return false;
            }
        } else if (objective.kind == ObjectiveKind::Hold) {
            std::vector<ContestCandidate> candidates;
            contestCandidates(galaxy, factions, m_boardSystem, mission.poster, candidates);
            const auto match =
                std::find_if(candidates.begin(), candidates.end(), [&](const ContestCandidate& c) {
                    // The named side must be one of the two actually fighting,
                    // so a board cannot sell a defence of a bystander.
                    return c.system == objective.system &&
                           (c.owner == objective.faction || c.attacker == objective.faction);
                });
            if (match == candidates.end()) {
                setError(outError, "no such contest");
                return false;
            }
        } else if (objective.kind == ObjectiveKind::Escort) {
            std::vector<EscortCandidate> candidates;
            escortCandidates(galaxy, economy, factions, m_boardSystem, candidates);
            const auto match =
                std::find_if(candidates.begin(), candidates.end(), [&](const EscortCandidate& c) {
                    // Both halves are checked: a board cannot sell a run to
                    // somewhere the hauler named is not actually going, which
                    // would be a contract that could only ever fail.
                    return c.trader == objective.trader && c.system == objective.system;
                });
            if (match == candidates.end()) {
                setError(outError, "no such hauler");
                return false;
            }
        } else {
            setError(outError, "procedural contracts are haul, bounty, contest or escort");
            return false;
        }
    }
    mission.currentObjective = 0;
    m_offers.push_back(std::move(mission));
    return true;
}

bool MissionSim::accept(std::uint32_t offerIndex, float standingWithPoster, std::string* outError)
{
    if (offerIndex >= m_offers.size()) {
        setError(outError, "no such offer");
        return false;
    }
    if (m_active.size() >= m_params.maxActive) {
        setError(outError, "journal full");
        return false;
    }
    if (standingWithPoster < m_offers[offerIndex].minRep) {
        setError(outError, "reputation too low");
        return false;
    }
    m_active.push_back(std::move(m_offers[offerIndex]));
    m_offers.erase(m_offers.begin() + offerIndex);
    m_events.push_back({.kind = MissionEventKind::Accepted, .mission = m_active.back(), .objective = 0});
    return true;
}

bool MissionSim::abandon(std::uint32_t activeIndex)
{
    if (activeIndex >= m_active.size()) {
        return false;
    }
    m_events.push_back({.kind = MissionEventKind::Abandoned,
                        .mission = m_active[activeIndex],
                        .objective = m_active[activeIndex].currentObjective});
    removeActive(activeIndex);
    return true;
}

void MissionSim::setTracked(std::uint32_t activeIndex)
{
    if (activeIndex < m_active.size()) {
        m_tracked = activeIndex;
    }
}

bool MissionSim::advanceObjective(std::uint32_t activeIndex)
{
    Mission& mission = m_active[activeIndex];
    m_events.push_back({.kind = MissionEventKind::ObjectiveComplete,
                        .mission = mission,
                        .objective = mission.currentObjective});
    ++mission.currentObjective;
    if (mission.currentObjective < mission.objectives.size()) {
        return false;
    }
    m_events.push_back(
        {.kind = MissionEventKind::Completed, .mission = mission, .objective = mission.currentObjective - 1});
    removeActive(activeIndex);
    return true;
}

void MissionSim::removeActive(std::uint32_t activeIndex)
{
    m_active.erase(m_active.begin() + activeIndex);
    if (m_tracked >= m_active.size()) {
        m_tracked = m_active.empty() ? 0 : static_cast<std::uint32_t>(m_active.size()) - 1;
    }
}

void MissionSim::notifyDock(std::uint32_t system, std::uint32_t station)
{
    for (std::uint32_t i = 0; i < m_active.size();) {
        const MissionObjective& objective = m_active[i].objectives[m_active[i].currentObjective];
        if (objective.kind == ObjectiveKind::Dock && objective.system == system &&
            objective.station == station) {
            if (advanceObjective(i)) {
                continue;
            }
        }
        ++i;
    }
}

void MissionSim::notifyKill(std::uint32_t victimFaction, std::uint32_t system)
{
    for (std::uint32_t i = 0; i < m_active.size();) {
        MissionObjective& objective = m_active[i].objectives[m_active[i].currentObjective];
        if (objective.kind == ObjectiveKind::Kill && objective.faction == victimFaction &&
            (objective.system == kAnySystem || objective.system == system) && objective.kills > 0) {
            --objective.kills;
            if (objective.kills == 0 && advanceObjective(i)) {
                continue;
            }
        }
        ++i;
    }
}

void MissionSim::notifyContestResolved(std::uint32_t system, std::uint32_t winner)
{
    for (std::uint32_t i = 0; i < m_active.size();) {
        const MissionObjective& objective = m_active[i].objectives[m_active[i].currentObjective];
        if (objective.kind != ObjectiveKind::Hold || objective.system != system) {
            ++i;
            continue;
        }
        if (objective.faction == winner) {
            if (advanceObjective(i)) {
                continue;
            }
            ++i;
            continue;
        }
        // The side the contract named lost. Queued as Lost rather than
        // Failed precisely so the game charges no standing: the player flew
        // the battle, and the poster lost more than they did.
        m_events.push_back({.kind = MissionEventKind::Lost,
                            .mission = m_active[i],
                            .objective = m_active[i].currentObjective});
        removeActive(i);
    }
}

void MissionSim::notifyTraderArrived(std::uint32_t trader, std::uint32_t system)
{
    for (std::uint32_t i = 0; i < m_active.size();) {
        const MissionObjective& objective = m_active[i].objectives[m_active[i].currentObjective];
        // The system is checked as well as the trader because a hauler outlives
        // its haul: it lands, goes Idle, and picks a new destination minutes
        // later. Only the arrival the contract was written about pays.
        if (objective.kind == ObjectiveKind::Escort && objective.trader == trader &&
            objective.system == system) {
            if (advanceObjective(i)) {
                continue;
            }
        }
        ++i;
    }
}

void MissionSim::notifyTraderLost(std::uint32_t trader, bool byPlayer)
{
    for (std::uint32_t i = 0; i < m_active.size();) {
        const MissionObjective& objective = m_active[i].objectives[m_active[i].currentObjective];
        if (objective.kind != ObjectiveKind::Escort || objective.trader != trader) {
            ++i;
            continue;
        }
        m_events.push_back({.kind = byPlayer ? MissionEventKind::Failed : MissionEventKind::Lost,
                            .mission = m_active[i],
                            .objective = m_active[i].currentObjective});
        removeActive(i);
    }
}

void MissionSim::notifyPosition(std::uint32_t system, const core::DVec3& position)
{
    for (std::uint32_t i = 0; i < m_active.size();) {
        const MissionObjective& objective = m_active[i].objectives[m_active[i].currentObjective];
        if (objective.kind == ObjectiveKind::FlyTo && objective.system == system &&
            core::length(position - objective.position) <= objective.radius) {
            if (advanceObjective(i)) {
                continue;
            }
        }
        ++i;
    }
}

float MissionSim::recordDelivery(std::uint32_t activeIndex,
                                 std::uint32_t system,
                                 std::uint32_t station,
                                 float available)
{
    if (activeIndex >= m_active.size() || available <= 0.0f) {
        return 0.0f;
    }
    MissionObjective& objective = m_active[activeIndex].objectives[m_active[activeIndex].currentObjective];
    if (objective.kind != ObjectiveKind::Deliver || objective.system != system ||
        objective.station != station) {
        return 0.0f;
    }
    const float delivered = std::min(available, objective.units);
    objective.units -= delivered;
    if (objective.units <= 0.0f) {
        objective.units = 0.0f;
        (void)advanceObjective(activeIndex);
    }
    return delivered;
}

void MissionSim::takeEvents(std::vector<MissionEvent>& out)
{
    out.insert(out.end(), m_events.begin(), m_events.end());
    m_events.clear();
}

namespace {

void writeObjective(core::BinaryWriter& writer, const MissionObjective& objective)
{
    writer.write(static_cast<std::uint32_t>(objective.kind));
    writer.write(objective.system);
    writer.write(objective.station);
    writer.write(objective.commodity);
    writer.write(objective.units);
    writer.write(objective.faction);
    writer.write(objective.kills);
    writer.write(objective.trader); // v13
    writer.write(objective.position.x);
    writer.write(objective.position.y);
    writer.write(objective.position.z);
    writer.write(objective.radius);
    writer.writeString(objective.text);
}

[[nodiscard]] bool readObjective(core::BinaryReader& reader, MissionObjective& objective)
{
    std::uint32_t kind = 0;
    // ⚑ The bound is the LAST member and must move with it. It read FlyTo up
    // to here, which was written when FlyTo was last — so from Phase 8u a
    // saved Hold objective (offered or active) failed this check and took the
    // whole world load down with it. Nothing caught it because a save only
    // carries one if a contest happens to be in reach of the board when it is
    // taken. The round-trip test beside this one now covers every kind.
    if (!reader.read(kind) || kind > static_cast<std::uint32_t>(ObjectiveKind::Escort)) {
        return false;
    }
    objective.kind = static_cast<ObjectiveKind>(kind);
    return reader.read(objective.system) && reader.read(objective.station) &&
           reader.read(objective.commodity) && reader.read(objective.units) &&
           reader.read(objective.faction) && reader.read(objective.kills) && reader.read(objective.trader) &&
           reader.read(objective.position.x) && reader.read(objective.position.y) &&
           reader.read(objective.position.z) && reader.read(objective.radius) &&
           reader.readString(objective.text);
}

void writeMission(core::BinaryWriter& writer, const Mission& mission)
{
    writer.writeString(mission.title);
    writer.writeString(mission.campaignId);
    writer.write(mission.poster);
    writer.write(mission.rewardCredits);
    writer.write(mission.standingReward);
    writer.write(mission.standingPenalty);
    writer.write(mission.deadline);
    writer.write(mission.minRep);
    writer.write(static_cast<std::uint32_t>(mission.objectives.size()));
    for (const MissionObjective& objective : mission.objectives) {
        writeObjective(writer, objective);
    }
    writer.write(mission.currentObjective);
}

[[nodiscard]] bool readMission(core::BinaryReader& reader, Mission& mission)
{
    std::uint32_t objectiveCount = 0;
    if (!reader.readString(mission.title) || !reader.readString(mission.campaignId) ||
        !reader.read(mission.poster) || !reader.read(mission.rewardCredits) ||
        !reader.read(mission.standingReward) || !reader.read(mission.standingPenalty) ||
        !reader.read(mission.deadline) || !reader.read(mission.minRep) || !reader.read(objectiveCount)) {
        return false;
    }
    mission.objectives.resize(objectiveCount);
    for (MissionObjective& objective : mission.objectives) {
        if (!readObjective(reader, objective)) {
            return false;
        }
    }
    return reader.read(mission.currentObjective) && mission.currentObjective < mission.objectives.size();
}

} // namespace

void MissionSim::save(core::BinaryWriter& writer) const
{
    writer.write(m_systemCount);
    writer.write(m_factionCount);
    writer.write(m_commodityCount);
    writer.write(m_traderCount); // v13: what an Escort objective indexes into
    writer.write(static_cast<std::uint32_t>(m_offers.size()));
    for (const Mission& mission : m_offers) {
        writeMission(writer, mission);
    }
    writer.write(static_cast<std::uint32_t>(m_active.size()));
    for (const Mission& mission : m_active) {
        writeMission(writer, mission);
    }
    writer.write(m_boardSystem);
    writer.write(m_boardStation);
    writer.write(m_boardAccumulator);
    writer.write(m_tracked);
    writer.write(m_campaignStage);
    const core::Rng::RawState rngState = m_rng.rawState();
    writer.write(rngState.state);
    writer.write(rngState.inc);
}

bool MissionSim::load(core::BinaryReader& reader)
{
    std::uint32_t systemCount = 0;
    std::uint32_t factionCount = 0;
    std::uint32_t commodityCount = 0;
    std::uint32_t traderCount = 0;
    if (!reader.read(systemCount) || systemCount != m_systemCount || !reader.read(factionCount) ||
        factionCount != m_factionCount || !reader.read(commodityCount) ||
        commodityCount != m_commodityCount || !reader.read(traderCount) || traderCount != m_traderCount) {
        return false; // galaxy/defs mismatch: initialize() first
    }
    std::uint32_t offerCount = 0;
    if (!reader.read(offerCount)) {
        return false;
    }
    m_offers.resize(offerCount);
    for (Mission& mission : m_offers) {
        if (!readMission(reader, mission)) {
            return false;
        }
    }
    std::uint32_t activeCount = 0;
    if (!reader.read(activeCount) || activeCount > m_params.maxActive) {
        return false;
    }
    m_active.resize(activeCount);
    for (Mission& mission : m_active) {
        if (!readMission(reader, mission)) {
            return false;
        }
    }
    core::Rng::RawState rngState;
    if (!reader.read(m_boardSystem) || !reader.read(m_boardStation) || !reader.read(m_boardAccumulator) ||
        !reader.read(m_tracked) || !reader.read(m_campaignStage) || !reader.read(rngState.state) ||
        !reader.read(rngState.inc)) {
        return false;
    }
    m_rng.setRawState(rngState);
    m_events.clear();
    return true;
}

} // namespace sol::sim
