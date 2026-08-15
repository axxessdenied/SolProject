#include "content.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace game {

using namespace sol;

namespace {

[[nodiscard]] bool hasExtension(const std::string& path, const char* extension)
{
    const std::size_t length = std::char_traits<char>::length(extension);
    return path.size() >= length &&
           path.compare(path.size() - length, length, extension) == 0;
}

// The Lua-visible API ("sol" table). Deliberately small and explicit — this
// surface is the mod API (engine plan §Scripting).

scripting::EntityHandle spawnShip(GameContent& content, const std::string& id)
{
    const assets::ShipDef* def = content.defs().findShip(id.c_str());
    if (def == nullptr) {
        SOL_LOG_WARN("spawn_ship: no ship def '%s' (try print(sol.ships()))", id.c_str());
        return {};
    }
    const ecs::Entity entity = content.world().spawnShipFromDef(*def, content.defs());
    SOL_LOG_INFO("spawned '%s' (%s)", def->name.c_str(), def->id.c_str());
    return scripting::toHandle(entity);
}

std::string listShips(GameContent& content)
{
    std::string ids;
    for (const assets::ShipDef& def : content.defs().ships()) {
        if (!ids.empty()) {
            ids += ", ";
        }
        ids += def.id;
    }
    return ids;
}

std::string targetName(GameContent& content)
{
    return content.world().currentTarget().name;
}

double targetDistance(GameContent& content)
{
    const NavTarget& target = content.world().currentTarget();
    const double distance =
        length(target.position - content.world().shipState().position) - target.surfaceRadius;
    return distance > 0.0 ? distance : 0.0;
}

double shipSpeed(GameContent& content)
{
    return length(content.world().shipState().velocity);
}

int entityCount(GameContent& content)
{
    return static_cast<int>(content.world().entityCount());
}

} // namespace

bool GameContent::initialize(const std::string& dataDirectory, const std::string& modsDirectory,
                             SpaceWorld* world)
{
    m_world = world;

    // Layer order: base game first (mod zero), then mods sorted by name.
    // Mods are the first-level subdirectories of the mods dir; listFiles is
    // recursive, so derive their names from the returned paths.
    m_layerDirectories = {dataDirectory};
    std::set<std::string> modNames;
    const std::string modsPrefix = modsDirectory + "/";
    for (const std::string& path : platform::listFiles(modsDirectory.c_str())) {
        std::string relative = path;
        if (relative.rfind(modsPrefix, 0) == 0) {
            relative = relative.substr(modsPrefix.size());
        }
        const std::size_t slash = relative.find('/');
        if (slash != std::string::npos) {
            modNames.insert(relative.substr(0, slash));
        }
    }
    for (const std::string& name : modNames) {
        m_layerDirectories.push_back(modsDirectory + "/" + name);
        SOL_LOG_INFO("mod layer: %s", name.c_str());
    }

    registerBindings();
    if (!reloadDefs()) {
        return false; // boot data must be valid; the error was logged
    }
    m_world->applyDefs(m_defs);
    runBootScripts();
    rebuildWatchList();
    SOL_LOG_INFO("content: %zu layer(s), %zu ship / %zu weapon / %zu faction def(s)",
                 m_layerDirectories.size(), m_defs.ships().size(), m_defs.weapons().size(),
                 m_defs.factions().size());
    return true;
}

void GameContent::registerBindings()
{
    m_vm.registerFunction<&spawnShip>("sol", "spawn_ship", this);
    m_vm.registerFunction<&listShips>("sol", "ships", this);
    m_vm.registerFunction<&targetName>("sol", "target_name", this);
    m_vm.registerFunction<&targetDistance>("sol", "target_distance", this);
    m_vm.registerFunction<&shipSpeed>("sol", "speed", this);
    m_vm.registerFunction<&entityCount>("sol", "entity_count", this);
}

bool GameContent::reloadDefs()
{
    assets::DefDatabase fresh;
    std::string error;
    for (const std::string& layer : m_layerDirectories) {
        if (!fresh.mergeDirectory(layer.c_str(), &error)) {
            SOL_LOG_ERROR("data defs: %s", error.c_str());
            return false;
        }
    }
    m_defs = std::move(fresh);
    return true;
}

void GameContent::runBootScripts()
{
    for (const std::string& layer : m_layerDirectories) {
        const std::string script = layer + "/scripts/init.lua";
        if (platform::fileModificationTime(script.c_str()) == 0) {
            continue;
        }
        std::string error;
        if (!m_vm.doFile(script.c_str(), &error)) {
            SOL_LOG_ERROR("%s", error.c_str());
        }
    }

    lua_State* state = m_vm.raw();
    lua_getglobal(state, "on_tick");
    m_hasTickHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_tickHookFailed = false;
}

void GameContent::rebuildWatchList()
{
    m_watched.clear();
    for (const std::string& layer : m_layerDirectories) {
        for (const std::string& path : platform::listFiles(layer.c_str())) {
            const bool isScript = hasExtension(path, ".lua");
            if (!isScript && !hasExtension(path, ".toml")) {
                continue;
            }
            m_watched.push_back({.path = path,
                                 .modificationTime = platform::fileModificationTime(path.c_str()),
                                 .isScript = isScript});
        }
    }
}

void GameContent::poll(double nowSeconds)
{
    if (m_lastPollTime >= 0.0 && nowSeconds - m_lastPollTime < kPollIntervalSeconds) {
        return;
    }
    m_lastPollTime = nowSeconds;

    const std::vector<WatchedFile> previous = std::move(m_watched);
    rebuildWatchList();

    bool defsChanged = false;
    bool scriptsChanged = false;
    auto noteChange = [&](const WatchedFile& file) {
        (file.isScript ? scriptsChanged : defsChanged) = true;
    };
    for (const WatchedFile& file : m_watched) {
        const auto old = std::find_if(previous.begin(), previous.end(),
                                      [&](const WatchedFile& f) { return f.path == file.path; });
        if (old == previous.end() || old->modificationTime != file.modificationTime) {
            noteChange(file);
        }
    }
    for (const WatchedFile& file : previous) { // deletions
        const auto current = std::find_if(m_watched.begin(), m_watched.end(),
                                          [&](const WatchedFile& f) { return f.path == file.path; });
        if (current == m_watched.end()) {
            noteChange(file);
        }
    }

    if (defsChanged) {
        if (reloadDefs()) {
            m_world->applyDefs(m_defs);
            SOL_LOG_INFO("data defs reloaded");
        } else {
            SOL_LOG_ERROR("data def reload failed; keeping previous defs");
        }
    }
    if (scriptsChanged) {
        SOL_LOG_INFO("scripts changed; re-running boot scripts");
        runBootScripts();
    }
}

void GameContent::tick(double dt)
{
    if (!m_hasTickHook || m_tickHookFailed) {
        return;
    }
    std::string error;
    if (!m_vm.callGlobal("on_tick", &error, dt)) {
        SOL_LOG_ERROR("on_tick disabled until scripts reload: %s", error.c_str());
        m_tickHookFailed = true;
    }
}

void GameContent::executeConsole(const char* command)
{
    SOL_LOG_INFO("> %s", command);
    std::string error;
    if (!m_vm.doString(command, "console", &error)) {
        SOL_LOG_ERROR("%s", error.c_str());
    }
    // A console command may have (re)defined the tick hook.
    lua_State* state = m_vm.raw();
    lua_getglobal(state, "on_tick");
    const bool hasHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    if (hasHook && !m_hasTickHook) {
        m_hasTickHook = true;
        m_tickHookFailed = false;
    }
}

} // namespace game
