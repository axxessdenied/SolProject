#include "sol/ui/dev_ui.hpp"

#include "sol/core/log.hpp"

#include <imgui.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sol::ui {

namespace {

// Ring buffer console fed by the core log sink.
struct ConsoleBuffer
{
    static constexpr std::size_t kMaxLines = 256;
    std::vector<std::pair<core::LogLevel, std::string>> lines;
    bool scrollToBottom = false;

    void add(core::LogLevel level, const char* message)
    {
        if (lines.size() >= kMaxLines) {
            lines.erase(lines.begin());
        }
        lines.emplace_back(level, message);
        scrollToBottom = true;
    }
};

ConsoleBuffer g_console;

void consoleLogSink(core::LogLevel level, const char* message, void* /*userData*/)
{
    g_console.add(level, message);
}

ImVec4 levelColor(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::Warn:
        return {1.0f, 0.8f, 0.3f, 1.0f};
    case core::LogLevel::Error:
    case core::LogLevel::Fatal:
        return {1.0f, 0.35f, 0.35f, 1.0f};
    default:
        return {0.75f, 0.78f, 0.82f, 1.0f};
    }
}

} // namespace

void DevUi::initialize()
{
    core::setLogSink(&consoleLogSink, nullptr);
}

void DevUi::shutdown()
{
    core::setLogSink(nullptr, nullptr);
}

