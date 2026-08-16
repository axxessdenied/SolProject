#include "content.hpp"

#include "sol/core/log.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cstdio>
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

// As spawn_pilot, with an allegiance: factionIndex is 1-based into
// sol.factions (the runtime table: majors then clans).
scripting::EntityHandle spawnPilotFaction(GameContent& content, const std::string& id,
                                          const std::string& role, double factionIndex)
{
    const std::size_t faction = static_cast<std::size_t>(factionIndex) - 1;
    if (faction >= content.world().factions().size()) {
        SOL_LOG_WARN("spawn_pilot_faction: faction %d out of range (see sol.factions())",
                     static_cast<int>(factionIndex));
        return {};
    }
    const assets::ShipDef* def = content.defs().findShip(id.c_str());
    if (def == nullptr) {
        SOL_LOG_WARN("spawn_pilot_faction: no ship def '%s'", id.c_str());
        return {};
    }
    PilotRole pilotRole = role == "trader"   ? PilotRole::Trader
                          : role == "patrol" ? PilotRole::Patrol
                                             : PilotRole::Fighter;
    const ecs::Entity entity = content.world().spawnPilotFromDef(
        *def, content.defs(), pilotRole, static_cast<std::uint32_t>(faction));
    SOL_LOG_INFO("spawned %s pilot '%s' for %s", role.c_str(), def->name.c_str(),
                 content.world().factions()[faction].name.c_str());
    return scripting::toHandle(entity);
}

bool pilotAttackPlayer(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().pilotAttackPlayer(scripting::toEntity(ship));
}

bool pilotEngageEnemy(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().pilotEngageEnemy(scripting::toEntity(ship));
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

// Destinations reachable from this system's gates, comma-separated.
std::string listGates(GameContent& content)
{
    std::string names;
    for (const GateInstance& gate : content.world().gates()) {
        if (!names.empty()) {
            names += ", ";
        }
        names += content.world().galaxy().systems[gate.toSystem].name;
    }
    return names;
}

bool jumpToSystem(GameContent& content, const std::string& destination)
{
    return content.world().jumpToSystem(destination.c_str());
}

// Dev/console QoL: select a spawned ship as the nav target by name part.
bool targetShip(GameContent& content, const std::string& namePart)
{
    return content.world().targetShipByName(namePart.c_str());
}

// Autopilot to the selected target, arriving arrivalRangeMeters out
// (<= 0 keeps the current setting; the F key reuses it too).
bool autopilotEngage(GameContent& content, double arrivalRangeMeters)
{
    SpaceWorld& world = content.world();
    if (arrivalRangeMeters > 0.0) {
        world.setAutopilotArrivalRange(arrivalRangeMeters);
    }
    return world.engageAutopilot();
}

bool autopilotOff(GameContent& content)
{
    const bool wasActive = content.world().autopilotActive();
    content.world().disengageAutopilot();
    return wasActive;
}

// "idle/in-transit" agent counts: direct evidence the NPC layer is hauling.
std::string traderStats(GameContent& content)
{
    int idle = 0;
    int transit = 0;
    for (const sol::sim::EconomyTrader& trader : content.world().economy().traders()) {
        (trader.phase == sol::sim::TraderPhase::InTransit ? transit : idle) += 1;
    }
    return std::to_string(idle) + " idle, " + std::to_string(transit) + " in transit";
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

// --- Outfitting & fleet (Phase 8a; all mutations require being docked) ---

std::string listModules(GameContent& content)
{
    std::string lines;
    for (const assets::ModuleDef& def : content.defs().modules()) {
        if (!lines.empty()) {
            lines += "\n";
        }
        constexpr const char* kSlotNames[] = {"shield", "engine", "cargo", "utility"};
        lines += def.id + " (" + kSlotNames[static_cast<std::size_t>(def.slot)] + ", " +
                 std::to_string(static_cast<int>(def.price)) + " cr)";
    }
    return lines;
}

std::string listCrewDefs(GameContent& content)
{
    std::string lines;
    for (const assets::CrewDef& def : content.defs().crew()) {
        if (!lines.empty()) {
            lines += "\n";
        }
        lines += def.id + " (" + def.role + ", " +
                 std::to_string(static_cast<int>(def.price)) + " cr)";
    }
    return lines;
}

// The active ship's fit: def, weapon, modules, crew, value, deductible.
std::string fitInfo(GameContent& content)
{
    SpaceWorld& world = content.world();
    const game::OwnedShip& ship = world.activeShip();
    std::string info = ship.defId + " | weapon: " + (ship.weaponId.empty() ? "-" : ship.weaponId);
    info += " | modules:";
    if (ship.moduleIds.empty()) {
        info += " -";
    }
    for (const std::string& id : ship.moduleIds) {
        info += " " + id;
    }
    info += " | crew:";
    if (ship.crewIds.empty()) {
        info += " -";
    }
    for (const std::string& id : ship.crewIds) {
        info += " " + id;
    }
    info += " | value " + std::to_string(static_cast<int>(world.shipValue(ship))) +
            " cr, deductible " + std::to_string(static_cast<int>(world.insuranceDeductible())) +
            " cr";
    return info;
}

// Fleet listing, 1-based to match the select_ship/sell_ship arguments.
std::string listFleet(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    for (std::size_t i = 0; i < world.fleet().size(); ++i) {
        const game::OwnedShip& ship = world.fleet()[i];
        if (!lines.empty()) {
            lines += "\n";
        }
        lines += std::to_string(i + 1) + ": " + ship.defId;
        if (i == world.activeShipIndex()) {
            lines += " (active)";
        } else {
            const auto& systems = world.galaxy().systems;
            lines += ship.storedSystem < systems.size()
                         ? " (stored: " + systems[ship.storedSystem].name + ")"
                         : " (stored)";
        }
    }
    return lines;
}

bool buyModule(GameContent& content, const std::string& id)
{
    return content.world().buyModule(id.c_str());
}

bool sellModule(GameContent& content, const std::string& id)
{
    return content.world().sellModule(id.c_str());
}

bool buyWeapon(GameContent& content, const std::string& id)
{
    return content.world().buyWeapon(id.c_str());
}

bool buyShip(GameContent& content, const std::string& id)
{
    return content.world().buyShip(id.c_str());
}

bool sellShip(GameContent& content, double index)
{
    return content.world().sellShip(static_cast<std::size_t>(index) - 1);
}

bool selectShip(GameContent& content, double index)
{
    return content.world().switchShip(static_cast<std::size_t>(index) - 1);
}

bool hireCrew(GameContent& content, const std::string& id)
{
    return content.world().hireCrew(id.c_str());
}

bool fireCrew(GameContent& content, const std::string& id)
{
    return content.world().fireCrew(id.c_str());
}

double insuranceQuote(GameContent& content)
{
    return content.world().insuranceDeductible();
}

// Dev cheat: teleport to a station-relative offset (mission/combat tests).
bool warpOffset(GameContent& content, double station, double dx, double dy, double dz)
{
    return content.world().warpToStationOffset(static_cast<std::uint32_t>(station),
                                               {dx, dy, dz});
}

// Dev cheat, same spirit as spawn_ship: outfitting tests need capital.
double addCredits(GameContent& content, double amount)
{
    content.world().addCredits(amount);
    return content.world().playerCredits();
}

// --- Factions & reputation (Phase 8b; faction indices are 1-based) ---

std::string listFactions(GameContent& content)
{
    std::string lines;
    const std::vector<game::GameFaction>& factions = content.world().factions();
    for (std::size_t i = 0; i < factions.size(); ++i) {
        if (!lines.empty()) {
            lines += "\n";
        }
        lines += std::to_string(i + 1) + ": " + factions[i].name +
                 (factions[i].pirate ? " (pirate clan)" : " (major)");
    }
    return lines;
}

std::string listStandings(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    for (std::size_t i = 0; i < world.factions().size(); ++i) {
        if (!lines.empty()) {
            lines += "\n";
        }
        const std::uint32_t faction = static_cast<std::uint32_t>(i);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%+.1f",
                      static_cast<double>(world.factionSim().standing(faction)));
        lines += std::to_string(i + 1) + ": " + world.factions()[i].name + " " + buffer +
                 " (" + world.playerAttitudeName(faction) + ")";
        if (faction == world.systemOwnerFaction(world.currentSystemIndex())) {
            lines += " [local]"; // owns the system the player is in
        }
    }
    return lines;
}

// Non-neutral pairs only; the full matrix is quadratic in clans.
std::string listRelations(GameContent& content)
{
    SpaceWorld& world = content.world();
    const std::size_t count = world.factions().size();
    std::string lines;
    for (std::uint32_t a = 0; a < count; ++a) {
        for (std::uint32_t b = a + 1; b < count; ++b) {
            const float value = world.factionSim().relation(a, b);
            if (value == 0.0f) {
                continue;
            }
            if (!lines.empty()) {
                lines += "\n";
            }
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%+.1f", static_cast<double>(value));
            lines += world.factions()[a].name + " vs " + world.factions()[b].name + ": " +
                     buffer + (world.factionSim().atWar(a, b) ? " (WAR)" : "");
        }
    }
    return lines.empty() ? "(all neutral)" : lines;
}

std::string listRaids(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const float intensity = world.factionSim().raidIntensity(s);
        if (intensity < 0.05f) {
            continue;
        }
        if (!lines.empty()) {
            lines += "\n";
        }
        const std::uint32_t raider = world.factionSim().lastRaider(s);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(intensity));
        lines += world.galaxy().systems[s].name + ": " + buffer;
        if (raider < world.factions().size()) {
            lines += " by " + world.factions()[raider].name;
        }
    }
    return lines.empty() ? "(no recent raids)" : lines;
}

