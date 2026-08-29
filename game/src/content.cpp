#include "content.hpp"

#include "game_audio.hpp"
#include "lod_report.hpp"
#include "map_ui.hpp"
#include "model_roles.hpp"
#include "ship_ui.hpp"
#include "target_pick.hpp"

#include "sol/assets/mesh_lod.hpp"
#include "sol/core/log.hpp"
#include "sol/core/profiler.hpp"
#include "sol/platform/file_io.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace game {

using namespace sol;

namespace {

[[nodiscard]] const char* markerKindName(sol::ui::MapMarkerRow::Kind kind)
{
    switch (kind) {
    case sol::ui::MapMarkerRow::Kind::Star:
        return "star";
    case sol::ui::MapMarkerRow::Kind::Planet:
        return "planet";
    case sol::ui::MapMarkerRow::Kind::Station:
        return "station";
    case sol::ui::MapMarkerRow::Kind::Gate:
        return "gate";
    case sol::ui::MapMarkerRow::Kind::Signal:
        return "signal";
    case sol::ui::MapMarkerRow::Kind::Field:
        return "field";
    case sol::ui::MapMarkerRow::Kind::Wreck:
        return "wreck";
    case sol::ui::MapMarkerRow::Kind::Bookmark:
        return "bookmark";
    case sol::ui::MapMarkerRow::Kind::Objective:
        return "objective";
    }
    return "?";
}

[[nodiscard]] bool hasExtension(const std::string& path, const char* extension)
{
    const std::size_t length = std::char_traits<char>::length(extension);
    return path.size() >= length && path.compare(path.size() - length, length, extension) == 0;
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

scripting::EntityHandle spawnPilot(GameContent& content, const std::string& id, const std::string& role)
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
        SOL_LOG_WARN("spawn_pilot: unknown role '%s' (fighter/trader/patrol); using fighter", role.c_str());
    }
    const ecs::Entity entity = content.world().spawnPilotFromDef(*def, content.defs(), pilotRole);
    SOL_LOG_INFO("spawned %s pilot '%s' (%s)", role.c_str(), def->name.c_str(), def->id.c_str());
    return scripting::toHandle(entity);
}

