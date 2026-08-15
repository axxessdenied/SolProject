#include "sol/ecs/registry.hpp"

namespace sol::ecs {

Entity Registry::create()
{
    if (!m_freeList.empty()) {
        const std::uint32_t index = m_freeList.back();
        m_freeList.pop_back();
        return Entity{.index = index, .generation = m_generations[index]};
    }
    const std::uint32_t index = static_cast<std::uint32_t>(m_generations.size());
    m_generations.push_back(0);
    return Entity{.index = index, .generation = 0};
}

void Registry::destroy(Entity entity)
{
    SOL_ASSERT(isAlive(entity));
    for (const std::unique_ptr<PoolBase>& pool : m_pools) {
        if (pool != nullptr && pool->contains(entity.index)) {
            pool->remove(entity.index);
        }
    }
    ++m_generations[entity.index];
    m_freeList.push_back(entity.index);
}

} // namespace sol::ecs
