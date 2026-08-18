#pragma once

#include "input_actions.hpp"
#include "space_world.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/platform/input_bindings.hpp"
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

    // The live control bindings (Phase 8k), so the console can read and change
    // them - which is what makes a rebind verifiable in a drive script without
    // pixel-perfect clicking through a scrolling list. Owned by main.cpp's
    // Settings; null until it is handed over.
    void setBindings(sol::platform::BindingTable* bindings) { m_bindings = bindings; }
    [[nodiscard]] sol::platform::BindingTable* bindings() const { return m_bindings; }

    // Mission-builder draft for the Lua board hook (sol.mission_* bindings
    // in content.cpp assemble it; sol.mission_post validates and clears it).
    [[nodiscard]] sol::sim::Mission& missionDraft() { return m_missionDraft; }
    [[nodiscard]] bool missionDraftOpen() const { return m_missionDraftOpen; }
    void setMissionDraftOpen(bool open) { m_missionDraftOpen = open; }

    // The site whose signal_loot hook is running right now, so sol.set_loot
    // can only ever write the loot it was called about (Phase 8e).
    [[nodiscard]] std::uint32_t lootSystem() const { return m_lootSystem; }
    [[nodiscard]] std::uint32_t lootSignal() const { return m_lootSignal; }
    // The wreck whose wreck_loot hook is running right now (Phase 8f); 0 when
    // none is. Wrecks and sites share sol.set_loot — it is the same loot.
    [[nodiscard]] std::uint32_t lootWreck() const { return m_lootWreck; }

    // The station whose dock_request hook is running right now (Phase 8r), so
    // sol.grant_docking and sol.deny_docking can only ever answer the hail
    // they were called about. ~0u outside the hook.
    [[nodiscard]] std::uint32_t dockRequestStation() const { return m_dockRequestStation; }
    void noteDockAnswered() { m_dockAnswered = true; }

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
    sol::platform::BindingTable* m_bindings = nullptr; // Phase 8k; main.cpp owns it
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
    // Exploration hooks (Phase 8e).
    std::vector<SurveyEvent> m_surveyEvents; // per-tick scratch
    std::uint32_t m_lootSystem = 0xffff'ffffu;
    std::uint32_t m_lootSignal = 0xffff'ffffu;
    bool m_hasLootHook = false;
    bool m_lootHookFailed = false;
    bool m_hasSignalFoundHook = false;
    bool m_signalFoundHookFailed = false;
    // Mining & salvage hooks (Phase 8f).
    std::vector<WreckEvent> m_wreckEvents; // per-tick scratch
    std::vector<RockEvent> m_rockEvents;   // per-tick scratch
    std::uint32_t m_lootWreck = 0;
    bool m_hasWreckLootHook = false;
    bool m_wreckLootHookFailed = false;
    bool m_hasRockMinedHook = false;
    bool m_rockMinedHookFailed = false;
    // Docking clearance hook (Phase 8r). m_dockRequestStation is the station
    // the hook is being asked about, which is what sol.grant_docking and
    // sol.deny_docking answer against - the same context-while-the-hook-runs
    // shape m_lootSignal has, and what makes those two builders refuse to be
    // called from anywhere else. m_dockAnswered is how the scriptless default
    // knows the hook declined to say anything.
    std::uint32_t m_dockRequestStation = 0xffff'ffffu;
    bool m_dockAnswered = false;
    bool m_hasDockRequestHook = false;
    bool m_dockRequestHookFailed = false;
    // Pilot comms hook (Phase 8s). No station index to hold here: whether a
    // hail is being answered is SpaceWorld::answeringHail(), which closes
    // itself on the first answer, so the "exactly one reply" rule and the
    // "only inside the hook" guard are the same fact rather than two.
    bool m_hasPilotHailHook = false;
    bool m_pilotHailHookFailed = false;
};

} // namespace game
