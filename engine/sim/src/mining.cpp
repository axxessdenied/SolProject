#include "sol/sim/mining.hpp"

#include "sol/core/assert.hpp"

#include <algorithm>
#include <cmath>

namespace sol::sim {

namespace {

// Own stream, seeded from the system seed: adding draws here never perturbs
// the galaxy generator or the signal pass (core plan 2.2 PRNG rule).
constexpr std::uint64_t kFieldStream = 501;

// Fields and rocks live in the packed depletion key; the generator's
// ceilings are far below these, and initialize() asserts the field one.
constexpr std::uint32_t kMaxFieldsPerSystem = 8;
constexpr std::uint32_t kMaxRocksPerField = 1024;

[[nodiscard]] std::uint32_t countInRange(core::Rng& rng, std::uint32_t low, std::uint32_t high)
{
    return low + (high > low ? rng.range(high - low + 1) : 0);
}

[[nodiscard]] double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

// Unit direction over the whole sphere: a rock tumbles about any axis, unlike
// the flattened disc everything is *placed* on.
[[nodiscard]] core::Vec3 randomUnitVector(core::Rng& rng)
{
    constexpr double kTau = 6.283185307179586476925;
    const double z = rng.nextDouble01() * 2.0 - 1.0;
    const double theta = kTau * rng.nextDouble01();
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {static_cast<float>(r * std::cos(theta)), static_cast<float>(r * std::sin(theta)),
            static_cast<float>(z)};
}

// Picks an ore by region-tier weight. Falls back to the first entry when the
// weights are all zero, so a badly tuned table still yields something.
[[nodiscard]] std::uint32_t pickOre(core::Rng& rng, const std::vector<OreEntry>& ores,
                                    std::size_t tier)
{
    float total = 0.0f;
    for (const OreEntry& ore : ores) {
        total += ore.weight[tier] > 0.0f ? ore.weight[tier] : 0.0f;
    }
    if (!(total > 0.0f)) {
        return ores.front().commodity;
    }
    float roll = rng.nextFloat01() * total;
    for (const OreEntry& ore : ores) {
        const float weight = ore.weight[tier] > 0.0f ? ore.weight[tier] : 0.0f;
        if (roll < weight) {
            return ore.commodity;
        }
        roll -= weight;
    }
    return ores.back().commodity;
}

} // namespace

void MiningSim::initialize(const Galaxy& galaxy, const MiningParams& params,
                           std::uint32_t commodityCount, std::uint64_t seed)
{
    m_params = params;
    m_systemCount = static_cast<std::uint32_t>(galaxy.systems.size());
    m_commodityCount = commodityCount;
    m_seed = seed;
    m_fieldCounts.assign(m_systemCount, 0);
    m_depletion.clear();
    m_wrecks.clear();
    m_refineJobs.clear();
    m_nextWreckId = 1;

    std::vector<AsteroidFieldSpec> fields;
    for (std::uint32_t i = 0; i < m_systemCount; ++i) {
        fieldsFor(galaxy, i, fields);
        SOL_ASSERT(fields.size() <= kMaxFieldsPerSystem);
        m_fieldCounts[i] = static_cast<std::uint8_t>(fields.size());
    }
}

void MiningSim::fieldsFor(const Galaxy& galaxy, std::uint32_t system,
                          std::vector<AsteroidFieldSpec>& out) const
{
    out.clear();
    if (system >= galaxy.systems.size() || m_params.ores.empty()) {
        return; // nothing mineable: no fields rather than empty ones
    }
    const SystemSpec& spec = galaxy.systems[system];
    core::Rng rng(spec.seed, kFieldStream);
    const std::size_t tier = static_cast<std::size_t>(spec.region);
    const std::uint32_t count =
        countInRange(rng, m_params.fieldCount[tier][0], m_params.fieldCount[tier][1]);
    const core::DVec3 hub = playfieldHub(spec);
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        AsteroidFieldSpec field;
        const double distance =
            lerp(m_params.fieldMinDistance, m_params.fieldMaxDistance, rng.nextDouble01());
        field.center = hub + randomPlayfieldDirection(rng) * distance;
        field.radius = lerp(m_params.fieldRadiusMin, m_params.fieldRadiusMax, rng.nextDouble01());
        field.rockCount =
            countInRange(rng, m_params.rockCount[tier][0], m_params.rockCount[tier][1]);
        field.seed = rng.nextU64();
        out.push_back(field);
    }
}

