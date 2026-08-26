// Phase 3 ECS storage spike: minimal sparse-set vs archetype/SoA prototypes,
// benchmarked on the engine plan's representative access patterns (2.5):
// ~10k ships iterated linearly per sim tick, sparse component churn on
// projectiles, partial-component queries, random handle lookups.
//
// This is decision input, not engine code. Both prototypes share one workload
// driver so they execute identical operation sequences. Entities are bare
// u32 indices; generations are omitted because they cost the same in either
// storage design. Run the sol_ecs_storage_bench target manually in the
// release preset; outcome recorded in docs/decisions/001-ecs-storage-model.md.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr double kDt = 1.0 / 60.0;
constexpr float kDtF = static_cast<float>(kDt);
constexpr std::uint32_t kShipCount = 10'000;
constexpr std::uint32_t kProjectileCount = 2'000;
constexpr std::uint32_t kShieldChurnPerTick = 256;
constexpr int kWarmupTicks = 16;
constexpr int kMeasuredTicks = 240;
constexpr int kLookupReps = 20;
constexpr std::uint32_t kLookupsPerRep = 100'000;
constexpr std::uint32_t kAbsent = 0xffff'ffffu;

// Accumulated by every workload and printed at the end so the optimizer
// cannot discard the simulated work.
double g_sink = 0.0;

struct Position
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Velocity
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Shield
{
    float strength = 0.0f;
    float regen = 0.0f;
};

struct Lifetime
{
    float remaining = 0.0f;
};

struct Rng
{
    std::uint32_t state = 0x1234'5678u;

    std::uint32_t next()
    {
        std::uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
};

// --------------------------------------------------------------------------
// Prototype A: sparse-set. One set per component type; dense value array +
// dense entity array + entity-indexed sparse redirection table. O(1) add and
// swap-and-pop remove; queries iterate one dense set and probe the others.
// --------------------------------------------------------------------------

template <typename T>
class SparseSet
{
public:
    void add(std::uint32_t entity, const T& value)
    {
        if (entity >= m_sparse.size()) {
            m_sparse.resize(entity + 1, kAbsent);
        }
        m_sparse[entity] = static_cast<std::uint32_t>(m_entities.size());
        m_entities.push_back(entity);
        m_values.push_back(value);
    }

    void remove(std::uint32_t entity)
    {
        const std::uint32_t dense = m_sparse[entity];
        const std::uint32_t last = static_cast<std::uint32_t>(m_entities.size()) - 1;
        if (dense != last) {
            m_entities[dense] = m_entities[last];
            m_values[dense] = m_values[last];
            m_sparse[m_entities[dense]] = dense;
        }
        m_entities.pop_back();
        m_values.pop_back();
        m_sparse[entity] = kAbsent;
    }

    [[nodiscard]] bool has(std::uint32_t entity) const
    {
        return entity < m_sparse.size() && m_sparse[entity] != kAbsent;
    }

    [[nodiscard]] T* tryGet(std::uint32_t entity)
    {
        const std::uint32_t dense = m_sparse[entity];
        return dense == kAbsent ? nullptr : &m_values[dense];
    }

    [[nodiscard]] const T* tryGet(std::uint32_t entity) const
    {
        const std::uint32_t dense = m_sparse[entity];
        return dense == kAbsent ? nullptr : &m_values[dense];
    }

    [[nodiscard]] std::size_t size() const { return m_entities.size(); }

    [[nodiscard]] const std::vector<std::uint32_t>& entities() const { return m_entities; }

    [[nodiscard]] std::vector<T>& values() { return m_values; }

    [[nodiscard]] const std::uint32_t* sparseData() const { return m_sparse.data(); }

    [[nodiscard]] T* valuesData() { return m_values.data(); }

private:
    std::vector<std::uint32_t> m_sparse;
    std::vector<std::uint32_t> m_entities;
    std::vector<T> m_values;
};

class SparseWorld
{
public:
    static constexpr const char* kName = "sparse-set";

