#pragma once

// Lua 5.4 VM wrapper. Exceptions are off engine-wide; Lua is compiled as C
// and reports errors by longjmp, so every entry into the VM goes through a
// protected call and errors surface as bool + out-error strings (with a
// traceback appended). The VM opens the sandboxed library set only — no io,
// os, package, or debug — and print is routed to the output sink (dev
// console) or the engine log.

#include <sol/scripting/binding.hpp>

#include <sol/core/assert.hpp>

#include <lua.hpp>

#include <cstddef>
#include <string>
#include <type_traits>

namespace sol::scripting {

class ScriptVm
{
public:
    ScriptVm();
    ~ScriptVm();
    ScriptVm(const ScriptVm&) = delete;
    ScriptVm& operator=(const ScriptVm&) = delete;

    // Receives each print() line (without trailing newline).
    using OutputSink = void (*)(const char* line, void* userData);
    void setOutputSink(OutputSink sink, void* userData);

    // chunkName appears in error messages and tracebacks (e.g. "console").
    [[nodiscard]] bool doString(const char* code, const char* chunkName,
                                std::string* outError = nullptr);
    [[nodiscard]] bool doFile(const char* path, std::string* outError = nullptr);

    template <auto Fn>
    void registerFunction(const char* tableName, const char* name)
    {
        pushBindingTable(tableName);
        lua_pushcfunction(m_state, &detail::FreeFunction<Fn>::call);
        lua_setfield(m_state, -2, name);
        lua_pop(m_state, 1);
    }

    template <auto Fn>
    void registerFunction(const char* tableName, const char* name,
                          typename detail::ContextFunction<Fn>::ContextType* context)
    {
        SOL_ASSERT(context != nullptr);
        pushBindingTable(tableName);
        lua_pushlightuserdata(m_state, context);
        lua_pushcclosure(m_state, &detail::ContextFunction<Fn>::call, 1);
        lua_setfield(m_state, -2, name);
        lua_pop(m_state, 1);
    }

    // Calls a global function, discarding results; false if absent or on
    // runtime error.
    template <typename... Args>
    [[nodiscard]] bool callGlobal(const char* name, std::string* outError, const Args&... args)
    {
        lua_pushcfunction(m_state, &detail::messageHandler);
        const int handlerIndex = lua_gettop(m_state);
        if (lua_getglobal(m_state, name) != LUA_TFUNCTION) {
            lua_pop(m_state, 2);
            if (outError != nullptr) {
                *outError = std::string(name) + " is not a function";
            }
            return false;
        }
        (Stack<std::decay_t<Args>>::push(m_state, args), ...);
        return protectedCall(static_cast<int>(sizeof...(Args)), handlerIndex, outError);
    }

    template <typename T>
    [[nodiscard]] bool getGlobal(const char* name, T& out)
    {
        lua_getglobal(m_state, name);
        const bool matched = Stack<T>::matches(m_state, -1);
        if (matched) {
            out = Stack<T>::get(m_state, -1);
        }
        lua_pop(m_state, 1);
        return matched;
    }

    template <typename T>
    void setGlobal(const char* name, const T& value)
    {
        Stack<T>::push(m_state, value);
        lua_setglobal(m_state, name);
    }

    // Live bytes in the Lua heap (tracked by the custom allocator).
    [[nodiscard]] std::size_t memoryUsed() const { return m_bytesAllocated; }

    // Escape hatch for the binding layer and tests; gameplay code goes
    // through registered functions.
    [[nodiscard]] lua_State* raw() { return m_state; }

private:
    // Pushes the named global table, creating it if needed; nullptr pushes
    // the globals table itself.
    void pushBindingTable(const char* tableName);
    [[nodiscard]] bool run(const char* code, std::size_t length, const char* chunkName,
                           std::string* outError);
    // Expects handler at handlerIndex and function + args on top; pops both.
    [[nodiscard]] bool protectedCall(int argCount, int handlerIndex, std::string* outError);
    void writeOutput(const char* line);
    static int luaPrint(lua_State* state);

    lua_State* m_state = nullptr;
    std::size_t m_bytesAllocated = 0;
    OutputSink m_sink = nullptr;
    void* m_sinkUserData = nullptr;
};

} // namespace sol::scripting