void MiningSim::rocksFor(const Galaxy& galaxy, std::uint32_t system, std::uint32_t field,
                         std::vector<RockSpec>& out) const
{
    out.clear();
    std::vector<AsteroidFieldSpec> fields;
    fieldsFor(galaxy, system, fields);
    if (field >= fields.size()) {
        return;
    }
    const AsteroidFieldSpec& spec = fields[field];
    const std::size_t tier = static_cast<std::size_t>(galaxy.systems[system].region);
    const std::uint32_t rockCount = std::min(spec.rockCount, kMaxRocksPerField);
    core::Rng rng(spec.seed, kFieldStream);
    out.reserve(rockCount);
    for (std::uint32_t i = 0; i < rockCount; ++i) {
        RockSpec rock;
        // sqrt keeps the scatter area-uniform, so a field does not read as a
        // dense knot with a thin halo.
        const double distance = spec.radius * std::sqrt(rng.nextDouble01());
        rock.position = spec.center + randomPlayfieldDirection(rng) * distance;
        const double sizeFraction = rng.nextDouble01();
        rock.radius = lerp(m_params.rockRadiusMin, m_params.rockRadiusMax, sizeFraction);
        rock.tumbleAxis = randomUnitVector(rng);
        rock.tumbleRate = rng.rangeFloat(0.01f, 0.12f);
        rock.commodity = pickOre(rng, m_params.ores, tier);
        // Yield tracks size, so a big rock is visibly worth stopping for, with
        // a little jitter and the region's richness on top.
        const double base = lerp(m_params.yieldMin, m_params.yieldMax, sizeFraction);
        const double jitter = lerp(0.85, 1.15, rng.nextDouble01());
        rock.yieldUnits = static_cast<float>(base * jitter
                                             * m_params.regionYieldMultiplier[tier]);
        rock.seed = rng.nextU64();
        out.push_back(rock);
    }
}

std::uint32_t MiningSim::fieldCount(std::uint32_t system) const
{
    return system < m_fieldCounts.size() ? m_fieldCounts[system] : 0;
}

std::uint64_t MiningSim::rockKey(std::uint32_t system, std::uint32_t field, std::uint32_t rock)
{
    return (static_cast<std::uint64_t>(system) << 32) | (static_cast<std::uint64_t>(field) << 16)
           | rock;
}

std::size_t MiningSim::findDepletion(std::uint64_t key) const
{
    const auto it = std::lower_bound(
        m_depletion.begin(), m_depletion.end(), key,
        [](const RockDepletion& record, std::uint64_t value) { return record.key < value; });
    if (it == m_depletion.end() || it->key != key) {
        return m_depletion.size();
    }
    return static_cast<std::size_t>(it - m_depletion.begin());
}

float MiningSim::unitsTaken(std::uint32_t system, std::uint32_t field, std::uint32_t rock) const
{
    const std::size_t index = findDepletion(rockKey(system, field, rock));
    return index < m_depletion.size() ? m_depletion[index].unitsTaken : 0.0f;
}

float MiningSim::unitsLeft(std::uint32_t system, std::uint32_t field, std::uint32_t rock,
                           float totalUnits) const
{
    const float left = totalUnits - unitsTaken(system, field, rock);
    return left > 0.0f ? left : 0.0f;
}