// Dev cheat for verifying gates/hostility without the long rep grind.
double setStanding(GameContent& content, double factionIndex, double value)
{
    SpaceWorld& world = content.world();
    const std::size_t faction = static_cast<std::size_t>(factionIndex) - 1;
    if (faction >= world.factions().size()) {
        SOL_LOG_WARN("set_rep: faction %d out of range", static_cast<int>(factionIndex));
        return 0.0;
    }
    world.factionSim().setStanding(static_cast<std::uint32_t>(faction),
                                   static_cast<float>(value));
    return world.factionSim().standing(static_cast<std::uint32_t>(faction));
}

// --- Exploration & scanning (Phase 8e) ---

[[nodiscard]] const char* regionWord(sol::sim::Region region)
{
    switch (region) {
    case sol::sim::Region::Core:
        return "core";
    case sol::sim::Region::Frontier:
        return "frontier";
    case sol::sim::Region::Fringe:
        return "fringe";
    }
    return "?";
}

// What the player knows, in the order the ladder runs. Unknown systems are
// absent on purpose: the console is not a way around the fog.
std::string listKnowledge(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    std::uint32_t known = 0;
    for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
        const sol::sim::KnowledgeState state = world.survey().knowledge(i);
        if (state == sol::sim::KnowledgeState::Unknown) {
            continue;
        }
        ++known;
        if (!lines.empty()) {
            lines += "\n";
        }
        lines += world.galaxy().systems[i].name + ": ";
        switch (state) {
        case sol::sim::KnowledgeState::Charted:
            lines += "charted";
            break;
        case sol::sim::KnowledgeState::Visited:
            lines += "visited";
            break;
        case sol::sim::KnowledgeState::Surveyed:
            lines += "SURVEYED";
            break;
        case sol::sim::KnowledgeState::Unknown:
            break;
        }
        if (i == world.currentSystemIndex()) {
            lines += " [here]";
        }
    }
    return lines.empty() ? "(nothing known)"
                         : lines + "\n" + std::to_string(known) + " of "
                               + std::to_string(world.galaxy().systems.size()) + " systems";
}

// Every site in this system the player has found, with its state and range.
std::string listSignals(GameContent& content)
{
    SpaceWorld& world = content.world();
    const std::uint32_t system = world.currentSystemIndex();
    const sol::core::DVec3 position = world.shipState().position;
    std::string lines;
    for (const SignalInstance& signal : world.signals()) {
        if (!world.survey().signalDiscovered(system, signal.index)) {
            continue;
        }
        if (!lines.empty()) {
            lines += "\n";
        }
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%u: %s, %.0f km", signal.index + 1,
                      world.survey().signalResolved(system, signal.index)
                          ? sol::sim::signalKindName(signal.kind)
                          : "unidentified contact",
                      length(signal.position - position) / 1000.0);
        lines += buffer;
        if (world.survey().signalEmptied(system, signal.index)) {
            lines += " (emptied)";
        }
    }
    char summary[96];
    std::snprintf(summary, sizeof(summary), "%zu site(s) in %s, %u found",
                  world.signals().size(), world.currentSystemName(),
                  static_cast<std::uint32_t>(std::count_if(
                      world.signals().begin(), world.signals().end(),
                      [&](const SignalInstance& s) {
                          return world.survey().signalDiscovered(system, s.index);
                      })));
    return lines.empty() ? std::string(summary) : lines + "\n" + summary;
}

double pulseScan(GameContent& content)
{
    return static_cast<double>(content.world().pulseScan());
}

// Dev pacing: resolves whatever is targeted without flying into range.
bool scanTarget(GameContent& content)
{
    return content.world().scanCurrentTarget();
}

bool salvageNearest(GameContent& content)
{
    return content.world().trySalvageNearest(SpaceWorld::kSalvageRange);
}

std::string surveyLedger(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    for (const sol::sim::SurveyEntry& entry : world.survey().ledger()) {
        if (!lines.empty()) {
            lines += "\n";
        }
        const char* kind = entry.kind == sol::sim::SurveyKind::System   ? "system"
                           : entry.kind == sol::sim::SurveyKind::Body   ? "body"
                           : entry.kind == sol::sim::SurveyKind::Site   ? "site"
                                                                        : "completion";
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s: %s (%s%s) %.0f cr",
                      world.galaxy().systems[entry.system].name.c_str(), kind,
                      regionWord(entry.region), entry.firstDiscovery ? ", uncharted" : "",
                      entry.value);
        lines += buffer;
    }
    char total[64];
    std::snprintf(total, sizeof(total), "total %.0f cr", world.survey().ledgerValue());
    return lines.empty() ? std::string("(ledger empty)") : lines + "\n" + total;
}

double sellSurvey(GameContent& content)
{
    return content.world().sellSurveyData();
}

// --- Mining, salvage & refining (Phase 8f) -----------------------------------

std::string listFields(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::vector<sol::sim::AsteroidFieldSpec> fields;
    world.mining().fieldsFor(world.galaxy(), world.currentSystemIndex(), fields);
    const sol::core::DVec3 ship = world.shipState().position;
    std::string lines;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%zu: %u rocks, r %.0f km, %.0f km away", i + 1,
                      fields[i].rockCount, fields[i].radius / 1000.0,
                      sol::core::length(fields[i].center - ship) / 1000.0);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    char summary[96];
    std::snprintf(summary, sizeof(summary), "%zu field(s) in %s", fields.size(),
                  world.currentSystemName());
    return lines.empty() ? std::string(summary) : lines + "\n" + summary;
}

