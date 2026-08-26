#pragma once

#include "sol/ecs/entity.hpp"
#include "sol/ecs/registry.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace sol::ecs {

// Deferred structural changes so systems can iterate views without mutating
// pool storage mid-iteration (and, once the job system lands, record from
// worker threads into per-system buffers merged at a sync point).
//
// create() reserves a real handle immediately - only pool mutations are
// deferred. flush() applies operations in record order; operations targeting
// an entity that is no longer alive at apply time are skipped, so e.g. two
// systems may both destroy the same projectile in one tick.
class CommandBuffer
{
public:
    explicit CommandBuffer(Registry& registry) : m_registry(&registry) {}

    [[nodiscard]] Entity create() { return m_registry->create(); }

    void destroy(Entity entity) { m_ops.push_back(Op{.kind = OpKind::kDestroy, .entity = entity}); }

    template <typename T>
    void add(Entity entity, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "command buffers store recorded components by memcpy");
        const std::size_t offset = alignUp(m_data.size(), alignof(T));
        m_data.resize(offset + sizeof(T));
        std::memcpy(m_data.data() + offset, &value, sizeof(T));
        m_ops.push_back(Op{
            .kind = OpKind::kAdd,
            .entity = entity,
            .dataOffset = static_cast<std::uint32_t>(offset),
            .applyAdd = [](Registry& registry,
                           Entity target,
                           const void* data) { registry.emplace<T>(target, *static_cast<const T*>(data)); },
        });
    }

    template <typename T>
    void remove(Entity entity)
    {
        m_ops.push_back(Op{
            .kind = OpKind::kRemove,
            .entity = entity,
            .applyRemove = [](Registry& registry, Entity target) { registry.remove<T>(target); },
        });
    }

    [[nodiscard]] bool empty() const { return m_ops.empty(); }

    void flush()
    {
        for (const Op& op : m_ops) {
            if (!m_registry->isAlive(op.entity)) {
                continue;
            }
            switch (op.kind) {
            case OpKind::kAdd:
                op.applyAdd(*m_registry, op.entity, m_data.data() + op.dataOffset);
                break;
            case OpKind::kRemove:
                op.applyRemove(*m_registry, op.entity);
                break;
            case OpKind::kDestroy:
                m_registry->destroy(op.entity);
                break;
            }
        }
        m_ops.clear();
        m_data.clear();
    }

private:
    enum class OpKind : std::uint8_t
    {
        kAdd,
        kRemove,
        kDestroy,
    };

    struct Op
    {
        OpKind kind = OpKind::kDestroy;
        Entity entity;
        std::uint32_t dataOffset = 0;
        void (*applyAdd)(Registry&, Entity, const void*) = nullptr;
        void (*applyRemove)(Registry&, Entity) = nullptr;
    };

    [[nodiscard]] static constexpr std::size_t alignUp(std::size_t value, std::size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    Registry* m_registry;
    std::vector<Op> m_ops;
    std::vector<std::byte> m_data;
};

} // namespace sol::ecs