// As spawn_pilot, with an allegiance: factionIndex is 1-based into
// sol.factions (the runtime table: majors then clans).
scripting::EntityHandle
spawnPilotFaction(GameContent& content, const std::string& id, const std::string& role, double factionIndex)
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
    SOL_LOG_INFO("spawned %s pilot '%s' for %s",
                 role.c_str(),
                 def->name.c_str(),
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

// Predation (Phase 8x §D). Which hauler is fair game is a fact about the
// galaxy and stays in C++; whether cargo outranks the war in front of you is
// strategy and stays in pilot_think.
bool pilotHuntTrader(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().pilotHuntTrader(scripting::toEntity(ship));
}

bool pilotEngageThreat(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().pilotEngageThreat(scripting::toEntity(ship));
}

bool pilotUnderFire(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().pilotUnderFire(scripting::toEntity(ship));
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
bool pilotPatrolOffset(GameContent& content, scripting::EntityHandle ship, double dx, double dy, double dz)
{
    const core::DVec3 waypoint = content.world().stationPosition() + core::DVec3{dx, dy, dz};
    return content.world().pilotPatrolTo(scripting::toEntity(ship), waypoint);
}

double pilotHull(GameContent& content, scripting::EntityHandle ship)
{
    return content.world().shipHullFraction(scripting::toEntity(ship));
}

// --- Audio (Phase 8t) ---

// The model catalog (Phase 9). Prints the index each row resolved to, because
// that index is what every RenderShape holds and what the renderer looks up -
// so a drive can tell "the def is missing" from "the def is there and the
// entity is pointing at the wrong row".
std::string listModels(GameContent& content)
{
    std::string out;
    const auto& models = content.defs().models();
    const auto& materials = content.defs().materials();
    for (std::size_t i = 0; i < models.size(); ++i) {
        const assets::ModelDef& def = models[i];
        // ⚑ Phase 25 stage A: the surface half of this line is the MATERIAL's,
        // and a row that names one carries none of it itself. Reading the model
        // here would have printed an empty texture and an unlit membrane for
        // every migrated row - a probe quietly disagreeing with the picture,
        // which is the one thing a probe must never do.
        const assets::MaterialDef* material =
            def.materialIndex < materials.size() ? &materials[def.materialIndex] : nullptr;
        // Phase 12: the blend state is reported too. Without it a drive cannot
        // tell a translucent row from an opaque one, which is the same gap this
        // probe exists to close for the mesh and texture indices.
        char film[24] = {};
        if (material != nullptr && material->translucent) {
            std::snprintf(film, sizeof(film), " [film a%.2f]", static_cast<double>(material->alpha));
        }
        char line[256] = {};
        std::snprintf(line,
                      sizeof(line),
                      "%s#%zu %s: %s/%s r%.0f a%.0f%s%s%s <%s>",
                      i == 0 ? "" : "\n",
                      i,
                      def.id.c_str(),
                      def.mesh.c_str(),
                      material != nullptr ? material->texture.c_str() : "?",
                      static_cast<double>(def.radius),
                      static_cast<double>(def.avoidRadius),
                      def.solid ? "" : " [pass-through]",
                      material != nullptr && material->emissive > 0.0f ? " [lit]" : "",
                      film,
                      material != nullptr ? material->id.c_str() : "unresolved");
        out += line;

        // ⚑ Phase 25 stage C: WHAT THE MATERIAL DECLARES, because otherwise the
        // only way to see a slot or a param is to look at the picture and guess.
        // The declaration is the half of a material that has no visible
        // consequence a drive can measure - a wrong `glow_strength` looks like a
        // lighting opinion - so it is printed, values included. Appended rather
        // than squeezed into the buffer above: a material may declare several of
        // each, and a probe that truncates is a probe that lies.
        if (material != nullptr && (!material->slots.empty() || !material->params.empty())) {
            out += " {";
            bool first = true;
            for (const assets::MaterialSlot& slot : material->slots) {
                out += first ? "" : ", ";
                first = false;
                out += slot.name + "=" + slot.texture;
            }
            for (const assets::MaterialParam& param : material->params) {
                char value[64] = {};
                std::snprintf(
                    value, sizeof(value), "%s=%.3f", param.name.c_str(), static_cast<double>(param.value));
                out += first ? "" : ", ";
                first = false;
                out += value;
            }
            out += "}";
        }
    }
    if (out.empty()) {
        return "no model defs";
    }
    char footer[64] = {};
    std::snprintf(footer, sizeof(footer), "\n%zu model(s)", models.size());
    return out + footer;
}

std::string listSounds(GameContent& content)
{
    // Named gameAudio, not audio: a local called `audio` shadows the sol::audio
    // namespace inside these functions.
    GameAudio* gameAudio = content.audio();
    if (gameAudio == nullptr) {
        return "audio: not initialized";
    }
    std::string out;
    for (const assets::SoundDef& def : content.defs().sounds()) {
        if (!out.empty()) {
            out += ", ";
        }
        out += def.id;
        // A cue whose asset failed to cook is in the defs and not in the bank;
        // saying so here is what separates "wrong id" from "missing file".
        if (gameAudio->find(def.id.c_str()) == audio::kNoSound) {
            out += "(missing)";
        }
    }
    return out.empty() ? "no sound defs" : out;
}

std::string audioReport(GameContent& content)
{
    // Named gameAudio, not audio: a local called `audio` shadows the sol::audio
    // namespace inside these functions.
    GameAudio* gameAudio = content.audio();
    if (gameAudio == nullptr) {
        return "audio: not initialized";
    }
    if (!gameAudio->deviceOpen()) {
        return "audio: NO DEVICE (running silent), " + std::to_string(gameAudio->cueCount()) +
               " cue(s) loaded";
    }
    const platform::AudioDeviceInfo info = gameAudio->deviceInfo();
    char buffer[224] = {};
    (void)std::snprintf(buffer,
                        sizeof(buffer),
                        "audio: %u Hz, %u frame buffer, %zu cue(s), vol %.2f/%.2f, "
                        "%u voice(s) active, "
                        "%llu played, %llu stolen, %llu dropped, %llu underrun(s)",
                        info.sampleRate,
                        info.bufferFrames,
                        gameAudio->cueCount(),
                        static_cast<double>(gameAudio->masterVolume()),
                        static_cast<double>(gameAudio->effectsVolume()),
                        gameAudio->activeVoices(),
                        static_cast<unsigned long long>(gameAudio->playedCues()),
                        static_cast<unsigned long long>(gameAudio->stolenVoices()),
                        static_cast<unsigned long long>(gameAudio->droppedCommands()),
                        static_cast<unsigned long long>(gameAudio->underruns()));
    return buffer;
}

// sol.play_sound("sol.explosion") plays at the listener; adding x,y,z places
// it in the world, where it is attenuated and panned like any other cue.
bool playSound(GameContent& content, const char* id)
{
    // Named gameAudio, not audio: a local called `audio` shadows the sol::audio
    // namespace inside these functions.
    GameAudio* gameAudio = content.audio();
    if (gameAudio == nullptr) {
        return false;
    }
    const audio::SoundId cue = gameAudio->find(id);
    if (cue == audio::kNoSound) {
        SOL_LOG_WARN("play_sound: no cue '%s'", id);
        return false;
    }
    gameAudio->play2D(cue);
    return true;
}

bool playSoundAt(GameContent& content, const char* id, double x, double y, double z)
{
    // Named gameAudio, not audio: a local called `audio` shadows the sol::audio
    // namespace inside these functions.
    GameAudio* gameAudio = content.audio();
    if (gameAudio == nullptr) {
        return false;
    }
    const audio::SoundId cue = gameAudio->find(id);
    if (cue == audio::kNoSound) {
        SOL_LOG_WARN("play_sound_at: no cue '%s'", id);
        return false;
    }
    gameAudio->playAt(cue, core::DVec3{x, y, z});
    return true;
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
//
// This PLAYS THE FULL TRANSITION (Phase 8v) and so returns before arriving —
// deliberately, because it is the lever drives use to exercise the real path,
// and a lever that silently skipped the transition would be a second
// implementation of jumping. sol.jump_to() is the instant teleport.
bool jumpNearestGate(GameContent& content)
{
    return content.world().jumpNearestGate(1.0e30);
}

// Where a jump has got to, so a drive can assert the sequence without reading
// a pixel: phase, elapsed, streak strength and destination.
std::string jumpState(GameContent& content)
{
    const SpaceWorld& world = content.world();
    const sol::sim::JumpTransition& jump = world.jumpTransition();
    const char* phase = "idle";
    switch (jump.phase()) {
    case sol::sim::JumpPhase::Idle:
        phase = "idle";
        break;
    case sol::sim::JumpPhase::Tunnel:
        phase = "tunnel";
        break;
    case sol::sim::JumpPhase::Arrive:
        phase = "arrive";
        break;
    }
    char buffer[192];
    if (!jump.active()) {
        std::snprintf(buffer, sizeof(buffer), "idle in %s", world.currentSystemName());
        return buffer;
    }
    const std::uint32_t destination = jump.destination();
    const char* destinationName =
        destination < world.galaxy().systems.size() ? world.galaxy().systems[destination].name.c_str() : "?";
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s t=%.2f warp=%.2f sky=%.2f -> %s",
                  phase,
                  jump.elapsed(),
                  jump.warp(),
                  jump.skyScale(),
                  destinationName);
    return buffer;
}

// Distance to the nearest gate in metres, or -1 with no gates in system.
double gateDistance(GameContent& content)
{
    return content.world().nearestGateDistance();
}

// Dev shortcut: dock at the nearest station regardless of range and without
// asking anyone. Kept exactly as it was through Phase 8r on purpose — every
// drive script in the repo uses it, and the clearance flow has its own
// binding (sol.request_dock) beside it.
bool dockNearest(GameContent& content)
{
    return content.world().tryDockNearestStation(1.0e30);
}

// The real thing: hail the nearest station and let the dispatcher answer.
bool requestDock(GameContent& content)
{
    return content.world().requestDocking();
}

// What the clearance is, if any — the probe a drive asserts the state machine
// through, rather than reading a pixel.
std::string describeClearance(GameContent& content)
{
    SpaceWorld& world = content.world();
    if (world.isDocked()) {
        return std::string("docked at ") + world.dockedStationName();
    }
    if (!world.hasClearance()) {
        return "no clearance";
    }
    const SpaceWorld::DockClearance& clearance = world.clearance();
    const sol::sim::SystemSpec& spec = world.galaxy().systems[world.currentSystemIndex()];
    char buffer[192] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s berth %u - %.0f m away, %.0f s left",
                  spec.stations[clearance.station].name.c_str(),
                  clearance.berth + 1,
                  length(world.clearedBerthPoint() - world.shipState().position),
                  clearance.secondsLeft);
    return buffer;
}

// Every berth of a station, with the one number the geometry has to get right:
// how far the capture sphere stays clear of the 130 m avoidance sphere.
std::string listBerths(GameContent& content, double stationIndex)
{
    SpaceWorld& world = content.world();
    const sol::sim::SystemSpec& spec = world.galaxy().systems[world.currentSystemIndex()];
    const auto station = static_cast<std::uint32_t>(stationIndex < 0.0 ? 0.0 : stationIndex);
    if (station >= spec.stations.size()) {
        return "no such station in this system";
    }
    std::string out = spec.stations[station].name + ":\n";
    const sol::core::DVec3 ship = world.shipState().position;
    for (std::uint32_t berth = 0; berth < sol::sim::kBerthCount; ++berth) {
        const sol::core::DVec3 point = sol::sim::berthPoint(spec.stations[station].position, berth);
        char buffer[160] = {};
        std::snprintf(buffer,
                      sizeof(buffer),
                      "  berth %u - %.0f m off the hub, %.0f m from the ship\n",
                      berth + 1,
                      length(point - spec.stations[station].position),
                      length(point - ship));
        out += buffer;
    }
    return out;
}

// The dock_request hook's two builders. Both refuse outside the hook, the same
// way sol.set_loot does, so a script cannot clear itself to dock from on_tick.
bool grantDocking(GameContent& content, double berth, const char* message)
{
    const std::uint32_t station = content.dockRequestStation();
    if (station == 0xffff'ffffu) {
        SOL_LOG_WARN("grant_docking: only valid inside dock_request");
        return false;
    }
    content.noteDockAnswered();
    return content.world().grantDocking(station,
                                        static_cast<std::uint32_t>(berth < 1.0 ? 0.0 : berth - 1.0),
                                        message != nullptr ? message : "Cleared to dock.");
}

bool denyDocking(GameContent& content, const char* message)
{
    const std::uint32_t station = content.dockRequestStation();
    if (station == 0xffff'ffffu) {
        SOL_LOG_WARN("deny_docking: only valid inside dock_request");
        return false;
    }
    content.noteDockAnswered();
    content.world().denyDocking(station, message != nullptr ? message : "Clearance denied.");
    return true;
}

// The pilot_hail hook's three builders (Phase 8s). All three refuse outside
// the hook, the same way the docking pair above does, so a script cannot talk
// itself into a free market report from on_tick. The guard is
// SpaceWorld::answeringHail(), which the first answer closes — so "only inside
// the hook" and "answer with exactly one" are one fact rather than two.
bool hailReply(GameContent& content, const char* message)
{
    if (!content.world().answeringHail()) {
        SOL_LOG_WARN("hail_reply: only valid inside pilot_hail");
        return false;
    }
    return content.world().replyHail(message != nullptr ? message : "Nothing to report.");
}

bool hailTipMarket(GameContent& content, const char* message)
{
    if (!content.world().answeringHail()) {
        SOL_LOG_WARN("hail_tip_market: only valid inside pilot_hail");
        return false;
    }
    // The message is the sentiment only: C++ appends which market, because
    // naming it is a claim about the galaxy rather than a turn of phrase.
    return content.world().tipMarket(message != nullptr ? message : "Prices were good at");
}

bool hailTipPlace(GameContent& content, const char* message)
{
    if (!content.world().answeringHail()) {
        SOL_LOG_WARN("hail_tip_place: only valid inside pilot_hail");
        return false;
    }
    return content.world().tipPlace(message != nullptr ? message : "Saw something unclaimed out in");
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

// The same fleet with its routes read back (Phase 8x): what every hauler is
// doing, and every one of them that is in this system right now — which is
// exactly the set that gets a body in stage 2. `sol.trader_stats` answers
// whether the layer runs at all; this answers where.
std::string traderRoutes(GameContent& content)
{
    SpaceWorld& world = content.world();
    const sol::sim::Economy& economy = world.economy();
    const sol::sim::Galaxy& galaxy = world.galaxy();
    const std::uint32_t here = world.currentSystemIndex();

    const auto stationName = [&](std::uint32_t market) -> const char* {
        if (market >= economy.markets().size()) {
            return "?";
        }
        const sol::sim::StationMarket& row = economy.markets()[market];
        return galaxy.systems[row.systemIndex].stations[row.stationIndex].name.c_str();
    };

    std::uint32_t byLeg[4] = {0, 0, 0, 0};
    std::string lines;
    char buffer[256];
    for (std::uint32_t t = 0; t < economy.traders().size(); ++t) {
        const sol::sim::TraderRoute route = economy.route(t);
        byLeg[static_cast<std::size_t>(route.leg)] += 1;
        if (route.system != here) {
            continue; // elsewhere, or between gates and nowhere at all
        }
        const sol::sim::EconomyTrader& trader = economy.traders()[t];
        // `commodity` is whatever it last hauled and stays set when the hold
        // is empty, so a deadheading trader would otherwise be reported
        // carrying a cargo it does not have.
        char hold[48];
        if (trader.cargo > 0.0f && trader.commodity < world.commodityIds().size()) {
            std::snprintf(hold,
                          sizeof(hold),
                          "%s %.0fu",
                          world.commodityIds()[trader.commodity].c_str(),
                          static_cast<double>(trader.cargo));
        } else {
            std::snprintf(hold, sizeof(hold), "empty");
        }
        const char* body = world.traderHasBody(t) ? " *" : "";
        if (route.leg == sol::sim::TraderLeg::None) {
            std::snprintf(
                buffer, sizeof(buffer), "#%u idle at %s (%s)%s", t, stationName(route.toMarket), hold, body);
        } else {
            std::snprintf(buffer,
                          sizeof(buffer),
                          "#%u %s  %s -> %s  %s %.0f%% (%u hop)%s",
                          t,
                          hold,
                          stationName(route.fromMarket),
                          stationName(route.toMarket),
                          route.leg == sol::sim::TraderLeg::Depart ? "departing" : "arriving",
                          static_cast<double>(route.progress) * 100.0,
                          route.hops,
                          body);
        }
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::uint32_t drawn = 0;
    for (std::uint32_t t = 0; t < economy.traders().size(); ++t) {
        drawn += world.traderHasBody(t) ? 1u : 0u;
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%zu traders: %u idle, %u departing, %u jumping, %u arriving; %u drawn "
                  "here; %u lost this session",
                  economy.traders().size(),
                  byLeg[0],
                  byLeg[1],
                  byLeg[2],
                  byLeg[3],
                  drawn,
                  world.traderLossCount());
    return (lines.empty() ? std::string("(none in this system)") : lines) + "\n" + buffer;
}

// What attrition reads (Phase 8x): the danger a system poses to a haul flying
// through it, and the two pieces of already-existing state it is made of.
std::string systemDanger(GameContent& content, double systemIndex)
{
    SpaceWorld& world = content.world();
    const auto system = static_cast<std::uint32_t>(systemIndex);
    if (system >= world.galaxy().systems.size()) {
        return "no such system";
    }
    const sol::sim::FactionSim& factions = world.factionSim();
    const sol::sim::SystemContest contest = factions.contestOf(system);
    char buffer[192];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s: danger %.3f (raids %.2f, contest %.2f)%s",
                  world.galaxy().systems[system].name.c_str(),
                  static_cast<double>(factions.danger(system)),
                  static_cast<double>(factions.raidIntensity(system)),
                  static_cast<double>(contest.live() ? contest.pressure : 0.0f),
                  system == world.currentSystemIndex() ? " [HERE: sheltered from attrition]" : "");
    return buffer;
}

// How well one place is policed (Phase 30 stage A). Both halves are printed
// side by side deliberately: the whole of decisions/019 is that they answer two
// different questions, and a probe showing only the live number cannot tell a
// quiet fringe system from a core system somebody is currently burning down.
std::string systemSecurity(GameContent& content, double systemIndex)
{
    SpaceWorld& world = content.world();
    const auto system = static_cast<std::uint32_t>(systemIndex);
    if (system >= world.galaxy().systems.size()) {
        return "no such system";
    }
    const float baseline = world.systemSecurityBaseline(system);
    const float live = world.systemSecurity(system);
    const std::uint32_t owner = world.systemOwnerFaction(system);
    const char* held = baseline > 0.0f ? "policed by" : baseline < 0.0f ? "held by" : "nobody polices";
    char buffer[256];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s: security %+.3f (baseline %+.3f - danger %.3f), %s %s",
                  world.galaxy().systems[system].name.c_str(),
                  static_cast<double>(live),
                  static_cast<double>(baseline),
                  static_cast<double>(world.factionSim().danger(system)),
                  held,
                  owner < world.factions().size() ? world.factions()[owner].name.c_str() : "no one");
    return buffer;
}

// Stand in a system directly (Phase 30 stage C). A response is a property of
// where you are, and routing eight gates to reach a place is how a stage goes
// unverified. Same call the death-respawn path already makes.
bool enterSystemAt(GameContent& content, double systemIndex)
{
    return content.world().enterSystem(static_cast<std::uint32_t>(systemIndex));
}

// Dispatch a response at a point, and say what came (Phase 30 stage C). The
// probe reports both halves because "nobody came" and "two were diverted" are
// the same call with a different rating behind it, and telling them apart from
// the cockpit alone would mean waiting out a flight that never starts.
std::string dispatchResponse(GameContent& content, double x, double y, double z)
{
    SpaceWorld& world = content.world();
    const sol::core::DVec3 at{x, y, z};
    const std::uint32_t sent = world.respondTo(at, 0xffff'ffffu, game::ResponseCause::WeaponsFire);
    const game::SpaceWorld::ResponseReport& report = world.lastResponse();
    char buffer[256];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s: live %+.3f, reach %.0f km -> %u diverted, %u launched%s",
                  world.galaxy().systems[world.currentSystemIndex()].name.c_str(),
                  static_cast<double>(report.live),
                  report.reach / 1000.0,
                  report.diverted,
                  report.spawned,
                  sent == 0 ? " (NOBODY CAME)" : "");
    return buffer;
}

// The gradient, galaxy-wide, in one call - this is Phase 30 stage A's own exit
// criterion made runnable rather than a thing to be eyeballed system by system.
// Baselines only: the live rating moves under the player's feet, and what stage
// A claims is a property of the GENERATOR.
std::string securityHistogram(GameContent& content)
{
    SpaceWorld& world = content.world();
    const std::vector<sol::sim::SystemSpec>& systems = world.galaxy().systems;
    // core / frontier / fringe, then the clan band, which cuts across regions.
    double sum[3] = {0.0, 0.0, 0.0};
    std::uint32_t seen[3] = {0, 0, 0};
    float lowest[3] = {2.0f, 2.0f, 2.0f};
    float highest[3] = {-2.0f, -2.0f, -2.0f};
    double clanSum = 0.0;
    std::uint32_t clanSeen = 0;
    float clanDeepest = 0.0f;
    std::uint32_t zeroes = 0;
    for (std::uint32_t i = 0; i < systems.size(); ++i) {
        const float value = systems[i].security;
        if (value < 0.0f) {
            clanSum += static_cast<double>(value);
            ++clanSeen;
            clanDeepest = std::min(clanDeepest, value);
            continue;
        }
        if (value == 0.0f) {
            ++zeroes;
            continue;
        }
        const auto tier = static_cast<std::size_t>(systems[i].region);
        sum[tier] += static_cast<double>(value);
        ++seen[tier];
        lowest[tier] = std::min(lowest[tier], value);
        highest[tier] = std::max(highest[tier], value);
    }
    std::string out;
    static constexpr const char* kTierNames[3] = {"core     ", "frontier ", "fringe   "};
    char buffer[192];
    for (std::size_t tier = 0; tier < 3; ++tier) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s %3u system(s)  mean %+.3f  [%+.3f .. %+.3f]\n",
                      kTierNames[tier],
                      seen[tier],
                      seen[tier] > 0 ? sum[tier] / seen[tier] : 0.0,
                      seen[tier] > 0 ? static_cast<double>(lowest[tier]) : 0.0,
                      seen[tier] > 0 ? static_cast<double>(highest[tier]) : 0.0);
        out += buffer;
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "clan-held %3u system(s)  mean %+.3f  deepest %+.3f\n"
                  "unpoliced %3u system(s) at exactly 0\n",
                  clanSeen,
                  clanSeen > 0 ? clanSum / clanSeen : 0.0,
                  static_cast<double>(clanDeepest),
                  zeroes);
    out += buffer;
    return out;
}

// What the board would be offered here (Phase 8x §E). Printed against the
// player's own system rather than against a docked station, so the eligibility
// rule can be read while flying: an escort candidate is a hauler DEPARTING
// this system, and the set empties by itself as each one reaches its gate.
std::string escortCandidates(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::vector<sol::sim::EscortCandidate> candidates;
    world.missionSim().escortCandidates(
        world.galaxy(), world.economy(), world.factionSim(), world.currentSystemIndex(), candidates);
    std::string lines;
    char buffer[256];
    for (const sol::sim::EscortCandidate& c : candidates) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "#%u -> %s  %s %.0fu, %u hop(s), danger %.2f%s",
                      c.trader,
                      c.system < world.galaxy().systems.size() ? world.galaxy().systems[c.system].name.c_str()
                                                               : "?",
                      c.cargo > 0.0f && c.commodity < world.commodityIds().size()
                          ? world.commodityIds()[c.commodity].c_str()
                          : "empty",
                      static_cast<double>(c.cargo),
                      c.jumps,
                      static_cast<double>(c.danger),
                      world.traderHasBody(c.trader) ? " *" : "");
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%zu escortable hauler(s) leaving %s",
                  candidates.size(),
                  world.galaxy().systems[world.currentSystemIndex()].name.c_str());
    return (lines.empty() ? std::string("(none)") : lines) + "\n" + buffer;
}

// Dev lever for a loss. Goes down the same road a real one does - through the
// body if this trader has one here - because a lever that reaches a state the
// running game cannot is a second implementation (8u).
bool killTrader(GameContent& content, double traderIndex)
{
    return content.world().killCoarseTrader(static_cast<std::uint32_t>(traderIndex));
}

// The same lever for a mining ship (Phase 8x stage 6), taking the market index
// sol.miners() prints. False means there is no such body here, which is the
// only honest answer: a miner has no record apart from its outpost's draw.
bool killMiner(GameContent& content, double market)
{
    return content.world().killMinerPuppet(static_cast<std::uint32_t>(market));
}