// Rocks of one field (1-based, as the field listing prints them), with what
// each still holds — the readout that proves depletion survived a jump.
std::string listRocks(GameContent& content, double fieldNumber)
{
    SpaceWorld& world = content.world();
    const std::uint32_t field = fieldNumber >= 1.0 ? static_cast<std::uint32_t>(fieldNumber) - 1
                                                   : 0;
    std::vector<sol::sim::RockSpec> rocks;
    world.mining().rocksFor(world.galaxy(), world.currentSystemIndex(), field, rocks);
    if (rocks.empty()) {
        return "no such field";
    }
    const sol::core::DVec3 ship = world.shipState().position;
    std::string lines;
    float left = 0.0f;
    float total = 0.0f;
    for (std::size_t i = 0; i < rocks.size(); ++i) {
        const float remaining = world.mining().unitsLeft(world.currentSystemIndex(), field,
                                                         static_cast<std::uint32_t>(i),
                                                         rocks[i].yieldUnits);
        left += remaining;
        total += rocks[i].yieldUnits;
        if (remaining <= 0.0f) {
            continue; // spent rocks are gone; listing them is noise
        }
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "%zu: %s %.1f/%.1f, r %.0f m, %.0f km", i + 1,
                      world.commodityIds()[rocks[i].commodity < world.commodityIds().size()
                                               ? rocks[i].commodity
                                               : 0]
                          .c_str(),
                      static_cast<double>(remaining), static_cast<double>(rocks[i].yieldUnits),
                      rocks[i].radius, sol::core::length(rocks[i].position - ship) / 1000.0);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    char summary[96];
    std::snprintf(summary, sizeof(summary), "field %u: %.0f of %.0f units left",
                  field + 1, static_cast<double>(left), static_cast<double>(total));
    return lines.empty() ? std::string(summary) : lines + "\n" + summary;
}

std::string listWrecks(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    for (const sol::sim::WreckRecord& wreck : world.mining().wrecks()) {
        float cargo = 0.0f;
        for (const sol::sim::SignalCargo& stack : wreck.contents.cargo) {
            cargo += stack.units;
        }
        char buffer[192];
        std::snprintf(buffer, sizeof(buffer), "#%u %s in %s: %.0f units, %.0f cr%s, %.0f s left",
                      wreck.id, wreck.name.c_str(),
                      wreck.system < world.galaxy().systems.size()
                          ? world.galaxy().systems[wreck.system].name.c_str()
                          : "?",
                      static_cast<double>(cargo), wreck.contents.credits,
                      wreck.contents.moduleId.empty() ? "" : ", module",
                      wreck.decayRemaining);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    return lines.empty() ? std::string("(no wrecks)") : lines;
}

// Dev pacing: empties whatever the boresight is on without holding the beam.
bool mineAhead(GameContent& content)
{
    return content.world().mineAhead();
}

bool warpToRock(GameContent& content)
{
    return content.world().warpToNearestRock();
}

// Selects a nav target by name fragment (T-cycling is the player path). Useful
// for anything the cycle reaches: a field, a wreck, a gate, a station.
bool selectTargetByName(GameContent& content, const char* namePart)
{
    SpaceWorld& world = content.world();
    const std::string_view want(namePart != nullptr ? namePart : "");
    if (want.empty()) {
        return false;
    }
    const std::span<const NavTarget> targets = world.navTargets();
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].name.find(want) != std::string::npos) {
            return world.selectTarget(i);
        }
    }
    return false;
}

bool orderRefine(GameContent& content, double units)
{
    std::string error;
    if (!content.world().orderRefine(static_cast<float>(units), &error)) {
        SOL_LOG_WARN("refine: %s", error.c_str());
        return false;
    }
    return true;
}

double collectRefined(GameContent& content)
{
    return static_cast<double>(content.world().collectRefined());
}

std::string listRefineJobs(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    for (const sol::sim::RefineJob& job : world.mining().refineJobs()) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "market %u: %.0f %s -> %.0f %s, %s", job.market,
                      static_cast<double>(job.inputUnits),
                      job.inputCommodity < world.commodityIds().size()
                          ? world.commodityIds()[job.inputCommodity].c_str()
                          : "?",
                      static_cast<double>(job.outputUnits),
                      job.outputCommodity < world.commodityIds().size()
                          ? world.commodityIds()[job.outputCommodity].c_str()
                          : "?",
                      job.secondsRemaining > 0.0 ? "running" : "ready");
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
        if (job.secondsRemaining > 0.0) {
            char wait[48];
            std::snprintf(wait, sizeof(wait), " (%.0f s)", job.secondsRemaining);
            lines += wait;
        }
    }
    return lines.empty() ? std::string("(no refinery orders)") : lines;
}

// --- Economy coherence (Phase 8g) ---

// The galaxy's books on one screen. The exit criteria for this phase are
// stated against this report rather than against an impression, so it stays
// in as a live tool rather than being scaffolding that gets deleted.
std::string economyReport(GameContent& content)
{
    SpaceWorld& world = content.world();
    const sol::sim::Economy& economy = world.economy();
    const std::size_t commodities = world.commodityIds().size();
    if (commodities == 0 || economy.markets().empty()) {
        return "(no economy)";
    }

    std::vector<double> stock(commodities, 0.0);
    std::vector<double> capacity(commodities, 0.0);
    std::vector<double> production(commodities, 0.0);
    std::vector<double> consumption(commodities, 0.0);
    std::vector<std::uint32_t> starved(commodities, 0);
    std::vector<std::uint32_t> empty(commodities, 0);
    std::vector<std::uint32_t> full(commodities, 0);
    std::uint32_t throttled = 0;

    for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
        const sol::sim::StationMarket& market = economy.markets()[m];
        const float cap = economy.capacityOf(m);
        if (economy.satisfaction(m) < 0.99f) {
            ++throttled;
        }
        const sol::sim::EconomyArchetype* archetype =
            market.archetype < economy.params().archetypes.size()
                ? &economy.params().archetypes[market.archetype]
                : nullptr;
        for (std::uint32_t c = 0; c < commodities; ++c) {
            const float units = economy.stock(m, c);
            stock[c] += units;
            capacity[c] += cap;
            if (units <= cap * 0.01f) {
                ++empty[c];
            } else if (units >= cap * 0.99f) {
                ++full[c];
            }
            if (archetype != nullptr) {
                if (c < archetype->production.size()) {
                    production[c] += archetype->production[c];
                }
                if (c < archetype->consumption.size()) {
                    consumption[c] += archetype->consumption[c];
                }
                if (c < archetype->feedstock.size()) {
                    consumption[c] += archetype->feedstock[c];
                }
            }
            if (economy.limitingCommodity(m) == c) {
                ++starved[c];
            }
        }
    }

    std::string lines;
    char buffer[256];
    for (std::uint32_t c = 0; c < commodities; ++c) {
        const double fill = capacity[c] > 0.0 ? 100.0 * stock[c] / capacity[c] : 0.0;
        std::snprintf(buffer, sizeof(buffer),
                      "%-14s %5.1f%% full  prod %6.2f  use %6.2f  (%u empty, %u full, "
                      "%u starved on it)",
                      world.commodityIds()[c].c_str(), fill, production[c], consumption[c],
                      empty[c], full[c], starved[c]);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::snprintf(buffer, sizeof(buffer), "%zu markets, %u throttled by feedstock, %.0f s elapsed",
                  economy.markets().size(), throttled, world.worldSeconds());
    return lines + "\n" + buffer;
}

