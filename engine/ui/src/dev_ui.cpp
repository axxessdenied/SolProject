#include "sol/ui/dev_ui.hpp"

#include "dev_ui_platform.hpp"

#include "sol/core/log.hpp"
#include "sol/rhi/descriptors.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sol::ui {

namespace {

platform::Window* g_hookedWindow = nullptr;

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

bool messageHookTrampoline(void* windowHandle, std::uint32_t message, std::uint64_t wParam,
                           std::int64_t lParam)
{
    return devUiPlatformMessageHook(windowHandle, message, wParam, lParam);
}

void checkVkResult(VkResult result)
{
    if (result != VK_SUCCESS) {
        SOL_LOG_ERROR("[imgui] Vulkan call failed (%d)", static_cast<int>(result));
    }
}

ImVec4 levelColor(core::LogLevel level)
{
    switch (level) {
    case core::LogLevel::Warn: return {1.0f, 0.8f, 0.3f, 1.0f};
    case core::LogLevel::Error:
    case core::LogLevel::Fatal: return {1.0f, 0.35f, 0.35f, 1.0f};
    default: return {0.75f, 0.78f, 0.82f, 1.0f};
    }
}

} // namespace

bool DevUi::initialize(platform::Window& window, rhi::Context& context, VkFormat colorFormat,
                       VkFormat depthFormat, std::uint32_t swapchainImageCount)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini litter

    if (!devUiPlatformInit(window.nativeHandle())) {
        SOL_LOG_ERROR("[imgui] platform backend init failed");
        return false;
    }

    m_descriptorPool = rhi::createTextureDescriptorPool(context.device(), 8, /*allowFree=*/true);
    m_device = context.device();

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = context.instance();
    initInfo.PhysicalDevice = context.physicalDevice();
    initInfo.Device = context.device();
    initInfo.QueueFamily = context.graphicsQueueFamily();
    initInfo.Queue = context.graphicsQueue();
    initInfo.DescriptorPool = m_descriptorPool;
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = swapchainImageCount;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineRenderingCreateInfo = {};
    initInfo.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    static VkFormat s_colorFormat; // must outlive init (backend keeps the pointer)
    s_colorFormat = colorFormat;
    initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_colorFormat;
    initInfo.PipelineRenderingCreateInfo.depthAttachmentFormat = depthFormat;
    initInfo.CheckVkResultFn = &checkVkResult;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        SOL_LOG_ERROR("[imgui] Vulkan backend init failed");
        devUiPlatformShutdown();
        return false;
    }

    window.setMessageHook(&messageHookTrampoline);
    g_hookedWindow = &window;
    core::setLogSink(&consoleLogSink, nullptr);

    m_initialized = true;
    return true;
}

void DevUi::shutdown()
{
    if (!m_initialized) {
        return;
    }
    core::setLogSink(nullptr, nullptr);
    if (g_hookedWindow != nullptr) {
        g_hookedWindow->setMessageHook(nullptr);
        g_hookedWindow = nullptr;
    }
    ImGui_ImplVulkan_Shutdown();
    devUiPlatformShutdown();
    ImGui::DestroyContext();
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_initialized = false;
}

void DevUi::beginFrame(const OverlayStats& stats, const FlightHud& hud)
{
    ImGui_ImplVulkan_NewFrame();
    devUiPlatformNewFrame();
    ImGui::NewFrame();
    m_frameOpen = true;

    buildWindows(stats);
    if (hud.active) {
        buildFlightHud(hud);
    }
}

