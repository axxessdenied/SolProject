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

void DevUi::beginFrame(const OverlayStats& stats, const FlightHud& hud, StationPanel* station)
{
    ImGui_ImplVulkan_NewFrame();
    devUiPlatformNewFrame();
    ImGui::NewFrame();
    m_frameOpen = true;

    buildWindows(stats);
    if (hud.active) {
        buildFlightHud(hud);
    }
    if (station != nullptr) {
        buildStationPanel(*station);
    }
}

void DevUi::buildStationPanel(StationPanel& station)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkSize.x - 12.0f, 60.0f}, ImGuiCond_FirstUseEver,
                            {1.0f, 0.0f});
    ImGui::SetNextWindowSize({520.0f, 0.0f}, ImGuiCond_FirstUseEver);
    char title[128];
    std::snprintf(title, sizeof(title), "Station - %s###station", station.trade.stationName);
    if (ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Credits %.0f", station.trade.credits);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Text("Cargo %.0f / %.0f", station.trade.cargoUsed, station.trade.cargoCapacity);
        if (ImGui::BeginTabBar("station_tabs")) {
            if (ImGui::BeginTabItem("Trade")) {
                buildTradeTab(station.trade);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Outfitting")) {
                buildOutfittingTab(station);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Shipyard")) {
                buildShipyardTab(station);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Crew")) {
                buildCrewTab(station);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Missions")) {
                buildMissionsTab(station);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Factions")) {
                buildFactionsTab(station);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

namespace {

// A catalog table: Name | Detail | Price | <action button>. Returns the
// clicked row, or -1. fitted > 0 renders a count tag after the name.
int outfitCatalog(const char* tableId, std::span<const OutfitRow> rows, const char* action)
{
    int clicked = -1;
    if (rows.empty()) {
        ImGui::TextDisabled("(nothing available)");
        return clicked;
    }
    if (ImGui::BeginTable(tableId, 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableHeadersRow();
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            const OutfitRow& item = rows[static_cast<std::size_t>(row)];
            ImGui::TableNextRow();
            ImGui::PushID(row);
            ImGui::TableNextColumn();
            if (item.fitted > 0) {
                ImGui::Text("%s (x%d)", item.name, item.fitted);
            } else {
                ImGui::TextUnformatted(item.name);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(item.detail);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", item.price);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(action)) {
                clicked = row;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return clicked;
}

} // namespace

void DevUi::buildOutfittingTab(StationPanel& station)
{
    ImGui::TextWrapped("%s", station.fitSummary);
    ImGui::TextDisabled("Insurance deductible at current fit: %.0f cr", station.deductible);
    ImGui::SeparatorText("Modules (Sell removes one fitted instance at resale)");
    if (const int row = outfitCatalog("modules", station.modules, "Buy"); row >= 0) {
        station.action = {StationAction::Kind::BuyModule, station.modules[row].id, row};
    }
    // Selling goes through the same catalog rows: one Sell button per fitted id.
    for (int row = 0; row < static_cast<int>(station.modules.size()); ++row) {
        const OutfitRow& item = station.modules[static_cast<std::size_t>(row)];
        if (item.fitted <= 0) {
            continue;
        }
        ImGui::PushID(1000 + row);
        if (ImGui::SmallButton("Sell")) {
            station.action = {StationAction::Kind::SellModule, item.id, row};
        }
        ImGui::SameLine();
        ImGui::Text("%s (x%d fitted)", item.name, item.fitted);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Weapon mount (swap sells the old weapon at resale)");
    if (const int row = outfitCatalog("weapons", station.weapons, "Mount"); row >= 0) {
        station.action = {StationAction::Kind::BuyWeapon, station.weapons[row].id, row};
    }
}

void DevUi::buildShipyardTab(StationPanel& station)
{
    ImGui::SeparatorText("Your fleet");
    if (ImGui::BeginTable("fleet", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Ship", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (int row = 0; row < static_cast<int>(station.fleet.size()); ++row) {
            const FleetRow& ship = station.fleet[static_cast<std::size_t>(row)];
            ImGui::TableNextRow();
            ImGui::PushID(row);
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", ship.name,
                        ship.active ? " (active)" : ship.storedHere ? "" : " (elsewhere)");
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", ship.value);
            ImGui::TableNextColumn();
            if (!ship.active && ship.storedHere) {
                if (ImGui::SmallButton("Switch")) {
                    station.action = {StationAction::Kind::SwitchShip, "", row};
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Sell")) {
                    station.action = {StationAction::Kind::SellShip, "", row};
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("For sale (new ships store here until you switch)");
    if (const int row = outfitCatalog("ships", station.shipCatalog, "Buy"); row >= 0) {
        station.action = {StationAction::Kind::BuyShip, station.shipCatalog[row].id, row};
    }
}

void DevUi::buildCrewTab(StationPanel& station)
{
    ImGui::SeparatorText("Aboard");
    if (station.crewAboard.empty()) {
        ImGui::TextDisabled("(no crew aboard)");
    }
    for (int row = 0; row < static_cast<int>(station.crewAboard.size()); ++row) {
        const OutfitRow& member = station.crewAboard[static_cast<std::size_t>(row)];
        ImGui::PushID(row);
        if (ImGui::SmallButton("Dismiss")) {
            station.action = {StationAction::Kind::FireCrew, member.id, row};
        }
        ImGui::SameLine();
        ImGui::Text("%s - %s", member.name, member.detail);
        ImGui::PopID();
    }
    ImGui::SeparatorText("For hire (one-time fee, no refund)");
    if (const int row = outfitCatalog("crew", station.crewCatalog, "Hire"); row >= 0) {
        station.action = {StationAction::Kind::HireCrew, station.crewCatalog[row].id, row};
    }
}

void DevUi::buildMissionsTab(StationPanel& station)
{
    ImGui::SeparatorText("Board");
    if (station.missionOffers.empty()) {
        ImGui::TextDisabled("(no offers)");
    } else if (ImGui::BeginTable("mission_offers", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Contract", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 3.2f);
        ImGui::TableSetupColumn("Reward", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##accept", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(station.missionOffers.size()); ++i) {
            const MissionRow& offer = station.missionOffers[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (offer.campaign) {
                ImGui::TextColored({0.95f, 0.8f, 0.35f, 1.0f}, "%s", offer.title);
            } else {
                ImGui::TextUnformatted(offer.title);
            }
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", offer.detail);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f cr", offer.reward);
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            ImGui::BeginDisabled(!offer.acceptable);
            if (ImGui::SmallButton("Accept")) {
                station.action = {.kind = StationAction::Kind::AcceptMission, .index = i};
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Journal");
    if (station.missionJournal.empty()) {
        ImGui::TextDisabled("(no active missions)");
    } else if (ImGui::BeginTable("mission_journal", 3,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Mission", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Objective", ImGuiTableColumnFlags_WidthStretch, 3.6f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(station.missionJournal.size()); ++i) {
            const MissionRow& mission = station.missionJournal[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (mission.campaign) {
                ImGui::TextColored({0.95f, 0.8f, 0.35f, 1.0f}, "%s", mission.title);
            } else {
                ImGui::TextUnformatted(mission.title);
            }
            if (mission.tracked) {
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::TextDisabled("*");
            }
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", mission.detail);
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (!mission.tracked && ImGui::SmallButton("Track")) {
                station.action = {.kind = StationAction::Kind::TrackMission, .index = i};
            }
            if (!mission.tracked) {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Abandon")) {
                station.action = {.kind = StationAction::Kind::AbandonMission, .index = i};
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DevUi::buildFactionsTab(StationPanel& station)
{
    if (ImGui::BeginTable("factions", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Faction", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Standing", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableHeadersRow();
        for (const FactionRow& faction : station.factions) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(faction.name);
            ImGui::TableNextColumn();
            char label[32];
            std::snprintf(label, sizeof(label), "%+.0f", faction.standing);
            const ImVec4 color = faction.standing < -30.0f ? ImVec4{0.9f, 0.3f, 0.25f, 1.0f}
                                 : faction.standing > 30.0f
                                     ? ImVec4{0.35f, 0.85f, 0.4f, 1.0f}
                                     : ImVec4{0.75f, 0.75f, 0.75f, 1.0f};
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
            ImGui::ProgressBar((faction.standing + 100.0f) / 200.0f, {-1.0f, 0.0f}, label);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::Text("%s, %s", faction.attitude, faction.detail);
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Recent raids");
    if (station.factionNotes[0] == '\0') {
        ImGui::TextDisabled("(quiet lately)");
    } else {
        ImGui::TextWrapped("%s", station.factionNotes);
    }
}

void DevUi::buildTradeTab(TradePanel& trade)
{
    {
        if (ImGui::BeginTable("goods", 6,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Commodity", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Stock", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Held", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Buy", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Sell", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableHeadersRow();
            for (int row = 0; row < static_cast<int>(trade.rows.size()); ++row) {
                const TradeRow& goods = trade.rows[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::PushID(row);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(goods.name);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", goods.price);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", goods.stock);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", goods.cargo);
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("1")) {
                    trade.action = {row, 1.0f, true};
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("10")) {
                    trade.action = {row, 10.0f, true};
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("1##s")) {
                    trade.action = {row, 1.0f, false};
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("10##s")) {
                    trade.action = {row, 10.0f, false};
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Prices move with stock; NPC traders share this market.");
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

bool DevUi::wantsMouseCapture() const
{
    return m_initialized && ImGui::GetIO().WantCaptureMouse;
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

    // Boresight crosshair; flashes red while taking hits.
    const float flash = hud.damageFlash;
    const ImU32 hudColor = IM_COL32(140 + static_cast<int>(115.0f * flash),
                                    220 - static_cast<int>(140.0f * flash),
                                    160 - static_cast<int>(100.0f * flash), 200);
    draw->AddCircle(center, 10.0f, hudColor, 0, 1.5f);
    draw->AddLine({center.x - 18.0f, center.y}, {center.x - 10.0f, center.y}, hudColor, 1.5f);
    draw->AddLine({center.x + 10.0f, center.y}, {center.x + 18.0f, center.y}, hudColor, 1.5f);
    draw->AddLine({center.x, center.y - 18.0f}, {center.x, center.y - 10.0f}, hudColor, 1.5f);

    // Shield facings (decisions/002): fore arc above the crosshair, aft arc
    // below; arc length tracks the facing's remaining strength.
    {
        constexpr float kPi = 3.14159265f;
        auto shieldArc = [&](float fraction, bool fore) {
            if (fraction <= 0.0f) {
                return;
            }
            const float halfSpan = (kPi * 0.35f) * fraction;
            const float centerAngle = fore ? -kPi * 0.5f : kPi * 0.5f;
            draw->PathArcTo(center, 24.0f, centerAngle - halfSpan, centerAngle + halfSpan, 24);
            draw->PathStroke(IM_COL32(120, 190, 255, 210), 0, 2.5f);
        };
        shieldArc(hud.shieldFore, true);
        shieldArc(hud.shieldAft, false);
    }

    // Projectile lead marker: aim here to land shots on the target's path.
    if (hud.hasLead) {
        const core::Vec3 d = hud.leadDirectionCamera;
        if (d.z < -0.01f) {
            const float focal = (display.y * 0.5f) / hud.tanHalfFovY;
            const ImVec2 lead = {center.x + (d.x / -d.z) * focal,
                                 center.y - (d.y / -d.z) * focal};
            const ImU32 leadColor = IM_COL32(255, 130, 90, 230);
            draw->AddCircle(lead, 5.0f, leadColor, 0, 1.8f);
            draw->AddLine({lead.x - 9.0f, lead.y}, {lead.x - 5.0f, lead.y}, leadColor, 1.5f);
            draw->AddLine({lead.x + 5.0f, lead.y}, {lead.x + 9.0f, lead.y}, leadColor, 1.5f);
        }
    }

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
        if (hud.autopilot) {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored({0.8f, 0.6f, 1.0f, 1.0f}, "AUTO");
        }
        ImGui::SameLine(0.0f, 24.0f);
        char distance[32];
        formatDistance(hud.targetDistanceMeters, distance, sizeof(distance));
        ImGui::Text("TGT %s  %s", hud.targetName, distance);
        if (hud.targetFaction[0] != '\0') {
            const bool hostile = std::strcmp(hud.targetAttitude, "hostile") == 0;
            const bool friendly = std::strcmp(hud.targetAttitude, "friendly") == 0;
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextColored(hostile    ? ImVec4{0.9f, 0.3f, 0.25f, 1.0f}
                               : friendly ? ImVec4{0.35f, 0.85f, 0.4f, 1.0f}
                                          : ImVec4{0.7f, 0.7f, 0.7f, 1.0f},
                               "%s [%s]", hud.targetFaction, hud.targetAttitude);
        }
        if (hud.targetIsShip) {
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextColored({1.0f, 0.78f, 0.31f, 1.0f}, "[S %d/%d H %d%%]",
                               static_cast<int>(hud.targetShieldFore * 100.0f + 0.5f),
                               static_cast<int>(hud.targetShieldAft * 100.0f + 0.5f),
                               static_cast<int>(hud.targetHull * 100.0f + 0.5f));
        }
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
        ImGui::TextColored({0.47f, 0.75f, 1.0f, 1.0f}, "SHD %d/%d",
                           static_cast<int>(hud.shieldFore * 100.0f + 0.5f),
                           static_cast<int>(hud.shieldAft * 100.0f + 0.5f));
        ImGui::SameLine(0.0f, 12.0f);
        const float hullFraction = hud.hull;
        ImGui::TextColored(hullFraction > 0.5f ? ImVec4{0.8f, 0.85f, 0.8f, 1.0f}
                                               : ImVec4{1.0f, 0.45f, 0.35f, 1.0f},
                           "HUL %d%%", static_cast<int>(hullFraction * 100.0f + 0.5f));
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextDisabled("%s", hud.cameraMode);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextDisabled("SYS %s", hud.systemName);
        if (hud.gateInRange) {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored({0.5f, 0.9f, 1.0f, 1.0f}, "[J] JUMP");
        }
        if (hud.docked) {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored({0.55f, 0.86f, 0.63f, 1.0f}, "DOCKED %s  [G] UNDOCK",
                               hud.dockedStationName);
        } else if (hud.dockInRange) {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored({0.5f, 0.9f, 1.0f, 1.0f}, "[G] DOCK");
        }
        if (hud.missionTitle[0] != '\0') {
            ImGui::TextColored({0.95f, 0.8f, 0.35f, 1.0f}, "MSN %s:", hud.missionTitle);
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextUnformatted(hud.missionObjective);
            if (hud.missionDeadline > 0.0) {
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextColored({0.9f, 0.55f, 0.3f, 1.0f}, "(%d:%02d)",
                                   static_cast<int>(hud.missionDeadline) / 60,
                                   static_cast<int>(hud.missionDeadline) % 60);
            }
        }
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