// A station's books: how much of its nominal output it is managing and what
// is holding it back. Answers "why is this refinery not making anything".
std::string feedstockReport(GameContent& content)
{
    SpaceWorld& world = content.world();
    if (!world.isDocked()) {
        return "(not docked)";
    }
    const std::uint32_t market = world.dockedMarket();
    const char* limiting = world.marketLimiting(market);
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s: running at %.0f%% of nominal%s%s",
                  world.dockedStationName(),
                  static_cast<double>(world.marketSatisfaction(market)) * 100.0,
                  limiting[0] == '\0' ? "" : ", short of ", limiting);
    return buffer;
}

// What is left in the ground in a system, which is what a mining outpost
// there is actually living on.
std::string fieldStock(GameContent& content, int systemIndex)
{
    SpaceWorld& world = content.world();
    const auto system = systemIndex < 0
                            ? world.currentSystemIndex()
                            : static_cast<std::uint32_t>(systemIndex);
    if (system >= world.galaxy().systems.size()) {
        return "(no such system)";
    }
    std::string lines;
    char buffer[192];
    for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
        const float units = world.mining().systemStock(world.galaxy(), system, c);
        if (units <= 0.0f) {
            continue;
        }
        std::snprintf(buffer, sizeof(buffer), "%s: %.0f units in the ground",
                      world.commodityIds()[c].c_str(), static_cast<double>(units));
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::snprintf(buffer, sizeof(buffer), "%s: %u field(s), %zu rock(s) being worked",
                  world.galaxy().systems[system].name.c_str(),
                  world.mining().fieldCount(system), world.mining().depletionRecordCount());
    return (lines.empty() ? std::string("(no rock here)") : lines) + "\n" + buffer;
}

std::string marketMemory(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    char buffer[256];
    for (const sol::sim::MarketMemory& memory : world.survey().marketMemory()) {
        if (memory.market >= world.economy().markets().size()) {
            continue;
        }
        const std::uint32_t system = world.economy().markets()[memory.market].systemIndex;
        std::string prices;
        for (std::uint32_t c = 0; c < memory.prices.size(); ++c) {
            std::snprintf(buffer, sizeof(buffer), "%s%s %.2f", prices.empty() ? "" : ", ",
                          world.commodityIds()[c].c_str(),
                          static_cast<double>(memory.prices[c]));
            prices += buffer;
        }
        std::snprintf(buffer, sizeof(buffer), "%s (market %u), %.0f s ago: ",
                      world.galaxy().systems[system].name.c_str(), memory.market,
                      world.worldSeconds() - memory.takenAt);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer) + prices;
    }
    return lines.empty() ? std::string("(no market data)") : lines;
}

bool buyMarketIntel(GameContent& content)
{
    return content.world().buyMarketIntel();
}

std::string bestPrice(GameContent& content, const char* commodityId)
{
    SpaceWorld& world = content.world();
    for (std::uint32_t c = 0; c < world.commodityIds().size(); ++c) {
        if (world.commodityIds()[c] != commodityId) {
            continue;
        }
        std::uint32_t system = 0;
        float price = 0.0f;
        double age = 0.0;
        bool stale = false;
        if (!world.bestKnownPrice(c, &system, &price, &age, &stale)) {
            return "(never seen it anywhere)";
        }
        char buffer[192];
        std::snprintf(buffer, sizeof(buffer), "%s: %.2f at %s, %.0f s ago%s", commodityId,
                      static_cast<double>(price),
                      world.galaxy().systems[system].name.c_str(), age,
                      stale ? " (stale)" : "");
        return buffer;
    }
    return "(no such commodity)";
}

// Plots a gate route to a named system and returns the summary.
std::string plotRoute(GameContent& content, const char* systemName)
{
    SpaceWorld& world = content.world();
    for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
        if (world.galaxy().systems[i].name != systemName) {
            continue;
        }
        if (!world.plotRoute(i)) {
            return "no route";
        }
        std::string summary;
        for (const std::uint32_t hop : world.survey().route()) {
            summary += (summary.empty() ? "" : " > ") + world.galaxy().systems[hop].name;
        }
        return summary;
    }
    return "no such system";
}

// Dev cheat: marks a system visited without flying there (map/route testing).
bool chartSystem(GameContent& content, const char* systemName)
{
    SpaceWorld& world = content.world();
    for (std::uint32_t i = 0; i < world.galaxy().systems.size(); ++i) {
        if (world.galaxy().systems[i].name == systemName) {
            world.survey().setKnowledge(world.galaxy(), i, sol::sim::KnowledgeState::Visited);
            return true;
        }
    }
    return false;
}

// The signal_loot hook's builder: "commodityId:units,commodityId:units" plus
// credits and an optional module id. Validated here against the defs and the
// sim's caps - a script cannot invent a commodity or overfill a wreck.
bool setSignalLoot(GameContent& content, const char* cargoSpec, double credits,
                   const char* moduleId)
{
    if (content.lootSignal() == 0xffff'ffffu && content.lootWreck() == 0) {
        SOL_LOG_WARN("set_loot: only valid inside signal_loot or wreck_loot");
        return false;
    }
    SpaceWorld& world = content.world();
    sol::sim::SignalLoot loot;
    loot.credits = credits > 0.0 ? credits : 0.0;
    std::string_view spec(cargoSpec != nullptr ? cargoSpec : "");
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view entry = spec.substr(0, comma);
        const std::size_t colon = entry.find(':');
        if (colon == std::string_view::npos) {
            SOL_LOG_WARN("set_loot: bad cargo entry '%.*s'", static_cast<int>(entry.size()),
                         entry.data());
            return false;
        }
        const std::string id(entry.substr(0, colon));
        const std::uint32_t commodity = world.commodityIndex(id.c_str());
        if (commodity >= world.commodityIds().size()) {
            SOL_LOG_WARN("set_loot: unknown commodity '%s'", id.c_str());
            return false;
        }
        const double units = std::strtod(std::string(entry.substr(colon + 1)).c_str(), nullptr);
        loot.cargo.push_back({.commodity = commodity, .units = static_cast<float>(units)});
        if (comma == std::string_view::npos) {
            break;
        }
        spec.remove_prefix(comma + 1);
    }
    if (moduleId != nullptr && moduleId[0] != '\0') {
        if (content.defs().findModule(moduleId) == nullptr) {
            SOL_LOG_WARN("set_loot: unknown module '%s'", moduleId);
            return false;
        }
        loot.moduleId = moduleId;
    }
    if (content.lootWreck() != 0) {
        return world.applyWreckLoot(content.lootWreck(), std::move(loot));
    }
    return world.applySignalLoot(content.lootSystem(), content.lootSignal(), std::move(loot));
}

// Raid candidates for faction_think:
// "systemIndex:systemName:relation:ownerKind;..." with ownerKind m|p
// (system indices are the engine's 0-based ids, fed back to faction_raid).
std::string factionCandidates(GameContent& content, double factionIndex)
{
    SpaceWorld& world = content.world();
    const std::size_t faction = static_cast<std::size_t>(factionIndex) - 1;
    if (faction >= world.factions().size()) {
        return "";
    }
    std::vector<sol::sim::RaidCandidate> candidates;
    world.factionSim().raidCandidates(world.galaxy(), static_cast<std::uint32_t>(faction),
                                      candidates);
    std::string joined;
    for (const sol::sim::RaidCandidate& candidate : candidates) {
        if (!joined.empty()) {
            joined += ";";
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(candidate.relation));
        joined += std::to_string(candidate.system) + ":" +
                  world.galaxy().systems[candidate.system].name + ":" + buffer + ":" +
                  (candidate.owner < world.factions().size() &&
                           world.factions()[candidate.owner].pirate
                       ? "p"
                       : "m");
    }
    return joined;
}