// What is actually in the sky, read off the entities rather than off the
// record (Phase 8x). sol.traders() says who *should* have a body; this says
// who does, and the two disagreeing is the whole failure mode of a promotion.
std::string traderPuppets(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::vector<game::TraderPuppetInfo> puppets;
    world.traderPuppetInfo(puppets);
    std::string lines;
    char buffer[256];
    for (const game::TraderPuppetInfo& puppet : puppets) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "#%u %s  %.0f km out, %.0f m/s, %s",
                      puppet.traderIndex,
                      puppet.name.c_str(),
                      puppet.distance / 1000.0,
                      puppet.speed,
                      puppet.state);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%zu trader bod%s in %s",
                  puppets.size(),
                  puppets.size() == 1 ? "y" : "ies",
                  world.galaxy().systems[world.currentSystemIndex()].name.c_str());
    return (lines.empty() ? std::string("(none)") : lines) + "\n" + buffer;
}

// Who is working the rock (Phase 8x stage 6). Prints both halves of the
// promotion because only their disagreement is a bug: the bodies that exist,
// and every extractor station here that the books say is digging — so a mine
// listed with no ship is either a defect or the player's own doing, and the
// hold says which.
std::string minerPuppets(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::vector<game::MinerPuppetInfo> miners;
    world.minerPuppetInfo(miners);
    std::string lines;
    char buffer[256];
    for (const game::MinerPuppetInfo& miner : miners) {
        std::snprintf(buffer,
                      sizeof(buffer),
                      "m%u %s (%s)  %.0f km out, %s",
                      miner.market,
                      miner.name.c_str(),
                      miner.station.c_str(),
                      miner.distance / 1000.0,
                      miner.working ? "working" : "no rock");
        if (miner.working) {
            const std::size_t used = std::strlen(buffer);
            std::snprintf(buffer + used,
                          sizeof(buffer) - used,
                          " a rock %.0f m off at %.0f m/s",
                          miner.rockDistance,
                          miner.speed);
        }
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    const sol::sim::Economy& economy = world.economy();
    std::size_t extractors = 0;
    for (std::uint32_t m = 0; m < economy.markets().size(); ++m) {
        const sol::sim::StationMarket& row = economy.markets()[m];
        if (row.systemIndex != world.currentSystemIndex() ||
            row.archetype >= economy.params().archetypes.size() ||
            !economy.params().archetypes[row.archetype].extracts) {
            continue;
        }
        ++extractors;
        std::snprintf(buffer,
                      sizeof(buffer),
                      "  m%u %s: output %.0f%%%s",
                      m,
                      world.galaxy().systems[row.systemIndex].stations[row.stationIndex].name.c_str(),
                      static_cast<double>(economy.satisfaction(m)) * 100.0,
                      world.minerHold(m) > 0.0 ? " [no miner]" : "");
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%zu miner(s) for %zu outpost(s) in %s",
                  miners.size(),
                  extractors,
                  world.galaxy().systems[world.currentSystemIndex()].name.c_str());
    return (lines.empty() ? std::string("(none)") : lines) + "\n" + buffer;
}

// Who is going for whom (Phase 8x §D). A hunt has two halves that can fail
// separately - picking prey, and actually closing on it - so the probe prints
// the state as well as the pair: "travel" is a raider still crossing the
// system, "attack" is one that has arrived.
std::string traderHunters(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::vector<game::HunterInfo> hunters;
    world.hunterInfo(hunters);
    std::string lines;
    std::size_t hunting = 0;
    char buffer[256];
    for (const game::HunterInfo& hunter : hunters) {
        hunting += hunter.hunting ? 1u : 0u;
        if (hunter.hunting) {
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%s [%s] -> #%u %s  %.0f km",
                          hunter.name.c_str(),
                          hunter.state,
                          hunter.traderIndex,
                          hunter.prey.c_str(),
                          hunter.distance / 1000.0);
        } else {
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%s [%s] -> %s",
                          hunter.name.c_str(),
                          hunter.state,
                          hunter.prey.empty() ? "nothing" : hunter.prey.c_str());
        }
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%zu fighter(s) here, %zu hunting a hauler in %s",
                  hunters.size(),
                  hunting,
                  world.galaxy().systems[world.currentSystemIndex()].name.c_str());
    return (lines.empty() ? std::string("(no fighters)") : lines) + "\n" + buffer;
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
        lines += def.id + " (" + def.role + ", " + std::to_string(static_cast<int>(def.price)) + " cr)";
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
    info += " | value " + std::to_string(static_cast<int>(world.shipValue(ship))) + " cr, deductible " +
            std::to_string(static_cast<int>(world.insuranceDeductible())) + " cr";
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
            lines += ship.storedSystem < systems.size() ? " (stored: " + systems[ship.storedSystem].name + ")"
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
    return content.world().warpToStationOffset(static_cast<std::uint32_t>(station), {dx, dy, dz});
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
        std::snprintf(
            buffer, sizeof(buffer), "%+.1f", static_cast<double>(world.factionSim().standing(faction)));
        lines += std::to_string(i + 1) + ": " + world.factions()[i].name + " " + buffer + " (" +
                 world.playerAttitudeName(faction) + ")";
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
            lines += world.factions()[a].name + " vs " + world.factions()[b].name + ": " + buffer +
                     (world.factionSim().atWar(a, b) ? " (WAR)" : "");
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
    world.factionSim().setStanding(static_cast<std::uint32_t>(faction), static_cast<float>(value));
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
                         : lines + "\n" + std::to_string(known) + " of " +
                               std::to_string(world.galaxy().systems.size()) + " systems";
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
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%u: %s, %.0f km",
                      signal.index + 1,
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
    std::snprintf(summary,
                  sizeof(summary),
                  "%zu site(s) in %s, %u found",
                  world.signals().size(),
                  world.currentSystemName(),
                  static_cast<std::uint32_t>(std::count_if(
                      world.signals().begin(), world.signals().end(), [&](const SignalInstance& s) {
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
        const char* kind = entry.kind == sol::sim::SurveyKind::System ? "system"
                           : entry.kind == sol::sim::SurveyKind::Body ? "body"
                           : entry.kind == sol::sim::SurveyKind::Site ? "site"
                                                                      : "completion";
        char buffer[128];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s: %s (%s%s) %.0f cr",
                      world.galaxy().systems[entry.system].name.c_str(),
                      kind,
                      regionWord(entry.region),
                      entry.firstDiscovery ? ", uncharted" : "",
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

// --- Situational awareness (Phase 8h) ----------------------------------------

// The ship cycle in the order C walks it: whoever is attacking the player
// first, then hostiles, then the rest, each group nearest-first. Printing the
// order is how the threat ranking gets verified without flying a fight.
std::string listContacts(GameContent& content)
{
    SpaceWorld& world = content.world();
    std::vector<std::size_t> order;
    world.contactOrder(order);
    const sol::core::DVec3 ship = world.shipState().position;
    std::string lines;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const TargetInfo contact = world.contactInfo(order[i]);
        char buffer[192];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%zu: %s [%s] %.0f km",
                      i + 1,
                      contact.nav.name.c_str(),
                      contact.attitude[0] != '\0' ? contact.attitude : "unaffiliated",
                      sol::core::length(contact.nav.position - ship) / 1000.0);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    char summary[96];
    std::snprintf(summary, sizeof(summary), "%zu contact(s) in %s", order.size(), world.currentSystemName());
    return lines.empty() ? std::string(summary) : lines + "\n" + summary;
}

// The tracked mission's current objective and whether it currently holds a nav
// slot (Phase 8i). Printing the slot is how the append-only bookkeeping gets
// verified without reading pixels: the defect this item fixes was precisely a
// position the game knew and never said.
std::string describeObjective(GameContent& content)
{
    SpaceWorld& world = content.world();
    const sol::sim::MissionObjective* objective = world.trackedObjective();
    if (objective == nullptr) {
        return "no tracked mission";
    }
    const char* kindName = objective->kind == sol::sim::ObjectiveKind::FlyTo     ? "fly to"
                           : objective->kind == sol::sim::ObjectiveKind::Dock    ? "dock"
                           : objective->kind == sol::sim::ObjectiveKind::Deliver ? "deliver"
                           : objective->kind == sol::sim::ObjectiveKind::Hold    ? "hold"
                           : objective->kind == sol::sim::ObjectiveKind::Escort  ? "escort"
                                                                                 : "kill";
    const std::string where = world.objectiveDestinationText();
    std::string line = std::string(kindName) + ": " + objective->text;
    if (!where.empty()) {
        line += " - " + where;
    }
    if (objective->kind == sol::sim::ObjectiveKind::Escort) {
        // The charge's own progress, read off the record rather than off the
        // body: it is the number that says whether the contract is nearly won,
        // and it keeps answering while the hauler is in the gate network.
        const sol::sim::TraderRoute route = world.economy().route(objective->trader);
        char haul[128];
        std::snprintf(haul,
                      sizeof(haul),
                      "\ntrader #%u: leg %s %.0f%%, %u hop(s)%s",
                      objective->trader,
                      route.leg == sol::sim::TraderLeg::None     ? "none"
                      : route.leg == sol::sim::TraderLeg::Depart ? "depart"
                      : route.leg == sol::sim::TraderLeg::Jump   ? "jump"
                                                                 : "arrive",
                      static_cast<double>(route.progress) * 100.0,
                      route.hops,
                      world.traderHasBody(objective->trader) ? ", body here" : "");
        line += haul;
    }
    const std::size_t slot = world.objectiveTargetIndex();
    if (slot == SpaceWorld::kNoTarget) {
        return line + "\nno nav slot (nothing of this objective is in this system)";
    }
    char buffer[160];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "nav slot %zu: %s, %.1f km away, radius %.1f km",
                  slot,
                  world.navTargets()[slot].name.c_str(),
                  sol::core::length(world.navTargets()[slot].position - world.shipState().position) / 1000.0,
                  objective->radius / 1000.0);
    return line + "\n" + buffer;
}

// The H key's path: select the nearest hostile, or say plainly that there is
// not one, which is the answer a key that silently does nothing withholds.
std::string targetNearestHostile(GameContent& content)
{
    SpaceWorld& world = content.world();
    if (!world.selectNearestHostile()) {
        return "nothing hostile in this system";
    }
    const TargetInfo hostile = world.currentTargetInfo();
    return "targeting " + hostile.nav.name + " [" +
           (hostile.attitude[0] != '\0' ? hostile.attitude : "unaffiliated") + "]";
}

// The left-click's path (Phase 8j), at virtual-screen coordinates. This is the
// same call the mouse makes, routing included, so a drive script can verify
// what a click does without landing the cursor on a three-pixel blip.
std::string pickAt(GameContent& content, double x, double y)
{
    SpaceWorld& world = content.world();
    const ViewFrame& view = world.viewFrame();
    if (!view.valid) {
        return "no view frame yet (not in flight?)";
    }
    const PickResult pick = pickTarget(world, {static_cast<float>(x), static_cast<float>(y)});
    if (!selectPicked(world, pick)) {
        char buffer[128];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "nothing at (%.0f, %.0f) of %.0fx%.0f",
                      x,
                      y,
                      static_cast<double>(view.screenSize.x),
                      static_cast<double>(view.screenSize.y));
        return buffer;
    }
    return std::string(pick.route == PickRoute::Radar ? "radar: " : "space: ") +
           world.currentTargetInfo().nav.name;
}

// What a click takes while the cursor is captured for mouse-look: whatever is
// at the boresight.
std::string pickBoresightCommand(GameContent& content)
{
    SpaceWorld& world = content.world();
    if (!world.viewFrame().valid) {
        return "no view frame yet (not in flight?)";
    }
    const PickResult pick = pickBoresight(world);
    if (!selectPicked(world, pick)) {
        return "nothing on the boresight";
    }
    return "boresight: " + world.currentTargetInfo().nav.name;
}

// Writes down where the ship is. An empty name takes the generated one, which
// is the same path B takes when the player accepts the suggestion.
std::string bookmarkHere(GameContent& content, std::string name)
{
    SpaceWorld& world = content.world();
    if (!world.addBookmarkHere(name)) {
        return "too many bookmarks in this system";
    }
    const std::vector<sol::sim::Bookmark>& all = world.survey().bookmarks();
    return "bookmarked '" + all.back().name + "'";
}

std::string listBookmarks(GameContent& content)
{
    SpaceWorld& world = content.world();
    const sol::core::DVec3 ship = world.shipState().position;
    std::string lines;
    for (const sol::sim::Bookmark& bookmark : world.survey().bookmarks()) {
        const bool here = bookmark.system == world.currentSystemIndex();
        char buffer[224];
        if (here) {
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%u: %s - %.0f km away",
                          bookmark.id,
                          bookmark.name.c_str(),
                          sol::core::length(bookmark.position - ship) / 1000.0);
        } else {
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%u: %s - in %s",
                          bookmark.id,
                          bookmark.name.c_str(),
                          world.galaxy().systems[bookmark.system].name.c_str());
        }
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    char summary[96];
    std::snprintf(summary,
                  sizeof(summary),
                  "%zu bookmark(s), %u in %s",
                  world.survey().bookmarks().size(),
                  world.survey().bookmarkCountIn(world.currentSystemIndex()),
                  world.currentSystemName());
    return lines.empty() ? std::string(summary) : lines + "\n" + summary;
}

