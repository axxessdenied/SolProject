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

void FactionSim::initialize(const Galaxy& galaxy, const FactionSimParams& params, std::uint64_t seed)
{
    m_params = params;
    m_count = static_cast<std::uint32_t>(params.agents.size());
    m_systemCount = static_cast<std::uint32_t>(galaxy.systems.size());
    SOL_ASSERT(params.baselineRelations.size() == static_cast<std::size_t>(m_count) * m_count);
    SOL_ASSERT(params.initialStandings.size() == m_count);

    m_relations = params.baselineRelations;
    m_atWar.assign(static_cast<std::size_t>(m_count) * m_count, 0);
    for (std::uint32_t a = 0; a < m_count; ++a) {
        for (std::uint32_t b = a + 1; b < m_count; ++b) {
            refreshWar(a, b);
        }
    }
    m_standings = params.initialStandings;
    // Phase 36 stage A: nobody starts wanted. Sized off m_count rather than off
    // initialStandings so a params list that is short (which `standing` already
    // tolerates by bounds-checking) cannot leave this vector a different length
    // from the one every accessor here bounds-checks against.
    m_bounty.assign(m_count, 0.0f);
    m_raidIntensity.assign(m_systemCount, 0.0f);
    m_lastRaider.assign(m_systemCount, kNoFaction);

    // Territory (Phase 8u): the founding claim is the generated plan, and
    // ownership starts equal to it. The home system is the lowest-index
    // system holding a faction's claim — the rule ClanSpec::homeSystem
    // already states — and is derived here rather than saved.
    m_foundingClaim.resize(m_systemCount);
    for (std::uint32_t s = 0; s < m_systemCount; ++s) {
        m_foundingClaim[s] = galaxy.systems[s].factionIndex;
    }
    m_systemOwner = m_foundingClaim;
    m_contestAttacker.assign(m_systemCount, kNoFaction);
    m_contestPressure.assign(m_systemCount, 0.0f);
    m_homeSystem.assign(m_count, kNoFaction);
    for (std::uint32_t s = 0; s < m_systemCount; ++s) {
        const std::uint32_t claim = m_foundingClaim[s];
        if (claim < m_count && m_homeSystem[claim] == kNoFaction) {
            m_homeSystem[claim] = s;
        }
    }
    m_resolutions.clear();
    m_traderLosses.clear();
    m_dueDecisions.clear();
    m_stepAccumulator = 0.0;
    m_decisionAccumulator = 0.0;
    m_rng.seed(seed, kFactionStream);
}

void FactionSim::tick(double dt, Economy* economy, std::uint32_t shelteredSystem)
{
    if (m_count == 0) {
        return;
    }
    m_stepAccumulator += dt;
    while (m_stepAccumulator >= m_params.stepInterval) {
        m_stepAccumulator -= m_params.stepInterval;
        step(m_params.stepInterval, economy, shelteredSystem);
    }
    m_decisionAccumulator += dt;
    while (m_decisionAccumulator >= m_params.decisionInterval) {
        m_decisionAccumulator -= m_params.decisionInterval;
        for (std::uint32_t f = 0; f < m_count; ++f) { // index order: determinism
            // ⚑ See `FactionAgentParams::territorial`: a non-claimant takes no
            // roll, because the roll decides a raid it can never reach and the
            // draw would displace every trader loss after it.
            if (!m_params.agents[f].territorial) {
                continue;
            }
            m_dueDecisions.push_back({.faction = f, .roll = m_rng.nextFloat01()});
        }
    }
}

