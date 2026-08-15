#include "sol/core/events.hpp"

#include "sol/test/test.hpp"

#include <vector>

namespace {

using sol::core::EventBus;
using sol::core::SubscriptionId;

struct Damage
{
    int amount = 0;
};

struct Spawn
{
    int kind = 0;
};

} // namespace

SOL_TEST(events_publishIsDeferredUntilDispatch)
{
    EventBus bus;
    int received = 0;
    (void)bus.subscribe<Damage>([&received](const Damage& d) { received += d.amount; });

    bus.publish(Damage{.amount = 5});
    bus.publish(Damage{.amount = 7});
    SOL_CHECK(received == 0);
    SOL_CHECK(bus.queuedCount() == 2);

    bus.dispatch();
    SOL_CHECK(received == 12);
    SOL_CHECK(bus.queuedCount() == 0);

    bus.dispatch(); // queue already drained; nothing re-delivered
    SOL_CHECK(received == 12);
}

SOL_TEST(events_deliveryPreservesCrossTypePublishOrder)
{
    EventBus bus;
    std::vector<int> order;
    (void)bus.subscribe<Damage>([&order](const Damage& d) { order.push_back(d.amount); });
    (void)bus.subscribe<Spawn>([&order](const Spawn& s) { order.push_back(100 + s.kind); });

    bus.publish(Damage{.amount = 1});
    bus.publish(Spawn{.kind = 2});
    bus.publish(Damage{.amount = 3});
    bus.dispatch();

    SOL_CHECK(order == (std::vector<int>{1, 102, 3}));
}

SOL_TEST(events_multipleSubscribersAllReceive)
{
    EventBus bus;
    int a = 0;
    int b = 0;
    (void)bus.subscribe<Damage>([&a](const Damage& d) { a += d.amount; });
    (void)bus.subscribe<Damage>([&b](const Damage& d) { b += d.amount * 2; });

    bus.publish(Damage{.amount = 3});
    bus.dispatch();
    SOL_CHECK(a == 3);
    SOL_CHECK(b == 6);
}

SOL_TEST(events_unsubscribeStopsDelivery)
{
    EventBus bus;
    int received = 0;
    const SubscriptionId id =
        bus.subscribe<Damage>([&received](const Damage& d) { received += d.amount; });

    bus.publish(Damage{.amount = 1});
    bus.dispatch();
    SOL_CHECK(received == 1);

    bus.unsubscribe(id);
    bus.publish(Damage{.amount = 1});
    bus.dispatch();
    SOL_CHECK(received == 1);
}

SOL_TEST(events_handlerPublishesAreDeliveredSameDispatch)
{
    EventBus bus;
    std::vector<int> order;
    (void)bus.subscribe<Damage>([&bus, &order](const Damage& d) {
        order.push_back(d.amount);
        if (d.amount == 1) {
            bus.publish(Spawn{.kind = 9}); // chain: delivered later this dispatch
        }
    });
    (void)bus.subscribe<Spawn>([&order](const Spawn& s) { order.push_back(100 + s.kind); });

    bus.publish(Damage{.amount = 1});
    bus.publish(Damage{.amount = 2});
    bus.dispatch();

    // The chained Spawn lands after the already-queued Damage events.
    SOL_CHECK(order == (std::vector<int>{1, 2, 109}));
    SOL_CHECK(bus.queuedCount() == 0);
}

SOL_TEST(events_typesWithNoSubscribersAreDropped)
{
    EventBus bus;
    bus.publish(Spawn{.kind = 1});
    bus.dispatch(); // no handlers registered: must not crash
    SOL_CHECK(bus.queuedCount() == 0);
}