    std::uint32_t createShip(const Position& p, const Velocity& v)
    {
        const std::uint32_t e = allocate();
        m_positions.add(e, p);
        m_velocities.add(e, v);
        return e;
    }

    std::uint32_t createShipShielded(const Position& p, const Velocity& v, const Shield& s)
    {
        const std::uint32_t e = createShip(p, v);
        m_shields.add(e, s);
        return e;
    }

    std::uint32_t createProjectile(const Position& p, const Velocity& v, const Lifetime& l)
    {
        const std::uint32_t e = allocate();
        m_positions.add(e, p);
        m_velocities.add(e, v);
        m_lifetimes.add(e, l);
        return e;
    }

    void destroy(std::uint32_t entity)
    {
        m_positions.remove(entity);
        m_velocities.remove(entity);
        if (m_shields.has(entity)) {
            m_shields.remove(entity);
        }
        if (m_lifetimes.has(entity)) {
            m_lifetimes.remove(entity);
        }
        m_freeList.push_back(entity);
    }

    void addShield(std::uint32_t entity, const Shield& s) { m_shields.add(entity, s); }

    void removeShield(std::uint32_t entity) { m_shields.remove(entity); }

    [[nodiscard]] bool hasShield(std::uint32_t entity) const { return m_shields.has(entity); }

    [[nodiscard]] std::uint32_t shieldCount() const { return static_cast<std::uint32_t>(m_shields.size()); }

    [[nodiscard]] const Position& position(std::uint32_t entity) const { return *m_positions.tryGet(entity); }

    // Queries lead with the set that is smallest for the query in question
    // and probe the rest through the sparse table, as a real sparse-set ECS
    // would. The null checks stay even where membership happens to overlap.
    // Iteration hoists data pointers and counts into locals; otherwise the
    // writes made through fn may-alias the vector headers and MSVC reloads
    // them every iteration. The archetype prototype does the same.
    template <typename Fn>
    void forEachPosVel(Fn&& fn)
    {
        const std::uint32_t* entities = m_velocities.entities().data();
        Velocity* velocities = m_velocities.values().data();
        const std::size_t count = m_velocities.size();
        const std::uint32_t* posSparse = m_positions.sparseData();
        Position* posValues = m_positions.valuesData();
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t dense = posSparse[entities[i]];
            if (dense != kAbsent) {
                fn(posValues[dense], velocities[i]);
            }
        }
    }

    template <typename Fn>
    void forEachShieldPos(Fn&& fn)
    {
        const std::uint32_t* entities = m_shields.entities().data();
        Shield* shields = m_shields.values().data();
        const std::size_t count = m_shields.size();
        const std::uint32_t* posSparse = m_positions.sparseData();
        const Position* posValues = m_positions.valuesData();
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t dense = posSparse[entities[i]];
            if (dense != kAbsent) {
                fn(shields[i], posValues[dense]);
            }
        }
    }

    template <typename Fn>
    void forEachLifetime(Fn&& fn)
    {
        const std::uint32_t* entities = m_lifetimes.entities().data();
        Lifetime* lifetimes = m_lifetimes.values().data();
        const std::size_t count = m_lifetimes.size();
        for (std::size_t i = 0; i < count; ++i) {
            fn(entities[i], lifetimes[i]);
        }
    }

private:
    std::uint32_t allocate()
    {
        if (!m_freeList.empty()) {
            const std::uint32_t e = m_freeList.back();
            m_freeList.pop_back();
            return e;
        }
        return m_next++;
    }

    std::uint32_t m_next = 0;
    std::vector<std::uint32_t> m_freeList;
    SparseSet<Position> m_positions;
    SparseSet<Velocity> m_velocities;
    SparseSet<Shield> m_shields;
    SparseSet<Lifetime> m_lifetimes;
};