bool factionRaid(GameContent& content, double factionIndex, double systemIndex)
{
    const std::size_t faction = static_cast<std::size_t>(factionIndex) - 1;
    if (faction >= content.world().factions().size()) {
        return false;
    }
    return content.world().commitFactionRaid(static_cast<std::uint32_t>(faction),
                                             static_cast<std::uint32_t>(systemIndex));
}

// --- Missions & contracts (Phase 8c) ---
// Conventions match the faction API: faction indices 1-based (sol.factions),
// system indices the engine's 0-based ids (as in faction_candidates).

// Human summary of a mission's current objective, for board/journal listings.
std::string missionObjectiveLine(const sol::sim::Mission& mission)
{
    const sol::sim::MissionObjective& objective =
        mission.objectives[mission.currentObjective];
    std::string line = objective.text;
    if (objective.kind == sol::sim::ObjectiveKind::Kill) {
        line += " (" + std::to_string(objective.kills) + " left)";
    } else if (objective.kind == sol::sim::ObjectiveKind::Deliver) {
        line += " (" + std::to_string(static_cast<int>(objective.units)) + " units left)";
    }
    return line;
}

std::string listMissionBoard(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    const std::vector<sol::sim::Mission>& offers = world.missionSim().offers();
    for (std::size_t i = 0; i < offers.size(); ++i) {
        if (!lines.empty()) {
            lines += "\n";
        }
        const sol::sim::Mission& offer = offers[i];
        lines += std::to_string(i + 1) + ": " + offer.title + " (" +
                 (offer.poster < world.factions().size()
                      ? world.factions()[offer.poster].name
                      : std::string("?")) +
                 ", " + std::to_string(static_cast<int>(offer.rewardCredits)) + " cr";
        if (offer.minRep > -100.0f) {
            lines += ", rep " + std::to_string(static_cast<int>(offer.minRep)) + "+";
        }
        if (offer.deadline > 0.0) {
            lines += ", " + std::to_string(static_cast<int>(offer.deadline)) + "s";
        }
        lines += offer.campaign() ? ") [campaign]" : ")";
    }
    return lines.empty() ? "(no offers)" : lines;
}

std::string listMissions(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::string lines;
    const std::vector<sol::sim::Mission>& active = world.missionSim().active();
    for (std::size_t i = 0; i < active.size(); ++i) {
        if (!lines.empty()) {
            lines += "\n";
        }
        const sol::sim::Mission& mission = active[i];
        lines += std::to_string(i + 1) + ": " + mission.title + " [" +
                 std::to_string(mission.currentObjective + 1) + "/" +
                 std::to_string(mission.objectives.size()) + "] " +
                 missionObjectiveLine(mission);
        if (mission.deadline > 0.0) {
            lines += " (" + std::to_string(static_cast<int>(mission.deadline)) + "s left)";
        }
        if (i == world.missionSim().tracked()) {
            lines += " *";
        }
    }
    return lines.empty() ? "(no active missions)" : lines;
}

bool acceptMission(GameContent& content, double index)
{
    return content.world().acceptMission(static_cast<std::uint32_t>(index) - 1);
}

bool abandonMission(GameContent& content, double index)
{
    return content.world().abandonMission(static_cast<std::uint32_t>(index) - 1);
}

bool trackMission(GameContent& content, double index)
{
    SpaceWorld& world = content.world();
    const std::uint32_t active = static_cast<std::uint32_t>(index) - 1;
    if (active >= world.missionSim().active().size()) {
        return false;
    }
    world.missionSim().setTracked(active);
    return true;
}

double campaignStage(GameContent& content)
{
    return content.world().missionSim().campaignStage();
}

double setCampaignStage(GameContent& content, double stage)
{
    content.world().missionSim().setCampaignStage(
        stage >= 0.0 ? static_cast<std::uint32_t>(stage) : 0u);
    return content.world().missionSim().campaignStage();
}

// Dev listing of the raw candidates behind the docked board.
std::string missionCandidatesInfo(GameContent& content)
{
    SpaceWorld& world = content.world();
    if (!world.isDocked()) {
        return "(not docked)";
    }
    std::string lines;
    std::vector<sol::sim::HaulCandidate> hauls;
    world.missionSim().haulCandidates(world.galaxy(), world.economy(),
                                      world.currentSystemIndex(),
                                      world.dockedStationIndex(), hauls);
    for (const sol::sim::HaulCandidate& c : hauls) {
        if (!lines.empty()) {
            lines += "\n";
        }
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "haul: %s @ %s needs %.0f %s (%.0f%%, %u jumps)",
                      world.galaxy().systems[c.system].stations[c.station].name.c_str(),
                      world.galaxy().systems[c.system].name.c_str(),
                      static_cast<double>(c.units), world.commodityIds()[c.commodity].c_str(),
                      static_cast<double>(c.severity) * 100.0, c.jumps);
        lines += buffer;
    }
    std::vector<sol::sim::BountyCandidate> bounties;
    world.missionSim().bountyCandidates(world.galaxy(), world.factionSim(),
                                        world.currentSystemIndex(), bounties);
    for (const sol::sim::BountyCandidate& c : bounties) {
        if (!lines.empty()) {
            lines += "\n";
        }
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "bounty: %s raided by %s (%.2f, %u jumps)",
                      world.galaxy().systems[c.system].name.c_str(),
                      c.clan < world.factions().size() ? world.factions()[c.clan].name.c_str()
                                                       : "?",
                      static_cast<double>(c.intensity), c.jumps);
        lines += buffer;
    }
    return lines.empty() ? "(no candidates)" : lines;
}

// Navigation helpers for authored content: gates by 1-based index.
double gateDestination(GameContent& content, double gateIndex)
{
    const std::size_t index = static_cast<std::size_t>(gateIndex) - 1;
    const std::span<const GateInstance> gates = content.world().gates();
    return index < gates.size() ? static_cast<double>(gates[index].toSystem) : -1.0;
}

double stationCount(GameContent& content, double systemIndex)
{
    const auto& systems = content.world().galaxy().systems;
    const std::size_t system = static_cast<std::size_t>(systemIndex);
    return system < systems.size() ? static_cast<double>(systems[system].stations.size())
                                   : 0.0;
}

double currentSystemIndex(GameContent& content)
{
    return content.world().currentSystemIndex();
}

// 0-based station index while docked (mission dock objectives), or -1.
double dockedStationIndex(GameContent& content)
{
    const std::uint32_t station = content.world().dockedStationIndex();
    return station == 0xffff'ffffu ? -1.0 : static_cast<double>(station);
}

// --- The mission builder (Lua board hook assembles a draft, then posts) ---

