#pragma once

#include "sol/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

namespace sol::core {

using EventTypeId = std::uint32_t;
using SubscriptionId = std::uint32_t;

namespace detail {

[[nodiscard]] inline EventTypeId nextEventTypeId()
{
    static EventTypeId next = 0;
    return next++;
}

} // namespace detail

template <typename T>
[[nodiscard]] EventTypeId eventTypeId()
{
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>,
                  "event type ids are for plain unqualified types");
    static const EventTypeId id = detail::nextEventTypeId();
    return id;
}

// Queued, ordered event delivery. publish() records events; dispatch()
// delivers everything queued so far, in publish order, to the handlers
// subscribed for each event's type. Events published from inside a handler
// are appended and delivered within the same dispatch() call, which keeps
// cross-system event chains deterministic. Subscribing or unsubscribing
// during dispatch is a programmer error.
class EventBus
{
public:
    template <typename T>
    SubscriptionId subscribe(std::function<void(const T&)> handler)
    {
        SOL_ASSERT(!m_dispatching);
        SOL_ASSERT(handler != nullptr);
        const EventTypeId type = eventTypeId<T>();
        if (type >= m_channels.size()) {
            m_channels.resize(type + 1);
        }
        const SubscriptionId id = m_nextSubscription++;
        m_channels[type].push_back(Handler{
            .id = id,
            .fn = [handler = std::move(handler)](const void* event) {
                handler(*static_cast<const T*>(event));
            },
        });
        return id;
    }

    void unsubscribe(SubscriptionId id)
    {
        SOL_ASSERT(!m_dispatching);
        for (std::vector<Handler>& channel : m_channels) {
            for (std::size_t i = 0; i < channel.size(); ++i) {
                if (channel[i].id == id) {
                    channel.erase(channel.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
        }
    }

    template <typename T>
    void publish(const T& event)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "the event queue stores events by memcpy");
        const std::size_t offset = alignUp(m_data.size(), alignof(T));
        m_data.resize(offset + sizeof(T));
        std::memcpy(m_data.data() + offset, &event, sizeof(T));
        m_queue.push_back(Queued{
            .type = eventTypeId<T>(),
            .dataOffset = static_cast<std::uint32_t>(offset),
        });
    }

    // Delivers queued events in publish order, including events published by
    // handlers during this call, then clears the queue.
    void dispatch()
    {
        SOL_ASSERT(!m_dispatching);
        m_dispatching = true;
        // Indexed loop: handlers may publish, growing the queue and the data
        // buffer, so no iterators or data pointers are cached across calls.
        for (std::size_t i = 0; i < m_queue.size(); ++i) {
            const Queued queued = m_queue[i];
            if (queued.type >= m_channels.size()) {
                continue;
            }
            for (const Handler& handler : m_channels[queued.type]) {
                handler.fn(m_data.data() + queued.dataOffset);
            }
        }
        m_queue.clear();
        m_data.clear();
        m_dispatching = false;
    }

    [[nodiscard]] std::size_t queuedCount() const { return m_queue.size(); }

private:
    struct Handler
    {
        SubscriptionId id = 0;
        std::function<void(const void*)> fn;
    };

    struct Queued
    {
        EventTypeId type = 0;
        std::uint32_t dataOffset = 0;
    };

    [[nodiscard]] static constexpr std::size_t alignUp(std::size_t value, std::size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    std::vector<std::vector<Handler>> m_channels;
    std::vector<Queued> m_queue;
    std::vector<std::byte> m_data;
    SubscriptionId m_nextSubscription = 0;
    bool m_dispatching = false;
};

} // namespace sol::core
