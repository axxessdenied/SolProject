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
    std::uint64_t simTicks = 0;
    std::uint32_t simEntities = 0;
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