// --------------------------------------------------------------------------
// Prototype B: archetype/SoA. Entities grouped by component signature into
// tables of parallel columns; queries iterate whole matching tables with no
// per-entity probing; add/remove of a component moves the entity's row to
// another table. Columns are hardcoded per known component here where a real
// implementation would type-erase them - the iteration cost is the same.
// --------------------------------------------------------------------------

constexpr std::uint32_t kPositionBit = 1u << 0;
constexpr std::uint32_t kVelocityBit = 1u << 1;
constexpr std::uint32_t kShieldBit = 1u << 2;
constexpr std::uint32_t kLifetimeBit = 1u << 3;

struct Archetype
{
    std::uint32_t mask = 0;
    std::vector<std::uint32_t> entities;
    std::vector<Position> positions;
    std::vector<Velocity> velocities;
    std::vector<Shield> shields;
    std::vector<Lifetime> lifetimes;

    [[nodiscard]] std::uint32_t rowCount() const { return static_cast<std::uint32_t>(entities.size()); }
};

struct EntityLocation
{
    std::uint32_t archetype = kAbsent;
    std::uint32_t row = 0;
};

class ArchetypeWorld
{
public:
    static constexpr const char* kName = "archetype";

    std::uint32_t createShip(const Position& p, const Velocity& v)
    {
        const std::uint32_t e = allocate();
        Archetype& a = archetypeFor(kPositionBit | kVelocityBit);
        a.positions.push_back(p);
        a.velocities.push_back(v);
        insertRow(a, e);
        return e;
    }

    std::uint32_t createShipShielded(const Position& p, const Velocity& v, const Shield& s)
    {
        const std::uint32_t e = allocate();
        Archetype& a = archetypeFor(kPositionBit | kVelocityBit | kShieldBit);
        a.positions.push_back(p);
        a.velocities.push_back(v);
        a.shields.push_back(s);
        insertRow(a, e);
        return e;
    }

    std::uint32_t createProjectile(const Position& p, const Velocity& v, const Lifetime& l)
    {
        const std::uint32_t e = allocate();
        Archetype& a = archetypeFor(kPositionBit | kVelocityBit | kLifetimeBit);
        a.positions.push_back(p);
        a.velocities.push_back(v);
        a.lifetimes.push_back(l);
        insertRow(a, e);
        return e;
    }

    void destroy(std::uint32_t entity)
    {
        const EntityLocation loc = m_locations[entity];
        removeRow(m_archetypes[loc.archetype], loc.row);
        m_locations[entity].archetype = kAbsent;
        m_freeList.push_back(entity);
    }

    void addShield(std::uint32_t entity, const Shield& s) { moveEntity(entity, m_locations[entity], s); }

    void removeShield(std::uint32_t entity) { moveEntity(entity, m_locations[entity], {}); }

    [[nodiscard]] bool hasShield(std::uint32_t entity) const
    {
        const EntityLocation loc = m_locations[entity];
        return (m_archetypes[loc.archetype].mask & kShieldBit) != 0;
    }

    [[nodiscard]] std::uint32_t shieldCount() const
    {
        std::uint32_t count = 0;
        for (const Archetype& a : m_archetypes) {
            if ((a.mask & kShieldBit) != 0) {
                count += a.rowCount();
            }
        }
        return count;
    }

    [[nodiscard]] const Position& position(std::uint32_t entity) const
    {
        const EntityLocation loc = m_locations[entity];
        return m_archetypes[loc.archetype].positions[loc.row];
    }

    // Column pointers and counts are hoisted out of the row loops for the
    // same aliasing reason as in SparseWorld.
    template <typename Fn>
    void forEachPosVel(Fn&& fn)
    {
        forEachMatching(kPositionBit | kVelocityBit, [&fn](Archetype& a) {
            Position* positions = a.positions.data();
            Velocity* velocities = a.velocities.data();
            const std::uint32_t count = a.rowCount();
            for (std::uint32_t i = 0; i < count; ++i) {
                fn(positions[i], velocities[i]);
            }
        });
    }

