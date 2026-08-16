#include "sol/sim/faction_sim.hpp"

#include "sol/core/assert.hpp"
#include "sol/core/math/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sol::sim {

namespace {

constexpr std::uint64_t kFactionStream = 202;

[[nodiscard]] float clampScore(float value)
{
    return core::clamp(value, -100.0f, 100.0f);
}

} // namespace

void FactionSim::initialize(const Galaxy& galaxy, const FactionSimParams& params,
                            std::uint64_t seed)
{
    m_params = params;
    m_count = static_cast<std::uint32_t>(params.agents.size());
    m_systemCount = static_cast<std::uint32_t>(galaxy.systems.size());
    SOL_ASSERT(params.baselineRelations.size() ==
               static_cast<std::size_t>(m_count) * m_count);
    SOL_ASSERT(params.initialStandings.size() == m_count);

    m_relations = params.baselineRelations;
    m_atWar.assign(static_cast<std::size_t>(m_count) * m_count, 0);
    for (std::uint32_t a = 0; a < m_count; ++a) {
        for (std::uint32_t b = a + 1; b < m_count; ++b) {
            refreshWar(a, b);
        }
    }
    m_standings = params.initialStandings;
    m_raidIntensity.assign(m_systemCount, 0.0f);
    m_lastRaider.assign(m_systemCount, kNoFaction);
    m_dueDecisions.clear();
    m_stepAccumulator = 0.0;
    m_decisionAccumulator = 0.0;
    m_rng.seed(seed, kFactionStream);
}

void FactionSim::tick(double dt)
{
    if (m_count == 0) {
        return;
    }
    m_stepAccumulator += dt;
    while (m_stepAccumulator >= m_params.stepInterval) {
        m_stepAccumulator -= m_params.stepInterval;
        step(m_params.stepInterval);
    }
    m_decisionAccumulator += dt;
    while (m_decisionAccumulator >= m_params.decisionInterval) {
        m_decisionAccumulator -= m_params.decisionInterval;
        for (std::uint32_t f = 0; f < m_count; ++f) { // index order: determinism
            m_dueDecisions.push_back({.faction = f, .roll = m_rng.nextFloat01()});
        }
    }
}

void FactionSim::step(double dt)
{
    // Relations drift back toward the authored baseline, faster between
    // forgiving factions; war flags follow with hysteresis.
    for (std::uint32_t a = 0; a < m_count; ++a) {
        for (std::uint32_t b = a + 1; b < m_count; ++b) {
            const float baseline = m_params.baselineRelations[pairIndex(a, b)];
            float& value = m_relations[pairIndex(a, b)];
            const float forgiveness = 0.5f * (m_params.agents[a].forgiveness +
                                              m_params.agents[b].forgiveness);
            const float rate = m_params.driftPerSecond * forgiveness *
                               static_cast<float>(dt); // points per step
            const float delta = baseline - value;
            const float stepValue = delta > 0.0f ? std::min(delta, rate)
                                                 : std::max(delta, -rate);
            value = clampScore(value + stepValue);
            m_relations[pairIndex(b, a)] = value;
            refreshWar(a, b);
        }
    }
    if (m_params.raidIntensityHalfLife > 0.0) {
        const float decay = static_cast<float>(
            std::exp2(-dt / m_params.raidIntensityHalfLife));
        for (float& intensity : m_raidIntensity) {
            intensity *= decay;
        }
    }
}

void FactionSim::takeDueDecisions(std::vector<FactionDecision>& out)
{
    out.insert(out.end(), m_dueDecisions.begin(), m_dueDecisions.end());
    m_dueDecisions.clear();
}

