#pragma once

#include "space_world.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/scripting/vm.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace game {

// Phase 5 data-driven content: owns the def database and the Lua VM, loads
// both from an ordered list of mod layers (base game data dir = mod zero,
// then each subdirectory of the mods dir sorted by name), and hot-reloads
// either kind when a source file changes. Layer layout: *.toml defs anywhere
// under the layer root, boot script at scripts/init.lua.
class GameContent
{
public:
    // World must outlive this. Missing mods directory is fine.
    [[nodiscard]] bool initialize(const std::string& dataDirectory,
                                  const std::string& modsDirectory, SpaceWorld* world);

    // Polls watched TOML/Lua sources (throttled); on a def change reloads the
    // database and re-applies it to the world, on a script change re-runs the
    // boot scripts in layer order. Errors keep the previous state.
    void poll(double nowSeconds);

    // Runs the scripts' on_tick(dt) hook, if any layer defined one.
    void tick(double dt);

    // Console input: echoes, then evaluates as Lua in the shared VM.
    void executeConsole(const char* command);

    [[nodiscard]] const sol::assets::DefDatabase& defs() const { return m_defs; }
    [[nodiscard]] SpaceWorld& world() { return *m_world; }

    // Mission-builder draft for the Lua board hook (sol.mission_* bindings
    // in content.cpp assemble it; sol.mission_post validates and clears it).
    [[nodiscard]] sol::sim::Mission& missionDraft() { return m_missionDraft; }
    [[nodiscard]] bool missionDraftOpen() const { return m_missionDraftOpen; }
    void setMissionDraftOpen(bool open) { m_missionDraftOpen = open; }

private:
    struct WatchedFile
    {
        std::string path;
        std::uint64_t modificationTime = 0;
        bool isScript = false;
    };

    [[nodiscard]] bool reloadDefs();
    void runBootScripts();
    void rebuildWatchList();
    void registerBindings();
    // Re-opens the docked station's board and runs Lua mission_board over
    // freshly enumerated candidates (scriptless fallback: an empty board).
    void runMissionBoard();

    static constexpr double kPollIntervalSeconds = 0.5;

    std::vector<std::string> m_layerDirectories;
    sol::assets::DefDatabase m_defs;
    sol::scripting::ScriptVm m_vm;
    SpaceWorld* m_world = nullptr;
    std::vector<WatchedFile> m_watched;
    std::vector<SpaceWorld::PilotThink> m_pilotThinks; // per-tick scratch
    std::vector<sol::sim::FactionDecision> m_factionDecisions; // per-tick scratch
    std::vector<sol::sim::HaulCandidate> m_haulCandidates;     // per-board scratch
    std::vector<sol::sim::BountyCandidate> m_bountyCandidates;
    std::vector<sol::sim::MissionEvent> m_missionEvents; // per-tick scratch
    sol::sim::Mission m_missionDraft;
    bool m_missionDraftOpen = false;
    double m_lastPollTime = -1.0;
    bool m_hasTickHook = false;
    bool m_tickHookFailed = false; // logged once; reset on script reload
    bool m_hasPilotHook = false;
    bool m_pilotHookFailed = false;
    bool m_hasFactionHook = false;
    bool m_factionHookFailed = false;
    bool m_hasBoardHook = false;
    bool m_boardHookFailed = false;
    bool m_hasMissionEventHook = false;
    bool m_missionEventHookFailed = false;
};

} // namespace game