    template <typename Fn>
    void forEachShieldPos(Fn&& fn)
    {
        forEachMatching(kShieldBit | kPositionBit, [&fn](Archetype& a) {
            Shield* shields = a.shields.data();
            const Position* positions = a.positions.data();
            const std::uint32_t count = a.rowCount();
            for (std::uint32_t i = 0; i < count; ++i) {
                fn(shields[i], positions[i]);
            }
        });
    }

    template <typename Fn>
    void forEachLifetime(Fn&& fn)
    {
        forEachMatching(kLifetimeBit, [&fn](Archetype& a) {
            const std::uint32_t* entities = a.entities.data();
            Lifetime* lifetimes = a.lifetimes.data();
            const std::uint32_t count = a.rowCount();
            for (std::uint32_t i = 0; i < count; ++i) {
                fn(entities[i], lifetimes[i]);
            }
        });
    }

private:
    std::uint32_t allocate()
    {
        if (!m_freeList.empty()) {
            const std::uint32_t e = m_freeList.back();
            m_freeList.pop_back();
            return e;
        }
        const std::uint32_t e = m_next++;
        m_locations.resize(m_next);
        return e;
    }

    Archetype& archetypeFor(std::uint32_t mask)
    {
        for (Archetype& a : m_archetypes) {
            if (a.mask == mask) {
                return a;
            }
        }
        m_archetypes.push_back(Archetype{.mask = mask});
        return m_archetypes.back();
    }

    // Caller has already pushed the component columns; record the entity row.
    void insertRow(Archetype& a, std::uint32_t entity)
    {
        a.entities.push_back(entity);
        m_locations[entity] = EntityLocation{
            .archetype = static_cast<std::uint32_t>(&a - m_archetypes.data()),
            .row = a.rowCount() - 1,
        };
    }

    void removeRow(Archetype& a, std::uint32_t row)
    {
        const std::uint32_t last = a.rowCount() - 1;
        if (row != last) {
            a.entities[row] = a.entities[last];
            if ((a.mask & kPositionBit) != 0) {
                a.positions[row] = a.positions[last];
            }
            if ((a.mask & kVelocityBit) != 0) {
                a.velocities[row] = a.velocities[last];
            }
            if ((a.mask & kShieldBit) != 0) {
                a.shields[row] = a.shields[last];
            }
            if ((a.mask & kLifetimeBit) != 0) {
                a.lifetimes[row] = a.lifetimes[last];
            }
            m_locations[a.entities[row]].row = row;
        }
        a.entities.pop_back();
        if ((a.mask & kPositionBit) != 0) {
            a.positions.pop_back();
        }
        if ((a.mask & kVelocityBit) != 0) {
            a.velocities.pop_back();
        }
        if ((a.mask & kShieldBit) != 0) {
            a.shields.pop_back();
        }
        if ((a.mask & kLifetimeBit) != 0) {
            a.lifetimes.pop_back();
        }
    }

    // Adding or removing the shield component means relocating the whole row
    // to the archetype with the complementary signature - the structural cost
    // this spike exists to measure.
    void moveEntity(std::uint32_t entity, EntityLocation loc, const Shield& s)
    {
        Archetype& src = m_archetypes[loc.archetype];
        const std::uint32_t dstMask = src.mask ^ kShieldBit;
        Archetype& dst = archetypeFor(dstMask);
        // archetypeFor may reallocate m_archetypes; re-resolve the source.
        Archetype& srcAfter = m_archetypes[loc.archetype];

        dst.positions.push_back(srcAfter.positions[loc.row]);
        dst.velocities.push_back(srcAfter.velocities[loc.row]);
        if ((dstMask & kShieldBit) != 0) {
            dst.shields.push_back(s);
        }
        if ((dstMask & kLifetimeBit) != 0) {
            dst.lifetimes.push_back(srcAfter.lifetimes[loc.row]);
        }
        removeRow(srcAfter, loc.row);
        insertRow(dst, entity);
    }