// The system map of any system, as the screen would draw it (Phase 8q).
// Deliberately runs the real fill rather than a parallel read: the point is to
// assert what the *screen* shows at each rung of the knowledge ladder, and a
// second implementation here could agree with itself while disagreeing with
// the game.
std::string systemMap(GameContent& content, double index)
{
    const SpaceWorld& world = content.world();
    if (index < 0.0 || static_cast<std::size_t>(index) >= world.galaxy().systems.size()) {
        return "no such system";
    }
    // The screen falls back to the current system when asked for one the
    // player has never heard of, because such a system cannot be selected from
    // the list in the first place. A probe must not do that quietly: asked
    // about system 42, answering about system 3 would look like a fog-rule bug
    // in whichever direction the reader was already suspicious of.
    if (world.survey().knowledge(static_cast<std::uint32_t>(index)) == sol::sim::KnowledgeState::Unknown) {
        return "unknown system - nothing charted, the System tab cannot reach it";
    }
    std::deque<std::string> text;
    sol::ui::MapPanel panel;
    std::vector<sol::ui::MapSystemRow> systems;
    std::vector<sol::ui::MapLaneRow> lanes;
    std::vector<sol::ui::MapMarkerRow> markers;
    panel.viewSystem = static_cast<int>(index);
    fillMapPanel(world, text, panel, systems, lanes, markers);

    std::string lines = panel.viewSummary;
    for (const sol::ui::MapMarkerRow& marker : panel.markers) {
        char buffer[224];
        std::snprintf(
            buffer, sizeof(buffer), "%-9s %s - %s", markerKindName(marker.kind), marker.name, marker.detail);
        lines += "\n" + std::string(buffer);
    }
    char summary[64];
    std::snprintf(summary, sizeof(summary), "%zu marker(s)", panel.markers.size());
    return lines + "\n" + summary;
}

// The galaxy row and the system readout for one system, exactly as the map
// screen composes them (Phase 30 stage D). Same argument sol.system_map makes
// and the same construction: run the REAL fill, because what is being asserted
// is what the SCREEN says at each rung of the knowledge ladder, and a second
// read here could agree with itself while disagreeing with the game.
//
// ⚑ It prints the row's own `hasSecurity` and `securityAnswers` beside the
// composed text, because the whole of this stage is a knowledge rule and a
// band, and neither is visible in a sentence that has already been assembled
// out of them.
std::string mapRow(GameContent& content, double index)
{
    const SpaceWorld& world = content.world();
    if (index < 0.0 || static_cast<std::size_t>(index) >= world.galaxy().systems.size()) {
        return "no such system";
    }
    const auto system = static_cast<std::uint32_t>(index);
    if (world.survey().knowledge(system) == sol::sim::KnowledgeState::Unknown) {
        return "unknown system - the map draws no row for it at all";
    }
    std::deque<std::string> text;
    sol::ui::MapPanel panel;
    std::vector<sol::ui::MapSystemRow> systems;
    std::vector<sol::ui::MapLaneRow> lanes;
    std::vector<sol::ui::MapMarkerRow> markers;
    panel.viewSystem = static_cast<int>(index);
    panel.securityOverlay = true; // so the overlay legend is composed too
    fillMapPanel(world, text, panel, systems, lanes, markers);

    const sol::ui::MapSystemRow& row = panel.systems[system];
    char rating[128];
    if (row.hasSecurity) {
        std::snprintf(rating,
                      sizeof(rating),
                      "rating: shown %+.3f, a call here is answered: %s",
                      static_cast<double>(row.security),
                      row.securityAnswers ? "yes" : "NO");
    } else {
        std::snprintf(
            rating, sizeof(rating), "rating: HIDDEN - the row declines, this is not a visited system");
    }
    std::string lines = row.detail;
    lines += "\nreadout: ";
    lines += panel.viewSecurity;
    lines += "\n";
    lines += rating;
    lines += "\nlegend: ";
    lines += panel.securitySummary;
    return lines;
}

std::string deleteBookmark(GameContent& content, double id)
{
    return content.world().removeBookmark(static_cast<std::uint32_t>(id)) ? "deleted" : "no such bookmark";
}

// Dev teleport to a bookmark, the shape sol.warp_rock established in 8f.
std::string warpBookmark(GameContent& content, double id)
{
    SpaceWorld& world = content.world();
    const sol::sim::Bookmark* bookmark = world.survey().bookmark(static_cast<std::uint32_t>(id));
    if (bookmark == nullptr) {
        return "no such bookmark";
    }
    if (bookmark->system != world.currentSystemIndex()) {
        return "that bookmark is in another system";
    }
    (void)world.selectBookmark(bookmark->id);
    if (!world.warpTo(bookmark->position, 2000.0)) {
        return "cannot warp while docked";
    }
    return "warped to '" + bookmark->name + "'";
}

// The ship readout as text: how the screen gets verified without reading
// pixels, and the fastest way to see what a refit actually changed.
std::string shipInfo(GameContent& content)
{
    return shipInfoReport(content.world(), content.defs());
}

// --- Frame profiler (Phase 8n) -----------------------------------------------
//
// The overlay draws the same tree, but a screenshot is not an assertion: a
// drive has to be able to state a budget and have the run fail when it is
// missed. These read the same Profiler the overlay does.

// ⚑ Stage F's own probe, and the stage needs one more than most: the whole
// model catalog is 2,298 triangles and the frame is vsync-bound, so LOD
// selection can never be shown to work by a frame rate. What can be asserted is
// which level each instance chose, which is what this prints.
//
// `loaded` separates the two failures that look identical from outside: zero
// levels loaded is a COOK problem, levels loaded but nothing ever drawn below
// level 0 is a SELECTION problem.
std::string lodReportCommand(GameContent& content)
{
    (void)content;
    const LodReport& report = lodReport();
    char line[256];
    std::snprintf(line,
                  sizeof(line),
                  "levels loaded %u over %u model(s); drawn lod0 %u, lod1 %u, lod2 %u; biggest "
                  "with a chain %.1f px -> lod%u (switches at %.0f / %.0f, viewport %.0f px)",
                  report.levelsLoaded,
                  report.modelsWithLevels,
                  report.drawn[0],
                  report.drawn[1],
                  report.drawn[2],
                  static_cast<double>(report.largestChainedRadius),
                  report.largestChainedLevel,
                  static_cast<double>(sol::assets::kLevelSwitchPixels[0]),
                  static_cast<double>(sol::assets::kLevelSwitchPixels[1]),
                  static_cast<double>(report.viewportHeight));
    return line;
}

// ⚑ The instrument for Phase 19, and permanent for the same reason
// `sol.lod_pin` is: the whole claim of the phase is that a slot's answer lives
// in a file, and this is what reads the answer back out of a running game
// without a screenshot. `sol.roles()` after editing `models.toml` says whether
// the edit took, and the model INDEX is included because that is what the
// renderer and the sim actually key on.
std::string rolesReport(GameContent& content)
{
    const sol::assets::DefDatabase& defs = content.defs();
    std::string out;
    for (const sol::assets::RoleDef& role : defs.roles()) {
        const std::uint32_t index = defs.modelIndex(role.model.c_str());
        char line[160];
        std::snprintf(line,
                      sizeof(line),
                      "%s%s -> %s (model %u)",
                      out.empty() ? "" : ", ",
                      role.id.c_str(),
                      role.model.c_str(),
                      index);
        out += line;
    }
    return out.empty() ? "no [[role]] rows loaded" : out;
}

// ⚑ The A/B lever for the level policy (Phase 17). `sol.lods` says which level
// was drawn; this says which level to draw, so level 0 and level 1 can be
// compared at ONE camera position instead of at two distances - which is the
// only way to see the switch itself rather than the approach around it.
std::string lodPinCommand(GameContent& content, double level)
{
    (void)content;
    const std::int32_t requested = static_cast<std::int32_t>(level);
    lodPin() = requested < 0 ? kLodPinAutomatic : requested;
    if (lodPin() == kLodPinAutomatic) {
        return "lod pin off - levels are chosen by projected size again";
    }
    char line[128];
    std::snprintf(line,
                  sizeof(line),
                  "lod pinned to level %d - every chained model draws it, clamped to the levels "
                  "it has",
                  static_cast<int>(lodPin()));
    return line;
}

std::string perfReport(GameContent& content)
{
    (void)content;
    const sol::core::Profiler& profiler = sol::core::frameProfiler();
    if (!profiler.enabled()) {
        return "profiler disabled";
    }
    if (profiler.zoneCount() == 0) {
        return "no zones recorded yet";
    }
    std::string lines = "zone                     last    mean     max";
    bool anyExternal = false;
    for (std::uint32_t i = 0; i < profiler.zoneCount(); ++i) {
        const sol::core::ZoneReport zone = profiler.report(i);
        char row[192];
        std::snprintf(row,
                      sizeof(row),
                      "\n%*s%-*s %6.2f  %6.2f  %6.2f",
                      static_cast<int>(zone.depth) * 2,
                      "",
                      24 - static_cast<int>(zone.depth) * 2,
                      zone.name,
                      zone.lastMilliseconds,
                      zone.meanMilliseconds,
                      zone.maxMilliseconds);
        lines += row;
        if (zone.counter > 0) {
            char tail[48];
            std::snprintf(tail, sizeof(tail), "  n=%llu", static_cast<unsigned long long>(zone.counter));
            lines += tail;
        }
        if (zone.external) {
            lines += " *";
            anyExternal = true;
        }
    }
    // Phase 8o. Without this a reader compares a gpu.* row's `last` against
    // the cpu rows beside it and concludes the GPU disagrees with the CPU,
    // when in fact it is answering about an older frame. Mean and max are the
    // same samples shifted, so they compare fine - which is exactly why the
    // note names `last` and not the whole row.
    if (anyExternal) {
        lines += "\n* device-timed; `last` is a completed frame, not this one";
    }
    return lines;
}

// Mean milliseconds over the history window — the number a budget is stated
// against, because last is one frame's luck and max is one frame's worst.
// -1 for a zone that does not exist, so a typo in a drive fails loudly rather
// than passing as a fast zero.
double perfZone(GameContent& content, const std::string& name)
{
    (void)content;
    const sol::core::Profiler& profiler = sol::core::frameProfiler();
    const std::uint32_t index = profiler.findZone(name.c_str());
    return index == sol::core::kInvalidZone ? -1.0 : profiler.report(index).meanMilliseconds;
}

double perfZoneMax(GameContent& content, const std::string& name)
{
    (void)content;
    const sol::core::Profiler& profiler = sol::core::frameProfiler();
    const std::uint32_t index = profiler.findZone(name.c_str());
    return index == sol::core::kInvalidZone ? -1.0 : profiler.report(index).maxMilliseconds;
}

// The zone's input size, which is what turns "this pass is slow" into "this
// pass is slow because it was handed 15,300 pairs".
double perfZoneCount(GameContent& content, const std::string& name)
{
    (void)content;
    const sol::core::Profiler& profiler = sol::core::frameProfiler();
    const std::uint32_t index = profiler.findZone(name.c_str());
    return index == sol::core::kInvalidZone ? -1.0 : static_cast<double>(profiler.report(index).counter);
}

// Clears the history so a measurement can start from a known state rather
// than averaging in the load screen that preceded it.
std::string perfReset(GameContent& content)
{
    (void)content;
    sol::core::frameProfiler().reset();
    return "profiler reset";
}

// --- Controls (Phase 8k) -----------------------------------------------------
//
// The Controls screen is a scrolling list of 34 rows behind two menus, so
// driving a rebind by mouse is slow and brittle. These do the same thing the
// screen does, through the same table, which is what makes the exit criteria
// checkable in a script.

std::string listBindings(GameContent& content)
{
    const sol::platform::BindingTable* bindings = content.bindings();
    if (bindings == nullptr) {
        return "bindings unavailable";
    }
    std::string lines;
    for (std::uint32_t i = 0; i < kActionCount; ++i) {
        const Action action = static_cast<Action>(i);
        const sol::platform::InputChord chord = bindings->chordFor(i);
        lines += (lines.empty() ? "" : "\n") + std::string(actionId(action)) + " = " +
                 (chord.bound() ? sol::platform::chordName(chord) : "(unbound)");
    }
    return lines;
}

// sol.bind("jump", "K") - assigns, stealing exactly as the screen does, and
// says what it took so a script can assert on the conflict policy.
std::string bindAction(GameContent& content, const char* actionName, const char* chordName)
{
    sol::platform::BindingTable* bindings = content.bindings();
    if (bindings == nullptr) {
        return "bindings unavailable";
    }
    bool known = false;
    const Action action = actionFromId(actionName, known);
    if (!known) {
        return std::string("no such action '") + (actionName == nullptr ? "" : actionName) + "'";
    }
    const std::string name = chordName == nullptr ? "" : chordName;
    const sol::platform::InputChord chord = sol::platform::chordFromName(name);
    if (!chord.bound() && !name.empty()) {
        return "no such key or button '" + name + "'";
    }
    if (isReservedChord(chord)) {
        return name + " is reserved by the menus and cannot be bound";
    }
    const std::uint32_t stolen = bindings->assign(static_cast<std::uint32_t>(action), chord);
    std::string result = std::string(actionLabel(action)) + " = " +
                         (chord.bound() ? sol::platform::chordName(chord) : "(unbound)");
    if (stolen != sol::platform::BindingTable::kNoAction) {
        result += ", taken from " + std::string(actionLabel(static_cast<Action>(stolen))) + " (now unbound)";
    }
    return result;
}

std::string resetBindings(GameContent& content)
{
    sol::platform::BindingTable* bindings = content.bindings();
    if (bindings == nullptr) {
        return "bindings unavailable";
    }
    installDefaultBindings(*bindings);
    return "controls reset to defaults";
}

// --- World generation (Phase 13) ---------------------------------------------

