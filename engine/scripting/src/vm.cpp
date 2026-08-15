#include <sol/scripting/vm.hpp>

#include <sol/core/log.hpp>

#include <lua.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sol::scripting {

namespace {

void* luaAllocate(void* userData, void* pointer, std::size_t oldSize, std::size_t newSize)
{
    auto* bytesAllocated = static_cast<std::size_t*>(userData);
    if (newSize == 0) {
        if (pointer != nullptr) {
            *bytesAllocated -= oldSize;
        }
        std::free(pointer);
        return nullptr;
    }
    void* result = std::realloc(pointer, newSize);
    if (result != nullptr) {
        // When pointer is null, oldSize encodes the object kind, not a size.
        *bytesAllocated += newSize - (pointer != nullptr ? oldSize : 0);
    }
    return result;
}

int panicHandler(lua_State* state)
{
    const char* message = lua_tostring(state, -1);
    SOL_LOG_FATAL("lua panic (error outside a protected call): %s",
                  message != nullptr ? message : "(non-string error)");
    return 0;
}

} // namespace

namespace detail {

int messageHandler(lua_State* state)
{
    const char* message = lua_tostring(state, 1);
    if (message == nullptr) {
        message = luaL_tolstring(state, 1, nullptr);
    }
    luaL_traceback(state, state, message, 1);
    return 1;
}

} // namespace detail

ScriptVm::ScriptVm()
{
    m_state = lua_newstate(&luaAllocate, &m_bytesAllocated);
    SOL_ASSERT(m_state != nullptr);
    lua_atpanic(m_state, &panicHandler);

    // Sandboxed library set: no io, os, package, or debug (the mod API stays
    // deterministic and filesystem access goes through the engine).
    const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},         {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},   {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},   {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (const luaL_Reg& library : libraries) {
        luaL_requiref(m_state, library.name, library.func, 1);
        lua_pop(m_state, 1);
    }

    lua_pushlightuserdata(m_state, this);
    lua_pushcclosure(m_state, &ScriptVm::luaPrint, 1);
    lua_setglobal(m_state, "print");
}

ScriptVm::~ScriptVm()
{
    if (m_state != nullptr) {
        lua_close(m_state);
    }
}

void ScriptVm::setOutputSink(OutputSink sink, void* userData)
{
    m_sink = sink;
    m_sinkUserData = userData;
}

bool ScriptVm::doString(const char* code, const char* chunkName, std::string* outError)
{
    // '=' prefix: use the chunk name verbatim in messages/tracebacks.
    const std::string decorated = std::string("=") + chunkName;
    return run(code, std::strlen(code), decorated.c_str(), outError);
}

bool ScriptVm::doFile(const char* path, std::string* outError)
{
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        if (outError != nullptr) {
            *outError = std::string("cannot open ") + path;
        }
        return false;
    }
    std::string source;
    char buffer[4096];
    std::size_t bytesRead = 0;
    while ((bytesRead = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        source.append(buffer, bytesRead);
    }
    std::fclose(file);

    const std::string decorated = std::string("@") + path;
    return run(source.data(), source.size(), decorated.c_str(), outError);
}

void ScriptVm::pushBindingTable(const char* tableName)
{
    lua_pushglobaltable(m_state);
    if (tableName == nullptr) {
        return;
    }
    (void)luaL_getsubtable(m_state, -1, tableName);
    lua_remove(m_state, -2);
}

bool ScriptVm::run(const char* code, std::size_t length, const char* chunkName,
                   std::string* outError)
{
    lua_pushcfunction(m_state, &detail::messageHandler);
    const int handlerIndex = lua_gettop(m_state);
    if (luaL_loadbuffer(m_state, code, length, chunkName) != LUA_OK) {
        if (outError != nullptr) {
            const char* message = lua_tostring(m_state, -1);
            *outError = message != nullptr ? message : "(load failed)";
        }
        lua_pop(m_state, 2);
        return false;
    }
    return protectedCall(0, handlerIndex, outError);
}

bool ScriptVm::protectedCall(int argCount, int handlerIndex, std::string* outError)
{
    if (lua_pcall(m_state, argCount, 0, handlerIndex) != LUA_OK) {
        if (outError != nullptr) {
            const char* message = lua_tostring(m_state, -1);
            *outError = message != nullptr ? message : "(unknown error)";
        }
        lua_pop(m_state, 2);
        return false;
    }
    lua_pop(m_state, 1);
    return true;
}

void ScriptVm::writeOutput(const char* line)
{
    if (m_sink != nullptr) {
        m_sink(line, m_sinkUserData);
    } else {
        SOL_LOG_INFO("[lua] %s", line);
    }
}

int ScriptVm::luaPrint(lua_State* state)
{
    auto* vm = static_cast<ScriptVm*>(lua_touserdata(state, lua_upvalueindex(1)));
    const int count = lua_gettop(state);
    // A failing __tostring longjmps past this frame and leaks line's buffer;
    // bounded and dev-only, same tradeoff stock print makes.
    std::string line;
    for (int i = 1; i <= count; ++i) {
        std::size_t length = 0;
        const char* text = luaL_tolstring(state, i, &length);
        if (i > 1) {
            line.push_back('\t');
        }
        line.append(text, length);
        lua_pop(state, 1);
    }
    vm->writeOutput(line.c_str());
    return 0;
}

} // namespace sol::scripting
