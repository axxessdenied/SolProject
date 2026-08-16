#include "sol/sim/survey.hpp"

#include "sol/core/assert.hpp"

#include <algorithm>
#include <cmath>

namespace sol::sim {

namespace {

// Own stream, seeded from the system seed: adding draws here never perturbs
// the galaxy generator's content pass (core plan 2.2 PRNG rule).
constexpr std::uint64_t kSignalStream = 401;

// Bodies and signals live in 32-bit masks; the generator's ceilings are far
// below this, and initialize() asserts it rather than trusting the comment.
constexpr std::uint32_t kMaskBits = 32;

[[nodiscard]] std::uint32_t countInRange(core::Rng& rng, std::uint32_t low, std::uint32_t high)
{
    return low + (high > low ? rng.range(high - low + 1) : 0);
}

} // namespace

const char* signalKindName(SignalKind kind)
{
    switch (kind) {
    case SignalKind::Derelict:
        return "Derelict";
    case SignalKind::Cache:
        return "Cache";
    case SignalKind::Count:
        break;
    }
    return "Signal";
}

void SurveySim::initialize(const Galaxy& galaxy, const SurveyParams& params,
                           std::uint32_t commodityCount, std::uint64_t seed)
{
    m_params = params;
    m_systemCount = static_cast<std::uint32_t>(galaxy.systems.size());
    m_commodityCount = commodityCount;
    m_seed = seed;
    m_systems.assign(m_systemCount, SystemSurvey{});
    m_signalCounts.assign(m_systemCount, 0);
    m_ledger.clear();
    m_loot.clear();
    m_route.clear();

    std::vector<SignalSpec> signals;
    for (std::uint32_t i = 0; i < m_systemCount; ++i) {
        signalsFor(galaxy, i, signals);
        SOL_ASSERT(signals.size() < kMaskBits);
        SOL_ASSERT(bodyCount(galaxy, i) <= kMaskBits);
        m_signalCounts[i] = static_cast<std::uint8_t>(signals.size());
    }
}

void SurveySim::signalsFor(const Galaxy& galaxy, std::uint32_t system,
                           std::vector<SignalSpec>& out) const
{
    out.clear();
    if (system >= galaxy.systems.size()) {
        return;
    }
    const SystemSpec& spec = galaxy.systems[system];
    core::Rng rng(spec.seed, kSignalStream);
    const std::size_t tier = static_cast<std::size_t>(spec.region);
    const std::uint32_t count =
        countInRange(rng, m_params.signalCount[tier][0], m_params.signalCount[tier][1]);
    const core::DVec3 hub = playfieldHub(spec);
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SignalSpec signal;
        signal.kind = static_cast<SignalKind>(
            rng.range(static_cast<std::uint32_t>(SignalKind::Count)));
        const double distance =
            m_params.signalMinDistance
            + (m_params.signalMaxDistance - m_params.signalMinDistance) * rng.nextDouble01();
        signal.position = hub + randomPlayfieldDirection(rng) * distance;
        signal.seed = rng.nextU64();
        out.push_back(std::move(signal));
    }
}

std::uint32_t SurveySim::signalCount(std::uint32_t system) const
{
    return system < m_signalCounts.size() ? m_signalCounts[system] : 0;
}

std::uint32_t SurveySim::bodyCount(const Galaxy& galaxy, std::uint32_t system) const
{
    if (system >= galaxy.systems.size()) {
        return 0;
    }
    return 1 + static_cast<std::uint32_t>(galaxy.systems[system].planets.size());
}

KnowledgeState SurveySim::knowledge(std::uint32_t system) const
{
    return system < m_systems.size() ? m_systems[system].state : KnowledgeState::Unknown;
}

std::uint32_t SurveySim::knownSystemCount() const
{
    std::uint32_t known = 0;
    for (const SystemSurvey& system : m_systems) {
        known += system.state != KnowledgeState::Unknown ? 1u : 0u;
    }
    return known;
}