// Whether the galaxy adds up, measured over every system at once.
//
// ⚑ This exists for the same reason 8z's sol.system_map does: the claims this
// phase makes are galaxy-wide — "no outpost sits over an empty system", "a
// faction builds what it is" — and both span ~80 systems that no drive can fly.
// A screenshot cannot assert either one. Prints the coherence count first
// (which must read 0) and then the per-faction archetype mix.
std::string worldgenReport(GameContent& content)
{
    SpaceWorld& world = content.world();
    const sol::sim::Galaxy& galaxy = world.galaxy();
    const sol::sim::GalaxyParams& params = world.galaxyParams();
    const sol::sim::MiningParams& mining = world.mining().params();
    const std::vector<GameFaction>& factions = world.factions();
    const std::size_t archetypes = params.stationRules.size();
    if (archetypes == 0) {
        return "no station rules";
    }

    std::uint32_t rocklessSystems = 0;
    std::uint32_t extractorsWithoutRock = 0;
    std::uint32_t stations = 0;
    // [faction][archetype], with the lawless bucket last.
    const std::size_t buckets = factions.size() + 1;
    std::vector<std::uint32_t> mix(buckets * archetypes, 0);
    std::vector<std::uint32_t> perFaction(buckets, 0);

    for (std::uint32_t i = 0; i < galaxy.systems.size(); ++i) {
        const sol::sim::SystemSpec& spec = galaxy.systems[i];
        const bool rockless = sol::sim::fieldCountFor(spec, mining) == 0;
        rocklessSystems += rockless ? 1 : 0;
        // The owner, not the founding claim: a system that changed hands since
        // 8u is held by whoever holds it, and that is who the mix is about.
        const std::uint32_t owner = world.systemOwnerFaction(i);
        const std::size_t bucket = owner < factions.size() ? owner : factions.size();
        for (const sol::sim::StationSpec& station : spec.stations) {
            if (station.archetype >= archetypes) {
                continue;
            }
            ++stations;
            ++perFaction[bucket];
            ++mix[bucket * archetypes + station.archetype];
            if (rockless && params.stationRules[station.archetype].requiresField) {
                ++extractorsWithoutRock;
            }
        }
    }

    // ⚑ The counterfactual, generated here rather than by checking out dev and
    // rebuilding: the same params with the rock rule OFF is exactly what this
    // galaxy looked like before Phase 13, and generation is deterministic and
    // cheap, so the A/B costs a few milliseconds and needs no second binary.
    const sol::sim::Galaxy unguarded = sol::sim::generateGalaxy(params);
    std::uint32_t extractorsBefore = 0;
    std::uint32_t stationsBefore = 0;
    for (std::uint32_t i = 0; i < unguarded.systems.size(); ++i) {
        const bool rockless = sol::sim::fieldCountFor(unguarded.systems[i], mining) == 0;
        for (const sol::sim::StationSpec& station : unguarded.systems[i].stations) {
            ++stationsBefore;
            if (rockless && station.archetype < archetypes &&
                params.stationRules[station.archetype].requiresField) {
                ++extractorsBefore;
            }
        }
    }

    char buffer[256];
    std::string out;
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%zu systems, %u with no rock; %u stations; %u extractor(s) sited without rock"
                  " (rule off: %u of %u)",
                  galaxy.systems.size(),
                  rocklessSystems,
                  stations,
                  extractorsWithoutRock,
                  extractorsBefore,
                  stationsBefore);
    out += buffer;
    for (std::size_t b = 0; b < buckets; ++b) {
        if (perFaction[b] == 0) {
            continue;
        }
        std::snprintf(buffer,
                      sizeof(buffer),
                      "\n%-22s %3u:",
                      b < factions.size() ? factions[b].name.c_str() : "(lawless)",
                      perFaction[b]);
        out += buffer;
        for (std::size_t a = 0; a < archetypes; ++a) {
            std::snprintf(buffer, sizeof(buffer), " %.0f%%", 100.0 * mix[b * archetypes + a] / perFaction[b]);
            out += buffer;
        }
    }
    return out;
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
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%zu: %u rocks, r %.0f km, %.0f km away",
                      i + 1,
                      fields[i].rockCount,
                      fields[i].radius / 1000.0,
                      sol::core::length(fields[i].center - ship) / 1000.0);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    char summary[96];
    std::snprintf(summary, sizeof(summary), "%zu field(s) in %s", fields.size(), world.currentSystemName());
    return lines.empty() ? std::string(summary) : lines + "\n" + summary;
}

// Rocks of one field (1-based, as the field listing prints them), with what
// each still holds — the readout that proves depletion survived a jump.
std::string listRocks(GameContent& content, double fieldNumber)
{
    SpaceWorld& world = content.world();
    const std::uint32_t field = fieldNumber >= 1.0 ? static_cast<std::uint32_t>(fieldNumber) - 1 : 0;
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
        const float remaining = world.mining().unitsLeft(
            world.currentSystemIndex(), field, static_cast<std::uint32_t>(i), rocks[i].yieldUnits);
        left += remaining;
        total += rocks[i].yieldUnits;
        if (remaining <= 0.0f) {
            continue; // spent rocks are gone; listing them is noise
        }
        char buffer[160];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%zu: %s %.1f/%.1f, r %.0f m, %.0f km",
            i + 1,
            world.commodityIds()[rocks[i].commodity < world.commodityIds().size() ? rocks[i].commodity : 0]
                .c_str(),
            static_cast<double>(remaining),
            static_cast<double>(rocks[i].yieldUnits),
            rocks[i].radius,
            sol::core::length(rocks[i].position - ship) / 1000.0);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    char summary[96];
    std::snprintf(summary,
                  sizeof(summary),
                  "field %u: %.0f of %.0f units left",
                  field + 1,
                  static_cast<double>(left),
                  static_cast<double>(total));
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
        std::snprintf(buffer,
                      sizeof(buffer),
                      "#%u %s in %s: %.0f units, %.0f cr%s, %.0f s left",
                      wreck.id,
                      wreck.name.c_str(),
                      wreck.system < world.galaxy().systems.size()
                          ? world.galaxy().systems[wreck.system].name.c_str()
                          : "?",
                      static_cast<double>(cargo),
                      wreck.contents.credits,
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

// Pilot comms probes (Phase 8s), so a drive asserts the conversation rather
// than photographing it — the rule 8q and 8r both ended up writing down.
bool hailSelected(GameContent& content)
{
    return content.world().hailTarget();
}

// Select a ship contact by name, then hail it, so a drive does not have to
// walk the contact cycle blind. Contacts live past the nav targets in one
// index space, which is what selectTarget takes.
bool hailByName(GameContent& content, const char* namePart)
{
    SpaceWorld& world = content.world();
    const std::string_view want(namePart != nullptr ? namePart : "");
    if (want.empty()) {
        return false;
    }
    const std::size_t navCount = world.navTargets().size();
    for (std::size_t slot = 0; slot < world.contactCount(); ++slot) {
        if (world.contactInfo(slot).nav.name.find(want) == std::string::npos) {
            continue;
        }
        if (!world.selectTarget(navCount + slot)) {
            return false;
        }
        return world.hailTarget();
    }
    return false;
}

// What the player has been told: rumoured places and remembered prices, with
// their ages. Both stores existed before this item — the tip writes what B and
// sol.buy_intel already write — so this reads them back rather than a third.
std::string listTips(GameContent& content)
{
    SpaceWorld& world = content.world();
    const sol::sim::SurveySim& survey = world.survey();
    std::string lines;
    std::uint32_t rumours = 0;
    for (const sol::sim::Bookmark& bookmark : survey.bookmarks()) {
        if (bookmark.label != sol::sim::kTipLabel) {
            continue;
        }
        ++rumours;
        char buffer[224];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "place %u: %s in %s",
                      bookmark.id,
                      bookmark.name.c_str(),
                      world.galaxy().systems[bookmark.system].name.c_str());
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    for (const sol::sim::MarketMemory& memory : survey.marketMemory()) {
        const sol::sim::StationMarket& record = world.economy().markets()[memory.market];
        const sol::sim::SystemSpec& spec = world.galaxy().systems[record.systemIndex];
        char buffer[224];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "price %u: %s, %s - %.0f s old%s",
                      memory.market,
                      record.stationIndex < spec.stations.size()
                          ? spec.stations[record.stationIndex].name.c_str()
                          : "?",
                      spec.name.c_str(),
                      world.worldSeconds() - memory.takenAt,
                      survey.isStale(memory, world.worldSeconds()) ? " (stale)" : "");
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    char summary[128];
    std::snprintf(summary,
                  sizeof(summary),
                  "%u rumour(s), %zu market(s) remembered, %zu pilot(s) hailed here",
                  rumours,
                  survey.marketMemory().size(),
                  world.hailCount());
    return lines.empty() ? std::string(summary) : lines + "\n" + summary;
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
        std::snprintf(buffer,
                      sizeof(buffer),
                      "market %u: %.0f %s -> %.0f %s, %s",
                      job.market,
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
        const sol::sim::EconomyArchetype* archetype = market.archetype < economy.params().archetypes.size()
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
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%-14s %5.1f%% full  prod %6.2f  use %6.2f  (%u empty, %u full, "
                      "%u starved on it)",
                      world.commodityIds()[c].c_str(),
                      fill,
                      production[c],
                      consumption[c],
                      empty[c],
                      full[c],
                      starved[c]);
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%zu markets, %u throttled by feedstock, %.0f s elapsed",
                  economy.markets().size(),
                  throttled,
                  world.worldSeconds());
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
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s: running at %.0f%% of nominal%s%s",
                  world.dockedStationName(),
                  static_cast<double>(world.marketSatisfaction(market)) * 100.0,
                  limiting[0] == '\0' ? "" : ", short of ",
                  limiting);
    return buffer;
}

// What is left in the ground in a system, which is what a mining outpost
// there is actually living on.
std::string fieldStock(GameContent& content, int systemIndex)
{
    SpaceWorld& world = content.world();
    const auto system =
        systemIndex < 0 ? world.currentSystemIndex() : static_cast<std::uint32_t>(systemIndex);
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
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s: %.0f units in the ground",
                      world.commodityIds()[c].c_str(),
                      static_cast<double>(units));
        lines += (lines.empty() ? "" : "\n") + std::string(buffer);
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s: %u field(s), %zu rock(s) being worked",
                  world.galaxy().systems[system].name.c_str(),
                  world.mining().fieldCount(system),
                  world.mining().depletionRecordCount());
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
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%s%s %.2f",
                          prices.empty() ? "" : ", ",
                          world.commodityIds()[c].c_str(),
                          static_cast<double>(memory.prices[c]));
            prices += buffer;
        }
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s (market %u), %.0f s ago: ",
                      world.galaxy().systems[system].name.c_str(),
                      memory.market,
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
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s: %.2f at %s, %.0f s ago%s",
                      commodityId,
                      static_cast<double>(price),
                      world.galaxy().systems[system].name.c_str(),
                      age,
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

// Every station and gate in this system with what the player knows about it
// (Phase 8z). Deliberately lists ALL of them, found or not — a probe that
// showed only what the player can see could not tell "correctly hidden" from
// "missing", which is the distinction the whole phase is made of. That is
// stage 4's sol.hunters() rule: a probe that listed only hunts reported
// "0 hunts" while a raider crossed the system in plain sight.
//
// Prints the distance beside the state so a drive can say *why* something is
// still hidden — out of pulse range is not the same fact as unswept.
std::string listStructures(GameContent& content)
{
    SpaceWorld& world = content.world();
    const std::uint32_t system = world.currentSystemIndex();
    const sol::sim::SystemSpec& spec = world.galaxy().systems[system];
    const sol::core::DVec3 position = world.shipState().position;
    const sol::sim::SurveySim& survey = world.survey();
    std::string lines;
    const auto state = [](bool discovered, bool identified) {
        return identified ? "identified" : discovered ? "contact" : "hidden";
    };
    std::uint32_t known = 0;
    for (std::uint32_t i = 0; i < spec.stations.size(); ++i) {
        char buffer[160];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "station %u '%s': %s, %.0f km",
                      i,
                      spec.stations[i].name.c_str(),
                      state(survey.stationDiscovered(system, i), survey.stationIdentified(system, i)),
                      length(spec.stations[i].position - position) / 1000.0);
        lines += lines.empty() ? "" : "\n";
        lines += buffer;
        known += survey.stationDiscovered(system, i) ? 1u : 0u;
    }
    for (std::uint32_t i = 0; i < spec.gates.size(); ++i) {
        char buffer[160];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "gate %u -> %s: %s, %.0f km",
                      i,
                      world.galaxy().systems[spec.gates[i].toSystem].name.c_str(),
                      state(survey.gateDiscovered(system, i), survey.gateIdentified(system, i)),
                      length(spec.gates[i].position - position) / 1000.0);
        lines += lines.empty() ? "" : "\n";
        lines += buffer;
        known += survey.gateDiscovered(system, i) ? 1u : 0u;
    }
    char summary[128];
    std::snprintf(summary,
                  sizeof(summary),
                  "\n%zu structure(s) in %s, %u found; pulse %.0f km",
                  spec.stations.size() + spec.gates.size(),
                  world.currentSystemName(),
                  known,
                  static_cast<double>(world.scanRange()) / 1000.0);
    lines += summary;
    return lines;
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
bool setSignalLoot(GameContent& content, const char* cargoSpec, double credits, const char* moduleId)
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
            SOL_LOG_WARN("set_loot: bad cargo entry '%.*s'", static_cast<int>(entry.size()), entry.data());
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
    world.factionSim().raidCandidates(world.galaxy(), static_cast<std::uint32_t>(faction), candidates);
    std::string joined;
    for (const sol::sim::RaidCandidate& candidate : candidates) {
        if (!joined.empty()) {
            joined += ";";
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(candidate.relation));
        joined +=
            std::to_string(candidate.system) + ":" + world.galaxy().systems[candidate.system].name + ":" +
            buffer + ":" +
            (candidate.owner < world.factions().size() && world.factions()[candidate.owner].pirate ? "p"
                                                                                                   : "m");
    }
    return joined;
}