void DevUi::build(const OverlayStats& stats)
{
    // Perf overlay: fixed top-left, transparent, no interaction. Hidden skips
    // Begin and End *together* (Phase 8p) - gating only the Begin would leave
    // an unmatched End in a frame that otherwise looks perfectly healthy.
    if (m_overlayMode != OverlayMode::Hidden) {
        ImGui::SetNextWindowPos({12.0f, 12.0f});
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags overlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_AlwaysAutoResize |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("perf##overlay", nullptr, overlayFlags)) {
            // The one line Compact keeps: it answers the space complaint
            // without giving up the number a glance is usually after.
            ImGui::Text("%.1f fps  (%.2f ms)", stats.fps, stats.frameMilliseconds);
            if (m_overlayMode == OverlayMode::Full) {
                ImGui::Text("cam  %.2f  %.2f  %.2f",
                            stats.cameraPosition.x,
                            stats.cameraPosition.y,
                            stats.cameraPosition.z);
                ImGui::Text("speed %.1f m/s   draws %u", stats.cameraSpeed, stats.drawCalls);
                if (stats.lodDrawn[1] != 0 || stats.lodDrawn[2] != 0) {
                    // Only once something is actually drawing below level 0:
                    // a row that reads "lod 19/0/0" every frame is a row
                    // nobody reads by the second day.
                    ImGui::Text("lod  %u / %u / %u", stats.lodDrawn[0], stats.lodDrawn[1], stats.lodDrawn[2]);
                }
                ImGui::Text("sim  tick %llu   entities %u   alpha %.2f",
                            static_cast<unsigned long long>(stats.simTicks),
                            stats.simEntities,
                            stats.simAlpha);
                // Gameplay controls are rebindable (Phase 8k), so this crib names the
                // shipped layout rather than claiming to be the live one - the engine
                // dev UI has no way to reach the game's binding table, and a hint that
                // silently goes stale is the exact lie the HUD prompts stopped telling.
                ImGui::TextDisabled(
                    "defaults: RMB steer, WASD thrust, Tab cruise, X assist, V cam, T target");
                ImGui::TextDisabled("defaults: 1/2/3 pips WEP/ENG/SYS, 4 balance (rebind in Settings)");
                // Full is the only mode that advertises F2, which is the other
                // half of why Full is the default.
                ImGui::TextDisabled(
                    "F1 console, F2 overlay, F3 debug draw, F5 shaders, F9/F10 save/load (fixed)");

                // Zone tree (Phase 8n). Mean says where the frame goes; max says what
                // the hitch was, and an fps counter averages exactly that away - which
                // is why both columns are here and neither is enough alone.
                if (stats.profiler != nullptr && stats.profiler->enabled() &&
                    stats.profiler->zoneCount() > 0) {
                    const core::Profiler& profiler = *stats.profiler;
                    ImGui::Separator();
                    ImGui::Text("%-22s %7s %7s %7s", "zone", "last", "mean", "max");
                    bool anyExternal = false;
                    for (std::uint32_t i = 0; i < profiler.zoneCount(); ++i) {
                        const core::ZoneReport zone = profiler.report(i);
                        // Indent by nesting depth: a parent's time includes its
                        // children's, and the shape is what says so.
                        char label[64];
                        std::snprintf(
                            label, sizeof(label), "%*s%s", static_cast<int>(zone.depth) * 2, "", zone.name);
                        ImGui::Text("%-22s %6.2f  %6.2f  %6.2f",
                                    label,
                                    zone.lastMilliseconds,
                                    zone.meanMilliseconds,
                                    zone.maxMilliseconds);
                        if (zone.counter > 0) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("  n=%llu", static_cast<unsigned long long>(zone.counter));
                        }
                        if (zone.external) {
                            ImGui::SameLine();
                            ImGui::TextDisabled(" *");
                            anyExternal = true;
                        }
                    }
                    // Phase 8o: a device-timed row's `last` is about a frame that has
                    // already ended, so read beside a CPU row it looks like the two
                    // disagree. Mean and max are the same samples shifted and compare
                    // fine, which is why the note names `last` alone.
                    if (anyExternal) {
                        ImGui::TextDisabled("* device-timed; last = a completed frame, not this one");
                    }
                }
            }
        }
        ImGui::End();
    }

    if (m_showConsole) {
        ImGui::SetNextWindowSize({560.0f, 220.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({12.0f, 120.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Console", &m_showConsole)) {
            const float inputHeight = m_commandHandler != nullptr ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
            if (ImGui::BeginChild(
                    "##log", {0, -inputHeight}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
                for (const auto& [level, text] : g_console.lines) {
                    ImGui::PushStyleColor(ImGuiCol_Text, levelColor(level));
                    ImGui::TextUnformatted(text.c_str());
                    ImGui::PopStyleColor();
                }
                if (g_console.scrollToBottom && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 24.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
                g_console.scrollToBottom = false;
            }
            ImGui::EndChild();
            if (m_commandHandler != nullptr) {
                buildConsoleInput();
            }
        }
        ImGui::End();
    }

    // F1 toggles the console; F2 cycles the overlay (Phase 8p). Both are dev
    // keys handled here rather than game bindings, for the reason the crib
    // above already states: this layer cannot reach the game's binding table,
    // and a dev key dressed up as a player binding would be that lie in the
    // other direction. They are reserved in `kReservedKeys` so the Controls
    // screen cannot hand either of them out.
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        m_showConsole = !m_showConsole;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        m_overlayMode = m_overlayMode == OverlayMode::Full      ? OverlayMode::Compact
                        : m_overlayMode == OverlayMode::Compact ? OverlayMode::Hidden
                                                                : OverlayMode::Full;
    }
}

void DevUi::setCommandHandler(CommandHandler handler, void* userData)
{
    m_commandHandler = handler;
    m_commandUserData = userData;
}

int DevUi::consoleTextCallback(ImGuiInputTextCallbackData* data)
{
    auto* ui = static_cast<DevUi*>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || ui->m_commandHistory.empty()) {
        return 0;
    }
    const int count = static_cast<int>(ui->m_commandHistory.size());
    if (data->EventKey == ImGuiKey_UpArrow) {
        ui->m_historyIndex =
            ui->m_historyIndex < 0 ? count - 1 : (ui->m_historyIndex > 0 ? ui->m_historyIndex - 1 : 0);
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (ui->m_historyIndex < 0) {
            return 0;
        }
        ++ui->m_historyIndex;
        if (ui->m_historyIndex >= count) {
            ui->m_historyIndex = -1;
        }
    }
    data->DeleteChars(0, data->BufTextLen);
    if (ui->m_historyIndex >= 0) {
        data->InsertChars(0, ui->m_commandHistory[ui->m_historyIndex].c_str());
    }
    return 0;
}

void DevUi::buildConsoleInput()
{
    ImGui::SetNextItemWidth(-1.0f);
    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputText(
            "##command", m_commandBuffer, sizeof m_commandBuffer, flags, &DevUi::consoleTextCallback, this)) {
        if (m_commandBuffer[0] != '\0') {
            m_commandHistory.emplace_back(m_commandBuffer);
            m_commandHandler(m_commandBuffer, m_commandUserData);
            m_commandBuffer[0] = '\0';
        }
        m_historyIndex = -1;
        ImGui::SetKeyboardFocusHere(-1); // keep typing without re-clicking
    }
}

} // namespace sol::ui