float MiningSim::mineRock(std::uint32_t system, std::uint32_t field, std::uint32_t rock,
                          float totalUnits, float units)
{
    if (system >= m_systemCount || field >= fieldCount(system) || rock >= kMaxRocksPerField
        || !(units > 0.0f) || !(totalUnits > 0.0f)) {
        return 0.0f;
    }
    const std::uint64_t key = rockKey(system, field, rock);
    const std::size_t index = findDepletion(key);
    const float alreadyTaken = index < m_depletion.size() ? m_depletion[index].unitsTaken : 0.0f;
    const float available = totalUnits - alreadyTaken;
    if (!(available > 0.0f)) {
        return 0.0f;
    }
    const float taken = std::min(units, available);
    if (index < m_depletion.size()) {
        m_depletion[index].unitsTaken = alreadyTaken + taken;
    } else {
        // Insert in key order: the vector stays sorted for the binary search
        // and the save order stays stable.
        const auto at = std::lower_bound(
            m_depletion.begin(), m_depletion.end(), key,
            [](const RockDepletion& record, std::uint64_t value) { return record.key < value; });
        m_depletion.insert(at, RockDepletion{.key = key, .unitsTaken = taken});
    }
    return taken;
}

std::uint32_t MiningSim::addWreck(std::uint32_t system, const core::DVec3& position,
                                  std::string defId, std::string name, std::uint64_t seed)
{
    if (system >= m_systemCount) {
        return 0;
    }
    if (m_params.maxWrecks == 0) {
        return 0;
    }
    while (m_wrecks.size() >= m_params.maxWrecks) {
        m_wrecks.erase(m_wrecks.begin()); // oldest first; the store is a window
    }
    WreckRecord wreck;
    wreck.id = m_nextWreckId++;
    wreck.system = system;
    wreck.position = position;
    wreck.defId = std::move(defId);
    wreck.name = std::move(name);
    wreck.seed = seed;
    wreck.decayRemaining = m_params.wreckDecaySeconds;
    m_wrecks.push_back(std::move(wreck));
    return m_wrecks.back().id;
}