void SurveySim::addEntry(const Galaxy& galaxy, std::uint32_t system, SurveyKind kind)
{
    if (system >= galaxy.systems.size()) {
        return;
    }
    const SystemSpec& spec = galaxy.systems[system];
    const std::size_t tier = static_cast<std::size_t>(spec.region);
    // Nobody lives here, so nobody has charted it before you: the fringe pays
    // explorers, and an unsettled system pays most.
    const bool first = spec.stations.empty();
    double base = 0.0;
    switch (kind) {
    case SurveyKind::System:
        base = m_params.valueSystem;
        break;
    case SurveyKind::Body:
        base = m_params.valueBody;
        break;
    case SurveyKind::Site:
        base = m_params.valueSite;
        break;
    case SurveyKind::Completion:
        base = m_params.valueCompletion;
        break;
    case SurveyKind::Count:
        return;
    }
    const double value = base * static_cast<double>(m_params.regionMultiplier[tier])
                         * (first ? static_cast<double>(m_params.firstDiscoveryBonus) : 1.0);
    m_ledger.push_back({.system = system,
                        .kind = kind,
                        .region = spec.region,
                        .firstDiscovery = first,
                        .value = value});
}

void SurveySim::checkSurveyed(const Galaxy& galaxy, std::uint32_t system)
{
    if (system >= m_systems.size()) {
        return;
    }
    SystemSurvey& state = m_systems[system];
    if (state.state != KnowledgeState::Visited) {
        return; // not there yet, or already Surveyed and paid
    }
    const std::uint32_t bodies = bodyCount(galaxy, system);
    const std::uint32_t signals = signalCount(system);
    const std::uint32_t bodyMask = bodies >= kMaskBits ? ~0u : (1u << bodies) - 1u;
    const std::uint32_t signalMask = signals >= kMaskBits ? ~0u : (1u << signals) - 1u;
    if ((state.bodiesScanned & bodyMask) != bodyMask
        || (state.signalsResolved & signalMask) != signalMask) {
        return;
    }
    state.state = KnowledgeState::Surveyed;
    addEntry(galaxy, system, SurveyKind::Completion);
}

void SurveySim::notifyArrival(const Galaxy& galaxy, std::uint32_t system)
{
    if (system >= m_systems.size() || system >= galaxy.systems.size()) {
        return;
    }
    SystemSurvey& state = m_systems[system];
    if (state.state < KnowledgeState::Visited) {
        state.state = KnowledgeState::Visited;
        addEntry(galaxy, system, SurveyKind::System);
    }
    // A gate names where it goes: the map grows along the lanes you travel.
    for (const GateSpec& gate : galaxy.systems[system].gates) {
        if (gate.toSystem < m_systems.size()
            && m_systems[gate.toSystem].state == KnowledgeState::Unknown) {
            m_systems[gate.toSystem].state = KnowledgeState::Charted;
        }
    }
    checkSurveyed(galaxy, system);

    // Route bookkeeping: advance past the systems already flown, and drop the
    // plot entirely once the player leaves it or reaches the end.
    const auto hit = std::find(m_route.begin(), m_route.end(), system);
    if (hit == m_route.end() || hit + 1 == m_route.end()) {
        m_route.clear();
    } else {
        m_route.erase(m_route.begin(), hit);
    }
}

void SurveySim::setKnowledge(const Galaxy& galaxy, std::uint32_t system, KnowledgeState state)
{
    if (system >= m_systems.size()) {
        return;
    }
    m_systems[system].state = state;
    if (state >= KnowledgeState::Visited && system < galaxy.systems.size()) {
        for (const GateSpec& gate : galaxy.systems[system].gates) {
            if (gate.toSystem < m_systems.size()
                && m_systems[gate.toSystem].state == KnowledgeState::Unknown) {
                m_systems[gate.toSystem].state = KnowledgeState::Charted;
            }
        }
    }
}

bool SurveySim::bodyScanned(std::uint32_t system, std::uint32_t body) const
{
    if (system >= m_systems.size() || body >= kMaskBits) {
        return false;
    }
    return (m_systems[system].bodiesScanned & (1u << body)) != 0;
}

bool SurveySim::notifyBodyScanned(const Galaxy& galaxy, std::uint32_t system, std::uint32_t body)
{
    if (system >= m_systems.size() || body >= bodyCount(galaxy, system) || body >= kMaskBits) {
        return false;
    }
    if (m_systems[system].state < KnowledgeState::Visited) {
        return false; // you cannot scan a body from another system
    }
    if (bodyScanned(system, body)) {
        return false;
    }
    m_systems[system].bodiesScanned |= 1u << body;
    addEntry(galaxy, system, SurveyKind::Body);
    checkSurveyed(galaxy, system);
    return true;
}

