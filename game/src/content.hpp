#pragma once

#include "input_actions.hpp"
#include "space_world.hpp"
#include "station_ui.hpp"

#include "sol/assets/data_defs.hpp"
#include "sol/platform/input_bindings.hpp"
#include "sol/scripting/vm.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace game {

// Phase 5 data-driven content: owns the def database and the Lua VM, loads
// both from an ordered list of mod layers (base game data dir = mod zero,
// then each subdirectory of the mods dir sorted by name), and hot-reloads
// either kind when a source file changes. Layer layout: *.toml defs anywhere
// under the layer root, boot script at scripts/init.lua.
//
// ⚑ Since Phase 24 stage S a layer can also carry ASSETS - `assets/` as source
// and `cooked/` as output - but nothing in this class touches them. Defs and
// scripts are what a layer means HERE; the cooked search path built from the
// same layer list lives in `asset_paths.hpp` and is consumed by the renderer
// and the audio bank. Two readers of one list, deliberately not merged: this
// one reloads at runtime, and that one is fixed once at startup.
// ⚑⚑⚑ THE ROSTER TABLE, AS TEXT, AND IT IS A FREE FUNCTION SO A TEST CAN
// MEASURE IT (Phase 32 stage C). `sol.rosters()` prints this into a console
// panel about 76 columns wide, and the first version ran to 88 on every major
// - the two-hull trader cell fell off the right edge, and NOTHING said so.
// Only the screenshot caught it, which is the second time in this phase alone
// (stage A's hull spine lost its verdict column the same way) and the third
// time in this project. So the width is a claim `game.unit` checks against
// shipped content rather than a sentence in a comment.
//
// One faction per PAIR of lines: id, patrol and raider on the first, trader
// indented under it. The split is measured rather than arbitrary - trader is
// the only cell any shipped faction fills with more than one hull.
[[nodiscard]] std::vector<std::string> rosterLines(const sol::assets::DefDatabase& defs);

// The console panel's usable width. `sol.mounts` measured the same number in
// Phase 31 stage B1 and wrote it in a comment; this is that number with a test
// behind it.
inline constexpr std::size_t kConsoleColumns = 76;

class GameContent
{
public:
    // World must outlive this. `modLayerDirectories` is the full path of each
    // mod, in name order, and empty is the normal case - `main.cpp` finds them
    // with `game::discoverModLayers` because the cooked ASSET search path needs
    // the same list before this is called (Phase 24 stage S).
    [[nodiscard]] bool initialize(const std::string& dataDirectory,
                                  std::span<const std::string> modLayerDirectories,
                                  SpaceWorld* world);

    // The other half of SpaceWorld::resetForNewGame (Phase 27): re-applies the
    // already-loaded defs to the fresh world, regenerates its galaxy, and
    // re-runs the boot scripts so the campaign's Lua-side state starts over.
    //
    // ⚑ Deliberately NOT `initialize` again. That re-reads every def file off
    // disk and re-registers the whole binding surface into the VM, neither of
    // which a new game needs - the defs have not changed and the bindings are
    // already there. This is `initialize`'s tail, which is the part that is
    // about the WORLD rather than about the process.
    // FALSE when the new seed's galaxy could not place an authored system;
    // the errors are already logged. See SpaceWorld::generateUniverse.
    [[nodiscard]] bool restartForNewGame();

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

    // Audio (Phase 8t), so sol.play_sound can fire a cue from a script and
    // sol.audio can read the device back. Owned by main.cpp; null is normal
    // (no output device), and the bindings say so rather than lying.
    void setAudio(GameAudio* audio) { m_audio = audio; }

    [[nodiscard]] GameAudio* audio() const { return m_audio; }

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

    // What the house is saying at the dock the player is standing on (Phase 35
    // stage B), composed once per dock and read by the Bar tab's presenter.
    // Empty when undocked or when the station has no room.
    [[nodiscard]] std::span<const BarLine> barTalk() const { return m_barTalk; }

    [[nodiscard]] const char* barRoom() const { return m_barRoom.c_str(); }

    // Who is behind the bar, already worded (Phase 35 stage C) - name, trade,
    // and whether they have seen the player before. Empty only where the
    // station has no room, which is also where the tab is not on the strip.
    [[nodiscard]] const char* barKeeper() const { return m_barKeeper.c_str(); }

    // The `bar_talk` hook's five builders, in the shape the pilot_hail trio
    // has: each spends one of the room's lines, appends the fact C++ picked,
    // and refuses outside the hook. `kind` is which fact to append.
    enum class BarFact : std::uint32_t
    {
        None = 0, // words only
        Shortage,
        Raid,
        Front,
        Hauler,
        // Phase 35 stage C: somebody in a room within reach who is worth going
        // to see. The only one of the five that points at a PERSON rather than
        // at a market, a raid, a war or a run.
        Cast,
        // And whoever is standing in front of you, saying something of their
        // own. Words only, like `None` - the difference is entirely the TOPIC.
        //
        // ⚑⚑⚑ IT IS A SEPARATE KIND *ONLY* SO THAT THE TOPIC DIFFERS, AND THAT
        // IS STAGE A'S LESSON APPLIED RATHER THAN RESTATED. Sharing `None`'s
        // "Talk" made a character's line and the scriptless quiet-night
        // fallback indistinguishable to a test counting topics by name - which
        // is the exact instrument stage A built after finding that deleting a
        // house fact left the suite green because another line took its place.
        // *A conserved total is not a checksum*, and two speakers under one
        // topic is how a total gets conserved by accident.
        Speaker,
    };

