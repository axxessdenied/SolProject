#pragma once

#include <cstdint>

namespace sol::ecs {

// Generational entity handle: a slot index into the registry plus the
// generation the slot had when this handle was issued. Destroying an entity
// bumps its slot's generation, so stale handles compare dead instead of
// aliasing whatever gets recycled into the slot.
struct Entity
{
    std::uint32_t index = 0xffff'ffffu;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool operator==(const Entity&) const = default;
};

inline constexpr Entity kNullEntity{};

[[nodiscard]] constexpr bool isNull(Entity entity)
{
    return entity.index == kNullEntity.index;
}

} // namespace sol::ecs