void FactionSim::step(double dt, Economy* economy, std::uint32_t shelteredSystem)
{
    // Relations drift back toward the authored baseline, faster between
    // forgiving factions; war flags follow with hysteresis.
    for (std::uint32_t a = 0; a < m_count; ++a) {
        for (std::uint32_t b = a + 1; b < m_count; ++b) {
            const float baseline = m_params.baselineRelations[pairIndex(a, b)];
            float& value = m_relations[pairIndex(a, b)];
            const float forgiveness =
                0.5f * (m_params.agents[a].forgiveness + m_params.agents[b].forgiveness);
            const float rate =
                m_params.driftPerSecond * forgiveness * static_cast<float>(dt); // points per step
            const float delta = baseline - value;
            const float stepValue = delta > 0.0f ? std::min(delta, rate) : std::max(delta, -rate);
            value = clampScore(value + stepValue);
            m_relations[pairIndex(b, a)] = value;
            refreshWar(a, b);
        }
    }
    if (m_params.raidIntensityHalfLife > 0.0) {
        const float decay = static_cast<float>(std::exp2(-dt / m_params.raidIntensityHalfLife));
        for (float& intensity : m_raidIntensity) {
            intensity *= decay;
        }
    }
    // A siege nobody sustains lapses, and that is the defender winning by
    // attrition — no separate code path, just the decay running out.
    if (m_params.contestHalfLife > 0.0) {
        const float decay = static_cast<float>(std::exp2(-dt / m_params.contestHalfLife));
        for (std::uint32_t s = 0; s < m_systemCount; ++s) {
            if (m_contestAttacker[s] == kNoFaction) {
                continue;
            }
            m_contestPressure[s] *= decay;
            if (m_contestPressure[s] < m_params.contestFloor) {
                resolveContest(s, false);
            }
        }
    }
    // The cost of all of the above, paid by the traffic (Phase 8x). Last,
    // because it reads the danger the rest of this step just left behind.
    if (economy != nullptr) {
        attrition(*economy, dt, shelteredSystem);
    }
}

void FactionSim::attrition(Economy& economy, double dt, std::uint32_t shelteredSystem)
{
    if (m_params.traderLossPerSecond <= 0.0f) {
        return;
    }
    const std::vector<EconomyTrader>& fleet = economy.traders();
    for (std::uint32_t t = 0; t < fleet.size(); ++t) {
        if (fleet[t].phase != TraderPhase::InTransit) {
            continue;
        }
        const TraderRoute route = economy.route(t);
        // ⚑ A trader can only be lost somewhere it could have been SEEN.
        // Depart and Arrive are exactly the windows a body exists in, and Jump
        // is honestly nowhere — so the gate network is the safe part of a haul
        // and a system is where the risk lives. That is not a convenience: it
        // means every loss rolled here is one the player could have flown to
        // and prevented, which is the whole premise of an escort contract.
        if (route.leg != TraderLeg::Depart && route.leg != TraderLeg::Arrive) {
            continue;
        }
        if (route.system >= m_systemCount || route.system == shelteredSystem) {
            continue;
        }
        const float risk = danger(route.system) * m_params.traderLossPerSecond * static_cast<float>(dt);
        if (risk <= 0.0f) {
            continue; // quiet system: no roll at all, so peace costs no entropy
        }
        if (m_rng.nextFloat01() >= risk) {
            continue;
        }
        if (economy.loseTrader(t)) {
            recordTraderLoss(route.system, t);
        }
    }
}

void FactionSim::takeDueDecisions(std::vector<FactionDecision>& out)
{
    out.insert(out.end(), m_dueDecisions.begin(), m_dueDecisions.end());
    m_dueDecisions.clear();
}

void FactionSim::raidCandidates(const Galaxy& galaxy,
                                std::uint32_t faction,
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
        // Held now, not claimed at generation: reach follows the border, so
        // a faction raids onward from ground it has taken (Phase 8u).
        if (m_systemOwner[s] == faction) {
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
        const std::uint32_t owner = m_systemOwner[s];
        if (depth[s] == kUnvisited || owner == kNoFaction || owner == faction || owner >= m_count) {
            continue;
        }
        const float pairRelation = relation(faction, owner);
        if (atWar(faction, owner) || pairRelation < m_params.hostileThreshold) {
            out.push_back({.system = s, .owner = owner, .relation = pairRelation});
        }
    }
}

