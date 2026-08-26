#pragma once

// Typed handles crossing the Lua boundary (engine plan §Scripting). A handle
// is a 64-bit value boxed as full userdata whose metatable identifies the
// handle type, so a wrong-type or forged argument fails the binding-layer
// check instead of aliasing another handle kind. Values stay opaque to Lua.

#include <lua.hpp>

#include <cstdint>

#include <sol/ecs/entity.hpp>

namespace sol::scripting {

template <typename TagT>
struct ScriptHandle
{
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool operator==(const ScriptHandle&) const = default;
};

// Boxes value as userdata tagged with the named metatable (created on first
// use, with __eq/__tostring).
void pushHandleValue(lua_State* state, const char* metatable, std::uint64_t value);

// False when the value at index is not a handle of this exact type.
[[nodiscard]] bool getHandleValue(lua_State* state, int index, const char* metatable, std::uint64_t& out);

struct EntityTag
{
    static constexpr const char* kMetatable = "Sol.Entity";
};

using EntityHandle = ScriptHandle<EntityTag>;

[[nodiscard]] constexpr EntityHandle toHandle(ecs::Entity entity)
{
    return {static_cast<std::uint64_t>(entity.index) | (static_cast<std::uint64_t>(entity.generation) << 32)};
}

[[nodiscard]] constexpr ecs::Entity toEntity(EntityHandle handle)
{
    return {static_cast<std::uint32_t>(handle.value & 0xffff'ffffu),
            static_cast<std::uint32_t>(handle.value >> 32)};
}

} // namespace sol::scripting
