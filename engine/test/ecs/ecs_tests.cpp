#include "sol/ecs/ecs.hpp"
#include "sol/test/test.hpp"

#include <vector>

namespace {

using sol::ecs::CommandBuffer;
using sol::ecs::Entity;
using sol::ecs::Registry;

struct Pos
{
    double x = 0.0;
    double y = 0.0;
};

struct Vel
{
    double x = 0.0;
    double y = 0.0;
};

struct Tag
{
    int value = 0;
};

// Non-trivially-copyable on purpose: exercises the move path in swap-and-pop.
struct Blob
{
    std::vector<int> data;
};

} // namespace

SOL_TEST(ecs_entityLifecycleAndGenerationRecycling)
{
    Registry registry;
    const Entity a = registry.create();
    const Entity b = registry.create();
    SOL_CHECK(registry.isAlive(a));
    SOL_CHECK(registry.isAlive(b));
    SOL_CHECK(registry.aliveCount() == 2);
    SOL_CHECK(a.index != b.index);

    registry.destroy(a);
    SOL_CHECK(!registry.isAlive(a));
    SOL_CHECK(registry.isAlive(b));
    SOL_CHECK(registry.aliveCount() == 1);

    const Entity recycled = registry.create();
    SOL_CHECK(recycled.index == a.index);
    SOL_CHECK(recycled.generation == a.generation + 1);
    SOL_CHECK(registry.isAlive(recycled));
    SOL_CHECK(!registry.isAlive(a));
}

SOL_TEST(ecs_componentAddGetRemove)
{
    Registry registry;
    const Entity e = registry.create();
    SOL_CHECK(!registry.has<Pos>(e));

    registry.emplace<Pos>(e, Pos{.x = 1.5, .y = -2.0});
    SOL_CHECK(registry.has<Pos>(e));
    SOL_CHECK(registry.get<Pos>(e).x == 1.5);
    SOL_CHECK(registry.tryGet<Pos>(e) != nullptr);
    SOL_CHECK(registry.tryGet<Vel>(e) == nullptr);

    registry.get<Pos>(e).y = 7.0;
    SOL_CHECK(registry.get<Pos>(e).y == 7.0);

    registry.remove<Pos>(e);
    SOL_CHECK(!registry.has<Pos>(e));
    SOL_CHECK(registry.tryGet<Pos>(e) == nullptr);
}

SOL_TEST(ecs_swapAndPopKeepsOtherValuesIntact)
{
    Registry registry;
    Entity entities[5];
    for (int i = 0; i < 5; ++i) {
        entities[i] = registry.create();
        registry.emplace<Tag>(entities[i], Tag{.value = i * 10});
    }

    registry.remove<Tag>(entities[2]);
    SOL_CHECK(!registry.has<Tag>(entities[2]));
    for (const int i : {0, 1, 3, 4}) {
        SOL_CHECK(registry.get<Tag>(entities[i]).value == i * 10);
    }
}

SOL_TEST(ecs_staleHandlesNeverAliasRecycledEntities)
{
    Registry registry;
    const Entity stale = registry.create();
    registry.emplace<Pos>(stale, Pos{.x = 1.0});
    registry.destroy(stale);

    const Entity fresh = registry.create();
    SOL_CHECK(fresh.index == stale.index);
    registry.emplace<Pos>(fresh, Pos{.x = 99.0});

    SOL_CHECK(!registry.has<Pos>(stale));
    SOL_CHECK(registry.tryGet<Pos>(stale) == nullptr);
    SOL_CHECK(registry.get<Pos>(fresh).x == 99.0);
}