// --- Territory (Phase 8u) ---
// Every system whose owner has moved off its founding claim, plus every live
// contest. Faction indices printed 1-based, matching sol.factions.
std::string listTerritory(GameContent& content)
{
    SpaceWorld& world = content.world();
    const sol::sim::FactionSim& factions = world.factionSim();
    const auto name = [&](std::uint32_t faction) -> std::string {
        return faction < world.factions().size()
                   ? world.factions()[faction].name + " (" + std::to_string(faction + 1) + ")"
                   : std::string("nobody");
    };
    std::string lines;
    for (std::uint32_t s = 0; s < world.galaxy().systems.size(); ++s) {
        const std::uint32_t owner = factions.systemOwner(s);
        const std::uint32_t claim = factions.foundingClaim(s);
        const sol::sim::SystemContest contest = factions.contestOf(s);
        if (owner == claim && !contest.live()) {
            continue;
        }
        if (!lines.empty()) {
            lines += "\n";
        }
        lines += std::to_string(s) + " " + world.galaxy().systems[s].name + ": " + name(owner);
        if (owner != claim) {
            lines += " (taken from " + name(claim) + ")";
        }
        if (contest.live()) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(contest.pressure));
            lines += " - contested by " + name(contest.attacker) + " at " + buffer +
                     (factions.contested(s) ? "" : " (below threshold)");
        }
    }
    return lines.empty() ? "(every system is held by its founding claim)" : lines;
}

// One system in full, including the facts that make it un-contestable.
std::string contestReport(GameContent& content, double systemIndex)
{
    SpaceWorld& world = content.world();
    const auto system = static_cast<std::uint32_t>(systemIndex);
    if (system >= world.galaxy().systems.size()) {
        return "no such system";
    }
    const sol::sim::FactionSim& factions = world.factionSim();
    const auto name = [&](std::uint32_t faction) -> std::string {
        return faction < world.factions().size()
                   ? world.factions()[faction].name + " (" + std::to_string(faction + 1) + ")"
                   : std::string("nobody");
    };
    const std::uint32_t owner = factions.systemOwner(system);
    const sol::sim::SystemContest contest = factions.contestOf(system);
    std::string line = world.galaxy().systems[system].name + ": held by " + name(owner) +
                       ", founding claim " + name(factions.foundingClaim(system));
    if (owner < world.factions().size() && factions.homeSystem(owner) == system) {
        line += " [HOME: cannot be contested]";
    }
    if (!contest.live()) {
        return line + "; no contest";
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(contest.pressure));
    return line + "; contested by " + name(contest.attacker) + " at pressure " + buffer +
           (factions.contested(system) ? " (live)" : " (below threshold)");
}

// Dev lever: the exit criteria are stated against this, because a contest
// driven by real raids takes many 60 s decision cycles to reach a flip.
bool setContest(GameContent& content, double systemIndex, double factionIndex, double pressure)
{
    SpaceWorld& world = content.world();
    const auto system = static_cast<std::uint32_t>(systemIndex);
    if (system >= world.galaxy().systems.size()) {
        return false;
    }
    const auto oneBased = static_cast<std::int64_t>(factionIndex);
    const std::uint32_t attacker =
        oneBased >= 1 && static_cast<std::size_t>(oneBased) <= world.factions().size()
            ? static_cast<std::uint32_t>(oneBased - 1)
            : sol::sim::kNoFaction;
    world.factionSim().setContest(system, attacker, static_cast<float>(pressure));
    return world.factionSim().contestOf(system).attacker == attacker;
}

// Dev lever: hand a system over outright, ending any contest.
bool flipSystem(GameContent& content, double systemIndex, double factionIndex)
{
    SpaceWorld& world = content.world();
    const std::size_t faction = static_cast<std::size_t>(factionIndex) - 1;
    if (faction >= world.factions().size()) {
        return false;
    }
    return world.factionSim().flipSystem(static_cast<std::uint32_t>(systemIndex),
                                         static_cast<std::uint32_t>(faction));
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
    const sol::sim::MissionObjective& objective = mission.objectives[mission.currentObjective];
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
                 (offer.poster < world.factions().size() ? world.factions()[offer.poster].name
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
                 std::to_string(mission.objectives.size()) + "] " + missionObjectiveLine(mission);
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
    content.world().missionSim().setCampaignStage(stage >= 0.0 ? static_cast<std::uint32_t>(stage) : 0u);
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
    world.missionSim().haulCandidates(
        world.galaxy(), world.economy(), world.currentSystemIndex(), world.dockedStationIndex(), hauls);
    for (const sol::sim::HaulCandidate& c : hauls) {
        if (!lines.empty()) {
            lines += "\n";
        }
        char buffer[160];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "haul: %s @ %s needs %.0f %s (%.0f%%, %u jumps)",
                      world.galaxy().systems[c.system].stations[c.station].name.c_str(),
                      world.galaxy().systems[c.system].name.c_str(),
                      static_cast<double>(c.units),
                      world.commodityIds()[c.commodity].c_str(),
                      static_cast<double>(c.severity) * 100.0,
                      c.jumps);
        lines += buffer;
    }
    std::vector<sol::sim::BountyCandidate> bounties;
    world.missionSim().bountyCandidates(
        world.galaxy(), world.factionSim(), world.currentSystemIndex(), bounties);
    for (const sol::sim::BountyCandidate& c : bounties) {
        if (!lines.empty()) {
            lines += "\n";
        }
        char buffer[160];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "bounty: %s raided by %s (%.2f, %u jumps)",
                      world.galaxy().systems[c.system].name.c_str(),
                      c.clan < world.factions().size() ? world.factions()[c.clan].name.c_str() : "?",
                      static_cast<double>(c.intensity),
                      c.jumps);
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
    return system < systems.size() ? static_cast<double>(systems[system].stations.size()) : 0.0;
}

double currentSystemIndex(GameContent& content)
{
    return content.world().currentSystemIndex();
}

// The campaign's only handle on a place somebody PUT somewhere (Phase 29 stage
// D, decisions/018 decision 5). Returns the galaxy index, or -1.
//
// ⚑⚑ IT LOOKS THE ID UP ON THE GALAXY RATHER THAN ON A DEF, AND THAT IS ONE
// LOOKUP COVERING BOTH KINDS FOR FREE. `applyAuthoredFields` stamps
// `SystemSpec::authoredId` for a `[[system]]` and for a
// `[[constellation.system]]` member alike, so a member is addressable here
// with no code that knows constellations exist - and the answer is about the
// galaxy that was actually generated rather than about what a file asked for,
// which is the difference between "this place exists" and "somebody wrote a
// row". A def whose placement was refused never reaches this.
//
// ⚑ Every other system lookup in the 158 bindings goes by NAME, and none of
// them could be used for this: procedural names are drawn after placement, so
// they are a fact about one seed at one system count. An id is not.
double systemIndexById(GameContent& content, const std::string& id)
{
    const std::vector<sol::sim::SystemSpec>& systems = content.world().galaxy().systems;
    for (std::size_t i = 0; i < systems.size(); ++i) {
        if (systems[i].authoredId == id) {
            return static_cast<double>(i);
        }
    }
    return -1.0;
}

// 0-based station index while docked (mission dock objectives), or -1.
double dockedStationIndex(GameContent& content)
{
    const std::uint32_t station = content.world().dockedStationIndex();
    return station == 0xffff'ffffu ? -1.0 : static_cast<double>(station);
}

// --- The mission builder (Lua board hook assembles a draft, then posts) ---