    template <typename Fn>
    void forEachMatching(std::uint32_t queryMask, Fn&& fn)
    {
        for (Archetype& a : m_archetypes) {
            if ((a.mask & queryMask) == queryMask && a.rowCount() > 0) {
                fn(a);
            }
        }
    }

    std::uint32_t m_next = 0;
    std::vector<std::uint32_t> m_freeList;
    std::vector<EntityLocation> m_locations;
    std::vector<Archetype> m_archetypes;
};

// --------------------------------------------------------------------------
// Workloads. Deterministic setup and identical operation sequences for both
// worlds; per-tick wall times sampled individually.
// --------------------------------------------------------------------------

struct Samples
{
    std::vector<double> microseconds;

    [[nodiscard]] double median() const
    {
        std::vector<double> sorted = microseconds;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }

    [[nodiscard]] double mean() const
    {
        double sum = 0.0;
        for (const double v : microseconds) {
            sum += v;
        }
        return sum / static_cast<double>(microseconds.size());
    }
};

enum WorkloadId : std::size_t
{
    kIntegrate = 0,
    kShieldRegen,
    kProjectileChurn,
    kShieldChurn,
    kRandomLookup,
    kShieldRegenAfterChurn,
    kWorkloadCount,
};

constexpr const char* kWorkloadNames[kWorkloadCount] = {
    "integrate 12k pos+vel / tick",
    "shield regen 2k of 12k / tick",
    "projectile churn ~500 / tick",
    "shield add+remove 256 / tick",
    "random position lookup x100k",
    "shield regen after churn / tick",
};

template <typename TickFn>
Samples benchTicks(int warmup, int measured, TickFn&& tick)
{
    for (int i = 0; i < warmup; ++i) {
        tick();
    }
    Samples samples;
    samples.microseconds.reserve(static_cast<std::size_t>(measured));
    for (int i = 0; i < measured; ++i) {
        const auto start = std::chrono::steady_clock::now();
        tick();
        const auto stop = std::chrono::steady_clock::now();
        samples.microseconds.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    }
    return samples;
}

Position spawnPosition(std::uint32_t i)
{
    const double f = static_cast<double>(i);
    return Position{.x = f * 10.0, .y = f * 0.5, .z = f * (-2.0)};
}

Velocity spawnVelocity(std::uint32_t i)
{
    const double f = static_cast<double>(i % 97);
    return Velocity{.x = 1.0 + f, .y = -0.25 * f, .z = 3.0};
}

template <typename World>
void populate(World& world, std::vector<std::uint32_t>& ships)
{
    ships.reserve(kShipCount);
    for (std::uint32_t i = 0; i < kShipCount; ++i) {
        if (i % 5 == 0) {
            ships.push_back(world.createShipShielded(
                spawnPosition(i), spawnVelocity(i), Shield{.strength = 50.0f, .regen = 1.5f}));
        } else {
            ships.push_back(world.createShip(spawnPosition(i), spawnVelocity(i)));
        }
    }
    for (std::uint32_t i = 0; i < kProjectileCount; ++i) {
        // Stagger initial lifetimes across four ticks so ~500 expire per tick
        // from the first measured tick onward.
        world.createProjectile(spawnPosition(kShipCount + i),
                               spawnVelocity(kShipCount + i),
                               Lifetime{.remaining = kDtF * static_cast<float>(1 + i % 4)});
    }
}