    [[nodiscard]] bool sayBarLine(BarFact kind, const char* message);

    [[nodiscard]] bool composingBarTalk() const { return m_barLinesLeft > 0; }

private:
    struct WatchedFile
    {
        std::string path;
        std::uint64_t modificationTime = 0;
        bool isScript = false;
    };

    [[nodiscard]] bool reloadDefs();
    // ⚑⚑ WHAT EACH FACTION FIELDS, SAID OUT LOUD ONCE PER LOAD (Phase 32 stage
    // C). This is the half of the stage that makes an absence legible: a cell
    // a faction DECLARED it builds nothing for prints as `-`, and a cell that
    // is merely empty prints as `(unset)` and carries a warning, because those
    // are two different states and until this stage both of them were silence.
    // It runs on the hot-reload path too, so emptying a roster in a text editor
    // says so on the next save rather than at the next system load.
    void logRosters() const;
    void runBootScripts();
    void rebuildWatchList();
    void registerBindings();
    // Re-opens the docked station's board and runs Lua mission_board over
    // freshly enumerated candidates (scriptless fallback: an empty board).
    void runMissionBoard();
    // Composes the docked station's bar talk: the house's own lines, then up to
    // `roomTalkLines` lines about the wider galaxy chosen by C++ and worded by
    // Lua's bar_talk hook (scriptless fallback: C++ words them too).
    void runBarTalk();
    // The scriptless default, and the order it spends lines in.
    void defaultBarTalk();
    // Lets whoever is in the room say something of their own, before the talk
    // about the wider galaxy (Phase 35 stage C). ONE hook for the whole cast,
    // dispatching on the character id - which is `campaign.lua`'s shape rather
    // than a hook per person: the spine is a TABLE keyed by stage with two
    // global entry points, and a hook per character would be a global function
    // per row of a data file that a mod cannot extend without editing C++.
    void runCharacterTalk(const SpaceWorld::CastSeat& seat, std::uint32_t visits, std::int32_t regard);

    static constexpr double kPollIntervalSeconds = 0.5;

    std::vector<std::string> m_layerDirectories;
    sol::assets::DefDatabase m_defs;
    sol::scripting::ScriptVm m_vm;
    SpaceWorld* m_world = nullptr;
    sol::platform::BindingTable* m_bindings = nullptr; // Phase 8k; main.cpp owns it
    GameAudio* m_audio = nullptr;                      // Phase 8t; main.cpp owns it
    std::vector<WatchedFile> m_watched;
    std::vector<SpaceWorld::PilotThink> m_pilotThinks;         // per-tick scratch
    std::vector<sol::sim::FactionDecision> m_factionDecisions; // per-tick scratch
    std::vector<sol::sim::HaulCandidate> m_haulCandidates;     // per-board scratch
    std::vector<sol::sim::BountyCandidate> m_bountyCandidates;
    std::vector<sol::sim::ContestCandidate> m_contestCandidates; // Phase 8u
    std::vector<sol::sim::EscortCandidate> m_escortCandidates;   // Phase 8x
    std::vector<sol::sim::CastCandidate> m_castCandidates;       // Phase 35 stage C
    std::vector<sol::sim::MissionEvent> m_missionEvents;         // per-tick scratch
    sol::sim::Mission m_missionDraft;
    // Bar talk (Phase 35 stage B). The cache is BOUND TO A DOCK rather than
    // driven by the dock event alone, and that is not tidiness: a docked LOAD
    // deliberately clears m_dockEventPending so board offers are not re-rolled
    // (`space_world.cpp`, loadFrom), so an event-only rule would have left
    // every loaded save opening on an empty room. Binding self-heals that and
    // every other path that forgets to raise the event.
    std::vector<BarLine> m_barTalk;
    std::string m_barRoom;
    std::string m_barKeeper;    // the worded heading
    std::uint64_t m_barWho = 0; // the save key of whoever is in there
    bool m_hasCharacterHook = false;
    bool m_characterHookFailed = false;
    std::uint32_t m_barSystem = 0xffff'ffffu;
    std::uint32_t m_barStation = 0xffff'ffffu;
    std::uint32_t m_barVisits = 0;
    // The facts C++ picked this visit, already worded. The hook chooses which
    // to spend and on what words; it never gets to name a place.
    std::string m_barShortage;
    std::string m_barRaid;
    std::string m_barFront;
    std::string m_barHauler;
    std::string m_barCast;
    int m_barLinesLeft = 0; // > 0 only while bar_talk runs: the "inside the hook" guard
    bool m_hasBarTalkHook = false;
    bool m_barTalkHookFailed = false;
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