void FactionSim::raidCandidates(const Galaxy& galaxy, std::uint32_t faction,
                                std::vector<RaidCandidate>& out) const
{
    out.clear();
    if (faction >= m_count || galaxy.systems.size() != m_systemCount) {
        return;
    }
    // Multi-source BFS from the faction's territory, capped at raidReach.
    constexpr std::uint8_t kUnvisited = 0xff;
    std::vector<std::uint8_t> depth(m_systemCount, kUnvisited);
    std::vector<std::uint32_t> frontier;
    for (std::uint32_t s = 0; s < m_systemCount; ++s) {
        if (galaxy.systems[s].factionIndex == faction) {
            depth[s] = 0;
            frontier.push_back(s);
        }
    }
    std::vector<std::uint32_t> next;
    for (std::uint8_t d = 1; d <= m_params.raidReach && !frontier.empty(); ++d) {
        next.clear();
        for (const std::uint32_t index : frontier) {
            for (const GateSpec& gate : galaxy.systems[index].gates) {
                if (depth[gate.toSystem] == kUnvisited) {
                    depth[gate.toSystem] = d;
                    next.push_back(gate.toSystem);
                }
            }
        }
        frontier.swap(next);
    }
    for (std::uint32_t s = 0; s < m_systemCount; ++s) {
        const std::uint32_t owner = galaxy.systems[s].factionIndex;
        if (depth[s] == kUnvisited || owner == kNoFaction || owner == faction ||
            owner >= m_count) {
            continue;
        }
        const float pairRelation = relation(faction, owner);
        if (atWar(faction, owner) || pairRelation < m_params.hostileThreshold) {
            out.push_back({.system = s, .owner = owner, .relation = pairRelation});
        }
    }
}

void FactionSim::applyDefaultDecision(const Galaxy& galaxy, Economy* economy,
                                      const FactionDecision& decision)
{
    if (decision.faction >= m_count ||
        decision.roll >= m_params.agents[decision.faction].aggression) {
        return;
    }
    std::vector<RaidCandidate> candidates;
    raidCandidates(galaxy, decision.faction, candidates);
    const RaidCandidate* worst = nullptr;
    for (const RaidCandidate& candidate : candidates) {
        if (worst == nullptr || candidate.relation < worst->relation) {
            worst = &candidate;
        }
    }
    if (worst != nullptr) {
        (void)commitRaid(galaxy, economy, decision.faction, worst->system);
    }
}

bool FactionSim::commitRaid(const Galaxy& galaxy, Economy* economy, std::uint32_t faction,
                            std::uint32_t targetSystem)
{
    std::vector<RaidCandidate> candidates;
    raidCandidates(galaxy, faction, candidates);
    const auto it = std::find_if(candidates.begin(), candidates.end(),
                                 [&](const RaidCandidate& c) { return c.system == targetSystem; });
    if (it == candidates.end()) {
        return false;
    }
    if (economy != nullptr) {
        economy->raidSystem(targetSystem, m_params.raidStockFraction);
    }
    const std::uint32_t owner = it->owner;
    const float value = clampScore(relation(faction, owner) - m_params.raidRelationHit);
    m_relations[pairIndex(faction, owner)] = value;
    m_relations[pairIndex(owner, faction)] = value;
    refreshWar(faction, owner);
    m_raidIntensity[targetSystem] += 1.0f;
    m_lastRaider[targetSystem] = faction;
    return true;
}

float FactionSim::relation(std::uint32_t a, std::uint32_t b) const
{
    if (a >= m_count || b >= m_count || a == b) {
        return 0.0f;
    }
    return m_relations[pairIndex(a, b)];
}

bool FactionSim::atWar(std::uint32_t a, std::uint32_t b) const
{
    if (a >= m_count || b >= m_count || a == b) {
        return false;
    }
    return m_atWar[pairIndex(a, b)] != 0;
}

void FactionSim::setRelation(std::uint32_t a, std::uint32_t b, float value)
{
    if (a >= m_count || b >= m_count || a == b) {
        return;
    }
    m_relations[pairIndex(a, b)] = clampScore(value);
    m_relations[pairIndex(b, a)] = m_relations[pairIndex(a, b)];
    refreshWar(a, b);
}