void DevUi::buildWindows(const OverlayStats& stats)
{
    // Perf overlay: fixed top-left, transparent, no interaction.
    ImGui::SetNextWindowPos({12.0f, 12.0f});
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags overlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("perf##overlay", nullptr, overlayFlags)) {
        ImGui::Text("%.1f fps  (%.2f ms)", stats.fps, stats.frameMilliseconds);
        ImGui::Text("cam  %.2f  %.2f  %.2f", stats.cameraPosition.x, stats.cameraPosition.y,
                    stats.cameraPosition.z);
        ImGui::Text("speed %.1f m/s   draws %u", stats.cameraSpeed, stats.drawCalls);
        ImGui::Text("sim  tick %llu   entities %u   alpha %.2f",
                    static_cast<unsigned long long>(stats.simTicks), stats.simEntities,
                    stats.simAlpha);
        ImGui::TextDisabled("RMB steer, WASD thrust, Tab cruise, X assist, V camera, T target");
        ImGui::TextDisabled("1/2/3 pips WEP/ENG/SYS, 4 balance");
        ImGui::TextDisabled("F1 console, F3 debug draw, F5 shaders, F9/F10 save/load");
    }
    ImGui::End();

    if (m_showConsole) {
        ImGui::SetNextWindowSize({560.0f, 220.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({12.0f, 120.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Console", &m_showConsole)) {
            const float inputHeight =
                m_commandHandler != nullptr ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
            if (ImGui::BeginChild("##log", {0, -inputHeight}, ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
                for (const auto& [level, text] : g_console.lines) {
                    ImGui::PushStyleColor(ImGuiCol_Text, levelColor(level));
                    ImGui::TextUnformatted(text.c_str());
                    ImGui::PopStyleColor();
                }
                if (g_console.scrollToBottom &&
                    ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 24.0f) {
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

    // F1 toggles the console.
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        m_showConsole = !m_showConsole;
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
        ui->m_historyIndex = ui->m_historyIndex < 0 ? count - 1
                                                    : (ui->m_historyIndex > 0 ? ui->m_historyIndex - 1
                                                                              : 0);
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
    if (ImGui::InputText("##command", m_commandBuffer, sizeof m_commandBuffer, flags,
                         &DevUi::consoleTextCallback, this)) {
        if (m_commandBuffer[0] != '\0') {
            m_commandHistory.emplace_back(m_commandBuffer);
            m_commandHandler(m_commandBuffer, m_commandUserData);
            m_commandBuffer[0] = '\0';
        }
        m_historyIndex = -1;
        ImGui::SetKeyboardFocusHere(-1); // keep typing without re-clicking
    }
}

namespace {

void formatDistance(double meters, char* buffer, std::size_t size)
{
    if (meters < 10'000.0) {
        std::snprintf(buffer, size, "%.0f m", meters);
    } else if (meters < 1.0e9) {
        std::snprintf(buffer, size, "%.1f km", meters / 1000.0);
    } else {
        std::snprintf(buffer, size, "%.2f Mkm", meters / 1.0e9);
    }
}

void formatSpeed(float metersPerSecond, char* buffer, std::size_t size)
{
    if (metersPerSecond < 10'000.0f) {
        std::snprintf(buffer, size, "%.1f m/s", metersPerSecond);
    } else {
        std::snprintf(buffer, size, "%.0f km/s", metersPerSecond / 1000.0f);
    }
}

void formatSpeedSigned(float metersPerSecond, char* buffer, std::size_t size)
{
    if (std::abs(metersPerSecond) < 10'000.0f) {
        std::snprintf(buffer, size, "%+.1f m/s", metersPerSecond);
    } else {
        std::snprintf(buffer, size, "%+.0f km/s", metersPerSecond / 1000.0f);
    }
}

} // namespace

void DevUi::buildFlightHud(const FlightHud& hud)
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 center = {display.x * 0.5f, display.y * 0.5f};
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    // Boresight crosshair.
    const ImU32 hudColor = IM_COL32(140, 220, 160, 200);
    draw->AddCircle(center, 10.0f, hudColor, 0, 1.5f);
    draw->AddLine({center.x - 18.0f, center.y}, {center.x - 10.0f, center.y}, hudColor, 1.5f);
    draw->AddLine({center.x + 10.0f, center.y}, {center.x + 18.0f, center.y}, hudColor, 1.5f);
    draw->AddLine({center.x, center.y - 18.0f}, {center.x, center.y - 10.0f}, hudColor, 1.5f);

    // Target marker: project the camera-space direction; clamp to a screen
    // ring when the target is outside the view (or behind).
    {
        const core::Vec3 d = hud.targetDirectionCamera;
        const float focal = (display.y * 0.5f) / hud.tanHalfFovY;
        const ImU32 targetColor = IM_COL32(255, 200, 80, 220);
        bool onScreen = false;
        ImVec2 marker = center;
        if (d.z < -0.01f) {
            marker = {center.x + (d.x / -d.z) * focal, center.y - (d.y / -d.z) * focal};
            const float margin = 24.0f;
            onScreen = marker.x > margin && marker.x < (display.x - margin) && marker.y > margin &&
                       marker.y < (display.y - margin);
        }
        if (onScreen) {
            draw->AddCircle(marker, 14.0f, targetColor, 4, 2.0f); // diamond
            char distance[32];
            formatDistance(hud.targetDistanceMeters, distance, sizeof(distance));
            char closing[32];
            formatSpeedSigned(hud.closingSpeedMetersPerSecond, closing, sizeof(closing));
            char label[96];
            std::snprintf(label, sizeof(label), "%s  %s  %s", hud.targetName, distance, closing);
            draw->AddText({marker.x + 18.0f, marker.y - 6.0f}, targetColor, label);
        } else {
            // Edge arrow toward the target.
            const float screenX = d.x;
            const float screenY = -d.y; // screen y grows downward
            const float len = std::sqrt((screenX * screenX) + (screenY * screenY));
            const float nx = len > 0.0001f ? screenX / len : 0.0f;
            const float ny = len > 0.0001f ? screenY / len : -1.0f;
            const float ringRadius = std::min(display.x, display.y) * 0.38f;
            const ImVec2 tip = {center.x + nx * ringRadius, center.y + ny * ringRadius};
            const ImVec2 back = {center.x + nx * (ringRadius - 16.0f),
                                 center.y + ny * (ringRadius - 16.0f)};
            const ImVec2 side = {-ny * 7.0f, nx * 7.0f};
            draw->AddTriangleFilled(tip, {back.x + side.x, back.y + side.y},
                                    {back.x - side.x, back.y - side.y}, targetColor);
        }
    }

    // Readout strip, bottom center.
    ImGui::SetNextWindowPos({center.x, display.y - 16.0f}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.4f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("flight##hud", nullptr, flags)) {
        char speed[32];
        formatSpeed(hud.speedMetersPerSecond, speed, sizeof(speed));
        ImGui::Text("SPD %s", speed);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextColored(hud.assist ? ImVec4{0.55f, 0.86f, 0.63f, 1.0f}
                                      : ImVec4{1.0f, 0.55f, 0.4f, 1.0f},
                           hud.assist ? "ASSIST" : "MANUAL");
        if (hud.boost) {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored({1.0f, 0.8f, 0.3f, 1.0f}, "BOOST");
        }
        if (hud.cruise) {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored({0.5f, 0.75f, 1.0f, 1.0f}, "CRUISE");
        }
        ImGui::SameLine(0.0f, 24.0f);
        char distance[32];
        formatDistance(hud.targetDistanceMeters, distance, sizeof(distance));
        ImGui::Text("TGT %s  %s", hud.targetName, distance);
        ImGui::SameLine(0.0f, 24.0f);
        // Pips as filled/empty bars, WEP charge as a percentage.
        auto pipBar = [&](const char* name, int pips, ImVec4 color) {
            char bar[16];
            int i = 0;
            for (; i < pips && i < hud.pipMax && i < 15; ++i) {
                bar[i] = '|';
            }
            for (; i < hud.pipMax && i < 15; ++i) {
                bar[i] = '.';
            }
            bar[i] = '\0';
            ImGui::TextColored(color, "%s %s", name, bar);
            ImGui::SameLine(0.0f, 12.0f);
        };
        pipBar("WEP", hud.pipsWeapons, {1.0f, 0.6f, 0.45f, 1.0f});
        pipBar("ENG", hud.pipsEngines, {0.55f, 0.86f, 0.63f, 1.0f});
        pipBar("SYS", hud.pipsShields, {0.5f, 0.75f, 1.0f, 1.0f});
        ImGui::Text("CAP %d%%", static_cast<int>(hud.weaponCharge * 100.0f + 0.5f));
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextDisabled("%s", hud.cameraMode);
    }
    ImGui::End();
}

void DevUi::render(VkCommandBuffer commandBuffer)
{
    if (!m_frameOpen) {
        return;
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    m_frameOpen = false;
}

void DevUi::discardFrame()
{
    if (m_frameOpen) {
        ImGui::EndFrame();
        m_frameOpen = false;
    }
}

} // namespace sol::ui