bool missionBegin(GameContent& content, const std::string& title, double posterIndex,
                  double rewardCredits, double repReward, double repPenalty,
                  const std::string& campaignId)
{
    const std::size_t poster = static_cast<std::size_t>(posterIndex) - 1;
    if (poster >= content.world().factions().size()) {
        SOL_LOG_WARN("mission_begin: poster %d out of range", static_cast<int>(posterIndex));
        return false;
    }
    sol::sim::Mission& draft = content.missionDraft();
    draft = sol::sim::Mission{};
    draft.title = title;
    draft.campaignId = campaignId;
    draft.poster = static_cast<std::uint32_t>(poster);
    draft.rewardCredits = rewardCredits;
    draft.standingReward = static_cast<float>(repReward);
    draft.standingPenalty = static_cast<float>(repPenalty);
    content.setMissionDraftOpen(true);
    return true;
}

bool missionDeadline(GameContent& content, double seconds)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    content.missionDraft().deadline = seconds > 0.0 ? seconds : 0.0;
    return true;
}

bool missionMinRep(GameContent& content, double value)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    content.missionDraft().minRep = static_cast<float>(value);
    return true;
}

bool missionObjDock(GameContent& content, double system, double station,
                    const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    content.missionDraft().objectives.push_back(
        {.kind = sol::sim::ObjectiveKind::Dock,
         .system = static_cast<std::uint32_t>(system),
         .station = static_cast<std::uint32_t>(station),
         .text = text});
    return true;
}

bool missionObjDeliver(GameContent& content, double system, double station,
                       const std::string& commodityId, double units, const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    const std::uint32_t commodity = content.world().commodityIndex(commodityId.c_str());
    if (commodity >= content.world().commodityIds().size()) {
        SOL_LOG_WARN("mission_obj_deliver: unknown commodity '%s'", commodityId.c_str());
        return false;
    }
    content.missionDraft().objectives.push_back(
        {.kind = sol::sim::ObjectiveKind::Deliver,
         .system = static_cast<std::uint32_t>(system),
         .station = static_cast<std::uint32_t>(station),
         .commodity = commodity,
         .units = static_cast<float>(units),
         .text = text});
    return true;
}

// count ships of a faction (1-based); system < 0 means anywhere.
bool missionObjKill(GameContent& content, double factionIndex, double count, double system,
                    const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    const std::size_t faction = static_cast<std::size_t>(factionIndex) - 1;
    if (faction >= content.world().factions().size()) {
        SOL_LOG_WARN("mission_obj_kill: faction %d out of range",
                     static_cast<int>(factionIndex));
        return false;
    }
    content.missionDraft().objectives.push_back(
        {.kind = sol::sim::ObjectiveKind::Kill,
         .system = system < 0.0 ? sol::sim::kAnySystem : static_cast<std::uint32_t>(system),
         .faction = static_cast<std::uint32_t>(faction),
         .kills = static_cast<std::uint32_t>(count),
         .text = text});
    return true;
}

// Position is relative to the target system's first station (or its primary
// planet when the system has none) — Lua has no absolute-coordinate source.
bool missionObjFlyTo(GameContent& content, double system, double dx, double dy, double dz,
                     double radius, const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    const auto& systems = content.world().galaxy().systems;
    const std::size_t systemIndex = static_cast<std::size_t>(system);
    if (systemIndex >= systems.size()) {
        SOL_LOG_WARN("mission_obj_flyto: system %d out of range", static_cast<int>(system));
        return false;
    }
    const sol::sim::SystemSpec& spec = systems[systemIndex];
    const core::DVec3 anchor = !spec.stations.empty()
                                   ? spec.stations[0].position
                                   : spec.planets[spec.primaryPlanet].position;
    content.missionDraft().objectives.push_back(
        {.kind = sol::sim::ObjectiveKind::FlyTo,
         .system = static_cast<std::uint32_t>(systemIndex),
         .position = anchor + core::DVec3{dx, dy, dz},
         .radius = radius,
         .text = text});
    return true;
}

bool missionPost(GameContent& content)
{
    if (!content.missionDraftOpen()) {
        SOL_LOG_WARN("mission_post: no draft (call sol.mission_begin first)");
        return false;
    }
    content.setMissionDraftOpen(false);
    SpaceWorld& world = content.world();
    std::string error;
    if (!world.missionSim().postOffer(world.galaxy(), world.economy(), world.factionSim(),
                                      content.missionDraft(), &error)) {
        SOL_LOG_WARN("mission_post: '%s' refused: %s", content.missionDraft().title.c_str(),
                     error.c_str());
        return false;
    }
    return true;
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
    m_vm.registerFunction<&spawnPilotFaction>("sol", "spawn_pilot_faction", this);
    m_vm.registerFunction<&pilotAttackPlayer>("sol", "pilot_attack_player", this);
    m_vm.registerFunction<&pilotEngageEnemy>("sol", "pilot_engage_enemy", this);
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
    m_vm.registerFunction<&listGates>("sol", "gates", this);
    m_vm.registerFunction<&jumpToSystem>("sol", "jump_to", this);
    m_vm.registerFunction<&targetShip>("sol", "target_ship", this);
    m_vm.registerFunction<&traderStats>("sol", "trader_stats", this);
    m_vm.registerFunction<&autopilotEngage>("sol", "autopilot", this);
    m_vm.registerFunction<&autopilotOff>("sol", "autopilot_off", this);
    m_vm.registerFunction<&listModules>("sol", "modules", this);
    m_vm.registerFunction<&listCrewDefs>("sol", "crew_defs", this);
    m_vm.registerFunction<&fitInfo>("sol", "fit", this);
    m_vm.registerFunction<&listFleet>("sol", "fleet", this);
    m_vm.registerFunction<&buyModule>("sol", "buy_module", this);
    m_vm.registerFunction<&sellModule>("sol", "sell_module", this);
    m_vm.registerFunction<&buyWeapon>("sol", "buy_weapon", this);
    m_vm.registerFunction<&buyShip>("sol", "buy_ship", this);
    m_vm.registerFunction<&sellShip>("sol", "sell_ship", this);
    m_vm.registerFunction<&selectShip>("sol", "select_ship", this);
    m_vm.registerFunction<&hireCrew>("sol", "hire_crew", this);
    m_vm.registerFunction<&fireCrew>("sol", "fire_crew", this);
    m_vm.registerFunction<&insuranceQuote>("sol", "insurance_quote", this);
    m_vm.registerFunction<&addCredits>("sol", "add_credits", this);
    m_vm.registerFunction<&warpOffset>("sol", "warp", this);
    m_vm.registerFunction<&listFactions>("sol", "factions", this);
    m_vm.registerFunction<&listStandings>("sol", "rep", this);
    m_vm.registerFunction<&listRelations>("sol", "relations", this);
    m_vm.registerFunction<&listRaids>("sol", "raids", this);
    m_vm.registerFunction<&setStanding>("sol", "set_rep", this);
    m_vm.registerFunction<&factionCandidates>("sol", "faction_candidates", this);
    m_vm.registerFunction<&factionRaid>("sol", "faction_raid", this);
    // sol.mission_board (the listing) and the global mission_board hook (the
    // Lua-defined composer) live in different namespaces; no collision.
    m_vm.registerFunction<&listMissionBoard>("sol", "mission_board", this);
    m_vm.registerFunction<&listMissions>("sol", "missions", this);
    m_vm.registerFunction<&acceptMission>("sol", "accept_mission", this);
    m_vm.registerFunction<&abandonMission>("sol", "abandon_mission", this);
    m_vm.registerFunction<&trackMission>("sol", "track_mission", this);
    m_vm.registerFunction<&campaignStage>("sol", "campaign_stage", this);
    m_vm.registerFunction<&setCampaignStage>("sol", "set_campaign_stage", this);
    m_vm.registerFunction<&missionCandidatesInfo>("sol", "mission_candidates", this);
    m_vm.registerFunction<&gateDestination>("sol", "gate_destination", this);
    m_vm.registerFunction<&stationCount>("sol", "station_count", this);
    m_vm.registerFunction<&currentSystemIndex>("sol", "system_index", this);
    m_vm.registerFunction<&dockedStationIndex>("sol", "docked_station_index", this);
    m_vm.registerFunction<&missionBegin>("sol", "mission_begin", this);
    m_vm.registerFunction<&missionDeadline>("sol", "mission_deadline", this);
    m_vm.registerFunction<&missionMinRep>("sol", "mission_min_rep", this);
    m_vm.registerFunction<&missionObjDock>("sol", "mission_obj_dock", this);
    m_vm.registerFunction<&missionObjDeliver>("sol", "mission_obj_deliver", this);
    m_vm.registerFunction<&missionObjKill>("sol", "mission_obj_kill", this);
    m_vm.registerFunction<&missionObjFlyTo>("sol", "mission_obj_flyto", this);
    m_vm.registerFunction<&missionPost>("sol", "mission_post", this);
    // Exploration (Phase 8e). sol.set_loot is the signal_loot hook's builder;
    // the rest are read-outs plus two dev levers (scan, chart).
    m_vm.registerFunction<&listKnowledge>("sol", "knowledge", this);
    m_vm.registerFunction<&listSignals>("sol", "signals", this);
    m_vm.registerFunction<&pulseScan>("sol", "pulse", this);
    m_vm.registerFunction<&scanTarget>("sol", "scan", this);
    m_vm.registerFunction<&salvageNearest>("sol", "salvage", this);
    m_vm.registerFunction<&surveyLedger>("sol", "survey_ledger", this);
    m_vm.registerFunction<&sellSurvey>("sol", "sell_survey", this);
    m_vm.registerFunction<&plotRoute>("sol", "route", this);
    m_vm.registerFunction<&chartSystem>("sol", "chart", this);
    m_vm.registerFunction<&setSignalLoot>("sol", "set_loot", this);
    // Mining, salvage & refining (Phase 8f). sol.set_loot above doubles as the
    // wreck_loot builder; these are read-outs plus one dev lever (mine).
    m_vm.registerFunction<&listFields>("sol", "fields", this);
    m_vm.registerFunction<&listRocks>("sol", "rocks", this);
    m_vm.registerFunction<&listWrecks>("sol", "wrecks", this);
    m_vm.registerFunction<&mineAhead>("sol", "mine", this);
    m_vm.registerFunction<&warpToRock>("sol", "warp_rock", this);
    m_vm.registerFunction<&selectTargetByName>("sol", "target", this);
    m_vm.registerFunction<&orderRefine>("sol", "refine", this);
    m_vm.registerFunction<&collectRefined>("sol", "collect", this);
    m_vm.registerFunction<&listRefineJobs>("sol", "refine_jobs", this);
    // Economy coherence (Phase 8g): the report the exit criteria are stated
    // against, plus the read-outs that explain a station's or a system's
    // books when the report says something is wrong.
    m_vm.registerFunction<&economyReport>("sol", "economy_report", this);
    m_vm.registerFunction<&feedstockReport>("sol", "feedstock", this);
    m_vm.registerFunction<&fieldStock>("sol", "field_stock", this);
    m_vm.registerFunction<&marketMemory>("sol", "market_memory", this);
    m_vm.registerFunction<&buyMarketIntel>("sol", "buy_intel", this);
    m_vm.registerFunction<&bestPrice>("sol", "best_price", this);
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
    if (!fresh.validateFactions(&error)) { // cross-def check needs the merged set
        SOL_LOG_ERROR("data defs: %s", error.c_str());
        return false;
    }
    m_defs = std::move(fresh);
    return true;
}