bool SurveySim::signalDiscovered(std::uint32_t system, std::uint32_t signal) const
{
    if (system >= m_systems.size() || signal >= kMaskBits) {
        return false;
    }
    return (m_systems[system].signalsDiscovered & (1u << signal)) != 0;
}

bool SurveySim::signalResolved(std::uint32_t system, std::uint32_t signal) const
{
    if (system >= m_systems.size() || signal >= kMaskBits) {
        return false;
    }
    return (m_systems[system].signalsResolved & (1u << signal)) != 0;
}

bool SurveySim::signalEmptied(std::uint32_t system, std::uint32_t signal) const
{
    if (system >= m_systems.size() || signal >= kMaskBits) {
        return false;
    }
    return (m_systems[system].signalsEmptied & (1u << signal)) != 0;
}

bool SurveySim::notifySignalDiscovered(std::uint32_t system, std::uint32_t signal)
{
    if (system >= m_systems.size() || signal >= signalCount(system)) {
        return false;
    }
    if (signalDiscovered(system, signal)) {
        return false;
    }
    m_systems[system].signalsDiscovered |= 1u << signal;
    return true;
}

bool SurveySim::notifySignalResolved(const Galaxy& galaxy, std::uint32_t system,
                                     std::uint32_t signal)
{
    if (system >= m_systems.size() || signal >= signalCount(system)) {
        return false;
    }
    if (signalResolved(system, signal)) {
        return false;
    }
    m_systems[system].signalsDiscovered |= 1u << signal;
    m_systems[system].signalsResolved |= 1u << signal;
    addEntry(galaxy, system, SurveyKind::Site);
    checkSurveyed(galaxy, system);
    return true;
}

