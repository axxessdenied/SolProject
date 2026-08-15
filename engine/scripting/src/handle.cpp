#include <sol/scripting/handle.hpp>

#include <lua.hpp>

#include <cinttypes>
#include <cstdio>

namespace sol::scripting {

namespace {

int handleEquals(lua_State* state)
{
    auto* left = static_cast<std::uint64_t*>(lua_touserdata(state, 1));
    auto* right = static_cast<std::uint64_t*>(lua_touserdata(state, 2));
    bool equal = false;
    // __eq fires for userdata pairs of any metatable mix; only same-type
    // handles with equal values compare equal.
    if (left != nullptr && right != nullptr && lua_getmetatable(state, 1) != 0) {
        if (lua_getmetatable(state, 2) != 0) {
            equal = lua_rawequal(state, -1, -2) != 0 && *left == *right;
        }
    }
    lua_pushboolean(state, equal ? 1 : 0);
    return 1;
}

int handleToString(lua_State* state)
{
    auto* slot = static_cast<std::uint64_t*>(lua_touserdata(state, 1));
    char text[64];
    std::snprintf(text, sizeof text, "%s(0x%016" PRIx64 ")",
                  lua_tostring(state, lua_upvalueindex(1)), *slot);
    lua_pushstring(state, text);
    return 1;
}

} // namespace

void pushHandleValue(lua_State* state, const char* metatable, std::uint64_t value)
{
    auto* slot = static_cast<std::uint64_t*>(lua_newuserdatauv(state, sizeof(std::uint64_t), 0));
    *slot = value;
    if (luaL_newmetatable(state, metatable) != 0) {
        lua_pushcfunction(state, &handleEquals);
        lua_setfield(state, -2, "__eq");
        lua_pushstring(state, metatable);
        lua_pushcclosure(state, &handleToString, 1);
        lua_setfield(state, -2, "__tostring");
        // Opaque: scripts cannot reach into or extend a handle.
        lua_pushboolean(state, 0);
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
}

bool getHandleValue(lua_State* state, int index, const char* metatable, std::uint64_t& out)
{
    auto* slot = static_cast<std::uint64_t*>(luaL_testudata(state, index, metatable));
    if (slot == nullptr) {
        return false;
    }
    out = *slot;
    return true;
}

} // namespace sol::scripting
