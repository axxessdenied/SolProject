#include "sol/ecs/snapshot.hpp"

#include "sol/core/serialize.hpp"
#include "sol/ecs/ecs.hpp"
#include "sol/test/test.hpp"

#include <cstdint>

namespace {

using sol::core::BinaryReader;
using sol::core::BinaryWriter;
using sol::ecs::Entity;
using sol::ecs::Registry;
using sol::ecs::Snapshot;

struct Pos
{
    double x = 0.0;
    double y = 0.0;
};

struct Hp
{
    int value = 0;
};

constexpr std::uint32_t kPosId = 1;
constexpr std::uint32_t kHpId = 2;

Snapshot makeSchema()
{
    Snapshot snapshot;
    snapshot.component<Pos>(kPosId);
    snapshot.component<Hp>(kHpId);
    return snapshot;
}

// A world with component variety, a destroyed slot, and a recycled slot.
void buildWorld(Registry& registry, Entity out[4])
{
    out[0] = registry.create();
    registry.emplace<Pos>(out[0], Pos{.x = 1.0, .y = 2.0});
    registry.emplace<Hp>(out[0], Hp{.value = 100});

    out[1] = registry.create();
    registry.emplace<Pos>(out[1], Pos{.x = -3.0, .y = 4.0});

    out[2] = registry.create(); // destroyed: slot dead, generation bumped
    registry.emplace<Hp>(out[2], Hp{.value = 5});
    registry.destroy(out[2]);

    out[3] = registry.create(); // recycles out[2]'s slot at generation 1
    registry.emplace<Hp>(out[3], Hp{.value = 55});
    registry.destroy(out[1]); // leaves a hole in the free list at save time
}

} // namespace

SOL_TEST(snapshot_saveLoadRoundTripsWorldState)
{
    const Snapshot schema = makeSchema();
    Registry original;
    Entity entities[4];
    buildWorld(original, entities);

    BinaryWriter writer;
    schema.save(original, writer);

    Registry loaded;
    BinaryReader reader(writer.data());
    SOL_CHECK(schema.load(loaded, reader));

    SOL_CHECK(loaded.aliveCount() == original.aliveCount());
    SOL_CHECK(loaded.isAlive(entities[0]));
    SOL_CHECK(!loaded.isAlive(entities[1])); // destroyed before save
    SOL_CHECK(!loaded.isAlive(entities[2])); // stale handle stays stale
    SOL_CHECK(loaded.isAlive(entities[3])); // recycled slot, right generation

    SOL_CHECK(loaded.get<Pos>(entities[0]).x == 1.0);
    SOL_CHECK(loaded.get<Pos>(entities[0]).y == 2.0);
    SOL_CHECK(loaded.get<Hp>(entities[0]).value == 100);
    SOL_CHECK(loaded.get<Hp>(entities[3]).value == 55);
    SOL_CHECK(!loaded.has<Pos>(entities[1]));
    SOL_CHECK(!loaded.has<Hp>(entities[2]));

    // A slot freed before save must be recyclable after load.
    const Entity recycled = loaded.create();
    SOL_CHECK(recycled.index == entities[1].index);
    SOL_CHECK(recycled.generation == entities[1].generation + 1);
}

SOL_TEST(snapshot_saveLoadSaveIsByteIdentical)
{
    const Snapshot schema = makeSchema();
    Registry original;
    Entity entities[4];
    buildWorld(original, entities);

    BinaryWriter first;
    schema.save(original, first);

    Registry loaded;
    BinaryReader reader(first.data());
    SOL_CHECK(schema.load(loaded, reader));

    BinaryWriter second;
    schema.save(loaded, second);
    SOL_CHECK(first.data() == second.data());
}

SOL_TEST(snapshot_unknownChunksAreSkipped)
{
    // Save with the full schema, load with a schema that only knows Hp: the
    // Pos chunk must be skipped cleanly (forward compatibility).
    const Snapshot fullSchema = makeSchema();
    Registry original;
    Entity entities[4];
    buildWorld(original, entities);

    BinaryWriter writer;
    fullSchema.save(original, writer);

    Snapshot hpOnly;
    hpOnly.component<Hp>(kHpId);
    Registry loaded;
    BinaryReader reader(writer.data());
    SOL_CHECK(hpOnly.load(loaded, reader));
    SOL_CHECK(loaded.get<Hp>(entities[0]).value == 100);
    SOL_CHECK(!loaded.has<Pos>(entities[0]));
    SOL_CHECK(loaded.aliveCount() == original.aliveCount());
}

SOL_TEST(snapshot_rejectsGarbageAndTruncation)
{
    const Snapshot schema = makeSchema();

    {
        Registry registry;
        BinaryWriter writer;
        writer.write(std::uint32_t{0x1234'5678u}); // wrong magic
        writer.write(Snapshot::kVersion);
        BinaryReader reader(writer.data());
        SOL_CHECK(!schema.load(registry, reader));
    }
    {
        Registry original;
        Entity entities[4];
        buildWorld(original, entities);
        BinaryWriter writer;
        schema.save(original, writer);

        // Truncate the blob at every prefix length; load must fail, never crash.
        const std::vector<std::byte>& bytes = writer.data();
        for (std::size_t length = 0; length < bytes.size(); length += 7) {
            Registry registry;
            BinaryReader reader(std::span<const std::byte>(bytes.data(), length));
            SOL_CHECK(!schema.load(registry, reader));
        }
    }
}