bool missionBegin(GameContent& content,
                  const std::string& title,
                  double posterIndex,
                  double rewardCredits,
                  double repReward,
                  double repPenalty,
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

bool missionObjDock(GameContent& content, double system, double station, const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    content.missionDraft().objectives.push_back({.kind = sol::sim::ObjectiveKind::Dock,
                                                 .system = static_cast<std::uint32_t>(system),
                                                 .station = static_cast<std::uint32_t>(station),
                                                 .text = text});
    return true;
}

bool missionObjDeliver(GameContent& content,
                       double system,
                       double station,
                       const std::string& commodityId,
                       double units,
                       const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    const std::uint32_t commodity = content.world().commodityIndex(commodityId.c_str());
    if (commodity >= content.world().commodityIds().size()) {
        SOL_LOG_WARN("mission_obj_deliver: unknown commodity '%s'", commodityId.c_str());
        return false;
    }
    content.missionDraft().objectives.push_back({.kind = sol::sim::ObjectiveKind::Deliver,
                                                 .system = static_cast<std::uint32_t>(system),
                                                 .station = static_cast<std::uint32_t>(station),
                                                 .commodity = commodity,
                                                 .units = static_cast<float>(units),
                                                 .text = text});
    return true;
}

// count ships of a faction (1-based); system < 0 means anywhere.
bool missionObjKill(
    GameContent& content, double factionIndex, double count, double system, const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    const std::size_t faction = static_cast<std::size_t>(factionIndex) - 1;
    if (faction >= content.world().factions().size()) {
        SOL_LOG_WARN("mission_obj_kill: faction %d out of range", static_cast<int>(factionIndex));
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
// Hold (Phase 8u): `faction` still holds `system` when its contest resolves.
// One builder covers both directions - name the owner for a defence, the
// attacker for an assault contract.
bool missionObjHold(GameContent& content, double system, double factionIndex, const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    const std::size_t faction = static_cast<std::size_t>(factionIndex) - 1;
    if (faction >= content.world().factions().size()) {
        SOL_LOG_WARN("mission_obj_hold: faction %d out of range", static_cast<int>(factionIndex));
        return false;
    }
    if (system < 0.0 || static_cast<std::size_t>(system) >= content.world().galaxy().systems.size()) {
        SOL_LOG_WARN("mission_obj_hold: system %d out of range", static_cast<int>(system));
        return false;
    }
    content.missionDraft().objectives.push_back({.kind = sol::sim::ObjectiveKind::Hold,
                                                 .system = static_cast<std::uint32_t>(system),
                                                 .faction = static_cast<std::uint32_t>(faction),
                                                 .text = text});
    return true;
}

// Escort (Phase 8x §E): coarse trader `trader` reaches `system` alive. The
// trader index is 0-based, unlike the 1-based faction indices above, because
// it is not a def table Lua ever names - it is the same number sol.traders()
// and the escort candidate string print, and translating it would make three
// surfaces disagree about one hauler.
bool missionObjEscort(GameContent& content, double trader, double system, const std::string& text)
{
    if (!content.missionDraftOpen()) {
        return false;
    }
    if (trader < 0.0 || static_cast<std::size_t>(trader) >= content.world().economy().traders().size()) {
        SOL_LOG_WARN("mission_obj_escort: trader %d out of range", static_cast<int>(trader));
        return false;
    }
    if (system < 0.0 || static_cast<std::size_t>(system) >= content.world().galaxy().systems.size()) {
        SOL_LOG_WARN("mission_obj_escort: system %d out of range", static_cast<int>(system));
        return false;
    }
    content.missionDraft().objectives.push_back({.kind = sol::sim::ObjectiveKind::Escort,
                                                 .system = static_cast<std::uint32_t>(system),
                                                 .trader = static_cast<std::uint32_t>(trader),
                                                 .text = text});
    return true;
}

bool missionObjFlyTo(GameContent& content,
                     double system,
                     double dx,
                     double dy,
                     double dz,
                     double radius,
                     const std::string& text)
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
    const core::DVec3 anchor =
        !spec.stations.empty() ? spec.stations[0].position : spec.planets[spec.primaryPlanet].position;
    content.missionDraft().objectives.push_back({.kind = sol::sim::ObjectiveKind::FlyTo,
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
    if (!world.missionSim().postOffer(
            world.galaxy(), world.economy(), world.factionSim(), content.missionDraft(), &error)) {
        SOL_LOG_WARN("mission_post: '%s' refused: %s", content.missionDraft().title.c_str(), error.c_str());
        return false;
    }
    return true;
}

} // namespace

bool GameContent::initialize(const std::string& dataDirectory,
                             std::span<const std::string> modLayerDirectories,
                             SpaceWorld* world)
{
    m_world = world;

    // Layer order: base game first (mod zero), then the mods in name order, so
    // a later name overwrites an earlier one in place.
    //
    // ⚑ THE SCAN THAT USED TO LIVE HERE MOVED OUT (Phase 24 stage S), and the
    // move is not tidying. Deriving layer names by prefix-matching
    // `platform::listFiles` output is the code that produced a mod layer named
    // `C:` in the first shipping build, and it sat in this function where no
    // suite could reach it. It is now `game::modLayerNames`, pure and tested,
    // and `main.cpp` runs it once - which it has to anyway, because the cooked
    // ASSET search path needs the same list before the renderer comes up.
    m_layerDirectories = {dataDirectory};
    m_layerDirectories.insert(
        m_layerDirectories.end(), modLayerDirectories.begin(), modLayerDirectories.end());

    registerBindings();
    if (!reloadDefs()) {
        return false; // boot data must be valid; the error was logged
    }
    m_world->applyDefs(m_defs);
    if (!m_world->generateUniverse(m_defs)) { // defs feed the generator params
        // Same treatment `validateRoles` gets four lines up: boot data must be
        // valid, and the errors naming file/id/rule are already logged.
        return false;
    }
    runBootScripts();
    rebuildWatchList();
    SOL_LOG_INFO("content: %zu layer(s), %zu ship / %zu weapon / %zu faction def(s)",
                 m_layerDirectories.size(),
                 m_defs.ships().size(),
                 m_defs.weapons().size(),
                 m_defs.factions().size());
    return true;
}

bool GameContent::restartForNewGame()
{
    if (m_world == nullptr) {
        return false;
    }
    m_world->applyDefs(m_defs);
    // ⚑⚑ THIS CAN FAIL WHERE BOOT SUCCEEDED, AND THE REASON IS WORTH KNOWING:
    // a `jumps_from` ring is a claim about a gate graph, the gate graph comes
    // from the seed, and a new game may carry a different one. So placement
    // satisfiability is a per-seed verdict and there is no load-time check
    // that could have settled it once for every galaxy the player will see.
    if (!m_world->generateUniverse(m_defs)) {
        return false;
    }
    runBootScripts();
    return true;
}

void GameContent::registerBindings()
{
    m_vm.registerFunction<&spawnShip>("sol", "spawn_ship", this);
    m_vm.registerFunction<&listShips>("sol", "ships", this);
    // Audio (Phase 8t). play_sound is the point of choosing data-driven cues
    // over hardcoded call sites: campaign.lua and pilot_hail can be heard.
    m_vm.registerFunction<&listSounds>("sol", "sounds", this);
    m_vm.registerFunction<&listModels>("sol", "models", this);
    m_vm.registerFunction<&audioReport>("sol", "audio", this);
    m_vm.registerFunction<&playSound>("sol", "play_sound", this);
    m_vm.registerFunction<&playSoundAt>("sol", "play_sound_at", this);
    m_vm.registerFunction<&targetName>("sol", "target_name", this);
    m_vm.registerFunction<&targetDistance>("sol", "target_distance", this);
    m_vm.registerFunction<&shipSpeed>("sol", "speed", this);
    m_vm.registerFunction<&entityCount>("sol", "entity_count", this);
    m_vm.registerFunction<&spawnPilot>("sol", "spawn_pilot", this);
    m_vm.registerFunction<&spawnPilotFaction>("sol", "spawn_pilot_faction", this);
    m_vm.registerFunction<&pilotAttackPlayer>("sol", "pilot_attack_player", this);
    m_vm.registerFunction<&pilotEngageEnemy>("sol", "pilot_engage_enemy", this);
    m_vm.registerFunction<&pilotHuntTrader>("sol", "pilot_hunt_trader", this);
    m_vm.registerFunction<&pilotEngageThreat>("sol", "pilot_engage_threat", this);
    m_vm.registerFunction<&pilotUnderFire>("sol", "pilot_under_fire", this);
    m_vm.registerFunction<&pilotFlee>("sol", "pilot_flee", this);
    m_vm.registerFunction<&pilotIdle>("sol", "pilot_idle", this);
    m_vm.registerFunction<&pilotPatrolOffset>("sol", "pilot_patrol_offset", this);
    m_vm.registerFunction<&pilotHull>("sol", "pilot_hull", this);
    m_vm.registerFunction<&systemName>("sol", "system", this);
    m_vm.registerFunction<&jumpNearestGate>("sol", "jump", this);
    m_vm.registerFunction<&jumpState>("sol", "jump_state", this);
    m_vm.registerFunction<&gateDistance>("sol", "gate_distance", this);
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
    m_vm.registerFunction<&traderRoutes>("sol", "traders", this);
    m_vm.registerFunction<&worldgenReport>("sol", "worldgen", this);
    m_vm.registerFunction<&traderPuppets>("sol", "puppets", this);
    m_vm.registerFunction<&minerPuppets>("sol", "miners", this);
    m_vm.registerFunction<&traderHunters>("sol", "hunters", this);
    m_vm.registerFunction<&systemDanger>("sol", "danger", this);
    m_vm.registerFunction<&systemSecurity>("sol", "security", this);
    m_vm.registerFunction<&securityHistogram>("sol", "security_map", this);
    m_vm.registerFunction<&dispatchResponse>("sol", "respond", this);
    m_vm.registerFunction<&enterSystemAt>("sol", "enter_system", this);
    m_vm.registerFunction<&escortCandidates>("sol", "escort_candidates", this);
    m_vm.registerFunction<&killTrader>("sol", "trader_kill", this);
    m_vm.registerFunction<&killMiner>("sol", "miner_kill", this);
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
    m_vm.registerFunction<&listTerritory>("sol", "territory", this);
    m_vm.registerFunction<&contestReport>("sol", "contest", this);
    m_vm.registerFunction<&setContest>("sol", "set_contest", this);
    m_vm.registerFunction<&flipSystem>("sol", "flip", this);
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
    m_vm.registerFunction<&systemIndexById>("sol", "system_by_id", this);
    m_vm.registerFunction<&dockedStationIndex>("sol", "docked_station_index", this);
    m_vm.registerFunction<&missionBegin>("sol", "mission_begin", this);
    m_vm.registerFunction<&missionDeadline>("sol", "mission_deadline", this);
    m_vm.registerFunction<&missionMinRep>("sol", "mission_min_rep", this);
    m_vm.registerFunction<&missionObjDock>("sol", "mission_obj_dock", this);
    m_vm.registerFunction<&missionObjDeliver>("sol", "mission_obj_deliver", this);
    m_vm.registerFunction<&missionObjKill>("sol", "mission_obj_kill", this);
    m_vm.registerFunction<&missionObjHold>("sol", "mission_obj_hold", this);
    m_vm.registerFunction<&missionObjEscort>("sol", "mission_obj_escort", this);
    m_vm.registerFunction<&missionObjFlyTo>("sol", "mission_obj_flyto", this);
    m_vm.registerFunction<&missionPost>("sol", "mission_post", this);
    // Exploration (Phase 8e). sol.set_loot is the signal_loot hook's builder;
    // the rest are read-outs plus two dev levers (scan, chart).
    m_vm.registerFunction<&listKnowledge>("sol", "knowledge", this);
    m_vm.registerFunction<&listSignals>("sol", "signals", this);
    // Phase 8z: what the player knows about the built things here.
    m_vm.registerFunction<&listStructures>("sol", "structures", this);
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
    m_vm.registerFunction<&listContacts>("sol", "contacts", this);
    // Docking clearance (Phase 8r). sol.grant_docking / sol.deny_docking are
    // the dock_request hook's builders; sol.request_dock is the player's own
    // action and sol.clearance / sol.berths are the probes a drive asserts
    // through. sol.dock above is unchanged and still bypasses all of it.
    m_vm.registerFunction<&requestDock>("sol", "request_dock", this);
    m_vm.registerFunction<&describeClearance>("sol", "clearance", this);
    m_vm.registerFunction<&listBerths>("sol", "berths", this);
    m_vm.registerFunction<&grantDocking>("sol", "grant_docking", this);
    m_vm.registerFunction<&denyDocking>("sol", "deny_docking", this);
    // Pilot comms (Phase 8s). sol.hail_reply / sol.hail_tip_market /
    // sol.hail_tip_place are the pilot_hail hook's builders; sol.hail and
    // sol.hail_target are the player's own action, and sol.tips reads back what
    // the two stores this item writes into already hold.
    m_vm.registerFunction<&hailSelected>("sol", "hail", this);
    m_vm.registerFunction<&hailByName>("sol", "hail_target", this);
    m_vm.registerFunction<&listTips>("sol", "tips", this);
    m_vm.registerFunction<&hailReply>("sol", "hail_reply", this);
    m_vm.registerFunction<&hailTipMarket>("sol", "hail_tip_market", this);
    m_vm.registerFunction<&hailTipPlace>("sol", "hail_tip_place", this);
    // Mission objectives and threat selection (Phase 8i).
    m_vm.registerFunction<&describeObjective>("sol", "objective", this);
    m_vm.registerFunction<&targetNearestHostile>("sol", "target_hostile", this);
    // Click-to-select (Phase 8j): the mouse's own path, at coordinates.
    m_vm.registerFunction<&pickAt>("sol", "pick", this);
    m_vm.registerFunction<&pickBoresightCommand>("sol", "pick_boresight", this);
    m_vm.registerFunction<&bookmarkHere>("sol", "bookmark", this);
    m_vm.registerFunction<&listBookmarks>("sol", "bookmarks", this);
    m_vm.registerFunction<&deleteBookmark>("sol", "bookmark_delete", this);
    // Remote system maps (Phase 8q): what the System tab draws for any system.
    m_vm.registerFunction<&systemMap>("sol", "system_map", this);
    m_vm.registerFunction<&mapRow>("sol", "map_row", this);
    m_vm.registerFunction<&warpBookmark>("sol", "warp_bookmark", this);
    m_vm.registerFunction<&shipInfo>("sol", "ship_info", this);
    // Which level each instance drew at (Phase 9 stage F), because no frame
    // rate on this content can answer that.
    m_vm.registerFunction<&lodReportCommand>("sol", "lods", this);
    m_vm.registerFunction<&lodPinCommand>("sol", "lod_pin", this);
    m_vm.registerFunction<&rolesReport>("sol", "roles", this);
    m_vm.registerFunction<&perfReport>("sol", "perf", this);
    m_vm.registerFunction<&perfZone>("sol", "perf_zone", this);
    m_vm.registerFunction<&perfZoneMax>("sol", "perf_zone_max", this);
    m_vm.registerFunction<&perfZoneCount>("sol", "perf_count", this);
    m_vm.registerFunction<&perfReset>("sol", "perf_reset", this);
    // Controls (Phase 8k): the same table the Controls screen edits.
    m_vm.registerFunction<&listBindings>("sol", "bindings", this);
    m_vm.registerFunction<&bindAction>("sol", "bind", this);
    m_vm.registerFunction<&resetBindings>("sol", "reset_bindings", this);
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
    // Phase 19: every slot the engine draws into must be filled, by a model
    // that exists. This runs on the HOT-RELOAD path too, and refusing here is
    // what makes that safe - a typo in a role leaves the running game on the
    // defs it already had rather than un-drawing every gate in the galaxy.
    if (!fresh.validateRoles(modelRoles(), &error)) {
        SOL_LOG_ERROR("data defs: %s", error.c_str());
        return false;
    }
    // Phase 25 stage A, and it refuses for the same reason the roles check
    // does: naming a material is exactly what makes a model give up its own
    // surface keys, so a name that resolves to nothing has nothing left to
    // draw with. On the HOT-RELOAD path this leaves the running game on the
    // defs it already had, which is what makes editing a material safe.
    if (!fresh.validateMaterials(&error)) {
        SOL_LOG_ERROR("data defs: %s", error.c_str());
        return false;
    }
    // Phase 29, and it refuses for the same reason the two above do: an
    // authored system whose faction or whose station archetype does not exist
    // has no fallback that is not a lie about where the campaign starts. On the
    // HOT-RELOAD path this leaves the running game on the defs it already had -
    // which matters more here than elsewhere, because the galaxy is built ONCE
    // and a reload never rebuilds it (see the log line below).
    if (!fresh.validateSystems(&error)) {
        SOL_LOG_ERROR("data defs: %s", error.c_str());
        return false;
    }
    m_defs = std::move(fresh);
    // Cue tuning follows the defs (Phase 8t): gain, jitter, rolloff and caps
    // are re-read, the cooked samples are not - retuning a cue is a file save,
    // recooking one is a build.
    if (m_audio != nullptr) {
        m_audio->reloadDefs(m_defs);
    }
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
    lua_getglobal(state, "dock_request");
    m_hasDockRequestHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_dockRequestHookFailed = false;
    lua_getglobal(state, "pilot_hail");
    m_hasPilotHailHook = lua_isfunction(state, -1);
    lua_pop(state, 1);
    m_pilotHailHookFailed = false;
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
    auto noteChange = [&](const WatchedFile& file) { (file.isScript ? scriptsChanged : defsChanged) = true; };
    for (const WatchedFile& file : m_watched) {
        const auto old = std::find_if(
            previous.begin(), previous.end(), [&](const WatchedFile& f) { return f.path == file.path; });
        if (old == previous.end() || old->modificationTime != file.modificationTime) {
            noteChange(file);
        }
    }
    for (const WatchedFile& file : previous) { // deletions
        const auto current = std::find_if(
            m_watched.begin(), m_watched.end(), [&](const WatchedFile& f) { return f.path == file.path; });
        if (current == m_watched.end()) {
            noteChange(file);
        }
    }

    if (defsChanged) {
        // ⚑⚑ Phase 29. "Hot reload comes free by staying in TOML" is true of the
        // PARSE and false of the EFFECT: reloadDefs swaps the whole DefDatabase
        // and never touches m_galaxy, which is built once and regenerated only
        // when the SEED differs. Re-placing authored systems under a running
        // campaign would move the player's own system out from under them, so
        // the galaxy deliberately does not reload - and this says so, because
        // an author editing systems.toml and seeing "data defs reloaded" would
        // otherwise reasonably conclude their edit had taken.
        std::vector<std::string> systemsBefore;
        for (const assets::SystemDef& system : m_defs.systems()) {
            systemsBefore.push_back(system.id);
        }
        if (reloadDefs()) {
            m_world->applyDefs(m_defs);
            SOL_LOG_INFO("data defs reloaded");
            std::vector<std::string> systemsAfter;
            for (const assets::SystemDef& system : m_defs.systems()) {
                systemsAfter.push_back(system.id);
            }
            if (systemsAfter != systemsBefore) {
                SOL_LOG_WARN("[[system]] rows changed, but the galaxy is generated once at startup - "
                             "restart to see authored systems move");
            }
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
        SOL_PROFILE_ZONE("lua.on_tick");
        std::string error;
        if (!m_vm.callGlobal("on_tick", &error, dt)) {
            SOL_LOG_ERROR("on_tick disabled until scripts reload: %s", error.c_str());
            m_tickHookFailed = true;
        }
    }

    // Pilot strategy: Lua thinks at 2 Hz per pilot; C++ steering flies the
    // chosen state every tick inside SpaceWorld.
    if (m_hasPilotHook && !m_pilotHookFailed) {
        // Counter is pilots that came due this tick, not pilots alive: the
        // hook runs at 2 Hz per pilot, so the two differ by ~30x.
        SOL_PROFILE_ZONE_NAMED(pilotZone, "lua.pilot_think");
        m_pilotThinks.clear();
        m_world->collectDuePilotThinks(dt, m_pilotThinks);
        SOL_PROFILE_COUNT(pilotZone, m_pilotThinks.size());
        for (const SpaceWorld::PilotThink& think : m_pilotThinks) {
            std::string error;
            if (!m_vm.callGlobal("pilot_think",
                                 &error,
                                 scripting::toHandle(think.entity),
                                 think.role,
                                 think.state,
                                 think.attitude,
                                 think.pirate)) {
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
    {
        SOL_PROFILE_ZONE_NAMED(factionZone, "lua.faction_think");
        m_factionDecisions.clear();
        m_world->factionSim().takeDueDecisions(m_factionDecisions);
        SOL_PROFILE_COUNT(factionZone, m_factionDecisions.size());
        for (const sol::sim::FactionDecision& decision : m_factionDecisions) {
            if (m_hasFactionHook && !m_factionHookFailed) {
                const game::GameFaction& faction = m_world->factions()[decision.faction];
                std::string error;
                if (m_vm.callGlobal("faction_think",
                                    &error,
                                    static_cast<double>(decision.faction + 1),
                                    faction.name.c_str(),
                                    faction.pirate,
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
    }

    // Campaign flavor: the world already applied payouts/penalties; authored
    // missions' transitions are forwarded to Lua's mission_event hook.
    SOL_PROFILE_ZONE("lua.mission_events");
    m_missionEvents.clear();
    m_world->takeMissionEvents(m_missionEvents);

    // Phase 8l: a mission leaving the journal is exactly when the board wants
    // re-composing - a campaign chain posts its next leg from the board hook,
    // and a freed active slot can unblock offers. Checked across every event,
    // not just the campaign ones the hook below forwards.
    bool boardDirty = false;
    for (const sol::sim::MissionEvent& event : m_missionEvents) {
        if (event.kind == sol::sim::MissionEventKind::Completed ||
            event.kind == sol::sim::MissionEventKind::Failed ||
            event.kind == sol::sim::MissionEventKind::Lost ||
            event.kind == sol::sim::MissionEventKind::Abandoned) {
            boardDirty = true;
            break;
        }
    }

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
            case sol::sim::MissionEventKind::Lost:
                kind = "lost";
                break;
            case sol::sim::MissionEventKind::Abandoned:
                kind = "abandoned";
                break;
            }
            std::string error;
            if (!m_vm.callGlobal("mission_event",
                                 &error,
                                 event.mission.campaignId.c_str(),
                                 kind,
                                 static_cast<double>(event.objective + 1))) {
                SOL_LOG_ERROR("mission_event disabled until scripts reload: %s", error.c_str());
                m_missionEventHookFailed = true;
                break;
            }
        }
    }

    // Mission board (Phase 8c): re-composed on every dock event, whenever a
    // mission just left the journal (Phase 8l), and otherwise on the docked
    // refresh cadence as a backstop for offers that rotate on their own. It
    // runs *after* the event drain above so that a campaign leg completed
    // this frame has already bumped the stage through mission_event, and the
    // composition that follows can post the next leg - which is what made
    // undock/redock the only way to continue a chain. openBoard resets the
    // refresh accumulator, so a re-compose here re-arms the backstop too.
    const bool dockEvent = m_world->consumeDockEvent();
    if (m_world->isDocked() && (dockEvent || boardDirty || m_world->missionSim().tickBoard(dt))) {
        runMissionBoard();
    }

    // Docking clearance (Phase 8r): the world queues a hail, the dispatcher
    // answers it. Same shape as the board and the loot hooks — C++ enumerates,
    // Lua composes, C++ validates — so a refusal can be written by a faction
    // rather than hardcoded here, and the scriptless default below still makes
    // the feature work with no scripts at all.
    std::uint32_t dockStation = 0;
    double dockRoll = 0.0;
    if (m_world->takeDockRequest(dockStation, dockRoll)) {
        const sol::sim::SystemSpec& spec = m_world->galaxy().systems[m_world->currentSystemIndex()];
        const std::uint32_t owner = m_world->systemOwnerFaction(m_world->currentSystemIndex());
        const bool owned = owner < m_world->factions().size();
        const bool hostile = owned && m_world->factionSim().playerHostile(owner);
        const char* ownerName = owned ? m_world->factions()[owner].name.c_str() : "Independent";
        const double standing = owned ? static_cast<double>(m_world->factionSim().standing(owner)) : 0.0;
        m_dockAnswered = false;
        if (m_hasDockRequestHook && !m_dockRequestHookFailed) {
            m_dockRequestStation = dockStation;
            std::string error;
            if (!m_vm.callGlobal("dock_request",
                                 &error,
                                 spec.stations[dockStation].name.c_str(),
                                 ownerName,
                                 standing,
                                 static_cast<double>(sol::sim::kBerthCount),
                                 hostile,
                                 dockRoll)) {
                SOL_LOG_ERROR("dock_request disabled until scripts reload: %s", error.c_str());
                m_dockRequestHookFailed = true;
            }
            m_dockRequestStation = 0xffff'ffffu;
        }
        if (!m_dockAnswered) {
            // The scriptless default, so the feature works with no scripts at
            // all. The refusal is the one Phase 8b already wrote — it just
            // reaches the player now instead of the log nobody reads.
            if (hostile) {
                m_world->denyDocking(dockStation,
                                     std::string("Clearance denied. ") + ownerName + " wants you gone.");
            } else {
                const std::uint32_t berth =
                    static_cast<std::uint32_t>(dockRoll * sol::sim::kBerthCount) % sol::sim::kBerthCount;
                (void)m_world->grantDocking(dockStation,
                                            berth,
                                            "Cleared for berth " + std::to_string(berth + 1) +
                                                ". Mind your approach.");
            }
        }
    }

    // Pilot comms (Phase 8s): the same drain one item later. C++ enumerates
    // everything the pilot could know, Lua composes the words and picks which
    // KIND of tip to offer, and C++ picks the fact itself — a tip is a claim
    // about the galaxy, and a hook allowed to name the position could bookmark
    // interstellar space.
    SpaceWorld::HailRequest hail;
    if (m_world->takeHailRequest(hail)) {
        if (m_hasPilotHailHook && !m_pilotHailHookFailed) {
            std::string error;
            if (!m_vm.callGlobal("pilot_hail",
                                 &error,
                                 hail.name.c_str(),
                                 hail.role,
                                 hail.factionName.c_str(),
                                 hail.attitude,
                                 hail.standing,
                                 hail.hostile,
                                 hail.canTipMarket,
                                 hail.canTipPlace,
                                 hail.roll)) {
                SOL_LOG_ERROR("pilot_hail disabled until scripts reload: %s", error.c_str());
                m_pilotHailHookFailed = true;
            }
        }
        // Still answering means the hook said nothing (or is not there), so the
        // scriptless default takes its turn and the feature works with no
        // scripts at all.
        if (m_world->answeringHail()) {
            if (hail.hostile) {
                (void)m_world->replyHail("Say another word and I'll answer with guns.");
            } else if (hail.canTipMarket && std::strcmp(hail.role, "trader") == 0) {
                (void)m_world->tipMarket("Prices were worth the trip at");
            } else if (hail.canTipPlace) {
                (void)m_world->tipPlace("There's something out there nobody's picked over, in");
            } else {
                (void)m_world->replyHail("Nothing to report. Fly safe.");
            }
        }
        m_world->finishHail();
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
                if (!m_vm.callGlobal(
                        "signal_found", &error, sol::sim::signalKindName(event.signalKind), systemName)) {
                    SOL_LOG_ERROR("signal_found disabled until scripts reload: %s", error.c_str());
                    m_signalFoundHookFailed = true;
                }
            }
            continue;
        }
        if (event.kind != SurveyEvent::Kind::SignalResolved || !m_hasLootHook || m_lootHookFailed) {
            continue;
        }
        const sol::sim::Region region = event.system < m_world->galaxy().systems.size()
                                            ? m_world->galaxy().systems[event.system].region
                                            : sol::sim::Region::Core;
        m_lootSystem = event.system;
        m_lootSignal = event.index;
        std::string error;
        if (!m_vm.callGlobal("signal_loot",
                             &error,
                             sol::sim::signalKindName(event.signalKind),
                             systemName,
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
        if (!m_vm.callGlobal("wreck_loot",
                             &error,
                             event.defId.c_str(),
                             systemName,
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
        if (!m_vm.callGlobal("rock_mined",
                             &error,
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
    missions.haulCandidates(world.galaxy(),
                            world.economy(),
                            world.currentSystemIndex(),
                            world.dockedStationIndex(),
                            m_haulCandidates);
    std::string hauls;
    for (const sol::sim::HaulCandidate& c : m_haulCandidates) {
        if (!hauls.empty()) {
            hauls += ";";
        }
        char buffer[64];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%.0f:%.2f:%u",
                      static_cast<double>(c.units),
                      static_cast<double>(c.severity),
                      c.jumps);
        const sol::sim::SystemSpec& spec = world.galaxy().systems[c.system];
        hauls += std::to_string(c.system) + ":" + std::to_string(c.station) + ":" +
                 world.commodityIds()[c.commodity] + ":" + buffer + ":" + spec.name + ":" +
                 spec.stations[c.station].name;
    }
    // Bounties: "system:clanIndex1based:intensity:jumps:systemName:clanName".
    m_bountyCandidates.clear();
    missions.bountyCandidates(
        world.galaxy(), world.factionSim(), world.currentSystemIndex(), m_bountyCandidates);
    std::string bounties;
    for (const sol::sim::BountyCandidate& c : m_bountyCandidates) {
        if (c.clan >= world.factions().size()) {
            continue;
        }
        if (!bounties.empty()) {
            bounties += ";";
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.2f:%u", static_cast<double>(c.intensity), c.jumps);
        bounties += std::to_string(c.system) + ":" + std::to_string(c.clan + 1) + ":" + buffer + ":" +
                    world.galaxy().systems[c.system].name + ":" + world.factions()[c.clan].name;
    }

    // Contests (Phase 8u):
    // "system:owner1based:attacker1based:pressure:jumps:sysName:ownerName:attackerName".
    m_contestCandidates.clear();
    missions.contestCandidates(
        world.galaxy(), world.factionSim(), world.currentSystemIndex(), owner, m_contestCandidates);
    std::string contests;
    for (const sol::sim::ContestCandidate& c : m_contestCandidates) {
        if (c.owner >= world.factions().size() || c.attacker >= world.factions().size()) {
            continue;
        }
        if (!contests.empty()) {
            contests += ";";
        }
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.2f:%u", static_cast<double>(c.pressure), c.jumps);
        contests += std::to_string(c.system) + ":" + std::to_string(c.owner + 1) + ":" +
                    std::to_string(c.attacker + 1) + ":" + buffer + ":" +
                    world.galaxy().systems[c.system].name + ":" + world.factions()[c.owner].name + ":" +
                    world.factions()[c.attacker].name;
    }

    // Escorts (Phase 8x §E):
    // "trader:system:station:commodityId:cargo:danger:jumps:sysName:stName".
    // The hauler is named by its COARSE INDEX and never by an entity - the
    // body it may or may not have right now is a view, and the contract has to
    // outlive it.
    m_escortCandidates.clear();
    missions.escortCandidates(
        world.galaxy(), world.economy(), world.factionSim(), world.currentSystemIndex(), m_escortCandidates);
    std::string escorts;
    for (const sol::sim::EscortCandidate& c : m_escortCandidates) {
        if (c.system >= world.galaxy().systems.size()) {
            continue;
        }
        const sol::sim::SystemSpec& spec = world.galaxy().systems[c.system];
        if (c.station >= spec.stations.size()) {
            continue;
        }
        if (!escorts.empty()) {
            escorts += ";";
        }
        char buffer[64];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%.0f:%.2f:%u",
                      static_cast<double>(c.cargo),
                      static_cast<double>(c.danger),
                      c.jumps);
        // A deadheading hauler has no commodity worth naming, and `commodity`
        // stays set from its last run - the same trap sol.traders() fell into.
        const char* hauling = c.cargo > 0.0f && c.commodity < world.commodityIds().size()
                                  ? world.commodityIds()[c.commodity].c_str()
                                  : "-";
        escorts += std::to_string(c.trader) + ":" + std::to_string(c.system) + ":" +
                   std::to_string(c.station) + ":" + hauling + ":" + buffer + ":" + spec.name + ":" +
                   spec.stations[c.station].name;
    }

    std::string error;
    if (!m_vm.callGlobal("mission_board",
                         &error,
                         world.dockedStationName(),
                         static_cast<double>(owner + 1),
                         world.factions()[owner].name.c_str(),
                         world.factions()[owner].pirate,
                         hauls.c_str(),
                         bounties.c_str(),
                         contests.c_str(),
                         escorts.c_str(),
                         static_cast<double>(missions.boardRoll()))) {
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