void GameContent::runBootScripts()
{
    // Every scripts/*.lua per layer, sorted, init.lua first (it defines the
    // shared helpers) — campaign content gets its own file (Phase 8c).
    for (const std::string& layer : m_layerDirectories) {
        const std::string scriptsDir = layer + "/scripts";
        std::vector<std::string> scripts;
        for (const std::string& path : platform::listFiles(scriptsDir.c_str())) {
            if (hasExtension(path, ".lua")) {
                scripts.push_back(path);
            }
        }
        std::sort(scripts.begin(), scripts.end());
        const auto init = std::find(scripts.begin(), scripts.end(), scriptsDir + "/init.lua");
        if (init != scripts.end()) {
            std::rotate(scripts.begin(), init, init + 1);
        }
        for (const std::string& script : scripts) {
            std::string error;
            if (!m_vm.doFile(script.c_str(), &error)) {
                SOL_LOG_ERROR("%s", error.c_str());
            }
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
    lua_getglobal(state, "faction_think");
    m_hasFactionHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_factionHookFailed = false;
    lua_getglobal(state, "mission_board");
    m_hasBoardHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_boardHookFailed = false;
    lua_getglobal(state, "mission_event");
    m_hasMissionEventHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_missionEventHookFailed = false;
    lua_getglobal(state, "signal_loot");
    m_hasLootHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_lootHookFailed = false;
    lua_getglobal(state, "signal_found");
    m_hasSignalFoundHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_signalFoundHookFailed = false;
    lua_getglobal(state, "wreck_loot");
    m_hasWreckLootHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_wreckLootHookFailed = false;
    lua_getglobal(state, "rock_mined");
    m_hasRockMinedHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_rockMinedHookFailed = false;
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
                                 think.role, think.state, think.attitude)) {
                SOL_LOG_ERROR("pilot_think disabled until scripts reload: %s", error.c_str());
                m_pilotHookFailed = true;
                break;
            }
        }
    }

    // Faction strategy (Phase 8b): the sim queues one decision per faction
    // at its slow cadence; Lua's faction_think chooses (it can query
    // sol.faction_candidates and commit sol.faction_raid), with the C++
    // default policy standing in when no hook is defined.
    m_factionDecisions.clear();
    m_world->factionSim().takeDueDecisions(m_factionDecisions);
    for (const sol::sim::FactionDecision& decision : m_factionDecisions) {
        if (m_hasFactionHook && !m_factionHookFailed) {
            const game::GameFaction& faction = m_world->factions()[decision.faction];
            std::string error;
            if (m_vm.callGlobal("faction_think", &error,
                                static_cast<double>(decision.faction + 1),
                                faction.name.c_str(), faction.pirate,
                                static_cast<double>(faction.aggression),
                                static_cast<double>(faction.forgiveness),
                                static_cast<double>(decision.roll))) {
                continue;
            }
            SOL_LOG_ERROR("faction_think disabled until scripts reload: %s", error.c_str());
            m_factionHookFailed = true;
        }
        m_world->applyDefaultFactionDecision(decision);
    }

    // Mission board (Phase 8c): re-composed on every dock event and on the
    // docked refresh cadence; Lua posts offers through the sol.mission_*
    // builder against the candidates runMissionBoard enumerates.
    const bool dockEvent = m_world->consumeDockEvent();
    if (m_world->isDocked() &&
        (dockEvent || m_world->missionSim().tickBoard(dt))) {
        runMissionBoard();
    }

    // Campaign flavor: the world already applied payouts/penalties; authored
    // missions' transitions are forwarded to Lua's mission_event hook.
    m_missionEvents.clear();
    m_world->takeMissionEvents(m_missionEvents);
    if (m_hasMissionEventHook && !m_missionEventHookFailed) {
        for (const sol::sim::MissionEvent& event : m_missionEvents) {
            if (event.mission.campaignId.empty()) {
                continue;
            }
            const char* kind = "accepted";
            switch (event.kind) {
            case sol::sim::MissionEventKind::Accepted:
                break;
            case sol::sim::MissionEventKind::ObjectiveComplete:
                kind = "objective";
                break;
            case sol::sim::MissionEventKind::Completed:
                kind = "completed";
                break;
            case sol::sim::MissionEventKind::Failed:
                kind = "failed";
                break;
            case sol::sim::MissionEventKind::Abandoned:
                kind = "abandoned";
                break;
            }
            std::string error;
            if (!m_vm.callGlobal("mission_event", &error, event.mission.campaignId.c_str(),
                                 kind, static_cast<double>(event.objective + 1))) {
                SOL_LOG_ERROR("mission_event disabled until scripts reload: %s",
                              error.c_str());
                m_missionEventHookFailed = true;
                break;
            }
        }
    }

    // Exploration (Phase 8e): a resolved site already holds the scriptless
    // default loot, and signal_loot may replace it through sol.set_loot - the
    // same "C++ enumerates, Lua composes, C++ validates" shape the board uses.
    // The signal's own seeded roll is the only entropy the hook gets.
    m_surveyEvents.clear();
    m_world->takeSurveyEvents(m_surveyEvents);
    for (const SurveyEvent& event : m_surveyEvents) {
        const char* systemName = event.system < m_world->galaxy().systems.size()
                                     ? m_world->galaxy().systems[event.system].name.c_str()
                                     : "";
        if (event.kind == SurveyEvent::Kind::SignalDiscovered) {
            if (m_hasSignalFoundHook && !m_signalFoundHookFailed) {
                std::string error;
                if (!m_vm.callGlobal("signal_found", &error,
                                     sol::sim::signalKindName(event.signalKind), systemName)) {
                    SOL_LOG_ERROR("signal_found disabled until scripts reload: %s",
                                  error.c_str());
                    m_signalFoundHookFailed = true;
                }
            }
            continue;
        }
        if (event.kind != SurveyEvent::Kind::SignalResolved || !m_hasLootHook
            || m_lootHookFailed) {
            continue;
        }
        const sol::sim::Region region =
            event.system < m_world->galaxy().systems.size()
                ? m_world->galaxy().systems[event.system].region
                : sol::sim::Region::Core;
        m_lootSystem = event.system;
        m_lootSignal = event.index;
        std::string error;
        if (!m_vm.callGlobal("signal_loot", &error,
                             sol::sim::signalKindName(event.signalKind), systemName,
                             regionWord(region),
                             static_cast<double>(event.seed >> 11) * 0x1.0p-53)) {
            SOL_LOG_ERROR("signal_loot disabled until scripts reload: %s", error.c_str());
            m_lootHookFailed = true;
        }
        m_lootSystem = 0xffff'ffffu;
        m_lootSignal = 0xffff'ffffu;
    }

    // Salvage (Phase 8f): a fresh wreck already holds the scriptless default
    // composed from the ship that died, and wreck_loot may replace it through
    // the same sol.set_loot — until the beam has been into it.
    m_wreckEvents.clear();
    m_world->takeWreckEvents(m_wreckEvents);
    for (const WreckEvent& event : m_wreckEvents) {
        if (!m_hasWreckLootHook || m_wreckLootHookFailed) {
            continue;
        }
        const char* systemName = event.system < m_world->galaxy().systems.size()
                                     ? m_world->galaxy().systems[event.system].name.c_str()
                                     : "";
        m_lootWreck = event.id;
        std::string error;
        if (!m_vm.callGlobal("wreck_loot", &error, event.defId.c_str(), systemName,
                             event.factionName.c_str(),
                             static_cast<double>(event.seed >> 11) * 0x1.0p-53)) {
            SOL_LOG_ERROR("wreck_loot disabled until scripts reload: %s", error.c_str());
            m_wreckLootHookFailed = true;
        }
        m_lootWreck = 0;
    }

    // Flavor: a rock finished. Fired once per rock rather than per bite, so a
    // held beam does not print ten lines a second.
    m_rockEvents.clear();
    m_world->takeRockEvents(m_rockEvents);
    for (const RockEvent& event : m_rockEvents) {
        if (!m_hasRockMinedHook || m_rockMinedHookFailed) {
            continue;
        }
        const std::vector<std::string>& ids = m_world->commodityIds();
        std::string error;
        if (!m_vm.callGlobal("rock_mined", &error,
                             event.commodity < ids.size() ? ids[event.commodity].c_str() : "",
                             static_cast<double>(event.units))) {
            SOL_LOG_ERROR("rock_mined disabled until scripts reload: %s", error.c_str());
            m_rockMinedHookFailed = true;
        }
    }
}

