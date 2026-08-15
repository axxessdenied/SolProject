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

scripting::EntityHandle spawnPilot(GameContent& content, const std::string& id,
                                   const std::string& role)
{
    const assets::ShipDef* def = content.defs().findShip(id.c_str());
    if (def == nullptr) {
        SOL_LOG_WARN("spawn_pilot: no ship def '%s'", id.c_str());
        return {};
    }
    PilotRole pilotRole = PilotRole::Fighter;
    if (role == "trader") {
        pilotRole = PilotRole::Trader;
    } else if (role == "patrol") {
        pilotRole = PilotRole::Patrol;
    } else if (role != "fighter") {
        SOL_LOG_WARN("spawn_pilot: unknown role '%s' (fighter/trader/patrol); using fighter",
                     role.c_str());
    }
    const ecs::Entity entity =
        content.world().spawnPilotFromDef(*def, content.defs(), pilotRole);
    SOL_LOG_INFO("spawned %s pilot '%s' (%s)", role.c_str(), def->name.c_str(), def->id.c_str());
    return scripting::toHandle(entity);
}

bool pilotAttackPlayer(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().pilotAttackPlayer(scripting::toEntity(ship));
}

bool pilotFlee(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().pilotFlee(scripting::toEntity(ship));
}

bool pilotIdle(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().pilotIdle(scripting::toEntity(ship));
}

// Waypoint given relative to the station (Lua has no absolute-coordinate
// source, and everything interesting orbits the station anyway).
bool pilotPatrolOffset(GameContent& content, scripting::EntityHandle ship, double dx, double dy,
                       double dz)
{
    const core::DVec3 waypoint = content.world().stationPosition() + core::DVec3{dx, dy, dz};
    return content.world().pilotPatrolTo(scripting::toEntity(ship), waypoint);
}

double pilotHull(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().shipHullFraction(scripting::toEntity(ship));
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
    return content.world().currentTargetInfo().nav.name;
}

double targetDistance(GameContent& content)
{
    const NavTarget target = content.world().currentTargetInfo().nav;
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

std::string systemName(GameContent& content)
{
    return content.world().currentSystemName();
}

// Dev shortcut: jump via the nearest gate regardless of range (real play
// gates through main.cpp's activation-range check on the J key).
bool jumpNearestGate(GameContent& content)
{
    return content.world().jumpNearestGate(1.0e30);
}

// Dev shortcut: dock at the nearest station regardless of range.
bool dockNearest(GameContent& content)
{
    return content.world().tryDockNearestStation(1.0e30);
}

bool undock(GameContent& content)
{
    return content.world().undock();
}

std::string dockedAt(GameContent& content)
{
    return content.world().dockedStationName();
}

// --- Trading (works while docked; market = the docked station) ---

std::string listCommodities(GameContent& content)
{
    std::string ids;
    for (const std::string& id : content.world().commodityIds()) {
        if (!ids.empty()) {
            ids += ", ";
        }
        ids += id;
    }
    return ids;
}

double commodityPrice(GameContent& content, const std::string& id)
{
    SpaceWorld& world = content.world();
    return world.economy().price(world.dockedMarket(), world.commodityIndex(id.c_str()));
}

double commodityStock(GameContent& content, const std::string& id)
{
    SpaceWorld& world = content.world();
    return world.economy().stock(world.dockedMarket(), world.commodityIndex(id.c_str()));
}

double buyCommodity(GameContent& content, const std::string& id, double units)
{
    SpaceWorld& world = content.world();
    const sol::sim::TradeResult result =
        world.playerBuy(world.commodityIndex(id.c_str()), static_cast<float>(units));
    return result.units;
}

double sellCommodity(GameContent& content, const std::string& id, double units)
{
    SpaceWorld& world = content.world();
    const sol::sim::TradeResult result =
        world.playerSell(world.commodityIndex(id.c_str()), static_cast<float>(units));
    return result.units;
}

double playerCredits(GameContent& content)
{
    return content.world().playerCredits();
}

double playerCargo(GameContent& content, const std::string& id)
{
    SpaceWorld& world = content.world();
    return world.playerCargo(world.commodityIndex(id.c_str()));
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
    m_world->generateUniverse(m_defs); // defs feed the generator params
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
    m_vm.registerFunction<&spawnPilot>("sol", "spawn_pilot", this);
    m_vm.registerFunction<&pilotAttackPlayer>("sol", "pilot_attack_player", this);
    m_vm.registerFunction<&pilotFlee>("sol", "pilot_flee", this);
    m_vm.registerFunction<&pilotIdle>("sol", "pilot_idle", this);
    m_vm.registerFunction<&pilotPatrolOffset>("sol", "pilot_patrol_offset", this);
    m_vm.registerFunction<&pilotHull>("sol", "pilot_hull", this);
    m_vm.registerFunction<&systemName>("sol", "system", this);
    m_vm.registerFunction<&jumpNearestGate>("sol", "jump", this);
    m_vm.registerFunction<&dockNearest>("sol", "dock", this);
    m_vm.registerFunction<&undock>("sol", "undock", this);
    m_vm.registerFunction<&dockedAt>("sol", "docked_at", this);
    m_vm.registerFunction<&listCommodities>("sol", "commodities", this);
    m_vm.registerFunction<&commodityPrice>("sol", "price", this);
    m_vm.registerFunction<&commodityStock>("sol", "stock", this);
    m_vm.registerFunction<&buyCommodity>("sol", "buy", this);
    m_vm.registerFunction<&sellCommodity>("sol", "sell", this);
    m_vm.registerFunction<&playerCredits>("sol", "credits", this);
    m_vm.registerFunction<&playerCargo>("sol", "cargo", this);
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
    lua_getglobal(state, "pilot_think");
    m_hasPilotHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_pilotHookFailed = false;
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
    if (m_hasTickHook && !m_tickHookFailed) {
        std::string error;
        if (!m_vm.callGlobal("on_tick", &error, dt)) {
            SOL_LOG_ERROR("on_tick disabled until scripts reload: %s", error.c_str());
            m_tickHookFailed = true;
        }
    }

    // Pilot strategy: Lua thinks at 2 Hz per pilot; C++ steering flies the
    // chosen state every tick inside SpaceWorld.
    if (m_hasPilotHook && !m_pilotHookFailed) {
        m_pilotThinks.clear();
        m_world->collectDuePilotThinks(dt, m_pilotThinks);
        for (const SpaceWorld::PilotThink& think : m_pilotThinks) {
            std::string error;
            if (!m_vm.callGlobal("pilot_think", &error, scripting::toHandle(think.entity),
                                 think.role, think.state)) {
                SOL_LOG_ERROR("pilot_think disabled until scripts reload: %s", error.c_str());
                m_pilotHookFailed = true;
                break;
            }
        }
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
