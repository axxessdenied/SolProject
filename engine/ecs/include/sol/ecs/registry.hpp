#pragma once

#include "sol/core/assert.hpp"
#include "sol/ecs/entity.hpp"
#include "sol/ecs/sparse_set.hpp"
#include "sol/ecs/view.hpp"

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace sol::ecs {

using ComponentTypeId = std::uint32_t;

namespace detail {

[[nodiscard]] inline ComponentTypeId nextComponentTypeId()
{
    static ComponentTypeId next = 0;
    return next++;
}

} // namespace detail

// Stable per-process id for a component type; assigned on first use.
template <typename T>
[[nodiscard]] ComponentTypeId componentTypeId()
{
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>,
                  "component type ids are for plain unqualified types");
    static const ComponentTypeId id = detail::nextComponentTypeId();
    return id;
}

class Registry
{
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) = default;
    Registry& operator=(Registry&&) = default;
    ~Registry() = default;

    [[nodiscard]] Entity create();

    // Removes every component the entity has and retires the handle.
    void destroy(Entity entity);

    [[nodiscard]] bool isAlive(Entity entity) const
    {
        return entity.index < m_generations.size() && m_generations[entity.index] == entity.generation;
    }

    [[nodiscard]] std::size_t aliveCount() const { return m_generations.size() - m_freeList.size(); }

    // Rebuilds the full handle for a slot (pools store bare indices; callers
    // iterating a pool need this to destroy or re-reference entities).
    [[nodiscard]] Entity entityFromIndex(std::uint32_t index) const
    {
        SOL_ASSERT(index < m_generations.size());
        return {index, m_generations[index]};
    }

    template <typename T, typename... Args>
    T& emplace(Entity entity, Args&&... args)
    {
        SOL_ASSERT(isAlive(entity));
        return poolFor<T>().emplace(entity.index, std::forward<Args>(args)...);
    }

    template <typename T>
    void remove(Entity entity)
    {
        SOL_ASSERT(isAlive(entity));
        poolFor<T>().remove(entity.index);
    }

    template <typename T>
    [[nodiscard]] bool has(Entity entity) const
    {
        const Pool<T>* pool = poolIfExists<T>();
        return isAlive(entity) && pool != nullptr && pool->contains(entity.index);
    }

    template <typename T>
    [[nodiscard]] T* tryGet(Entity entity)
    {
        if (!isAlive(entity)) {
            return nullptr;
        }
        Pool<T>* pool = poolIfExists<T>();
        return pool != nullptr ? pool->tryGet(entity.index) : nullptr;
    }

    template <typename T>
    [[nodiscard]] const T* tryGet(Entity entity) const
    {
        if (!isAlive(entity)) {
            return nullptr;
        }
        const Pool<T>* pool = poolIfExists<T>();
        return pool != nullptr ? pool->tryGet(entity.index) : nullptr;
    }

    template <typename T>
    [[nodiscard]] T& get(Entity entity)
    {
        SOL_ASSERT(isAlive(entity));
        return poolFor<T>().get(entity.index);
    }

    template <typename... Ts>
    [[nodiscard]] View<Ts...> view()
    {
        return View<Ts...>(m_generations, poolFor<Ts>()...);
    }

    // System-level access to a component pool's dense arrays for bulk or
    // job-parallel iteration (e.g. parallelFor over chunks). Structural
    // changes remain forbidden while iterating. Creates the pool on first use.
    template <typename T>
    [[nodiscard]] Pool<T>& storage()
    {
        return poolFor<T>();
    }

    // Const access requires the pool to already exist.
    template <typename T>
    [[nodiscard]] const Pool<T>& storage() const
    {
        const Pool<T>* pool = poolIfExists<T>();
        SOL_ASSERT(pool != nullptr);
        return *pool;
    }

    // The same, for a caller that cannot promise the pool exists: null rather
    // than an assert.
    //
    // A pool is created by the first `emplace`, so "no pool" and "no component
    // of this type anywhere" are the same state - and for a rare component in
    // a registry that is one of many, the second is the ordinary case rather
    // than an error. A const reader looking across every open world for a
    // component only a few of them ever hold has no way to promise anything,
    // and the alternatives are both worse: dropping const so `storage<T>()`
    // can CREATE the pool makes a read mutate, and asking `tryGet` per entity
    // needs the entity list this would have provided.
    template <typename T>
    [[nodiscard]] const Pool<T>* tryStorage() const
    {
        return poolIfExists<T>();
    }

private:
    friend class Snapshot; // world save/load reads and restores entity slots

    template <typename T>
    [[nodiscard]] Pool<T>& poolFor()
    {
        const ComponentTypeId id = componentTypeId<T>();
        if (id >= m_pools.size()) {
            m_pools.resize(id + 1);
        }
        if (m_pools[id] == nullptr) {
            m_pools[id] = std::make_unique<Pool<T>>();
        }
        return static_cast<Pool<T>&>(*m_pools[id]);
    }

    template <typename T>
    [[nodiscard]] Pool<T>* poolIfExists()
    {
        const ComponentTypeId id = componentTypeId<T>();
        if (id >= m_pools.size() || m_pools[id] == nullptr) {
            return nullptr;
        }
        return static_cast<Pool<T>*>(m_pools[id].get());
    }

    template <typename T>
    [[nodiscard]] const Pool<T>* poolIfExists() const
    {
        const ComponentTypeId id = componentTypeId<T>();
        if (id >= m_pools.size() || m_pools[id] == nullptr) {
            return nullptr;
        }
        return static_cast<const Pool<T>*>(m_pools[id].get());
    }

    std::vector<std::uint32_t> m_generations;
    std::vector<std::uint32_t> m_freeList;
    std::vector<std::unique_ptr<PoolBase>> m_pools;
};

} // namespace sol::ecs