const WreckRecord* MiningSim::wreck(std::uint32_t id) const
{
    for (const WreckRecord& record : m_wrecks) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

bool MiningSim::setWreckContents(std::uint32_t id, SignalLoot contents)
{
    if (!validSignalLoot(contents, m_commodityCount, m_params.maxCargoStacks)) {
        return false;
    }
    for (WreckRecord& record : m_wrecks) {
        if (record.id != id) {
            continue;
        }
        if (record.opened) {
            return false; // a hull already cut into is what it is
        }
        record.contents = std::move(contents);
        record.contentsSet = true;
        return true;
    }
    return false;
}

float MiningSim::cutWreckCargo(std::uint32_t id, float units, std::uint32_t* outCommodity)
{
    if (!(units > 0.0f)) {
        return 0.0f;
    }
    for (WreckRecord& record : m_wrecks) {
        if (record.id != id) {
            continue;
        }
        record.opened = true;
        while (!record.contents.cargo.empty()) {
            SignalCargo& stack = record.contents.cargo.front();
            if (!(stack.units > 0.0f)) {
                record.contents.cargo.erase(record.contents.cargo.begin());
                continue;
            }
            const float taken = std::min(units, stack.units);
            stack.units -= taken;
            if (outCommodity != nullptr) {
                *outCommodity = stack.commodity;
            }
            if (stack.units <= 0.001f) {
                record.contents.cargo.erase(record.contents.cargo.begin());
            }
            return taken;
        }
        return 0.0f;
    }
    return 0.0f;
}

bool MiningSim::removeWreck(std::uint32_t id)
{
    for (std::size_t i = 0; i < m_wrecks.size(); ++i) {
        if (m_wrecks[i].id == id) {
            m_wrecks.erase(m_wrecks.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

void MiningSim::wrecksIn(std::uint32_t system, std::vector<std::uint32_t>& out) const
{
    out.clear();
    for (const WreckRecord& record : m_wrecks) {
        if (record.system == system) {
            out.push_back(record.id);
        }
    }
}

double MiningSim::refineFee(float units) const
{
    return units > 0.0f ? static_cast<double>(units) * m_params.refineFeePerUnit : 0.0;
}

double MiningSim::refineDuration(float units) const
{
    return m_params.refineSecondsBase
           + m_params.refineSecondsPerUnit * static_cast<double>(units > 0.0f ? units : 0.0f);
}

float MiningSim::refineOutput(float units) const
{
    return units > 0.0f ? units * m_params.refineRatio : 0.0f;
}

bool MiningSim::startRefineJob(std::uint32_t market, std::uint32_t inputCommodity, float units,
                               std::uint32_t outputCommodity)
{
    if (!(units > 0.0f) || inputCommodity >= m_commodityCount
        || outputCommodity >= m_commodityCount
        || m_refineJobs.size() >= m_params.maxRefineJobs) {
        return false;
    }
    m_refineJobs.push_back({.market = market,
                            .inputCommodity = inputCommodity,
                            .outputCommodity = outputCommodity,
                            .inputUnits = units,
                            .outputUnits = refineOutput(units),
                            .secondsRemaining = refineDuration(units)});
    return true;
}

float MiningSim::readyAt(std::uint32_t market, std::uint32_t commodity) const
{
    float ready = 0.0f;
    for (const RefineJob& job : m_refineJobs) {
        if (job.market == market && job.outputCommodity == commodity
            && job.secondsRemaining <= 0.0) {
            ready += job.outputUnits;
        }
    }
    return ready;
}

double MiningSim::soonestAt(std::uint32_t market) const
{
    double soonest = -1.0;
    for (const RefineJob& job : m_refineJobs) {
        if (job.market != market || job.secondsRemaining <= 0.0) {
            continue;
        }
        if (soonest < 0.0 || job.secondsRemaining < soonest) {
            soonest = job.secondsRemaining;
        }
    }
    return soonest;
}

float MiningSim::collectAt(std::uint32_t market, std::uint32_t commodity, float maxUnits)
{
    if (!(maxUnits > 0.0f)) {
        return 0.0f;
    }
    float taken = 0.0f;
    for (std::size_t i = 0; i < m_refineJobs.size();) {
        RefineJob& job = m_refineJobs[i];
        if (job.market != market || job.outputCommodity != commodity
            || job.secondsRemaining > 0.0) {
            ++i;
            continue;
        }
        const float room = maxUnits - taken;
        if (!(room > 0.0f)) {
            break;
        }
        const float fromJob = std::min(room, job.outputUnits);
        taken += fromJob;
        job.outputUnits -= fromJob;
        if (job.outputUnits > 0.0f) {
            ++i; // a partial collection leaves the rest waiting at the station
        } else {
            m_refineJobs.erase(m_refineJobs.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    return taken;
}

void MiningSim::tick(double dt)
{
    if (!(dt > 0.0)) {
        return;
    }
    for (RefineJob& job : m_refineJobs) {
        if (job.secondsRemaining > 0.0) {
            job.secondsRemaining -= dt;
        }
    }
    for (std::size_t i = 0; i < m_wrecks.size();) {
        m_wrecks[i].decayRemaining -= dt;
        if (m_wrecks[i].decayRemaining <= 0.0) {
            m_wrecks.erase(m_wrecks.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void MiningSim::save(core::BinaryWriter& writer) const
{
    writer.write(m_systemCount);
    writer.write(m_commodityCount);
    writer.write(m_nextWreckId);
    writer.write(static_cast<std::uint32_t>(m_depletion.size()));
    for (const RockDepletion& record : m_depletion) {
        writer.write(record.key);
        writer.write(record.unitsTaken);
    }
    writer.write(static_cast<std::uint32_t>(m_wrecks.size()));
    for (const WreckRecord& record : m_wrecks) {
        writer.write(record.id);
        writer.write(record.system);
        writer.write(record.position.x);
        writer.write(record.position.y);
        writer.write(record.position.z);
        writer.writeString(record.defId);
        writer.writeString(record.name);
        writer.write(record.seed);
        writer.write(record.decayRemaining);
        writer.write(static_cast<std::uint8_t>(record.contentsSet ? 1 : 0));
        writer.write(static_cast<std::uint8_t>(record.opened ? 1 : 0));
        writer.write(static_cast<std::uint32_t>(record.contents.cargo.size()));
        for (const SignalCargo& cargo : record.contents.cargo) {
            writer.write(cargo.commodity);
            writer.write(cargo.units);
        }
        writer.write(record.contents.credits);
        writer.writeString(record.contents.moduleId);
    }
    writer.write(static_cast<std::uint32_t>(m_refineJobs.size()));
    for (const RefineJob& job : m_refineJobs) {
        writer.write(job.market);
        writer.write(job.inputCommodity);
        writer.write(job.outputCommodity);
        writer.write(job.inputUnits);
        writer.write(job.outputUnits);
        writer.write(job.secondsRemaining);
    }
}

bool MiningSim::load(core::BinaryReader& reader)
{
    std::uint32_t systemCount = 0;
    std::uint32_t commodityCount = 0;
    if (!reader.read(systemCount) || systemCount != m_systemCount || !reader.read(commodityCount)
        || commodityCount != m_commodityCount || !reader.read(m_nextWreckId)) {
        return false; // galaxy/defs mismatch: initialize() first
    }
    std::uint32_t depletionCount = 0;
    if (!reader.read(depletionCount)) {
        return false;
    }
    m_depletion.resize(depletionCount);
    std::uint64_t previousKey = 0;
    for (std::size_t i = 0; i < m_depletion.size(); ++i) {
        RockDepletion& record = m_depletion[i];
        if (!reader.read(record.key) || !reader.read(record.unitsTaken)
            || record.unitsTaken < 0.0f) {
            return false;
        }
        if (i > 0 && record.key <= previousKey) {
            return false; // the search below assumes sorted, unique keys
        }
        previousKey = record.key;
    }
    std::uint32_t wreckCount = 0;
    if (!reader.read(wreckCount)) {
        return false;
    }
    m_wrecks.resize(wreckCount);
    for (WreckRecord& record : m_wrecks) {
        std::uint8_t contentsSet = 0;
        std::uint8_t opened = 0;
        std::uint32_t cargoCount = 0;
        if (!reader.read(record.id) || !reader.read(record.system)
            || record.system >= m_systemCount || !reader.read(record.position.x)
            || !reader.read(record.position.y) || !reader.read(record.position.z)
            || !reader.readString(record.defId) || !reader.readString(record.name)
            || !reader.read(record.seed) || !reader.read(record.decayRemaining)
            || !reader.read(contentsSet) || !reader.read(opened) || !reader.read(cargoCount)
            || cargoCount > m_params.maxCargoStacks) {
            return false;
        }
        record.contentsSet = contentsSet != 0;
        record.opened = opened != 0;
        record.contents.cargo.resize(cargoCount);
        for (SignalCargo& cargo : record.contents.cargo) {
            if (!reader.read(cargo.commodity) || cargo.commodity >= m_commodityCount
                || !reader.read(cargo.units)) {
                return false;
            }
        }
        if (!reader.read(record.contents.credits)
            || !reader.readString(record.contents.moduleId)) {
            return false;
        }
    }
    std::uint32_t jobCount = 0;
    if (!reader.read(jobCount) || jobCount > m_params.maxRefineJobs) {
        return false;
    }
    m_refineJobs.resize(jobCount);
    for (RefineJob& job : m_refineJobs) {
        if (!reader.read(job.market) || !reader.read(job.inputCommodity)
            || job.inputCommodity >= m_commodityCount || !reader.read(job.outputCommodity)
            || job.outputCommodity >= m_commodityCount || !reader.read(job.inputUnits)
            || !reader.read(job.outputUnits) || !reader.read(job.secondsRemaining)) {
            return false;
        }
    }
    return true;
}

} // namespace sol::sim