void GameContent::runMissionBoard()
{
    SpaceWorld& world = *m_world;
    sol::sim::MissionSim& missions = world.missionSim();
    missions.openBoard(world.currentSystemIndex(), world.dockedStationIndex());
    if (!m_hasBoardHook || m_boardHookFailed) {
        return; // scriptless fallback: an empty board
    }
    const std::uint32_t owner = world.systemOwnerFaction(world.currentSystemIndex());
    if (owner >= world.factions().size()) {
        return; // ownerless station: nobody to post work
    }

    // Candidate strings, faction_candidates-style. Hauls:
    // "system:station:commodityId:units:severity:jumps:systemName:stationName"
    m_haulCandidates.clear();
    missions.haulCandidates(world.galaxy(), world.economy(), world.currentSystemIndex(),
                            world.dockedStationIndex(), m_haulCandidates);
    std::string hauls;
    for (const sol::sim::HaulCandidate& c : m_haulCandidates) {
        if (!hauls.empty()) {
            hauls += ";";
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.0f:%.2f:%u", static_cast<double>(c.units),
                      static_cast<double>(c.severity), c.jumps);
        const sol::sim::SystemSpec& spec = world.galaxy().systems[c.system];
        hauls += std::to_string(c.system) + ":" + std::to_string(c.station) + ":" +
                 world.commodityIds()[c.commodity] + ":" + buffer + ":" + spec.name + ":" +
                 spec.stations[c.station].name;
    }
    // Bounties: "system:clanIndex1based:intensity:jumps:systemName:clanName".
    m_bountyCandidates.clear();
    missions.bountyCandidates(world.galaxy(), world.factionSim(),
                              world.currentSystemIndex(), m_bountyCandidates);
    std::string bounties;
    for (const sol::sim::BountyCandidate& c : m_bountyCandidates) {
        if (c.clan >= world.factions().size()) {
            continue;
        }
        if (!bounties.empty()) {
            bounties += ";";
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.2f:%u", static_cast<double>(c.intensity),
                      c.jumps);
        bounties += std::to_string(c.system) + ":" + std::to_string(c.clan + 1) + ":" +
                    buffer + ":" + world.galaxy().systems[c.system].name + ":" +
                    world.factions()[c.clan].name;
    }

    std::string error;
    if (!m_vm.callGlobal("mission_board", &error, world.dockedStationName(),
                         static_cast<double>(owner + 1),
                         world.factions()[owner].name.c_str(),
                         world.factions()[owner].pirate, hauls.c_str(),
                         bounties.c_str(), static_cast<double>(missions.boardRoll()))) {
        SOL_LOG_ERROR("mission_board disabled until scripts reload: %s", error.c_str());
        m_boardHookFailed = true;
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