SOL_TEST(ecs_viewIteratesExactIntersection)
{
    Registry registry;
    Entity both[3];
    for (int i = 0; i < 3; ++i) {
        const Entity e = registry.create();
        registry.emplace<Pos>(e, Pos{.x = static_cast<double>(i)});
    }
    for (int i = 0; i < 3; ++i) {
        both[i] = registry.create();
        registry.emplace<Pos>(both[i], Pos{.x = 100.0 + i});
        registry.emplace<Vel>(both[i], Vel{.x = 1.0});
    }
    for (int i = 0; i < 2; ++i) {
        const Entity e = registry.create();
        registry.emplace<Vel>(e, Vel{.x = -1.0});
    }

    // Lead pool is the smallest participant: Vel has 5 entries vs Pos's 6.
    SOL_CHECK(registry.view<Pos, Vel>().sizeHint() == 5);

    int matches = 0;
    registry.view<Pos, Vel>().each([&](Entity entity, Pos& p, const Vel& v) {
        SOL_CHECK(registry.isAlive(entity));
        SOL_CHECK(p.x >= 100.0);
        SOL_CHECK(v.x == 1.0);
        p.x += 1000.0;
        ++matches;
    });
    SOL_CHECK(matches == 3);
    for (int i = 0; i < 3; ++i) {
        SOL_CHECK(registry.get<Pos>(both[i]).x == 1100.0 + i);
    }
}

SOL_TEST(ecs_destroyRemovesEntityFromAllPools)
{
    Registry registry;
    const Entity a = registry.create();
    registry.emplace<Pos>(a, Pos{});
    registry.emplace<Vel>(a, Vel{});
    const Entity b = registry.create();
    registry.emplace<Pos>(b, Pos{});
    registry.emplace<Vel>(b, Vel{});

    registry.destroy(a);
    int matches = 0;
    registry.view<Pos, Vel>().each([&matches](Entity, Pos&, Vel&) { ++matches; });
    SOL_CHECK(matches == 1);
}

SOL_TEST(ecs_commandBufferDefersUntilFlush)
{
    Registry registry;
    const Entity e = registry.create();
    registry.emplace<Pos>(e, Pos{.x = 1.0});

    CommandBuffer commands(registry);
    commands.add(e, Vel{.x = 5.0});
    commands.remove<Pos>(e);
    const Entity created = commands.create();
    commands.add(created, Tag{.value = 42});

    // create() reserves immediately; pool mutations wait for flush.
    SOL_CHECK(registry.isAlive(created));
    SOL_CHECK(!registry.has<Vel>(e));
    SOL_CHECK(registry.has<Pos>(e));
    SOL_CHECK(!registry.has<Tag>(created));
    SOL_CHECK(!commands.empty());

    commands.flush();
    SOL_CHECK(commands.empty());
    SOL_CHECK(registry.has<Vel>(e));
    SOL_CHECK(registry.get<Vel>(e).x == 5.0);
    SOL_CHECK(!registry.has<Pos>(e));
    SOL_CHECK(registry.get<Tag>(created).value == 42);
}

SOL_TEST(ecs_commandBufferSkipsOpsOnDeadEntities)
{
    Registry registry;
    const Entity e = registry.create();
    registry.emplace<Pos>(e, Pos{});

    CommandBuffer commands(registry);
    commands.destroy(e);
    commands.destroy(e);            // second destroy must be a no-op
    commands.add(e, Vel{.x = 1.0}); // recorded after destroy: skipped
    commands.flush();

    SOL_CHECK(!registry.isAlive(e));
    SOL_CHECK(registry.aliveCount() == 0);
}

SOL_TEST(ecs_nonTrivialComponentsSurviveSwapAndPop)
{
    Registry registry;
    Entity entities[3];
    for (int i = 0; i < 3; ++i) {
        entities[i] = registry.create();
        registry.emplace<Blob>(entities[i], Blob{.data = {i, i + 1, i + 2}});
    }

    registry.remove<Blob>(entities[0]); // last entry moves into slot 0
    SOL_CHECK(registry.get<Blob>(entities[1]).data == (std::vector<int>{1, 2, 3}));
    SOL_CHECK(registry.get<Blob>(entities[2]).data == (std::vector<int>{2, 3, 4}));
}