void FactionSim::applyDefaultDecision(const Galaxy& galaxy, Economy* economy, const FactionDecision& decision)
{
    if (decision.faction >= m_count || decision.roll >= m_params.agents[decision.faction].aggression) {
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

bool FactionSim::commitRaid(const Galaxy& galaxy,
                            Economy* economy,
                            std::uint32_t faction,
                            std::uint32_t targetSystem)
{
    std::vector<RaidCandidate> candidates;
    raidCandidates(galaxy, faction, candidates);
    const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const RaidCandidate& c) {
        return c.system == targetSystem;
    });
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
    // A raid is also a claim (Phase 8u): sustained raiding is what takes a
    // system, and one raid on its own decays away long before it does.
    pressSystem(targetSystem, faction, m_params.contestPerRaid);
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

void FactionSim::addStanding(std::uint32_t faction, float delta)
{
    if (faction < m_count) {
        m_standings[faction] = clampScore(m_standings[faction] + delta);
    }
}

float FactionSim::bounty(std::uint32_t faction) const
{
    return faction < m_count ? m_bounty[faction] : 0.0f;
}

bool FactionSim::wanted() const
{
    for (const float posted : m_bounty) {
        if (posted > 0.0f) {
            return true;
        }
    }
    return false;
}

void FactionSim::addBounty(std::uint32_t faction, float credits)
{
    if (faction < m_count) {
        // Floored at zero and NOT clamped by clampScore: that one is the
        // -100..100 reputation band, and a bounty is credits. Sharing it would
        // silently cap every price in the game at a hundred.
        m_bounty[faction] = std::max(0.0f, m_bounty[faction] + credits);
    }
}

void FactionSim::setBounty(std::uint32_t faction, float credits)
{
    if (faction < m_count) {
        m_bounty[faction] = std::max(0.0f, credits);
    }
}

void FactionSim::recordShipKill(std::uint32_t victimFaction)
{
    if (victimFaction >= m_count) {
        return;
    }
    m_standings[victimFaction] = clampScore(m_standings[victimFaction] - m_params.killPenalty);
    for (std::uint32_t f = 0; f < m_count; ++f) {
        if (f == victimFaction) {
            continue;
        }
        const float pairRelation = relation(victimFaction, f);
        if (!atWar(victimFaction, f) && pairRelation >= m_params.hostileThreshold) {
            continue; // no enmity, no gratitude
        }
        const float depth = core::clamp(-pairRelation / 100.0f, 0.0f, 1.0f);
        m_standings[f] = clampScore(m_standings[f] + m_params.killPenalty * m_params.killWebScale * depth);
    }
}

void FactionSim::recordTrade(std::uint32_t faction, double credits)
{
    if (faction >= m_count || credits <= 0.0) {
        return;
    }
    m_standings[faction] =
        clampScore(m_standings[faction] + static_cast<float>(credits) * m_params.commerceRate);
}

void FactionSim::setRaidIntensity(std::uint32_t system, float value)
{
    if (system < m_raidIntensity.size()) {
        m_raidIntensity[system] = value;
    }
}

float FactionSim::raidIntensity(std::uint32_t system) const
{
    return system < m_raidIntensity.size() ? m_raidIntensity[system] : 0.0f;
}

std::uint32_t FactionSim::lastRaider(std::uint32_t system) const
{
    return system < m_lastRaider.size() ? m_lastRaider[system] : kNoFaction;
}

float FactionSim::danger(std::uint32_t system) const
{
    if (system >= m_systemCount) {
        return 0.0f;
    }
    // Raid intensity counts recent raids and has no ceiling, so it is clamped
    // before it is weighted: the tenth raid in an hour does not make a system
    // ten times deadlier than the first.
    const float raids = core::clamp(m_raidIntensity[system], 0.0f, 1.0f);
    const float pressure =
        m_contestAttacker[system] != kNoFaction ? core::clamp(m_contestPressure[system], 0.0f, 1.0f) : 0.0f;
    return core::clamp(raids * m_params.dangerPerRaid + pressure * m_params.dangerPerContest, 0.0f, 1.0f);
}

void FactionSim::recordTraderLoss(std::uint32_t system, std::uint32_t trader)
{
    m_traderLosses.push_back({.system = system, .trader = trader});
    if (system >= m_systemCount) {
        return; // lost between gates: there is no ground to press
    }
    // ⚑ The sentence Phase 8u could not write. A contest was a meter only the
    // player could move, because nothing simulated a war's attrition and
    // crediting ambient dogfights would have invented state the sim did not
    // have. Traders that can be lost ARE that state.
    //
    // It only SUSTAINS a claim; it never opens one. Opening a contest names an
    // attacker, and attrition has none of its own — a hauler lost to whoever
    // was out there names nobody — so a raid stays the only thing that starts
    // a war, and this is what keeps it from lapsing while the raiding holds.
    const std::uint32_t attacker = m_contestAttacker[system];
    if (attacker != kNoFaction) {
        pressSystem(system, attacker, m_params.contestPerTraderLoss);
    }
}

void FactionSim::takeTraderLosses(std::vector<TraderLoss>& out)
{
    out.insert(out.end(), m_traderLosses.begin(), m_traderLosses.end());
    m_traderLosses.clear();
}

std::uint32_t FactionSim::systemOwner(std::uint32_t system) const
{
    return system < m_systemOwner.size() ? m_systemOwner[system] : kNoFaction;
}

std::uint32_t FactionSim::foundingClaim(std::uint32_t system) const
{
    return system < m_foundingClaim.size() ? m_foundingClaim[system] : kNoFaction;
}

std::uint32_t FactionSim::homeSystem(std::uint32_t faction) const
{
    return faction < m_homeSystem.size() ? m_homeSystem[faction] : kNoFaction;
}

SystemContest FactionSim::contestOf(std::uint32_t system) const
{
    if (system >= m_contestAttacker.size()) {
        return {};
    }
    return {.attacker = m_contestAttacker[system], .pressure = m_contestPressure[system]};
}

bool FactionSim::contested(std::uint32_t system) const
{
    return system < m_contestAttacker.size() && m_contestAttacker[system] != kNoFaction &&
           m_contestPressure[system] >= m_params.contestThreshold;
}

void FactionSim::pressSystem(std::uint32_t system, std::uint32_t attacker, float amount)
{
    if (system >= m_systemCount || attacker >= m_count) {
        return;
    }
    const std::uint32_t owner = m_systemOwner[system];
    if (owner == attacker || owner >= m_count) {
        return; // your own ground, or nobody's to take
    }
    // A faction's home is never contestable. Without it a faction can be
    // erased, and an ownerless galaxy has no boards, no catalogs and no way
    // to spend the standing the player built with it.
    if (m_homeSystem[owner] == system) {
        return;
    }
    std::uint32_t& holder = m_contestAttacker[system];
    if (holder == kNoFaction) {
        holder = attacker;
        m_contestPressure[system] = 0.0f;
    } else if (holder != attacker) {
        return; // one attacker at a time; a rival waits for this one to lapse
    }
    m_contestPressure[system] = core::clamp(m_contestPressure[system] + amount, 0.0f, 1.0f);
    if (m_contestPressure[system] >= 1.0f) {
        resolveContest(system, true);
    } else if (m_contestPressure[system] < m_params.contestFloor) {
        resolveContest(system, false);
    }
}

void FactionSim::resolveContest(std::uint32_t system, bool flipped)
{
    const std::uint32_t attacker = m_contestAttacker[system];
    const std::uint32_t holder = m_systemOwner[system];
    if (attacker == kNoFaction) {
        return;
    }
    if (flipped) {
        m_systemOwner[system] = attacker;
    }
    m_resolutions.push_back({
        .system = system,
        .winner = flipped ? attacker : holder,
        .loser = flipped ? holder : attacker,
        .flipped = flipped,
    });
    m_contestAttacker[system] = kNoFaction;
    m_contestPressure[system] = 0.0f;
}

void FactionSim::recordContestKill(std::uint32_t system, std::uint32_t victimFaction)
{
    if (system >= m_systemCount || m_contestAttacker[system] != victimFaction ||
        victimFaction == kNoFaction) {
        return; // not the faction pressing this system's claim
    }
    m_contestPressure[system] = core::clamp(m_contestPressure[system] - m_params.contestPerKill, 0.0f, 1.0f);
    if (m_contestPressure[system] < m_params.contestFloor) {
        resolveContest(system, false);
    }
}

void FactionSim::setContest(std::uint32_t system, std::uint32_t attacker, float pressure)
{
    if (system >= m_systemCount) {
        return;
    }
    if (attacker == kNoFaction || attacker >= m_count) {
        // Clearing a LIVE contest resolves it as a lapse rather than wiping
        // it silently. Decay to the floor is the natural way a contest ends
        // this way and it queues a resolution; if this lever did not, it
        // could leave a Hold objective waiting on an answer that never comes
        // - a state the running game can never produce.
        if (m_contestAttacker[system] != kNoFaction) {
            resolveContest(system, false);
        }
        m_contestAttacker[system] = kNoFaction;
        m_contestPressure[system] = 0.0f;
        return;
    }
    const std::uint32_t owner = m_systemOwner[system];
    if (owner == attacker || owner >= m_count || m_homeSystem[owner] == system) {
        return; // the same refusals pressSystem applies
    }
    m_contestAttacker[system] = attacker;
    m_contestPressure[system] = core::clamp(pressure, 0.0f, 1.0f);
    if (m_contestPressure[system] >= 1.0f) {
        resolveContest(system, true);
    } else if (m_contestPressure[system] < m_params.contestFloor) {
        resolveContest(system, false);
    }
}

bool FactionSim::flipSystem(std::uint32_t system, std::uint32_t owner)
{
    if (system >= m_systemCount || owner >= m_count || m_systemOwner[system] == owner) {
        return false;
    }
    const std::uint32_t previous = m_systemOwner[system];
    m_systemOwner[system] = owner;
    m_contestAttacker[system] = kNoFaction;
    m_contestPressure[system] = 0.0f;
    m_resolutions.push_back({
        .system = system,
        .winner = owner,
        .loser = previous,
        .flipped = true,
    });
    return true;
}

void FactionSim::takeResolutions(std::vector<ContestResolution>& out)
{
    out.insert(out.end(), m_resolutions.begin(), m_resolutions.end());
    m_resolutions.clear();
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
    // Phase 36 stage A (save v34): beside the standings, because it answers the
    // other half of the same question - what a faction thinks of you, and what
    // it has offered to have done about it.
    for (const float value : m_bounty) {
        writer.write(value);
    }
    for (const float value : m_raidIntensity) {
        writer.write(value);
    }
    for (const std::uint32_t raider : m_lastRaider) {
        writer.write(raider);
    }
    // Territory (Phase 8u, save v11). The founding claim and the home
    // systems re-derive from the galaxy in initialize, so only what has
    // actually moved is written.
    for (const std::uint32_t owner : m_systemOwner) {
        writer.write(owner);
    }
    for (const std::uint32_t attacker : m_contestAttacker) {
        writer.write(attacker);
    }
    for (const float pressure : m_contestPressure) {
        writer.write(pressure);
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
    for (float& value : m_bounty) {
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
    for (std::uint32_t& owner : m_systemOwner) {
        if (!reader.read(owner)) {
            return false;
        }
    }
    for (std::uint32_t& attacker : m_contestAttacker) {
        if (!reader.read(attacker)) {
            return false;
        }
    }
    for (float& pressure : m_contestPressure) {
        if (!reader.read(pressure)) {
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
    m_resolutions.clear(); // transient: a load is not news
    return true;
}

} // namespace sol::sim