bool SurveySim::notifySignalEmptied(std::uint32_t system, std::uint32_t signal)
{
    if (system >= m_systems.size() || signal >= signalCount(system)
        || !signalResolved(system, signal) || signalEmptied(system, signal)) {
        return false;
    }
    m_systems[system].signalsEmptied |= 1u << signal;
    const std::size_t index = findLoot(system, signal);
    if (index < m_loot.size()) {
        m_loot.erase(m_loot.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return true;
}

std::size_t SurveySim::findLoot(std::uint32_t system, std::uint32_t signal) const
{
    for (std::size_t i = 0; i < m_loot.size(); ++i) {
        if (m_loot[i].system == system && m_loot[i].signal == signal) {
            return i;
        }
    }
    return m_loot.size();
}

bool validSignalLoot(const SignalLoot& loot, std::uint32_t commodityCount,
                     std::uint32_t maxCargoStacks)
{
    if (loot.cargo.size() > maxCargoStacks || loot.credits < 0.0) {
        return false;
    }
    for (const SignalCargo& cargo : loot.cargo) {
        if (cargo.commodity >= commodityCount || !(cargo.units > 0.0f)) {
            return false;
        }
    }
    return true;
}

bool SurveySim::setLoot(std::uint32_t system, std::uint32_t signal, SignalLoot loot)
{
    if (!signalResolved(system, signal) || signalEmptied(system, signal)) {
        return false;
    }
    if (!validSignalLoot(loot, m_commodityCount, m_params.maxCargoStacks)) {
        return false;
    }
    const std::size_t index = findLoot(system, signal);
    if (index < m_loot.size()) {
        m_loot[index].loot = std::move(loot);
    } else {
        m_loot.push_back({.system = system, .signal = signal, .loot = std::move(loot)});
    }
    return true;
}

const SignalLoot* SurveySim::loot(std::uint32_t system, std::uint32_t signal) const
{
    const std::size_t index = findLoot(system, signal);
    return index < m_loot.size() ? &m_loot[index].loot : nullptr;
}

double SurveySim::ledgerValue() const
{
    double total = 0.0;
    for (const SurveyEntry& entry : m_ledger) {
        total += entry.value;
    }
    return total;
}

double SurveySim::sellLedger()
{
    const double total = ledgerValue();
    m_ledger.clear();
    return total;
}

void SurveySim::setRoute(std::vector<std::uint32_t> route)
{
    m_route.clear();
    for (const std::uint32_t system : route) {
        if (system >= m_systemCount) {
            m_route.clear(); // a bad plot leaves no route rather than half of one
            return;
        }
        m_route.push_back(system);
    }
    if (m_route.size() < 2) {
        m_route.clear(); // a route to where you already are is not a route
    }
}

std::uint32_t SurveySim::nextHop() const
{
    return m_route.size() >= 2 ? m_route[1] : kNoSystem;
}

void SurveySim::save(core::BinaryWriter& writer) const
{
    writer.write(m_systemCount);
    writer.write(m_commodityCount);
    for (const SystemSurvey& system : m_systems) {
        writer.write(static_cast<std::uint8_t>(system.state));
        writer.write(system.bodiesScanned);
        writer.write(system.signalsDiscovered);
        writer.write(system.signalsResolved);
        writer.write(system.signalsEmptied);
    }
    writer.write(static_cast<std::uint32_t>(m_ledger.size()));
    for (const SurveyEntry& entry : m_ledger) {
        writer.write(entry.system);
        writer.write(static_cast<std::uint32_t>(entry.kind));
        writer.write(static_cast<std::uint8_t>(entry.region));
        writer.write(static_cast<std::uint8_t>(entry.firstDiscovery ? 1 : 0));
        writer.write(entry.value);
    }
    writer.write(static_cast<std::uint32_t>(m_loot.size()));
    for (const LootRecord& record : m_loot) {
        writer.write(record.system);
        writer.write(record.signal);
        writer.write(static_cast<std::uint32_t>(record.loot.cargo.size()));
        for (const SignalCargo& cargo : record.loot.cargo) {
            writer.write(cargo.commodity);
            writer.write(cargo.units);
        }
        writer.write(record.loot.credits);
        writer.writeString(record.loot.moduleId);
    }
    writer.write(static_cast<std::uint32_t>(m_route.size()));
    for (const std::uint32_t system : m_route) {
        writer.write(system);
    }
}

bool SurveySim::load(core::BinaryReader& reader)
{
    std::uint32_t systemCount = 0;
    std::uint32_t commodityCount = 0;
    if (!reader.read(systemCount) || systemCount != m_systemCount || !reader.read(commodityCount)
        || commodityCount != m_commodityCount) {
        return false; // galaxy/defs mismatch: initialize() first
    }
    for (SystemSurvey& system : m_systems) {
        std::uint8_t state = 0;
        if (!reader.read(state) || state > static_cast<std::uint8_t>(KnowledgeState::Surveyed)
            || !reader.read(system.bodiesScanned) || !reader.read(system.signalsDiscovered)
            || !reader.read(system.signalsResolved) || !reader.read(system.signalsEmptied)) {
            return false;
        }
        system.state = static_cast<KnowledgeState>(state);
    }
    std::uint32_t ledgerCount = 0;
    if (!reader.read(ledgerCount)) {
        return false;
    }
    m_ledger.resize(ledgerCount);
    for (SurveyEntry& entry : m_ledger) {
        std::uint32_t kind = 0;
        std::uint8_t region = 0;
        std::uint8_t first = 0;
        if (!reader.read(entry.system) || entry.system >= m_systemCount || !reader.read(kind)
            || kind >= static_cast<std::uint32_t>(SurveyKind::Count) || !reader.read(region)
            || region > static_cast<std::uint8_t>(Region::Fringe) || !reader.read(first)
            || !reader.read(entry.value)) {
            return false;
        }
        entry.kind = static_cast<SurveyKind>(kind);
        entry.region = static_cast<Region>(region);
        entry.firstDiscovery = first != 0;
    }
    std::uint32_t lootCount = 0;
    if (!reader.read(lootCount)) {
        return false;
    }
    m_loot.resize(lootCount);
    for (LootRecord& record : m_loot) {
        std::uint32_t cargoCount = 0;
        if (!reader.read(record.system) || record.system >= m_systemCount
            || !reader.read(record.signal) || !reader.read(cargoCount)
            || cargoCount > m_params.maxCargoStacks) {
            return false;
        }
        record.loot.cargo.resize(cargoCount);
        for (SignalCargo& cargo : record.loot.cargo) {
            if (!reader.read(cargo.commodity) || cargo.commodity >= m_commodityCount
                || !reader.read(cargo.units)) {
                return false;
            }
        }
        if (!reader.read(record.loot.credits) || !reader.readString(record.loot.moduleId)) {
            return false;
        }
    }
    std::uint32_t routeCount = 0;
    if (!reader.read(routeCount)) {
        return false;
    }
    m_route.resize(routeCount);
    for (std::uint32_t& system : m_route) {
        if (!reader.read(system) || system >= m_systemCount) {
            return false;
        }
    }
    return true;
}

} // namespace sol::sim
