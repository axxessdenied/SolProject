#pragma once

#include "sol/core/math/vec.hpp"
#include "sol/core/profiler.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace sol::ui {

struct OverlayStats
{
    float fps = 0.0f;
    float frameMilliseconds = 0.0f;
    core::DVec3 cameraPosition;
    float cameraSpeed = 0.0f;
    std::uint32_t drawCalls = 0;
    // Opaque meshes drawn at each LOD level this frame (Phase 9 stage F), so a
    // level switch is something a person can WATCH rather than infer. All zero
    // draws the row exactly as it was before levels existed.
    std::uint32_t lodDrawn[3] = {};
    std::uint64_t simTicks = 0;
    std::uint32_t simEntities = 0;
    // How many systems are being ticked this frame (Phase 38 stage D). One for
    // the whole of the game's history until the cooling bubble; more than one
    // means a system the player has left is still running, which is otherwise
    // invisible by design - no radar contact, no spark, no sound. Zero draws
    // the row exactly as it read before, so a caller that never sets it loses
    // nothing.
    std::uint32_t simSystems = 0;
    float simAlpha = 0.0f;
    // Where the frame actually went (Phase 8n). Non-owning; null draws the
    // overlay exactly as it was before the profiler existed. Passed rather
    // than read off the global so the overlay can be driven with any
    // instance, and so this layer states its dependency instead of hiding it.
    const core::Profiler* profiler = nullptr;
};

// Dear ImGui dev/debug overlay (never player-facing UI - see engine plan 2.9).
//
// Content only: bringing ImGui up and closing a frame belong to ImGuiHost
// (Phase 9 stage C), of which this class is the first client. What is left
// here is the game's own two windows and the state behind them.
class DevUi
{
public:
    // How much of the perf overlay is on screen (Phase 8p). The console has
    // had a visibility flag since it existed and the overlay never did,
    // because until 8n's zone tree it was five short lines and nobody wanted
    // one. Compact is a third state rather than a plain on/off because the
    // complaint was *space*: hiding the window outright also costs the fps
    // line, which is the one a glance is usually looking for.
    enum class OverlayMode
    {
        Full,    // everything: stats, control crib, zone tree
        Compact, // the fps/frame-time line alone
        Hidden,  // no overlay window at all
    };

    // Points the core log sink at the console's ring buffer. No device work
    // and no ImGui context: an ImGuiHost has to be live before build() runs,
    // and this class does not care which one.
    void initialize();
    void shutdown();

    // Once per frame, between ImGuiHost::beginFrame and ImGuiHost::render;
    // builds the overlay + console windows. Player-facing screens are not this
    // class's business: they live on the custom UI stack (`sol::ui::UiContext`,
    // `sol/ui/screens.hpp`).
    void build(const OverlayStats& stats);

    // Console command line: submitted text goes to the handler (e.g. a Lua
    // VM); without one the input line is hidden and the console is log-only.
    using CommandHandler = void (*)(const char* command, void* userData);
    void setCommandHandler(CommandHandler handler, void* userData);

    // What is on screen at boot (Phase 23). Both fields already existed and
    // both were already reachable by F1/F2 - what was missing was any way for
    // a CLIENT to state a preference, so "the dev overlay is up at boot" was
    // a decision this layer made on everyone's behalf.
    //
    // ⚑⚑ THIS CLASS DELIBERATELY LEARNS NOTHING ABOUT SHIPPING. Whether a
    // build is one somebody else will run is the game's business, not the
    // engine's (AGENTS.md 4) - and the Forge is a second client of this same
    // library with a different right answer. Same split as the game supplying
    // its own name to userDataDirectory: mechanism here, policy there.
    //
    // ⚑ Hiding the console does NOT stop logging. initialize() points the
    // core log sink at the ring buffer regardless, so F1 later opens a console
    // with the whole session already in it rather than an empty one.
    void setConsoleVisible(bool visible) { m_showConsole = visible; }

    void setOverlayMode(OverlayMode mode) { m_overlayMode = mode; }

private:
    void buildConsoleInput();
    static int consoleTextCallback(ImGuiInputTextCallbackData* data);

    bool m_showConsole = true;
    // Full by default, so a cold boot looks exactly as it did before this
    // existed and the 8n/8o drives that screenshot the zone tree still work.
    OverlayMode m_overlayMode = OverlayMode::Full;

    CommandHandler m_commandHandler = nullptr;
    void* m_commandUserData = nullptr;
    char m_commandBuffer[512] = {};
    std::vector<std::string> m_commandHistory;
    int m_historyIndex = -1; // -1 = editing a fresh line
};

} // namespace sol::ui