template <typename World>
std::uint32_t runWorkloads(Samples (&results)[kWorkloadCount])
{
    World world;
    std::vector<std::uint32_t> ships;
    populate(world, ships);

    const auto integrateTick = [&world] {
        double sum = 0.0;
        world.forEachPosVel([&sum](Position& p, const Velocity& v) {
            p.x += v.x * kDt;
            p.y += v.y * kDt;
            p.z += v.z * kDt;
            sum += p.x;
        });
        g_sink += sum;
    };

    const auto shieldTick = [&world] {
        double sum = 0.0;
        world.forEachShieldPos([&sum](Shield& s, const Position& p) {
            s.strength = std::min(100.0f, s.strength + s.regen * kDtF);
            sum += static_cast<double>(s.strength) + p.y;
        });
        g_sink += sum;
    };

    results[kIntegrate] = benchTicks(kWarmupTicks, kMeasuredTicks, integrateTick);
    results[kShieldRegen] = benchTicks(kWarmupTicks, kMeasuredTicks, shieldTick);

    std::vector<std::uint32_t> expired;
    std::uint32_t spawnCounter = kShipCount + kProjectileCount;
    results[kProjectileChurn] = benchTicks(kWarmupTicks, kMeasuredTicks, [&] {
        expired.clear();
        world.forEachLifetime([&expired](std::uint32_t entity, Lifetime& l) {
            l.remaining -= kDtF;
            if (l.remaining <= 0.0f) {
                expired.push_back(entity);
            }
        });
        for (const std::uint32_t e : expired) {
            world.destroy(e);
        }
        for (std::size_t i = 0; i < expired.size(); ++i) {
            world.createProjectile(
                spawnPosition(spawnCounter), spawnVelocity(spawnCounter), Lifetime{.remaining = kDtF * 4.0f});
            ++spawnCounter;
        }
        g_sink += static_cast<double>(expired.size());
    });

    std::uint32_t churnCursor = 0;
    results[kShieldChurn] = benchTicks(kWarmupTicks, kMeasuredTicks, [&] {
        for (std::uint32_t i = 0; i < kShieldChurnPerTick; ++i) {
            const std::uint32_t ship = ships[(churnCursor + i * 7u) % kShipCount];
            if (world.hasShield(ship)) {
                world.removeShield(ship);
            } else {
                world.addShield(ship, Shield{.strength = 25.0f, .regen = 1.0f});
            }
        }
        churnCursor += kShieldChurnPerTick;
        g_sink += 1.0;
    });

    Rng rng;
    results[kRandomLookup] = benchTicks(2, kLookupReps, [&] {
        double sum = 0.0;
        for (std::uint32_t i = 0; i < kLookupsPerRep; ++i) {
            sum += world.position(ships[rng.next() % kShipCount]).x;
        }
        g_sink += sum;
    });

    // Same query as kShieldRegen, but after hundreds of churn ticks: the
    // toggling in kShieldChurn drifts the shielded population toward 50% of
    // ships (identically in both storages - same operation sequence) and
    // randomizes iteration-vs-probe order, so this row measures partial-query
    // cost on a churned world. Compare per-shield, using the printed count.
    results[kShieldRegenAfterChurn] = benchTicks(kWarmupTicks, kMeasuredTicks, shieldTick);
    return world.shieldCount();
}

} // namespace

int main()
{
    Samples sparse[kWorkloadCount];
    Samples archetype[kWorkloadCount];
    const std::uint32_t sparseShields = runWorkloads<SparseWorld>(sparse);
    const std::uint32_t archetypeShields = runWorkloads<ArchetypeWorld>(archetype);

    std::printf("ECS storage spike - %u ships (%u shielded), %u projectiles, dt %.4fs\n",
                kShipCount,
                kShipCount / 5,
                kProjectileCount,
                kDt);
    std::printf("median (mean) microseconds per tick, %d measured ticks\n\n", kMeasuredTicks);
    std::printf("%-34s %20s %20s %8s\n", "workload", "sparse-set", "archetype", "ratio");
    for (std::size_t i = 0; i < kWorkloadCount; ++i) {
        const double sMed = sparse[i].median();
        const double aMed = archetype[i].median();
        std::printf("%-34s %10.1f (%6.1f) %10.1f (%6.1f) %7.2fx\n",
                    kWorkloadNames[i],
                    sMed,
                    sparse[i].mean(),
                    aMed,
                    archetype[i].mean(),
                    sMed / aMed);
    }
    std::printf("\nratio > 1 means archetype is faster. shields after churn: %u / %u. "
                "checksum %.3e\n",
                sparseShields,
                archetypeShields,
                g_sink);
    return 0;
}
