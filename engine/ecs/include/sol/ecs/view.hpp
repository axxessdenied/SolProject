#pragma once

#include "sol/ecs/entity.hpp"
#include "sol/ecs/sparse_set.hpp"

#include <cstdint>
#include <tuple>
#include <vector>

namespace sol::ecs {

// Iteration over the entities that have every component in Ts. Leads with the
// smallest participating pool and probes the rest, per decision 001.
//
// Structural changes (create/destroy/emplace/remove) are forbidden while
// iterating; systems record them into a CommandBuffer and flush afterwards.
template <typename... Ts>
class View
{
    static_assert(sizeof...(Ts) > 0, "a view needs at least one component type");

public:
    View(const std::vector<std::uint32_t>& generations, Pool<Ts>&... pools)
        : m_generations(&generations), m_pools(&pools...)
    {
    }

    // Calls fn(Entity, Ts&...) for every matching entity.
    template <typename Fn>
    void each(Fn&& fn) const
    {
        const PoolBase* lead = leadPool();
        const std::vector<std::uint32_t>& indices = lead->entityIndices();
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const std::uint32_t entityIndex = indices[i];
            if ((std::get<Pool<Ts>*>(m_pools)->contains(entityIndex) && ...)) {
                fn(Entity{entityIndex, (*m_generations)[entityIndex]},
                   std::get<Pool<Ts>*>(m_pools)->get(entityIndex)...);
            }
        }
    }

    // Upper bound on the number of matches (the lead pool's size).
    [[nodiscard]] std::size_t sizeHint() const { return leadPool()->size(); }

private:
    [[nodiscard]] const PoolBase* leadPool() const
    {
        const PoolBase* lead = nullptr;
        (
            [&lead](const PoolBase* pool) {
                if (lead == nullptr || pool->size() < lead->size()) {
                    lead = pool;
                }
            }(std::get<Pool<Ts>*>(m_pools)),
            ...);
        return lead;
    }

    const std::vector<std::uint32_t>* m_generations;
    std::tuple<Pool<Ts>*...> m_pools;
};

} // namespace sol::ecs
