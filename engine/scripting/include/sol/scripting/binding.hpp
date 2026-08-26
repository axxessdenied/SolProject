#pragma once

// Hand-rolled Lua binding layer (engine plan §Scripting): C++ functions are
// registered explicitly and their arguments marshalled through Stack traits —
// this surface is the engine's public API and the mod API, so bindings stay
// small and auditable. Longjmp discipline: lua errors are raised only while
// every live local is trivially destructible; argument values that allocate
// (strings) are constructed only after all checks have passed.

#include <lua.hpp>

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

#include <sol/scripting/handle.hpp>

namespace sol::scripting {

template <typename T, typename Enable = void>
struct Stack;

template <>
struct Stack<bool>
{
    static constexpr const char* kTypeName = "boolean";

    static void push(lua_State* state, bool value) { lua_pushboolean(state, value ? 1 : 0); }

    [[nodiscard]] static bool matches(lua_State* state, int index) { return lua_isboolean(state, index); }

    [[nodiscard]] static bool get(lua_State* state, int index) { return lua_toboolean(state, index) != 0; }
};

// Exact integers only: 1.5 where an integer is expected is a type error, not
// a truncation.
template <typename T>
struct Stack<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
{
    static constexpr const char* kTypeName = "integer";

    static void push(lua_State* state, T value) { lua_pushinteger(state, static_cast<lua_Integer>(value)); }

    [[nodiscard]] static bool matches(lua_State* state, int index)
    {
        return lua_isinteger(state, index) != 0;
    }

    [[nodiscard]] static T get(lua_State* state, int index)
    {
        return static_cast<T>(lua_tointeger(state, index));
    }
};

template <typename T>
struct Stack<T, std::enable_if_t<std::is_floating_point_v<T>>>
{
    static constexpr const char* kTypeName = "number";

    static void push(lua_State* state, T value) { lua_pushnumber(state, static_cast<lua_Number>(value)); }

    [[nodiscard]] static bool matches(lua_State* state, int index) { return lua_isnumber(state, index) != 0; }

    [[nodiscard]] static T get(lua_State* state, int index)
    {
        return static_cast<T>(lua_tonumber(state, index));
    }
};

// Strict strings: numbers are not coerced.
template <>
struct Stack<std::string>
{
    static constexpr const char* kTypeName = "string";

    static void push(lua_State* state, const std::string& value)
    {
        lua_pushlstring(state, value.data(), value.size());
    }

    [[nodiscard]] static bool matches(lua_State* state, int index)
    {
        return lua_type(state, index) == LUA_TSTRING;
    }

    [[nodiscard]] static std::string get(lua_State* state, int index)
    {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        return std::string(text, length);
    }
};

// View into the Lua stack; valid only for the duration of the bound call.
template <>
struct Stack<const char*>
{
    static constexpr const char* kTypeName = "string";

    static void push(lua_State* state, const char* value) { lua_pushstring(state, value); }

    [[nodiscard]] static bool matches(lua_State* state, int index)
    {
        return lua_type(state, index) == LUA_TSTRING;
    }

    [[nodiscard]] static const char* get(lua_State* state, int index) { return lua_tostring(state, index); }
};

template <typename TagT>
struct Stack<ScriptHandle<TagT>>
{
    static constexpr const char* kTypeName = TagT::kMetatable;

    static void push(lua_State* state, ScriptHandle<TagT> handle)
    {
        pushHandleValue(state, TagT::kMetatable, handle.value);
    }

    [[nodiscard]] static bool matches(lua_State* state, int index)
    {
        std::uint64_t ignored = 0;
        return getHandleValue(state, index, TagT::kMetatable, ignored);
    }

    [[nodiscard]] static ScriptHandle<TagT> get(lua_State* state, int index)
    {
        std::uint64_t value = 0;
        (void)getHandleValue(state, index, TagT::kMetatable, value);
        return {value};
    }
};

namespace detail {

// Error handler for pcall: appends a Lua traceback to the error message.
int messageHandler(lua_State* state);

// Raises a Lua error (longjmp) on arity or type mismatch. Only trivially
// destructible locals are live at the raise points.
template <typename... Args>
void checkArguments(lua_State* state)
{
    constexpr int kCount = static_cast<int>(sizeof...(Args));
    if (lua_gettop(state) != kCount) {
        luaL_error(state, "expected %d argument(s), got %d", kCount, lua_gettop(state));
    }
    if constexpr (kCount > 0) {
        int index = 0;
        const bool matched[] = {(++index, Stack<std::decay_t<Args>>::matches(state, index))...};
        const char* const typeNames[] = {Stack<std::decay_t<Args>>::kTypeName...};
        for (int i = 0; i < kCount; ++i) {
            if (!matched[i]) {
                luaL_error(state,
                           "argument %d: expected %s, got %s",
                           i + 1,
                           typeNames[i],
                           luaL_typename(state, i + 1));
            }
        }
    }
}

template <typename R, typename... Args, std::size_t... Is>
int invokeAndPush(lua_State* state, R (*fn)(Args...), std::index_sequence<Is...>)
{
    if constexpr (std::is_void_v<R>) {
        fn(Stack<std::decay_t<Args>>::get(state, static_cast<int>(Is) + 1)...);
        return 0;
    } else {
        Stack<std::decay_t<R>>::push(state,
                                     fn(Stack<std::decay_t<Args>>::get(state, static_cast<int>(Is) + 1)...));
        return 1;
    }
}

template <auto Fn>
struct FreeFunction;

template <typename R, typename... Args, R (*Fn)(Args...)>
struct FreeFunction<Fn>
{
    static int call(lua_State* state)
    {
        checkArguments<Args...>(state);
        return invokeAndPush(state, Fn, std::index_sequence_for<Args...>{});
    }
};

template <typename Ctx, typename R, typename... Args, std::size_t... Is>
int invokeAndPushCtx(lua_State* state, R (*fn)(Ctx&, Args...), Ctx& context, std::index_sequence<Is...>)
{
    if constexpr (std::is_void_v<R>) {
        fn(context, Stack<std::decay_t<Args>>::get(state, static_cast<int>(Is) + 1)...);
        return 0;
    } else {
        Stack<std::decay_t<R>>::push(
            state, fn(context, Stack<std::decay_t<Args>>::get(state, static_cast<int>(Is) + 1)...));
        return 1;
    }
}

// Bound function whose first parameter is a context reference (e.g. the game
// world); the context pointer rides in a closure upvalue.
template <auto Fn>
struct ContextFunction;

template <typename Ctx, typename R, typename... Args, R (*Fn)(Ctx&, Args...)>
struct ContextFunction<Fn>
{
    using ContextType = Ctx;

    static int call(lua_State* state)
    {
        checkArguments<Args...>(state);
        auto* context = static_cast<Ctx*>(lua_touserdata(state, lua_upvalueindex(1)));
        return invokeAndPushCtx(state, Fn, *context, std::index_sequence_for<Args...>{});
    }
};

} // namespace detail

} // namespace sol::scripting