void FactionSim::refreshWar(std::uint32_t a, std::uint32_t b)
{
    const float value = m_relations[pairIndex(a, b)];
    std::uint8_t& war = m_atWar[pairIndex(a, b)];
    if (war == 0 && value <= m_params.warThreshold) {
        war = 1;
    } else if (war != 0 && value >= m_params.peaceThreshold) {
        war = 0;
    }
    m_atWar[pairIndex(b, a)] = war;
}

float FactionSim::standing(std::uint32_t faction) const
{
    return faction < m_count ? m_standings[faction] : 0.0f;
}

void FactionSim::setStanding(std::uint32_t faction, float value)
{
    if (faction < m_count) {
        m_standings[faction] = clampScore(value);
    }
}

void FactionSim::recordShipKill(std::uint32_t victimFaction)
{
    if (victimFaction >= m_count) {
        return;
    }
    m_standings[victimFaction] =
        clampScore(m_standings[victimFaction] - m_params.killPenalty);
    for (std::uint32_t f = 0; f < m_count; ++f) {
        if (f == victimFaction) {
            continue;
        }
        const float pairRelation = relation(victimFaction, f);
        if (!atWar(victimFaction, f) && pairRelation >= m_params.hostileThreshold) {
            continue; // no enmity, no gratitude
        }
        const float depth = core::clamp(-pairRelation / 100.0f, 0.0f, 1.0f);
        m_standings[f] = clampScore(m_standings[f] +
                                    m_params.killPenalty * m_params.killWebScale * depth);
    }
}

void FactionSim::recordTrade(std::uint32_t faction, double credits)
{
    if (faction >= m_count || credits <= 0.0) {
        return;
    }
    m_standings[faction] = clampScore(
        m_standings[faction] + static_cast<float>(credits) * m_params.commerceRate);
}

float FactionSim::raidIntensity(std::uint32_t system) const
{
    return system < m_raidIntensity.size() ? m_raidIntensity[system] : 0.0f;
}

std::uint32_t FactionSim::lastRaider(std::uint32_t system) const
{
    return system < m_lastRaider.size() ? m_lastRaider[system] : kNoFaction;
}

void FactionSim::save(core::BinaryWriter& writer) const
{
    writer.write(m_count);
    writer.write(m_systemCount);
    for (const float value : m_relations) {
        writer.write(value);
    }
    for (const std::uint8_t war : m_atWar) {
        writer.write(war);
    }
    for (const float value : m_standings) {
        writer.write(value);
    }
    for (const float value : m_raidIntensity) {
        writer.write(value);
    }
    for (const std::uint32_t raider : m_lastRaider) {
        writer.write(raider);
    }
    writer.write(m_stepAccumulator);
    writer.write(m_decisionAccumulator);
    const core::Rng::RawState rngState = m_rng.rawState();
    writer.write(rngState.state);
    writer.write(rngState.inc);
}

bool FactionSim::load(core::BinaryReader& reader)
{
    std::uint32_t count = 0;
    std::uint32_t systemCount = 0;
    if (!reader.read(count) || count != m_count || !reader.read(systemCount) ||
        systemCount != m_systemCount) {
        return false; // galaxy/defs mismatch: initialize() first
    }
    for (float& value : m_relations) {
        if (!reader.read(value)) {
            return false;
        }
    }
    for (std::uint8_t& war : m_atWar) {
        if (!reader.read(war)) {
            return false;
        }
    }
    for (float& value : m_standings) {
        if (!reader.read(value)) {
            return false;
        }
    }
    for (float& value : m_raidIntensity) {
        if (!reader.read(value)) {
            return false;
        }
    }
    for (std::uint32_t& raider : m_lastRaider) {
        if (!reader.read(raider)) {
            return false;
        }
    }
    core::Rng::RawState rngState;
    if (!reader.read(m_stepAccumulator) || !reader.read(m_decisionAccumulator) ||
        !reader.read(rngState.state) || !reader.read(rngState.inc)) {
        return false;
    }
    m_rng.setRawState(rngState);
    m_dueDecisions.clear();
    return true;
}

} // namespace sol::sim
